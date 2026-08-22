#include "Headless/Output.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>

#include "Core/Clock.h"
#include "Core/Time.h"
#include "Headless/Device.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/PresentationInfo.h"
#include "Seam/Presenter.h"
#include "Seam/SyncPoint.h"
#include "Testing/Test.h"

// Driven through a `HeadlessDevice` throughout, because that is how it is used: the completions are
// the device's to emit, and a test that reached past it would be exercising a path the composition
// root does not have.
//
// What is worth checking here is the three things this is a real implementation of rather than a
// double — the target ring's hold, the asynchrony of reconfiguration, and the honesty of what a flip
// reports. The arithmetic underneath is Vblank.Test.cpp's.

namespace
{
using namespace std::chrono_literals;

constexpr Instant At(std::int64_t milliseconds) noexcept
{
	return Monotonic::FromNanoseconds(milliseconds * 1'000'000);
}

OutputConfiguration Panel(PixelSize<DeviceSpace> resolution = { 800, 600 }, Duration period = 10ms)
{
	OutputConfiguration configuration;
	configuration.Resolution = resolution;
	configuration.Period = period;

	return configuration;
}

// The one-layer composite the frame loop builds today: the whole target, opaque, immediate.
PresentLayer Composite(std::uint32_t target, PixelSize<DeviceSpace> size)
{
	return PresentLayer{ .Target = target,
		                 .Blend = BlendMode::Opaque,
		                 .Acquire = SyncPoint::Immediate(),
		                 .Source = { {}, { static_cast<float>(size.Width), static_cast<float>(size.Height) } },
		                 .Destination = { {}, size },
		                 .Damage = {} };
}

// Everything a case needs, plus the observers a `FrameOutput` would have connected.
struct Harness
{
	ManualClock Clock{ At(1000) };
	HeadlessDevice Device{ Clock };
	HeadlessOutput* Panel = nullptr;

	std::optional<PresentationInfo> LastPresented;
	std::optional<OutputConfiguration> LastReconfigured;
	int Invalidations = 0;
	int Presents = 0;

	Connection<const PresentationInfo&> OnPresented;
	Connection<const OutputConfiguration&> OnReconfigured;
	Connection<> OnInvalidated;

	explicit Harness(const OutputConfiguration& configuration, HeadlessOutputPolicy policy = {})
	{
		Panel = Device.Add(configuration, policy);
		OnPresented.ConnectTo<&Harness::Presented>(Panel->Presented, *this);
		OnReconfigured.ConnectTo<&Harness::Reconfigured>(Panel->Reconfigured, *this);
		OnInvalidated.ConnectTo<&Harness::Invalidated>(Panel->TargetsInvalidated, *this);
	}

	void Presented(const PresentationInfo& info)
	{
		LastPresented = info;
		++Presents;
	}

	void Reconfigured(const OutputConfiguration& achieved) { LastReconfigured = achieved; }

	void Invalidated() { ++Invalidations; }

	// Acquire, present, and report whether the commit was taken.
	Result<void> Commit()
	{
		const std::optional<std::uint32_t> target = Panel->AcquireTarget();

		if (!target)
		{
			return Failure(EBUSY, "no free target");
		}

		const PresentLayer layer = Composite(*target, Panel->Configuration().Resolution);

		return Panel->Present({ &layer, 1 });
	}

	void RunTo(Instant now)
	{
		Clock.Set(now);
		(void)Device.Drain();
	}
};
} // namespace

GYRO_TEST(HeadlessOutput, TargetsAreBuiltForTheConfiguration)
{
	Harness harness{ Panel({ 800, 600 }) };

	GYRO_REQUIRE_EQ(harness.Panel->Targets().size(), std::size_t{ 2 });

	for (const RenderTarget& target : harness.Panel->Targets())
	{
		GYRO_CHECK(target.IsValid());
		GYRO_CHECK(target.IsMapped());
		GYRO_CHECK_EQ(target.Size, PixelSize<DeviceSpace>{ 800, 600 });
		GYRO_CHECK_EQ(target.AsMapped()->Length, std::size_t{ 800 * 600 * 4 });
	}
}

