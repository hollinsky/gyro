#include "Frame/Loop.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "Core/Clock.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Publication/Ring.h"
#include "Publication/Snapshot.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/PresentationInfo.h"
#include "Testing/Test.h"

// What is worth testing here is the *ordering*, which is the whole of what decision 80 moved into the
// portable tier and the reason the step takes no readiness set. The arithmetic it composes is asserted
// where it lives — FrameClock, Budget, and Timing each carry their own — so nothing below re-derives a
// deadline.
//
// Every case runs against the same output: a 100 Hz panel whose fake presenter holds two targets, so
// frame 8 is owed at 1010ms and each frame after it is ten milliseconds later. The renderer draws
// nothing, which is `NullEvaluator`: the loop's questions are about ordering and timing rather than
// about what a walk produces, and an output with nothing to draw is the floor case it has to
// schedule, present, and idle correctly anyway. The walk itself is Frame/Evaluator.Test.cpp's.

namespace
{
using namespace std::chrono_literals;

constexpr Instant At(std::int64_t milliseconds) noexcept
{
	return Monotonic::FromNanoseconds(milliseconds * 1'000'000);
}

OutputConfiguration Panel()
{
	OutputConfiguration configuration;
	configuration.Resolution = { 2560, 1440 };
	configuration.Period = 10ms;

	return configuration;
}

class FakePresenter final : public IPresenter
{
public:
	explicit FakePresenter(std::uint32_t targets = 2) : m_Count{ targets }
	{
		for (std::uint32_t index = 0; index < m_Count; ++index)
		{
			m_Targets[index] = RenderTarget{ .Size = { 2560, 1440 }, .Format = {}, .Memory = MappedImage{} };
		}
	}

	[[nodiscard]] std::span<const RenderTarget> Targets() const override { return { m_Targets.data(), m_Count }; }

	[[nodiscard]] std::optional<std::uint32_t> AcquireTarget() override
	{
		if (m_Held >= m_Count)
		{
			return std::nullopt;
		}

		const std::uint32_t index = m_Next;
		m_Next = (m_Next + 1) % m_Count;
		++m_Held;

		return index;
	}

	[[nodiscard]] std::uint32_t LayerCeiling() const noexcept override { return Planes; }

	[[nodiscard]] Result<void> TestLayers(std::span<const PresentLayer> layers) override
	{
		++Tests;
		TestedLayers = layers.size();

		if (RefuseTest || layers.size() > Planes)
		{
			return Failure(EINVAL, "fake presenter refuses that partition");
		}

		return {};
	}

	Result<void> Present(std::span<const PresentLayer> layers) override
	{
		++Presents;
		PresentedDamage = layers.empty() ? Region<DeviceSpace>{} : layers[0].Damage;
		PresentedLayers.assign(layers.begin(), layers.end());

		if (Refuse)
		{
			return Failure(EBUSY, "fake presenter refuses");
		}

		return {};
	}

	void Reconfigure(const OutputConfiguration& wanted) override { Requested = wanted; }

	// The flip lands. Releases the target the present was holding and re-anchors the clock, which is
	// what makes this the only input to every deadline.
	void Flip(Instant at, std::uint64_t sequence, Duration period = 10ms)
	{
		if (m_Held > 0)
		{
			--m_Held;
		}

		Presented.Emit({ .PresentedAt = at, .Period = period, .Sequence = sequence });
	}

	[[nodiscard]] std::uint32_t Held() const { return m_Held; }

	// The images are gone, so nothing is out on loan any more — a presenter that kept counting the old
	// set as held would be modelling a state that cannot exist. The signal alone leaves the free set
	// wrong, which is why this is a verb rather than an `Emit` at the call site.
	void InvalidateTargets()
	{
		m_Held = 0;
		TargetsInvalidated.Emit();
	}

	[[nodiscard]] std::uint32_t CommitDepth() const noexcept override { return Depth; }

	int Presents = 0;
	bool Refuse = false;

	// One by default, which is every backend that has no planes to offer. A case about promotion asks
	// for more.
	std::uint32_t Planes = 1;
	bool RefuseTest = false;
	int Tests = 0;
	std::size_t TestedLayers = 0;
	std::vector<PresentLayer> PresentedLayers;

	// One by default, which is KMS's rule. A case about pipelining asks for two, which is what a nested
	// output answers because its completion arrives from the host a whole refresh after the frame it is
	// about.
	std::uint32_t Depth = 1;

	Region<DeviceSpace> PresentedDamage{};
	OutputConfiguration Requested{};

private:
	std::array<RenderTarget, 4> m_Targets{};
	std::uint32_t m_Count = 0;
	std::uint32_t m_Next = 0;
	std::uint32_t m_Held = 0;
};

class FakeRenderer final : public IRenderer
{
public:
	Result<void> BindTargets(std::span<const RenderTarget>, ColorState) override { return {}; }

	void ReleaseTargets() noexcept override {}

	[[nodiscard]] Result<Submission> Record(const RecordRequest& request) override
	{
		++Records;
		RecordedItems = request.Items.size();
		RecordedTarget = request.Target;
		RecordedMode = request.Mode;
		RecordedGeneration = request.CostGeneration;
		RecordedDamage = request.Damage;

		if (Refuse)
		{
			return Failure(Code, "fake renderer refuses");
		}

		return Submission{ .Point = SyncPoint::Immediate(), .RecordCost = Cost };
	}

	[[nodiscard]] bool IsComplete(SyncPoint) const override { return true; }

	[[nodiscard]] std::size_t CollectCosts(std::span<GpuCost> into) override
	{
		const std::size_t count = std::min(into.size(), Costs.size() - m_Drained);

		for (std::size_t index = 0; index < count; ++index)
		{
			into[index] = Costs[m_Drained + index];
		}

		m_Drained += count;

		return count;
	}

	int Records = 0;
	bool Refuse = false;
	int Code = ENOMEM;
	std::uint32_t RecordedTarget = 0;
	std::size_t RecordedItems = 0;
	Duration Cost = 2ms;
	RenderMode RecordedMode = RenderMode::Planned;
	std::uint32_t RecordedGeneration = 0;
	Region<DeviceSpace> RecordedDamage{};

	std::array<GpuCost, 4> Costs{};

private:
	std::size_t m_Drained = 0;
};

// An evaluator that draws nothing and charges what it was told to, which is `SimulatedRenderer`'s
// arrangement one seam over and for the same reason: what decision 94 added is a duration, so a walk
// that reports one exercises the whole of it without a scene to walk.
class CostingEvaluator final : public IEvaluator
{
public:
	[[nodiscard]] DrawList Evaluate(const EvaluateRequest&) override
	{
		++Evaluations;

		return DrawList{ .Items = Items, .Damage = {}, .EvaluateCost = Cost };
	}

	int Evaluations = 0;
	Duration Cost{};

	// Bottom-first, the order the partition reads them in. Owned by the caller and outliving the loop,
	// which is what `DrawList::Items` being a span already asks of a real evaluator.
	std::span<const DrawItem> Items{};
};

// A source whose drain delivers a flip, which is how the ordering claim becomes observable: if the
// loop assessed before draining, the decision would be built on the previous anchor.
class FakeSource final : public IEventSource
{
public:
	[[nodiscard]] RawFd Descriptor() const noexcept override { return {}; }

	Result<void> Drain() override
	{
		++Drains;

		if (Deliver != nullptr && Pending)
		{
			Pending = false;
			Deliver->Flip(FlipAt, FlipSequence);
		}

		return {};
	}

