#include "Seam/Presenter.h"

#include <cerrno>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Core/Result.h"
#include "Core/Signal.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/PresentationInfo.h"
#include "Seam/RenderTarget.h"
#include "Testing/Test.h"

// An interface is worth testing where its *contract* is more than its signature, and decision 73 put
// most of this interface's content there: Present may never block and reports at the call site,
// Reconfigure returns before anything is programmed and completes by event, and an output between the
// two is not presenting. None of that is expressible in a declaration, so what is here is a backend
// that keeps the contract and the assertions that would catch one that does not.
//
// The fake is deliberately not a headless backend. It has no clock, no pixels, and no timing — it is
// the smallest thing that can be mid-reconfiguration, which is the state everything interesting here
// is about.

namespace
{
class FakePresenter : public IPresenter
{
public:
	FakePresenter()
	{
		m_Configuration.Resolution = { 1920, 1080 };
		m_Configuration.Period = PeriodFromHertz(60.0);
		m_Configuration.Format = { FormatXrgb8888, 0, ModifierLinear };
		BuildTargets();
	}

	[[nodiscard]] std::span<const RenderTarget> Targets() const override { return m_Targets; }

	[[nodiscard]] std::optional<std::uint32_t> AcquireTarget() override
	{
		for (std::uint32_t index = 0; index < m_Targets.size(); ++index)
		{
			if (!m_Held[index])
			{
				m_Held[index] = true;
				return index;
			}
		}

		return std::nullopt;
	}

	using IPresenter::Present;

	Result<void> Present(std::span<const PresentLayer> layers, PresentTrace) override
	{
		// The one refusal this fake models, and the one decision 73 names: an output between the two
		// verbs holds its last frame and does not present.
		if (m_Reconfiguring)
		{
			return Failure(EBUSY, "presenting an output mid-reconfiguration");
		}

		m_Presented.assign(layers.begin(), layers.end());
		++m_PresentCount;

		return {};
	}

	void Reconfigure(const OutputConfiguration& wanted) override
	{
		// Returns before the hardware is programmed. Everything the caller can observe about the new
		// configuration happens in Complete() below, which stands for the event the frame loop polls
		// for.
		m_Requested = wanted;
		m_Reconfiguring = true;
		m_Targets.clear();
		m_Held = {};
		TargetsInvalidated.Emit();
	}

	// The event. Achieves what was asked for, plus the variable-refresh range that could not have been
	// asked for — which is the whole reason the completion carries a configuration rather than nothing.
	void Complete()
	{
		m_Configuration = m_Requested;

		if (m_Configuration.Refresh.Enabled)
		{
			m_Configuration.Refresh.Shortest = m_Configuration.Period;
			m_Configuration.Refresh.Longest = PeriodFromHertz(48.0);
		}

		m_Reconfiguring = false;
		BuildTargets();
		Reconfigured.Emit(m_Configuration);
	}

	// A backend that could not honour the request. It still completes; the disagreement is what the
	// caller reads.
	void CompleteWithFallback(PixelSize<DeviceSpace> resolution)
	{
		m_Requested.Resolution = resolution;
		Complete();
	}

	void Flip(std::uint64_t sequence)
	{
		Presented.Emit(
			{
				.PresentedAt = Monotonic::FromMicroseconds(16'000),
				.Period = m_Configuration.Period,
				.Sequence = sequence,
				.Vsync = true,
				.HardwareClock = true,
			}
		);
	}

	[[nodiscard]] const std::vector<PresentLayer>& LastPresented() const { return m_Presented; }

	[[nodiscard]] int PresentCount() const { return m_PresentCount; }

private:
	void BuildTargets()
	{
		DmabufImage image;
		image.Planes[0] = { RawFd{ 11 }, 0, static_cast<std::uint32_t>(m_Configuration.Resolution.Width) * 4 };
		image.PlaneCount = 1;

		m_Targets.assign(2, RenderTarget{ m_Configuration.Resolution, m_Configuration.Format, image });
		m_Held = {};
	}

	std::vector<RenderTarget> m_Targets;
	std::array<bool, 2> m_Held{};
	std::vector<PresentLayer> m_Presented;
	OutputConfiguration m_Configuration;
	OutputConfiguration m_Requested;
	bool m_Reconfiguring = false;
	int m_PresentCount = 0;
};

// The shape every observer of the seam has: a long-lived object with methods and Connection members,
// per Core/Signal.h. Nothing here is a lambda, which is the design's claim about who observes.
class Watcher
{
public:
	void Watch(IPresenter& presenter)
	{
		m_Presented.ConnectTo<&Watcher::OnPresented>(presenter.Presented, *this);
		m_Reconfigured.ConnectTo<&Watcher::OnReconfigured>(presenter.Reconfigured, *this);
		m_Invalidated.ConnectTo<&Watcher::OnInvalidated>(presenter.TargetsInvalidated, *this);
	}