// The bound Frame/Loop.h names when it declines to treat a refused acquire as an error, asserted in
// steady state rather than from the first frame — a ring with nothing on the glass yet has one more
// image free than it ever will again, and the interesting number is the one it settles at.
//
// A buffer on a plane is held until the next flip retires it, so a double-buffered output in steady
// state has exactly one image to draw into and none at all between its present and its flip. That is
// what "cannot run ahead" means concretely: there is nowhere to build the next frame.
GYRO_TEST(HeadlessOutput, ADoubleBufferedOutputHasNowhereToRunAheadInto)
{
	Harness harness{ Panel() };

	GYRO_REQUIRE(harness.Commit().has_value());
	harness.RunTo(At(1011));
	GYRO_REQUIRE_EQ(harness.Presents, 1);

	// One on the glass, one to draw into.
	GYRO_CHECK_EQ(harness.Panel->FreeTargets(), 1U);

	GYRO_REQUIRE(harness.Commit().has_value());
	GYRO_CHECK_EQ(harness.Panel->FreeTargets(), 0U);

	harness.RunTo(At(1021));
	GYRO_CHECK_EQ(harness.Panel->FreeTargets(), 1U);
}

// A third image is what buys the lead: one on the glass, one awaiting its flip, and one still free —
// which is where decision 30's pipeline would build the frame after next. Nothing does that yet; what
// this asserts is that the backend can express it, since a ring that could not would make the
// mechanism untestable before it is written.
GYRO_TEST(HeadlessOutput, ATripleBufferedOutputKeepsOneTargetToRunAheadInto)
{
	Harness harness{ Panel(), HeadlessOutputPolicy{ .Targets = 3 } };

	GYRO_REQUIRE(harness.Commit().has_value());
	harness.RunTo(At(1011));
	GYRO_CHECK_EQ(harness.Panel->FreeTargets(), 2U);

	GYRO_REQUIRE(harness.Commit().has_value());
	GYRO_CHECK_EQ(harness.Panel->FreeTargets(), 1U);
}

// KMS refuses a second nonblocking commit on a CRTC that has not flipped, and so does this. The frame
// loop's own guard should make it unreachable, which is why it is worth stating at the seam.
GYRO_TEST(HeadlessOutput, ASecondCommitBeforeTheFlipIsRefused)
{
	Harness harness{ Panel(), HeadlessOutputPolicy{ .Targets = 3 } };

	GYRO_REQUIRE(harness.Commit().has_value());

	const Result<void> second = harness.Commit();
	GYRO_REQUIRE(!second.has_value());
	GYRO_CHECK_EQ(second.error().Code(), EBUSY);
}

// The flip lands at the vblank the commit latched into, and reports what the panel did rather than
// what it was configured to do.
GYRO_TEST(HeadlessOutput, AFlipReportsTheVblankItLandedOn)
{
	Harness harness{ Panel(), HeadlessOutputPolicy{ .PeriodError = 100us } };

	GYRO_REQUIRE(harness.Commit().has_value());

	// Nothing has happened yet: the commit was at 1000ms and vblank one is at 1010.1ms.
	harness.RunTo(At(1005));
	GYRO_CHECK_EQ(harness.Presents, 0);

	harness.RunTo(At(1011));
	GYRO_REQUIRE_EQ(harness.Presents, 1);
	GYRO_CHECK_EQ(harness.LastPresented->Sequence, 1U);
	GYRO_CHECK_EQ(harness.LastPresented->PresentedAt, At(1000) + 10ms + 100us);
	GYRO_CHECK_EQ(harness.LastPresented->Period, 10ms + 100us);
	GYRO_CHECK(harness.LastPresented->Vsync);
	GYRO_CHECK(harness.LastPresented->HardwareClock);
}