	int Drains = 0;
	FakePresenter* Deliver = nullptr;
	bool Pending = false;
	Instant FlipAt{};
	std::uint64_t FlipSequence = 0;
};

// A snapshot with nothing in it but a wake schedule, which is the only run the loop reads today.
class SnapshotBuffer
{
public:
	std::span<const std::byte> Build(std::uint64_t sequence, std::span<const Wake> wakes)
	{
		constexpr std::size_t offset = ((sizeof(SnapshotHeader) + alignof(Wake) - 1) / alignof(Wake)) * alignof(Wake);
		const std::size_t size = offset + wakes.size() * sizeof(Wake);

		SnapshotHeader header;
		header.Sequence = sequence;
		header.ByteSize = static_cast<std::uint32_t>(size);
		header.Wakes = { offset, static_cast<std::uint32_t>(wakes.size()), sizeof(Wake), alignof(Wake) };

		std::memcpy(m_Data.data(), &header, sizeof(header));

		if (!wakes.empty())
		{
			std::memcpy(m_Data.data() + offset, wakes.data(), wakes.size() * sizeof(Wake));
		}

		return { m_Data.data(), size };
	}

private:
	alignas(SnapshotBaseAlignment) std::array<std::byte, 512> m_Data{};
};

// Everything one case needs, wired the way the composition root wires it.
struct Harness
{
	ManualClock Clock{ At(1000) };
	SnapshotRing Ring;
	ReturnChannel Returns;
	CostingEvaluator Evaluator;

	FakePresenter Presenter;
	FakeRenderer Renderer;
	FakeSource Source;
	SnapshotBuffer Buffer;

	std::array<FrameOutput, 1> Outputs;
	std::array<IEventSource*, 1> Sources{ &Source };

	FrameLoop Loop{ Clock, Ring, Returns, Evaluator };

	// Two targets by default, because that is the shallowest ring a nonblocking flip can be built on and
	// most cases here are about one image going round. A case that is about *age* asks for three, since
	// two is the depth at which the image being drawn into is the one presented last and the buffer-age
	// question has no room to be wrong.
	explicit Harness(std::uint32_t targets = 2) : Presenter{ targets }
	{
		Outputs[0].Bind(Presenter, Renderer, 0, Panel());
		Loop.Bind(Outputs);
		Loop.Listen(Sources);
	}

	// Anchor the clock: frame 7 reached the glass at 1000ms, so frame 8 is owed at 1010ms.
	void Anchor() { Presenter.Flip(At(1000), 7); }

	void Publish(std::uint64_t sequence, std::span<const Wake> wakes)
	{
		GYRO_CHECK(Ring.Publish(Buffer.Build(sequence, wakes), 0));
	}

	FrameOutput& Output() { return Outputs[0]; }

	// What the last iteration told the dispatch side, drained the way `SnapshotOutbox::Collect` drains
	// it. The loop posts once per iteration whatever happened, so this always answers.
	FrameReport Reported()
	{
		FrameReport report{};

		while (Returns.Take(report))
		{
		}

		return report;
	}
};
} // namespace

GYRO_TEST(FrameLoop, DrainsEverySourceBeforeItAssessesAnything)
{
	Harness harness;

	// The anchor arrives *during* the drain, so a loop that read the clock first would decide against a
	// clock with no anchor at all and target no frame in particular.
	harness.Source.Deliver = &harness.Presenter;
	harness.Source.Pending = true;
	harness.Source.FlipAt = At(1000);
	harness.Source.FlipSequence = 7;

	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();

	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Source.Drains, 1);
	GYRO_REQUIRE(harness.Output().Clock().IsValid());
	GYRO_CHECK_EQ(harness.Output().Clock().LastSequence(), std::uint64_t{ 7 });
	GYRO_CHECK_EQ(harness.Output().Last().Sequence, std::uint64_t{ 8 });
}

GYRO_TEST(FrameLoop, AcquiresTheNewestSnapshotOncePerIteration)
{
	Harness harness;
	constexpr std::array<Wake, 1> settled{ Wake::Never() };

	harness.Publish(1, settled);
	harness.Publish(2, settled);

	(void)harness.Loop.Step();

	// Newest-wins: the intermediate publication is skipped rather than drained, and the watermark the
	// return channel now carries is the one the dispatch thread reclaims below.
	GYRO_CHECK_EQ(harness.Loop.Held(), std::uint64_t{ 2 });
	GYRO_CHECK_EQ(harness.Loop.Snapshot().Sequence(), std::uint64_t{ 2 });
}

GYRO_TEST(FrameLoop, RendersAndPresentsAnOutputThatOwesAFrame)
{
	Harness harness;

	harness.Anchor();
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();

	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Renderer.Records, 1);
	GYRO_CHECK_EQ(harness.Presenter.Presents, 1);
	GYRO_CHECK_EQ(harness.Output().Committed(), std::uint64_t{ 8 });
	GYRO_CHECK(harness.Output().IsFlipPending());

	// Damage was carried into both the record and the present, and cleared only once the present was
	// accepted.
	GYRO_CHECK(!harness.Renderer.RecordedDamage.IsEmpty());
	GYRO_CHECK(!harness.Presenter.PresentedDamage.IsEmpty());
	GYRO_CHECK(harness.Output().Damage().IsEmpty());
}

GYRO_TEST(FrameLoop, AnOutstandingFlipHoldsTheOutputUntilItLands)
{
	Harness harness;

	harness.Anchor();
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	// A second iteration a frame later. KMS would refuse a commit on a CRTC that has not flipped, so
	// the loop must not offer it one — this is the hardware's condition rather than the schedule's.
	harness.Clock.Set(At(1012));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Presenter.Presents, 1);
	GYRO_CHECK_EQ(harness.Renderer.Records, 1);

	// The flip lands, and the frame after it goes through.
	harness.Presenter.Flip(At(1010), 8);
	GYRO_CHECK(!harness.Output().IsFlipPending());

	harness.Clock.Set(At(1013));
	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Presenter.Presents, 2);
	GYRO_CHECK_EQ(harness.Output().Committed(), std::uint64_t{ 9 });
}

GYRO_TEST(FrameLoop, ARefusedPresentLeavesTheDamageAccumulated)
{
	Harness harness;

	harness.Anchor();
	harness.Presenter.Refuse = true;
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();

	(void)harness.Loop.Step();

	// Decision 35's invariant from the failure side: damage accumulates since the last *successful*
	// present, so a frame that did not reach the glass has lost nothing.
	GYRO_CHECK_EQ(harness.Presenter.Presents, 1);
	GYRO_CHECK(!harness.Output().Damage().IsEmpty());
	GYRO_CHECK(!harness.Output().IsFlipPending());
	GYRO_CHECK_EQ(harness.Output().Committed(), FrameClock::NoSequence);
}

GYRO_TEST(FrameLoop, ARefusedRecordPresentsNothingAndKeepsItsDamage)
{
	Harness harness;

	harness.Anchor();
	harness.Renderer.Refuse = true;
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();

	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Renderer.Records, 1);
	GYRO_CHECK_EQ(harness.Presenter.Presents, 0);
	GYRO_CHECK(!harness.Output().Damage().IsEmpty());
}