	void OnPresented(const PresentationInfo& info) { Frames.push_back(info); }

	void OnReconfigured(const OutputConfiguration& configuration) { Completions.push_back(configuration); }

	void OnInvalidated() { ++Invalidations; }

	std::vector<PresentationInfo> Frames;
	std::vector<OutputConfiguration> Completions;
	int Invalidations = 0;

private:
	Connection<const PresentationInfo&> m_Presented;
	Connection<const OutputConfiguration&> m_Reconfigured;
	Connection<> m_Invalidated;
};

[[nodiscard]] PresentLayer Layer(std::uint32_t target, PixelRect<DeviceSpace> destination)
{
	PresentLayer layer;
	layer.Target = target;
	layer.Destination = destination;
	layer.Damage.Add(destination);

	return layer;
}
} // namespace

GYRO_TEST(Presenter, PresentTakesALayerListOrderedBottomFirst)
{
	FakePresenter presenter;

	const PresentLayer layers[] = {
		Layer(0, { { 0, 0 }, { 1920, 1080 } }),
		Layer(0, { { 100, 100 }, { 64, 64 } }),
	};

	GYRO_REQUIRE(presenter.Present(layers).has_value());
	GYRO_REQUIRE_EQ(presenter.LastPresented().size(), std::size_t{ 2 });

	// Z is the list order and not a field, so the only assertion available is that the order survived
	// — which is the assertion that matters.
	GYRO_CHECK_EQ(presenter.LastPresented()[0].Destination, PixelRect<DeviceSpace>{ { 0, 0 }, { 1920, 1080 } });
	GYRO_CHECK_EQ(presenter.LastPresented()[1].Destination, PixelRect<DeviceSpace>{ { 100, 100 }, { 64, 64 } });
}

GYRO_TEST(Presenter, OneLayerIsTheOrdinaryCase)
{
	FakePresenter presenter;

	const PresentLayer composite[] = { Layer(0, { { 0, 0 }, { 1920, 1080 } }) };

	GYRO_CHECK(presenter.Present(composite).has_value());
	GYRO_CHECK_EQ(presenter.LastPresented().size(), std::size_t{ 1 });
	GYRO_CHECK(presenter.LastPresented().front().Blend == BlendMode::Opaque);
	GYRO_CHECK_EQ(presenter.LastPresented().front().Color, ColorState::Srgb());
}

GYRO_TEST(Presenter, AcquireYieldsNothingUnderBackpressureRatherThanWaiting)
{
	FakePresenter presenter;

	GYRO_CHECK(presenter.AcquireTarget().has_value());
	GYRO_CHECK(presenter.AcquireTarget().has_value());
	GYRO_CHECK(!presenter.AcquireTarget().has_value());
}

// Decision 73's promise, from the caller's side: the request returns, nothing is programmed yet, and
// the only thing that has happened is that the images are gone.
GYRO_TEST(Presenter, ReconfigureReturnsBeforeAnythingIsProgrammed)
{
	FakePresenter presenter;
	Watcher watcher;
	watcher.Watch(presenter);

	OutputConfiguration wanted;
	wanted.Generation = 7;
	wanted.Resolution = { 2560, 1440 };
	wanted.Period = PeriodFromHertz(144.0);
	wanted.Refresh.Enabled = true;
	wanted.Format = { FormatXrgb8888, 0, ModifierLinear };

	presenter.Reconfigure(wanted);

	GYRO_CHECK_EQ(watcher.Invalidations, 1);
	GYRO_CHECK(watcher.Completions.empty());
	GYRO_CHECK(presenter.Targets().empty());

	// And the output is not presenting in the meantime, which the loop is required to tolerate.
	const PresentLayer composite[] = { Layer(0, { { 0, 0 }, { 1920, 1080 } }) };
	const Result<void> refused = presenter.Present(composite);

	GYRO_REQUIRE(!refused.has_value());
	GYRO_CHECK_EQ(refused.error().Code(), EBUSY);
	GYRO_CHECK_EQ(presenter.PresentCount(), 0);

	presenter.Complete();

	GYRO_REQUIRE_EQ(watcher.Completions.size(), std::size_t{ 1 });
	GYRO_CHECK(!presenter.Targets().empty());
	GYRO_CHECK(presenter.Present(composite).has_value());
}

GYRO_TEST(Presenter, TheCompletionCarriesWhatCouldNotBeAskedFor)
{
	FakePresenter presenter;
	Watcher watcher;
	watcher.Watch(presenter);

	OutputConfiguration wanted;
	wanted.Generation = 2;
	wanted.Resolution = { 2560, 1440 };
	wanted.Period = PeriodFromHertz(144.0);
	wanted.Refresh.Enabled = true;
	wanted.Format = { FormatXrgb8888, 0, ModifierLinear };

	presenter.Reconfigure(wanted);
	presenter.Complete();

	GYRO_REQUIRE_EQ(watcher.Completions.size(), std::size_t{ 1 });

	const OutputConfiguration& achieved = watcher.Completions.front();

	GYRO_CHECK(wanted.SatisfiedBy(achieved));
	GYRO_CHECK(!achieved.Refresh.IsDegenerate());
	GYRO_CHECK_EQ(achieved.Refresh.Longest, PeriodFromHertz(48.0));
}

// A refusal reports itself by disagreement rather than by a second signal, which is the whole reason
// the completion carries a configuration.
GYRO_TEST(Presenter, ARequestTheHardwareCouldNotHonourReportsItselfByDisagreeing)
{
	FakePresenter presenter;
	Watcher watcher;
	watcher.Watch(presenter);

	OutputConfiguration wanted;
	wanted.Generation = 3;
	wanted.Resolution = { 3840, 2160 };
	wanted.Period = PeriodFromHertz(60.0);
	wanted.Format = { FormatXrgb8888, 0, ModifierLinear };

	presenter.Reconfigure(wanted);
	presenter.CompleteWithFallback({ 1920, 1080 });

	GYRO_REQUIRE_EQ(watcher.Completions.size(), std::size_t{ 1 });
	GYRO_CHECK(!wanted.SatisfiedBy(watcher.Completions.front()));
	GYRO_CHECK_EQ(watcher.Completions.front().Generation, std::uint64_t{ 3 });
}

// Two requests can be outstanding across a resume or a fast sequence of user changes. Without the
// generation echoed, a completion for the superseded one is indistinguishable from the one being
// waited on — and the frame loop would resume presenting against a configuration that is already gone.
GYRO_TEST(Presenter, ASupersededCompletionIsIdentifiable)
{
	FakePresenter presenter;
	Watcher watcher;
	watcher.Watch(presenter);

	OutputConfiguration first;
	first.Generation = 8;
	first.Resolution = { 2560, 1440 };
	first.Period = PeriodFromHertz(60.0);
	first.Format = { FormatXrgb8888, 0, ModifierLinear };

	OutputConfiguration second = first;
	second.Generation = 9;
	second.Resolution = { 1920, 1080 };

	presenter.Reconfigure(first);
	presenter.Complete();
	presenter.Reconfigure(second);
	presenter.Complete();

	GYRO_REQUIRE_EQ(watcher.Completions.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(watcher.Completions[0].Generation, std::uint64_t{ 8 });
	GYRO_CHECK_EQ(watcher.Completions[1].Generation, std::uint64_t{ 9 });
	GYRO_CHECK_EQ(watcher.Invalidations, 2);
}

// Present succeeding means the commit was accepted, never that anything reached the glass. The two
// are separate events because on every backend they genuinely are.
GYRO_TEST(Presenter, AnAcceptedCommitIsNotAPresentedFrame)
{
	FakePresenter presenter;
	Watcher watcher;
	watcher.Watch(presenter);

	const PresentLayer composite[] = { Layer(0, { { 0, 0 }, { 1920, 1080 } }) };

	GYRO_REQUIRE(presenter.Present(composite).has_value());
	GYRO_CHECK(watcher.Frames.empty());

	presenter.Flip(4210);

	GYRO_REQUIRE_EQ(watcher.Frames.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(watcher.Frames.front().Sequence, std::uint64_t{ 4210 });
	GYRO_CHECK(watcher.Frames.front().HardwareClock);
}

// The lifetime that runs on every boot: decision 41 destroys the presenter while the things observing
// it — a clock, a target cache — keep running. Core/Signal.h owns the mechanism; this is the seam's
// own use of it, and it is here because the observer outliving the presenter is the direction a
// compositor built on raw listeners gets wrong.
GYRO_TEST(Presenter, AnObserverSurvivesThePresenterItWatched)
{
	Watcher watcher;

	{
		FakePresenter presenter;
		watcher.Watch(presenter);
		presenter.Flip(1);
	}

	GYRO_CHECK_EQ(watcher.Frames.size(), std::size_t{ 1 });
}