// Seam/PresentationInfo.h is explicit that precision is per output rather than per machine: a virtual
// output's clock is flow control and is never precise while the panel beside it is.
GYRO_TEST(HeadlessOutput, AVirtualOutputReportsThatItsClockIsNotHardware)
{
	Harness harness{ Panel(), HeadlessOutputPolicy{ .HardwareClock = false } };

	GYRO_REQUIRE(harness.Commit().has_value());
	harness.RunTo(At(1011));

	GYRO_REQUIRE_EQ(harness.Presents, 1);
	GYRO_CHECK(!harness.LastPresented->HardwareClock);
}

// Decision 73 in a backend: the verb records and returns, and the transition completes on a drain some
// milliseconds later.
GYRO_TEST(HeadlessOutput, ReconfigurationIsInitiatedAndNotPerformed)
{
	Harness harness{ Panel({ 800, 600 }), HeadlessOutputPolicy{ .ModesetLatency = 20ms } };

	OutputConfiguration wanted = Panel({ 1920, 1080 }, 8ms);
	wanted.Generation = 4;
	harness.Panel->Reconfigure(wanted);

	// Nothing has changed at the call, which is the whole of what the verb promises.
	GYRO_CHECK_EQ(harness.Panel->Configuration().Resolution, PixelSize<DeviceSpace>{ 800, 600 });
	GYRO_CHECK_EQ(harness.Invalidations, 0);
	GYRO_CHECK(!harness.LastReconfigured.has_value());

	// The images go first, and Targets() is empty between the two signals.
	harness.RunTo(At(1001));
	GYRO_CHECK_EQ(harness.Invalidations, 1);
	GYRO_CHECK(harness.Panel->Targets().empty());
	GYRO_CHECK(!harness.LastReconfigured.has_value());

	// And an output mid-reconfiguration holds its last frame and refuses commits.
	const Result<void> refused = harness.Panel->Present({});
	GYRO_REQUIRE(!refused.has_value());
	GYRO_CHECK_EQ(refused.error().Code(), EBUSY);

	harness.RunTo(At(1021));
	GYRO_REQUIRE(harness.LastReconfigured.has_value());
	GYRO_CHECK_EQ(harness.LastReconfigured->Generation, 4U);
	GYRO_CHECK_EQ(harness.LastReconfigured->Resolution, PixelSize<DeviceSpace>{ 1920, 1080 });
	GYRO_CHECK_EQ(harness.Panel->Targets().size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(harness.Panel->Targets()[0].Size, PixelSize<DeviceSpace>{ 1920, 1080 });
	GYRO_CHECK(wanted.SatisfiedBy(*harness.LastReconfigured));
}

// Seam/OutputConfiguration.h has no failure signal, and this is how a request the hardware could not
// honour reports itself.
GYRO_TEST(HeadlessOutput, AModeThePanelDoesNotHaveComesBackAsSomethingElse)
{
	Harness harness{ Panel({ 800, 600 }) };

	const std::array<OutputMode, 2> modes{
		OutputMode{ { 800, 600 }, 10ms },
		OutputMode{ { 1024, 768 }, 10ms },
	};
	harness.Panel->SetModes(modes);

	const OutputConfiguration wanted = Panel({ 3840, 2160 }, 4ms);
	harness.Panel->Reconfigure(wanted);
	harness.RunTo(At(1001));

	GYRO_REQUIRE(harness.LastReconfigured.has_value());
	GYRO_CHECK(!wanted.SatisfiedBy(*harness.LastReconfigured));
	GYRO_CHECK_EQ(harness.LastReconfigured->Resolution, PixelSize<DeviceSpace>{ 800, 600 });
}

// The range is learned rather than requested, which is why SatisfiedBy ignores it.
GYRO_TEST(HeadlessOutput, TheVariableRefreshRangeIsLearnedAtTheTransition)
{
	Harness harness{ Panel() };
	harness.Panel->SetRefreshRange({ .Enabled = true, .Shortest = 7ms, .Longest = 20ms });

	OutputConfiguration wanted = Panel();
	wanted.Refresh.Enabled = true;
	harness.Panel->Reconfigure(wanted);
	harness.RunTo(At(1001));

	GYRO_REQUIRE(harness.LastReconfigured.has_value());
	GYRO_CHECK(wanted.SatisfiedBy(*harness.LastReconfigured));
	GYRO_CHECK_EQ(harness.LastReconfigured->Refresh.Shortest, 7ms);
	GYRO_CHECK_EQ(harness.LastReconfigured->Refresh.Longest, 20ms);
	GYRO_CHECK(!harness.LastReconfigured->Refresh.IsDegenerate());
}

// A panel with no range to report says so however it was asked, rather than inventing a degenerate one
// that reads as a variable-refresh output which never varies.
GYRO_TEST(HeadlessOutput, APanelWithNoRangeRefusesVariableRefresh)
{
	Harness harness{ Panel() };

	OutputConfiguration wanted = Panel();
	wanted.Refresh.Enabled = true;
	harness.Panel->Reconfigure(wanted);
	harness.RunTo(At(1001));

	GYRO_REQUIRE(harness.LastReconfigured.has_value());
	GYRO_CHECK(!harness.LastReconfigured->Refresh.Enabled);
	GYRO_CHECK(!wanted.SatisfiedBy(*harness.LastReconfigured));
}

// Seam/OutputConfiguration.h: presenting to a dark panel is an error rather than a silent no-op,
// because a frame loop still submitting to one is a bug that otherwise shows up as battery life.
GYRO_TEST(HeadlessOutput, AnUnpoweredOutputOwesNoFramesAndRefusesCommits)
{
	OutputConfiguration configuration = Panel();
	configuration.Powered = false;

	Harness harness{ configuration };

	GYRO_CHECK(harness.Panel->Targets().empty());
	GYRO_CHECK(!harness.Panel->AcquireTarget().has_value());

	const Result<void> refused = harness.Panel->Present({});
	GYRO_REQUIRE(!refused.has_value());
	GYRO_CHECK_EQ(refused.error().Code(), EINVAL);
}

// The glass is a buffer, which is what a golden-image test reads and a frame dump writes.
GYRO_TEST(HeadlessOutput, TheScanoutIsWhatWasPresented)
{
	Harness harness{ Panel({ 4, 4 }) };

	GYRO_CHECK(harness.Panel->Scanout().empty());

	const std::optional<std::uint32_t> target = harness.Panel->AcquireTarget();
	GYRO_REQUIRE(target.has_value());

	// Stand in for a renderer: write into the mapping the presenter handed out.
	const MappedImage* image = harness.Panel->Targets()[*target].AsMapped();
	GYRO_REQUIRE(image != nullptr);
	image->Pixels[0] = std::byte{ 0x7F };

	const PresentLayer layer = Composite(*target, { 4, 4 });
	GYRO_REQUIRE(harness.Panel->Present({ &layer, 1 }).has_value());
	harness.RunTo(At(1011));

	GYRO_REQUIRE_EQ(harness.Panel->Scanout().size(), std::size_t{ 4 * 4 * 4 });
	GYRO_CHECK_EQ(harness.Panel->Scanout()[0], std::byte{ 0x7F });
}

// The refusal that arrives after the frame's budget was planned. It is EBUSY rather than EINVAL, and
// the commit leaves nothing queued — the output owes exactly what it owed before.
GYRO_TEST(HeadlessOutput, AControllerRefusalLeavesTheOutputOwingItsFrame)
{
	Harness harness{ Panel() };
	harness.Panel->Catalog().RefuseNext(1);

	const Result<void> refused = harness.Commit();
	GYRO_REQUIRE(!refused.has_value());
	GYRO_CHECK_EQ(refused.error().Code(), EBUSY);
	GYRO_CHECK(!harness.Panel->IsFlipPending());

	harness.RunTo(At(1011));
	GYRO_CHECK_EQ(harness.Presents, 0);
}

// A commit naming a target nobody acquired is the caller's bug rather than a condition to retry.
GYRO_TEST(HeadlessOutput, ACommitMustNameAnAcquiredTarget)
{
	Harness harness{ Panel() };

	const PresentLayer layer = Composite(0, { 800, 600 });
	const Result<void> refused = harness.Panel->Present({ &layer, 1 });

	GYRO_REQUIRE(!refused.has_value());
	GYRO_CHECK_EQ(refused.error().Code(), EINVAL);
}