// The whole point of the counter: a refusal is a standing condition, so the run that reports it has to
// report the *first* reason rather than the latest, and it has to still be able to say how long it went
// on. A renderer that starts refusing and never stops is what a gym authoring an item no backend can
// draw looks like from here.
GYRO_TEST(FrameLoop, ARefusedRecordIsCountedAndItsFirstReasonKept)
{
	Harness harness;

	harness.Anchor();
	harness.Renderer.Refuse = true;

	GYRO_CHECK_EQ(harness.Output().Refused(), std::uint64_t{ 0 });
	GYRO_CHECK(!harness.Output().FirstRefusal());

	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	GYRO_REQUIRE(harness.Output().FirstRefusal().has_value());
	GYRO_CHECK_EQ(harness.Output().Refused(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(harness.Output().FirstRefusal()->Code(), ENOMEM);
	GYRO_CHECK_EQ(harness.Output().FirstRefusal()->Context(), std::string_view{ "fake renderer refuses" });

	// A second refusal for a different reason leaves the first one standing, because that is the one
	// that says what the run started doing wrong.
	harness.Renderer.Code = EINVAL;
	harness.Clock.Set(At(1012));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Output().Refused(), std::uint64_t{ 2 });
	GYRO_CHECK_EQ(harness.Output().FirstRefusal()->Code(), ENOMEM);
}

// A commit the presenter would not take is the same silence one stage later, and it is counted the same
// way: the frame did not reach the glass and the loop is the only party that saw why.
GYRO_TEST(FrameLoop, ARefusedPresentIsCountedTheSameWay)
{
	Harness harness;

	harness.Anchor();
	harness.Presenter.Refuse = true;
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();

	(void)harness.Loop.Step();

	GYRO_REQUIRE(harness.Output().FirstRefusal().has_value());
	GYRO_CHECK_EQ(harness.Output().Refused(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(harness.Output().FirstRefusal()->Code(), EBUSY);
}

// A frame nobody wanted, a target nobody had, and a flip still outstanding are all returns too, and
// none of them is a refusal. Counting them would make the figure mean *iterations that drew nothing*,
// which is the ordinary state of an idle compositor and would bury the one line worth reading.
// The one that was silently killing --gym=materials. A target comes out of the presenter's free set on
// acquire and only goes back by being presented, so a loop that acquired and then refused used to burn
// one per iteration: three targets, three attempts, and then an output that never tried again for the
// rest of the run. Holding the acquired index makes a standing refusal cost one target rather than all
// of them, which is what lets the count above mean *frames* rather than *buffers*.
GYRO_TEST(FrameLoop, ARefusedRecordKeepsItsTargetRatherThanBurningOne)
{
	Harness harness;

	harness.Anchor();
	harness.Renderer.Refuse = true;

	// One more iteration than the presenter has targets, which is exactly where it used to stop.
	constexpr int attempts = 6;

	for (int attempt = 0; attempt < attempts; ++attempt)
	{
		harness.Clock.Set(At(1002 + 10 * attempt));
		harness.Output().DamageWholeOutput();
		(void)harness.Loop.Step();
	}

	GYRO_CHECK_EQ(harness.Renderer.Records, attempts);
	GYRO_CHECK_EQ(harness.Output().Refused(), static_cast<std::uint64_t>(attempts));

	// The same image every time, and only ever one of them out of the presenter's hands.
	GYRO_CHECK_EQ(harness.Renderer.RecordedTarget, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(harness.Presenter.Held(), std::uint32_t{ 1 });

	// And the output recovers the moment the renderer does, which is the half a latch would have got
	// wrong: nothing here is remembered about the refusal except the count.
	harness.Renderer.Refuse = false;
	harness.Clock.Set(At(1002 + 10 * attempts));
	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Presenter.Presents, 1);
	GYRO_CHECK(harness.Output().Damage().IsEmpty());
	GYRO_CHECK_EQ(harness.Output().Refused(), static_cast<std::uint64_t>(attempts));
}

// The buffer-age case, and it needs three targets to exist at all: the image acquired for the third
// frame here has never been drawn into, and the one acquired for the fourth was last on the glass three
// frames ago. Scissoring to what changed since the last *present* would leave the first window position
// standing in it — a crisp copy of the window, appearing on every third frame.
//
// What the renderer is scissored to is therefore the join of the pending region with that target's own
// backlog, while what the *presenter* is told is the pending region alone: the glass holds the last
// frame that reached it, and repair to an image nobody has seen is not a change to the picture.
GYRO_TEST(FrameLoop, ATargetIsRedrawnForEveryDamageSinceItWasLastDrawn)
{
	Harness harness{ 3 };

	constexpr PixelRect<DeviceSpace> first{ { 0, 0 }, { 100, 100 } };
	constexpr PixelRect<DeviceSpace> second{ { 200, 0 }, { 100, 100 } };
	constexpr PixelRect<DeviceSpace> third{ { 400, 0 }, { 100, 100 } };

	harness.Anchor();

	harness.Clock.Set(At(1002));
	harness.Output().AddDamage(Region<DeviceSpace>{ first });
	(void)harness.Loop.Step();

	GYRO_REQUIRE_EQ(harness.Presenter.Presents, 1);
	GYRO_REQUIRE_EQ(harness.Renderer.RecordedTarget, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(harness.Renderer.RecordedDamage.Bounds(), first);

	harness.Presenter.Flip(At(1010), 8);
	harness.Clock.Set(At(1012));
	harness.Output().AddDamage(Region<DeviceSpace>{ second });
	(void)harness.Loop.Step();

	// Target one last held a frame from before any of this, so it owes the first window position as well
	// as the second — and the presenter is told about the second only.
	GYRO_REQUIRE_EQ(harness.Presenter.Presents, 2);
	GYRO_REQUIRE_EQ(harness.Renderer.RecordedTarget, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(harness.Renderer.RecordedDamage.Bounds(), (PixelRect<DeviceSpace>{ { 0, 0 }, { 300, 100 } }));
	GYRO_CHECK_EQ(harness.Presenter.PresentedDamage.Bounds(), second);

	harness.Presenter.Flip(At(1020), 9);
	harness.Clock.Set(At(1022));
	harness.Output().AddDamage(Region<DeviceSpace>{ third });
	(void)harness.Loop.Step();

	// And target two owes all three, because nothing has ever been drawn into it.
	GYRO_REQUIRE_EQ(harness.Presenter.Presents, 3);
	GYRO_REQUIRE_EQ(harness.Renderer.RecordedTarget, std::uint32_t{ 2 });
	GYRO_CHECK_EQ(harness.Renderer.RecordedDamage.Bounds(), (PixelRect<DeviceSpace>{ { 0, 0 }, { 500, 100 } }));
	GYRO_CHECK_EQ(harness.Presenter.PresentedDamage.Bounds(), third);

	// Round the ring: target zero was complete as of the first frame, so it owes the second and the third
	// and *not* the first, which is the half that says a backlog is cleared and not merely added to.
	harness.Presenter.Flip(At(1030), 10);
	harness.Clock.Set(At(1032));
	harness.Output().AddDamage(Region<DeviceSpace>{ first });
	(void)harness.Loop.Step();

	GYRO_REQUIRE_EQ(harness.Renderer.RecordedTarget, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(harness.Renderer.RecordedDamage.Bounds(), (PixelRect<DeviceSpace>{ { 0, 0 }, { 500, 100 } }));
	GYRO_CHECK_EQ(harness.Renderer.RecordedDamage.Rects().size(), std::size_t{ 3 });
}

// The failure mode the two regions exist to keep apart. A backlog is repair owed to an image, not pixels
// owed to the glass, so an output whose scene has settled must idle with two of its three targets still
// dirty with each other's history. An implementation that folded the two together would find something
// to redraw on every vblank for as long as the machine was on, on a screen where nothing is moving.
GYRO_TEST(FrameLoop, ASettledOutputIdlesWithItsOtherTargetsStillOwedRepair)
{
	Harness harness{ 3 };

	constexpr std::array<Wake, 1> settled{ Wake::Never() };
	harness.Publish(1, settled);

	harness.Anchor();
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	GYRO_REQUIRE_EQ(harness.Renderer.Records, 1);
	GYRO_REQUIRE(harness.Output().Damage().IsEmpty());

	harness.Presenter.Flip(At(1010), 8);

	for (int frame = 0; frame < 8; ++frame)
	{
		harness.Clock.Set(At(1012 + 10 * frame));
		(void)harness.Loop.Step();
	}

	GYRO_CHECK_EQ(harness.Renderer.Records, 1);
	GYRO_CHECK_EQ(harness.Presenter.Presents, 1);
}

// Damaging the whole output subsumes every backlog, so they are dropped there — which is also what keeps
// a rectangle in the *old* extent from outliving a mode set. Asserted through the join, because a
// backlog is not otherwise observable and should not be: the region handed to the renderer is the whole
// of what it is for.
GYRO_TEST(FrameLoop, WholeOutputDamageRetiresEveryBacklog)
{
	Harness harness{ 3 };

	harness.Anchor();
	harness.Clock.Set(At(1002));
	harness.Output().AddDamage(Region<DeviceSpace>{ PixelRect<DeviceSpace>{ { 0, 0 }, { 100, 100 } } });
	(void)harness.Loop.Step();

	harness.Presenter.Flip(At(1010), 8);
	harness.Clock.Set(At(1012));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	// One rectangle rather than two: the whole output already covers what target one was owed, so the
	// join has nothing to add and the renderer scissors once.
	GYRO_REQUIRE_EQ(harness.Renderer.RecordedTarget, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(harness.Renderer.RecordedDamage.Rects().size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(harness.Renderer.RecordedDamage.Bounds(), (PixelRect<DeviceSpace>{ {}, { 2560, 1440 } }));
}

// A refused present has not been accepted, so the presenter did not take the image either — the loop is
// still holding it and the next attempt draws into the same one.
GYRO_TEST(FrameLoop, ARefusedPresentKeepsItsTargetToo)
{
	Harness harness;

	harness.Anchor();
	harness.Presenter.Refuse = true;

	for (int attempt = 0; attempt < 6; ++attempt)
	{
		harness.Clock.Set(At(1002 + 10 * attempt));
		harness.Output().DamageWholeOutput();
		(void)harness.Loop.Step();
	}

	GYRO_CHECK_EQ(harness.Presenter.Presents, 6);
	GYRO_CHECK_EQ(harness.Presenter.Held(), std::uint32_t{ 1 });
	GYRO_CHECK_EQ(harness.Output().Refused(), std::uint64_t{ 6 });
}

// A target set that went away takes the held index with it, because the index named an image in the old
// set and the new set is not obliged to have one there at all.
GYRO_TEST(FrameLoop, LosingTheTargetsDropsTheHeldOne)
{
	Harness harness;

	harness.Anchor();
	harness.Renderer.Refuse = true;
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	GYRO_REQUIRE_EQ(harness.Presenter.Held(), std::uint32_t{ 1 });

	harness.Presenter.InvalidateTargets();
	harness.Renderer.Refuse = false;
	harness.Clock.Set(At(1012));
	(void)harness.Loop.Step();

	// Asked for again rather than reused: one out for the frame in flight, and the presenter was the
	// party that got to choose which.
	GYRO_CHECK_EQ(harness.Presenter.Presents, 1);
	GYRO_CHECK_EQ(harness.Presenter.Held(), std::uint32_t{ 1 });
}

GYRO_TEST(FrameLoop, NotDrawingIsNotBeingRefused)
{
	Harness harness;

	harness.Anchor();
	harness.Clock.Set(At(1002));

	// Nothing owes a frame: no damage, and the empty ring's scene is settled.
	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Renderer.Records, 0);
	GYRO_CHECK_EQ(harness.Output().Refused(), std::uint64_t{ 0 });
	GYRO_CHECK(!harness.Output().FirstRefusal());

	// And a frame that is wanted but whose flip has not landed is held rather than refused.
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();
	GYRO_REQUIRE(harness.Output().IsFlipPending());

	harness.Clock.Set(At(1012));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Output().Refused(), std::uint64_t{ 0 });
}

GYRO_TEST(FrameLoop, AStillSceneIsDrawnOnceAndThenTheOutputArmsNothing)
{
	Harness harness;
	constexpr std::array<Wake, 1> settled{ Wake::Never() };

	harness.Anchor();
	harness.Publish(1, settled);
	harness.Clock.Set(At(1002));

	(void)harness.Loop.Step();

	// **A settled scene still has to reach the glass once.** *Nothing falls due* is a statement about
	// the future and says nothing about whether this output has ever drawn what it is holding — and the
	// case that makes the difference visible is a window opening on a desktop that has gone quiet, which
	// publishes exactly this schedule. An output that read only the wake would leave the screen as it
	// was, for good.
	GYRO_CHECK_EQ(harness.Renderer.Records, 1);
	GYRO_CHECK_EQ(harness.Presenter.Presents, 1);

	harness.Presenter.Flip(At(1010), 8);

	harness.Clock.Set(At(1012));

	const Wake wake = harness.Loop.Step();

	// And then Architecture.md#doing-nothing-must-cost-nothing, reached from both sides at once: the
	// scene the output holds is on the screen, nothing wants another frame, and nothing is armed.
	GYRO_CHECK_EQ(harness.Renderer.Records, 1);
	GYRO_CHECK_EQ(harness.Presenter.Presents, 1);
	GYRO_CHECK(wake == Wake::Never());
}

GYRO_TEST(FrameLoop, ASceneThatChangedWhileTheOutputWasIdleIsDrawn)
{
	Harness harness;
	constexpr std::array<Wake, 1> settled{ Wake::Never() };

	harness.Anchor();
	harness.Publish(1, settled);
	harness.Clock.Set(At(1002));
	(void)harness.Loop.Step();

	harness.Presenter.Flip(At(1010), 8);
	harness.Clock.Set(At(1012));

	GYRO_REQUIRE(harness.Loop.Step() == Wake::Never());

	// A new scene, still settled, arriving at an output that has stopped arming anything. Nothing
	// damaged it — there is no per-node damage on the wire yet, and a still scene's schedule says
	// nothing falls due — so the only thing that says a frame is owed is that this snapshot has never
	// been drawn.
	harness.Publish(2, settled);
	harness.Clock.Set(At(1022));

	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Renderer.Records, 2);
	GYRO_CHECK_EQ(harness.Presenter.Presents, 2);
}

GYRO_TEST(FrameLoop, AContinuousContributorArmsAndDraws)
{
	Harness harness;
	constexpr std::array<Wake, 1> everyFrame{ Wake::EveryFrame() };

	harness.Anchor();
	harness.Publish(1, everyFrame);
	harness.Clock.Set(At(1002));

	const Wake wake = harness.Loop.Step();

	// Decision 69's motivating case — a spinner, a marquee, the console's blinking cursor — which a
	// boolean settled predicate could not express at all.
	GYRO_CHECK_EQ(harness.Presenter.Presents, 1);
	GYRO_CHECK(wake != Wake::Never());
}

GYRO_TEST(FrameLoop, AWakeRunThatIsNotThisOutputSetsIsNoInformation)
{
	Harness harness;
	constexpr std::array<Wake, 3> threeOutputs{ Wake::EveryFrame(), Wake::EveryFrame(), Wake::EveryFrame() };

	harness.Anchor();
	harness.Publish(1, threeOutputs);
	harness.Clock.Set(At(1002));

	(void)harness.Loop.Step();

	harness.Presenter.Flip(At(1010), 8);
	harness.Clock.Set(At(1012));

	const Wake wake = harness.Loop.Step();

	// Decision 84. The run is positional, and three wakes are not one output's schedule however
	// enthusiastic they are — indexing it would have this output run on a neighbour's cadence. So the
	// snapshot is drawn once, because it arrived and has not been, and then nothing is armed: an
	// unreadable schedule is no information, and `EveryFrame` is emphatically not what it says.
	GYRO_CHECK_EQ(harness.Presenter.Presents, 1);
	GYRO_CHECK(wake == Wake::Never());
}

GYRO_TEST(FrameLoop, LosingTheTargetsDamagesTheWholeOutputAndDropsTheCommitment)
{
	Harness harness;

	harness.Anchor();
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	GYRO_REQUIRE_EQ(harness.Output().Committed(), std::uint64_t{ 8 });

	harness.Presenter.InvalidateTargets();

	// Nothing recorded against a target that no longer exists survives it, and there is no previous
	// frame left for damage to be relative to.
	GYRO_CHECK(!harness.Output().IsFlipPending());
	GYRO_CHECK_EQ(harness.Output().Committed(), FrameClock::NoSequence);
	GYRO_CHECK(!harness.Output().Damage().IsEmpty());
}

GYRO_TEST(FrameLoop, ADiscardedFrameFreesTheOutputRatherThanFreezingIt)
{
	// Seam/Presenter.h's fourth signal, and the failure it exists to prevent: the commit was accepted,
	// so the loop marked the output flip-pending and will not serve it again until something clears
	// that. A host that drops the frame and says nothing leaves this output dark for good while every
	// other one carries on, with no error anywhere.
	Harness harness;

	harness.Anchor();
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	GYRO_REQUIRE(harness.Output().IsFlipPending());
	GYRO_REQUIRE(harness.Output().Clock().IsValid());
	GYRO_REQUIRE(harness.Output().Damage().IsEmpty());

	harness.Presenter.Missed.Emit();

	GYRO_CHECK(!harness.Output().IsFlipPending());
	GYRO_CHECK_EQ(harness.Output().Committed(), FrameClock::NoSequence);

	// The clock is invalidated rather than left running, because a discarded frame is not a late
	// observation — it is a boundary that produced none, and a prediction carried across it is derived
	// from a cadence with a hole in it.
	GYRO_CHECK(!harness.Output().Clock().IsValid());

	// And the pixels are on no screen, so the output owes them again.
	GYRO_CHECK(!harness.Output().Damage().IsEmpty());
}

GYRO_TEST(FrameLoop, AReconfigurationReanchorsTheClockAndInvalidatesTheRecord)
{
	Harness harness;

	harness.Anchor();
	const std::uint32_t before = harness.Output().Cost().Generation();

	OutputConfiguration achieved = Panel();
	achieved.Generation = 4;
	achieved.Period = 8ms;
	harness.Presenter.Reconfigured.Emit(achieved);

	// Decision 73's completion. A cost measured under the old mode is a cost from another
	// configuration, which is exactly what Budget's generation exists to say.
	GYRO_CHECK(harness.Output().Cost().Generation() != before);
	GYRO_CHECK(!harness.Output().Clock().IsValid());
	GYRO_CHECK_EQ(harness.Output().Configuration().Generation, std::uint64_t{ 4 });
}

// The pair above, run in the order Seam/Presenter.h fixes, against a mode that grew.
//
// Found by building a backend that performs a transition rather than a fake that reports one: the
// damage `TargetsInvalidated` accumulates is in the extent the output had *at the time*, and a mode set
// that raised the resolution left everything past the old extent outside the region the next frame
// scissors to — on a target that had just been reallocated.
GYRO_TEST(FrameLoop, AModeSetThatGrewTheOutputDamagesTheWholeNewMode)
{
	Harness harness;

	harness.Anchor();

	harness.Presenter.InvalidateTargets();
	GYRO_CHECK_EQ(harness.Output().Damage().Bounds(), (PixelRect<DeviceSpace>{ {}, { 2560, 1440 } }));

	OutputConfiguration achieved = Panel();
	achieved.Generation = 4;
	achieved.Resolution = { 3840, 2160 };
	harness.Presenter.Reconfigured.Emit(achieved);

	GYRO_CHECK_EQ(harness.Output().Damage().Bounds(), (PixelRect<DeviceSpace>{ {}, { 3840, 2160 } }));
}

GYRO_TEST(FrameLoop, GpuCostsAreCollectedBeforeAnythingIsAssessed)
{
	Harness harness;

	harness.Anchor();
	harness.Renderer.Costs[0] = { .Cost = 4ms,
		                          .Generation = harness.Output().Cost().Generation(),
		                          .Mode = RenderMode::Planned };
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();

	(void)harness.Loop.Step();

	// The budget this iteration decided against is the one the last frame's measurement moved, which is
	// the only reason to collect before assessing rather than after presenting.
	GYRO_CHECK(harness.Output().Cost().PlannedGpu() >= 4ms);
	GYRO_CHECK_EQ(harness.Renderer.RecordedGeneration, harness.Output().Cost().Generation());
}

GYRO_TEST(FrameLoop, TwoOutputsOnOneDeviceSerialiseOnTheQueue)
{
	ManualClock clock{ At(1000) };
	SnapshotRing ring;
	ReturnChannel returns;
	NullEvaluator evaluator;

	FakePresenter first;
	FakePresenter second;
	FakeRenderer renderer;

	std::array<FrameOutput, 2> outputs;
	outputs[0].Bind(first, renderer, 0, Panel(), {}, BudgetPolicy{ .InitialCpu = 1ms, .InitialGpu = 3ms });
	outputs[1].Bind(second, renderer, 0, Panel(), {}, BudgetPolicy{ .InitialCpu = 1ms, .InitialGpu = 3ms });

	FrameLoop loop{ clock, ring, returns, evaluator };
	loop.Bind(outputs);

	first.Flip(At(1000), 7);
	second.Flip(At(1001), 7);
	clock.Set(At(1002));
	outputs[0].DamageWholeOutput();
	outputs[1].DamageWholeOutput();

	(void)loop.Step();

	// Decision 29 as a loop variable. The first output records at 1002 and its work is done on the
	// device at 1006; the second records at once but cannot begin executing until then, so its
	// prediction is three milliseconds of GPU past the first's rather than past now.
	GYRO_CHECK_EQ(outputs[0].Last().DeviceFreeAt, At(1006));
	GYRO_CHECK_EQ(outputs[1].Last().DeviceFreeAt, At(1009));
}

// Decision 94's term arriving where the schedule can see it. The evaluator measures its own walk for
// `Submission::RecordCost`'s reason — the party that did the work is the one that knows where it
// started — and the loop files it against neither tier.
GYRO_TEST(FrameLoop, TheWalkIsFiledWhereNoTierCanTakeItAway)
{
	Harness harness;
	harness.Anchor();
	harness.Evaluator.Cost = 700us;
	harness.Output().DamageWholeOutput();

	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Evaluator.Evaluations, 1);
	GYRO_CHECK_EQ(harness.Output().Cost().IrreducibleCpu(), 700us);
	GYRO_CHECK_EQ(harness.Output().Cost().PlannedCpu(), harness.Renderer.Cost);
}

// The walk happened whether or not anything came of it, which is where it parts company with the
// record cost beside it: a refused record is not a frame's cost, and a list that was built is built.
GYRO_TEST(FrameLoop, ARefusedRecordStillPaidForItsWalk)
{
	Harness harness;
	harness.Anchor();
	harness.Evaluator.Cost = 700us;
	harness.Renderer.Refuse = true;
	harness.Output().DamageWholeOutput();

	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Output().Cost().IrreducibleCpu(), 700us);
	GYRO_CHECK_EQ(harness.Output().Cost().PlannedCpu(), Duration::zero());
}

GYRO_TEST(FrameLoop, ThePresentedSequenceIsTheSnapshotTheFrameWasDrawnFromAndNotTheWatermark)
{
	Harness harness;

	harness.Anchor();
	harness.Publish(1, {});
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	// The loop has moved on to a newer scene while the first is still in flight, which is the ordinary
	// state of a pipelined output. The watermark says what the frame thread has finished *reading*; the
	// presented pair says what an output has finished *showing*, and conflating them announces a frame
	// nobody has seen.
	harness.Publish(2, {});
	harness.Presenter.Flip(At(1010), 8);
	harness.Clock.Set(At(1012));
	(void)harness.Loop.Step();

	const FrameReport report = harness.Reported();

	GYRO_CHECK_EQ(report.Watermark, std::uint64_t{ 2 });
	GYRO_REQUIRE_EQ(report.Presented().size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(report.Presented()[0].Sequence, std::uint64_t{ 1 });
	GYRO_CHECK(report.Presented()[0].At == At(1010));
}

GYRO_TEST(FrameLoop, AnOutputTwoCommitsDeepAnswersTheOlderSceneFirst)
{
	Harness harness{ 3 };
	harness.Presenter.Depth = 2;

	harness.Anchor();
	harness.Publish(1, {});
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	harness.Publish(2, {});
	harness.Clock.Set(At(1012));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	GYRO_REQUIRE_EQ(harness.Presenter.Presents, 2);
	GYRO_CHECK_EQ(harness.Output().InFlight(), std::uint32_t{ 2 });

	// Two commits out, and the first completion answers the first of them. A loop that remembered only
	// the newest scene would report sequence 5 as having reached a glass that is still showing 4 — which
	// on the far side is a frame callback handed to a client whose pixels are in the queue.
	harness.Presenter.Flip(At(1010), 8);
	harness.Clock.Set(At(1013));
	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Reported().Presented()[0].Sequence, std::uint64_t{ 1 });

	harness.Presenter.Flip(At(1020), 9);
	harness.Clock.Set(At(1023));
	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Reported().Presented()[0].Sequence, std::uint64_t{ 2 });
}

GYRO_TEST(FrameLoop, AnIterationWithNoFlipReportsNoNewsRatherThanRepeatingItself)
{
	Harness harness;

	harness.Anchor();
	harness.Publish(1, {});
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	harness.Presenter.Flip(At(1010), 8);
	harness.Clock.Set(At(1012));
	(void)harness.Loop.Step();

	GYRO_REQUIRE_EQ(harness.Reported().Presented()[0].Sequence, std::uint64_t{ 1 });

	// A settled output flips nothing, and the report still crosses because the watermark has to. Zero is
	// what says so — repeating the pair would be indistinguishable from a second flip of the same scene,
	// and its timestamp would drift a frame further from the truth on every quiet iteration.
	harness.Clock.Set(At(1022));
	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Reported().Presented()[0].Sequence, std::uint64_t{ 0 });
}

GYRO_TEST(FrameLoop, AFrameTheHostAcceptedAndNeverShowedIsNeverReportedPresented)
{
	Harness harness;

	harness.Anchor();
	harness.Publish(1, {});
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	GYRO_REQUIRE(harness.Output().IsFlipPending());

	// Decision 124's fourth signal. The frame was drawn and accepted and is not on a screen, so there is
	// nothing to tell a client about it — the surfaces in that scene are still owed a frame, and a
	// callback here would be gyro reporting a photon that never left.
	harness.Presenter.Missed.Emit();

	harness.Clock.Set(At(1012));
	(void)harness.Loop.Step();

	GYRO_CHECK_EQ(harness.Reported().Presented()[0].Sequence, std::uint64_t{ 0 });
}
// A flip opens a slice on the glass row at the moment the panel scanned out, named for the frame the
// vblank showed and carrying the scene and the refresh as arguments. Stamped where it *happened*
// rather than where it was drained, because a slice placed where the loop learned of it moves with
// every late wake and reads a stutter of the reader as a lateness of the host. And it is a slice rather
// than a mark because what a person wants from this row is how long a picture was on the screen, which
// a mark cannot say.
GYRO_TEST(FrameLoop, TheGlassRowOpensWhereThePanelScannedOutAndNamesTheFrame)
{
	Harness harness;

	std::array<TraceRecord, 64> records{};
	TraceBuffer trace;
	trace.Arm(records, harness.Clock);

	// Armed after the anchor, because anchoring the clock is a flip with nothing in flight behind it —
	// a vblank that showed a frame gyro did not draw. That opens a slice on this row too, correctly and
	// with no name, and counting it here would be counting the harness rather than the loop.
	harness.Anchor();

	EnrollTracing(&trace);

	harness.Publish(1, {});
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	// The vblank happened at 1010 and the loop is not told about it until 1020, which is the
	// arrangement a feedback queue always has: the panel moved on before the reader woke up.
	harness.Presenter.Flip(At(1010), 8);
	harness.Clock.Set(At(1020));
	(void)harness.Loop.Step();

	EnrollTracing(nullptr);

	std::array<TraceEvent, 64> events{};
	const std::size_t count = trace.Copy(events);

	std::size_t opened = 0;
	bool stampedWhereLearned = false;
	std::uint64_t shown = 0;
	std::uint64_t drawn = 0;
	std::vector<std::pair<std::string_view, std::uint64_t>> arguments;

	for (std::size_t index = 0; index < count; ++index)
	{
		const TraceEvent& event = events[index];

		// What the work rows called this frame, which is the number the glass row has to agree with.
		if (event.Scope == TraceOutput(0) && event.Kind == TraceKind::Begin &&
		    std::string_view{ event.Name } == "frame")
		{
			drawn = event.Payload;
		}

		if (event.Scope != TraceGlass(0))
		{
			continue;
		}

		if (event.Kind == TraceKind::Attribute)
		{
			arguments.emplace_back(std::string_view{ event.Name }, event.Payload);

			// The same instant as the slice it hangs on, or the stable sort puts it elsewhere on the row
			// and the writer binds it to a slice it is not about.
			GYRO_CHECK(event.Stamp == At(1010));

			continue;
		}

		if (event.Kind != TraceKind::Begin)
		{
			continue;
		}

		++opened;
		shown = event.Payload;

		GYRO_CHECK_EQ(std::string_view{ event.Name }, std::string_view{ "frame" });
		GYRO_CHECK(event.Stamp == At(1010));

		// A tag rather than a flow: this frame is drawn on five rows and the words are what join them,
		// which is what let three thousand arrows out of a seven-second capture.
		GYRO_CHECK(!event.Arrow);

		stampedWhereLearned = stampedWhereLearned || event.Stamp == At(1020);
	}

	GYRO_CHECK_EQ(opened, std::size_t{ 1 });
	GYRO_CHECK(!stampedWhereLearned);

	// **The row says which frame, not which publication**, which is the whole of the correction: a
	// snapshot is coefficients rather than pixels, so a dozen refreshes from one publication are a dozen
	// different pictures and a row keyed on the scene drew them as one still block.
	GYRO_CHECK(drawn != 0);
	GYRO_CHECK_EQ(shown, drawn);

	// And the three counts that belong to the slice without being its identity: the publication it
	// drew from, the vblank it actually landed on, and the interval the host said its next refresh
	// would be — the number the clock schedules the next deadline from.
	GYRO_REQUIRE_EQ(arguments.size(), std::size_t{ 3 });
	GYRO_CHECK_EQ(arguments[0].first, std::string_view{ "scene" });
	GYRO_CHECK_EQ(arguments[0].second, std::uint64_t{ 1 });
	GYRO_CHECK_EQ(arguments[1].first, std::string_view{ "refresh" });
	GYRO_CHECK_EQ(arguments[1].second, std::uint64_t{ 8 });
	GYRO_CHECK_EQ(arguments[2].first, std::string_view{ "host period" });
	GYRO_CHECK_EQ(arguments[2].second, std::uint64_t{ 10'000'000 });
}

// A second flip from the same publication is a different picture and has to be drawn as one. The row
// used to merge on the scene, so an animation running from one snapshot came out as a single wide
// block claiming the screen had been frozen — which is exactly the shape a stutter has, reported on a
// compositor that was not stuttering.
GYRO_TEST(FrameLoop, TwoFramesFromOnePublicationAreTwoSlicesOnTheGlass)
{
	Harness harness;

	std::array<TraceRecord, 256> records{};
	TraceBuffer trace;
	trace.Arm(records, harness.Clock);

	harness.Anchor();

	EnrollTracing(&trace);

	harness.Publish(1, {});

	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	harness.Presenter.Flip(At(1010), 8);
	harness.Clock.Set(At(1011));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	harness.Presenter.Flip(At(1026), 9);
	harness.Clock.Set(At(1027));
	(void)harness.Loop.Step();

	EnrollTracing(nullptr);

	std::array<TraceEvent, 256> events{};
	const std::size_t count = trace.Copy(events);

	std::vector<std::uint64_t> shown;
	std::vector<std::uint64_t> scenes;

	for (std::size_t index = 0; index < count; ++index)
	{
		const TraceEvent& event = events[index];

		if (event.Scope != TraceGlass(0))
		{
			continue;
		}

		if (event.Kind == TraceKind::Begin)
		{
			shown.push_back(event.Payload);
		}

		if (event.Kind == TraceKind::Attribute && std::string_view{ event.Name } == "scene")
		{
			scenes.push_back(event.Payload);
		}
	}

	// Two frames, two slices, and consecutive: the second flip closed the first slice rather than
	// extending it.
	GYRO_REQUIRE_EQ(shown.size(), std::size_t{ 2 });
	GYRO_CHECK(shown[0] != shown[1]);

	// Both drew the same publication, which is the fact the old keying mistook for the same picture.
	GYRO_REQUIRE_EQ(scenes.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(scenes[0], std::uint64_t{ 1 });
	GYRO_CHECK_EQ(scenes[1], std::uint64_t{ 1 });
}

// The scene the loop is drawing from is a state and not an event, so the arrow that names it is drawn
// only where it changed. Re-marking it every iteration links one publish to itself over and over: a
// ten-second trace held one snapshot for a hundred and eighteen iterations and drew a hundred and
// eighteen arrows, which is a picture a reader has to disbelieve before they can read anything else.
GYRO_TEST(FrameLoop, AcquiredIsMarkedOnlyWhereTheHeldSnapshotChanged)
{
	Harness harness;

	std::array<TraceRecord, 128> records{};
	TraceBuffer trace;
	trace.Arm(records, harness.Clock);
	EnrollTracing(&trace);

	harness.Anchor();
	harness.Publish(1, {});
	harness.Clock.Set(At(1002));
	(void)harness.Loop.Step();

	harness.Clock.Set(At(1004));
	(void)harness.Loop.Step();

	harness.Clock.Set(At(1006));
	(void)harness.Loop.Step();

	harness.Publish(2, {});
	harness.Clock.Set(At(1008));
	(void)harness.Loop.Step();

	harness.Clock.Set(At(1010));
	(void)harness.Loop.Step();

	EnrollTracing(nullptr);

	std::array<TraceEvent, 128> events{};
	const std::size_t count = trace.Copy(events);

	std::size_t marks = 0;
	std::size_t samples = 0;

	for (std::size_t index = 0; index < count; ++index)
	{
		const TraceEvent& event = events[index];

		if (event.Kind == TraceKind::Mark && std::string_view{ event.Name } == "acquired")
		{
			++marks;

			// **The one arrow left in the picture, and it earns its place by joining two counts.** A
			// publication is numbered by the dispatch thread and a frame by the panel, so no name on one
			// side can find the other. Everything else that used to be a flow is a tag now.
			GYRO_CHECK(event.Arrow);
			GYRO_CHECK(event.Payload == TraceFlow(TraceDomain::Scene, marks).Value());
		}

		if (event.Kind == TraceKind::Count && std::string_view{ event.Name } == "held")
		{
			++samples;
		}
	}

	GYRO_CHECK_EQ(marks, std::size_t{ 2 });
	GYRO_CHECK_EQ(samples, std::size_t{ 5 });
}

// The refresh ruler tiles: a frame's span runs from the previous refresh's deadline to its own, so
// consecutive frames abut and the row is the grid every other row is read against. It used to start
// where the loop woke instead, which drew a sliver with a gap after it — an extent that looked like a
// budget, was actually the time left on arrival, and made work that fit inside its refresh read as an
// overrun on every frame.
GYRO_TEST(FrameLoop, TheRefreshRulerTilesSoConsecutiveFramesAbut)
{
	Harness harness;

	std::array<TraceRecord, 256> records{};
	TraceBuffer trace;
	trace.Arm(records, harness.Clock);
	EnrollTracing(&trace);

	harness.Anchor();
	harness.Publish(1, {});
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	harness.Presenter.Flip(At(1010), 8);
	harness.Clock.Set(At(1012));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	EnrollTracing(nullptr);

	std::array<TraceEvent, 256> events{};
	const std::size_t count = trace.Copy(events);

	std::vector<std::pair<Instant, std::uint64_t>> opened;
	std::vector<Instant> closed;

	for (std::size_t index = 0; index < count; ++index)
	{
		const TraceEvent& event = events[index];

		if (event.Scope != TraceGrid(0))
		{
			continue;
		}

		if (event.Kind == TraceKind::Begin)
		{
			opened.emplace_back(event.Stamp, event.Payload);

			// **Named for the refresh rather than for the frame aimed at it.** Every other `frame N` in
			// the picture is an extent that happened and this one is a forecast — it ends where the
			// commit is due rather than where the pixels appear — so sharing the word had a reader join
			// a prediction to four observations and read the ruler as the screen.
			GYRO_CHECK_EQ(std::string_view{ event.Name }, std::string_view{ "refresh" });
			GYRO_CHECK(!event.Arrow);
		}

		if (event.Kind == TraceKind::End)
		{
			closed.push_back(event.Stamp);
		}
	}

	GYRO_REQUIRE_EQ(opened.size(), std::size_t{ 2 });
	GYRO_REQUIRE_EQ(closed.size(), std::size_t{ 2 });

	// Consecutive frames, named for consecutive refreshes.
	GYRO_CHECK_EQ(opened[1].second, opened[0].second + 1);

	// **The second tile begins exactly where the first ended**, which is the whole property this row
	// has to have. Asked of the clock instead, the two edges disagree by the drift a re-anchoring flip
	// put between them — and Perfetto has no *slightly overlapping*, so a tile that starts a microsecond
	// early becomes a child of its neighbour and the ruler nests one level deeper every frame.
	GYRO_CHECK(opened[1].first == closed[0]);

	// The first has no predecessor to abut, so it runs from the wake that made it. Every one after it
	// is a whole refresh.
	GYRO_CHECK_EQ(Elapsed(opened[1].first, closed[1]), harness.Output().Clock().Period());
}

// The wait nothing used to draw: between the present that hands a frame over and the vblank that shows
// it, the frame is in no row at all, and on a sixty hertz panel that is a whole refresh a person is
// trying to account for. The lane is the commit slot, so how many lanes are occupied at an instant is
// the queue depth read without a counter.
GYRO_TEST(FrameLoop, AFrameOccupiesAFlightLaneFromItsCommitToTheVblankThatShowsIt)
{
	Harness harness;

	std::array<TraceRecord, 128> records{};
	TraceBuffer trace;
	trace.Arm(records, harness.Clock);
	EnrollTracing(&trace);

	harness.Anchor();
	harness.Publish(1, {});
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	GYRO_REQUIRE(harness.Output().IsFlipPending());

	// The vblank happens at 1010 and is drained at 1020. The lane closes at the host's instant, for the
	// same reason the glass row opens there: a late wake must not lengthen the wait it was late for.
	harness.Presenter.Flip(At(1010), 8);
	harness.Clock.Set(At(1020));
	(void)harness.Loop.Step();

	EnrollTracing(nullptr);

	std::array<TraceEvent, 128> events{};
	const std::size_t count = trace.Copy(events);

	std::size_t opened = 0;
	std::size_t closed = 0;
	Instant began{};
	Instant ended{};
	std::size_t laneOpened = count;
	std::size_t frameOpened = count;
	std::size_t framePresented = count;

	for (std::size_t index = 0; index < count; ++index)
	{
		const TraceEvent& event = events[index];

		// Where the frame this lane is for started being made, and where it was handed over.
		if (event.Scope == TraceOutput(0) && event.Kind == TraceKind::Begin)
		{
			if (std::string_view{ event.Name } == "frame" && frameOpened == count)
			{
				frameOpened = index;
			}

			if (std::string_view{ event.Name } == "present")
			{
				framePresented = index;
			}
		}

		if (event.Scope != TraceFlight(0, 0))
		{
			continue;
		}

		if (event.Kind == TraceKind::Begin)
		{
			++opened;
			began = event.Stamp;
			laneOpened = index;

			GYRO_CHECK_EQ(std::string_view{ event.Name }, std::string_view{ "frame" });
			GYRO_CHECK(!event.Arrow);
		}

		if (event.Kind == TraceKind::End)
		{
			++closed;
			ended = event.Stamp;
		}
	}

	GYRO_CHECK_EQ(opened, std::size_t{ 1 });
	GYRO_CHECK_EQ(closed, std::size_t{ 1 });
	GYRO_CHECK(began == At(1002));
	GYRO_CHECK(ended == At(1010));

	// **The lane opens after the frame was made and after it was handed over**, which the two stamps
	// cannot show on a clock a test sets by hand and the ring's own order can. Opened at the iteration's
	// clock read, this record came *first* — a frame waiting in a queue before it had been drawn, on
	// every frame of every capture — and it made *how many lanes are occupied is the queue depth* false,
	// since a lane was also occupied while its frame was still being recorded.
	GYRO_REQUIRE(frameOpened != count);
	GYRO_REQUIRE(framePresented != count);
	GYRO_REQUIRE(laneOpened != count);
	GYRO_CHECK(laneOpened > frameOpened);
	GYRO_CHECK(laneOpened > framePresented);
}

// A wake that draws nothing says why it drew nothing. Two thirds of this loop's iterations are the
// panel still holding the frame in front, and they used to leave a `serve` span four microseconds long
// with nothing inside it — three identical blocks per refresh of which one was the frame, which is the
// single thing that made these charts unreadable.
GYRO_TEST(FrameLoop, AWakeThatDeclinesToDrawSaysWhy)
{
	Harness harness;

	std::array<TraceRecord, 128> records{};
	TraceBuffer trace;
	trace.Arm(records, harness.Clock);

	harness.Anchor();
	harness.Publish(1, {});
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();
	(void)harness.Loop.Step();

	GYRO_REQUIRE(harness.Output().IsCommitFull());

	// Armed only now, so that what is counted below is the blocked wake alone.
	EnrollTracing(&trace);

	harness.Publish(2, {});
	harness.Clock.Set(At(1005));
	(void)harness.Loop.Step();

	EnrollTracing(nullptr);

	std::array<TraceEvent, 128> events{};
	const std::size_t count = trace.Copy(events);

	std::size_t declined = 0;
	std::size_t framesOnTheRow = 0;

	for (std::size_t index = 0; index < count; ++index)
	{
		const TraceEvent& event = events[index];

		if (event.Scope != TraceOutput(0))
		{
			continue;
		}

		if (event.Kind == TraceKind::Mark && std::string_view{ event.Name } == "queue full")
		{
			++declined;
		}

		if (event.Kind == TraceKind::Begin)
		{
			++framesOnTheRow;
		}
	}

	// One mark saying why, and nothing opened — a wake that declined is not a slice, because a slice
	// the same shape as the one a frame makes is what a reader has to open to tell them apart.
	GYRO_CHECK_EQ(declined, std::size_t{ 1 });
	GYRO_CHECK_EQ(framesOnTheRow, std::size_t{ 0 });
}

// Decision 152's partition reaching the seam: what the display engine draws, and what is left for the
// GPU.
//
// **The scene these run against is two items, and the interesting one is the top.** Everything below
// is about which of them lands where, so the items are the smallest that satisfy and fail
// `IsPromotable` — a texture that resamples not at all, and a solid, which no plane draws.

namespace
{
[[nodiscard]] DrawItem Promotable(PixelRect<DeviceSpace> where)
{
	DrawItem item{};

	item.Content = DrawTexture{ .Texture = TextureId{ 1, 1 }, .Source = {} };
	item.Shape = Quad::FromRect(
		{ { static_cast<float>(where.Origin.X), static_cast<float>(where.Origin.Y) },
	      { static_cast<float>(where.Extent.Width), static_cast<float>(where.Extent.Height) } }
	);
	item.Extent = { static_cast<float>(where.Extent.Width), static_cast<float>(where.Extent.Height) };
	item.Sampling = TransformClass{ .AxisAligned = true, .Upright = true, .UnitScale = true, .IntegerOffset = true };

	return item;
}

[[nodiscard]] DrawItem Composited()
{
	DrawItem item{};

	item.Content = DrawSolid{ .Red = 1.0F, .Green = 1.0F, .Blue = 1.0F, .Alpha = 1.0F };
	item.Shape = Quad::FromRect({ {}, { 2560.0F, 1440.0F } });
	item.Extent = { 2560.0F, 1440.0F };

	return item;
}
} // namespace

GYRO_TEST(FrameLoop, PromotesTheTopItemOntoAPlaneAndLeavesTheRestToTheGpu)
{
	Harness harness;
	const std::array<DrawItem, 2> items{ Composited(), Promotable({ { 100, 100 }, { 640, 480 } }) };

	harness.Presenter.Planes = 2;
	harness.Evaluator.Items = items;

	harness.Anchor();
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();

	(void)harness.Loop.Step();

	// The hardware was asked before anything was recorded, and it was asked about the partition that
	// was then committed.
	GYRO_CHECK_EQ(harness.Presenter.Tests, 1);
	GYRO_CHECK_EQ(harness.Presenter.TestedLayers, std::size_t{ 2 });

	// The composite drew the prefix and not the promoted item, which would otherwise be on the screen
	// twice — once under an opaque plane and once on it.
	GYRO_CHECK_EQ(harness.Renderer.RecordedItems, std::size_t{ 1 });

	GYRO_REQUIRE(harness.Presenter.PresentedLayers.size() == 2);
	GYRO_CHECK(!harness.Presenter.PresentedLayers[0].Target.IsTexture());
	GYRO_CHECK(harness.Presenter.PresentedLayers[1].Target.IsTexture());

	// The layer names the id the draw item carried, which is decision 153: nothing on the frame thread
	// performed a lookup.
	GYRO_CHECK_EQ(harness.Presenter.PresentedLayers[1].Target.Texture, TextureId{ 1, 1 });

	// Where it lands is the quad's own pixels, exactly, because a promotion resamples not at all.
	GYRO_CHECK_EQ(harness.Presenter.PresentedLayers[1].Destination.Origin.X, 100);
	GYRO_CHECK_EQ(harness.Presenter.PresentedLayers[1].Destination.Extent.Width, 640);
}

GYRO_TEST(FrameLoop, AnOutputWithOnePlaneCompositesEverything)
{
	Harness harness;
	const std::array<DrawItem, 2> items{ Composited(), Promotable({ { 100, 100 }, { 640, 480 } }) };

	harness.Evaluator.Items = items;

	harness.Anchor();
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();

	(void)harness.Loop.Step();

	// One plane is the composite's, so there is nothing to propose and nothing to ask about.
	GYRO_CHECK_EQ(harness.Presenter.Tests, 0);
	GYRO_CHECK_EQ(harness.Renderer.RecordedItems, std::size_t{ 2 });
	GYRO_CHECK_EQ(harness.Presenter.PresentedLayers.size(), std::size_t{ 1 });
}

GYRO_TEST(FrameLoop, ARefusedPartitionCompositesTheWholeFrame)
{
	Harness harness;
	const std::array<DrawItem, 2> items{ Composited(), Promotable({ { 100, 100 }, { 640, 480 } }) };

	harness.Presenter.Planes = 2;
	harness.Presenter.RefuseTest = true;
	harness.Evaluator.Items = items;

	harness.Anchor();
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();

	(void)harness.Loop.Step();

	// The driver said no — a format, a bandwidth limit, a shared scaler — and the frame composites.
	// Decision 35's one frame, and nothing a person sees.
	GYRO_CHECK_EQ(harness.Presenter.Tests, 1);
	GYRO_CHECK_EQ(harness.Renderer.RecordedItems, std::size_t{ 2 });
	GYRO_CHECK_EQ(harness.Presenter.PresentedLayers.size(), std::size_t{ 1 });
}

GYRO_TEST(FrameLoop, AllPromotableStillLeavesTheGpuOneItem)
{
	Harness harness;
	const std::array<DrawItem, 1> items{ Promotable({ {}, { 2560, 1440 } }) };

	harness.Presenter.Planes = 2;
	harness.Evaluator.Items = items;

	harness.Anchor();
	harness.Clock.Set(At(1002));
	harness.Output().DamageWholeOutput();

	(void)harness.Loop.Step();

	// The arrangement the mechanism exists for — every item on a plane and the GPU asleep — is the one
	// this loop cannot take yet, because the target was acquired before there was a list to partition
	// and no presenter can be handed one back. So the item is composited and the promotion is dropped,
	// which is a correct picture at the cost of the render pass. See the comment beside `m_Partition`.
	GYRO_CHECK_EQ(harness.Renderer.RecordedItems, std::size_t{ 1 });
	GYRO_CHECK_EQ(harness.Presenter.PresentedLayers.size(), std::size_t{ 1 });
}
