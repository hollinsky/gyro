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

	Result<void> Present(std::span<const PresentLayer> layers) override
	{
		++Presents;
		PresentedDamage = layers.empty() ? Region<DeviceSpace>{} : layers[0].Damage;

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

	int Presents = 0;
	bool Refuse = false;
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

		return DrawList{ .Items = {}, .Damage = {}, .EvaluateCost = Cost };
	}

	int Evaluations = 0;
	Duration Cost{};
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

	Harness()
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

GYRO_TEST(FrameLoop, AnIdleOutputArmsNothing)
{
	Harness harness;
	constexpr std::array<Wake, 1> settled{ Wake::Never() };

	harness.Anchor();
	harness.Publish(1, settled);
	harness.Clock.Set(At(1002));

	const Wake wake = harness.Loop.Step();

	// Architecture.md#doing-nothing-must-cost-nothing, reached from both sides at once: nothing wants a
	// frame and nothing is outstanding, so nothing is drawn and no timer is armed.
	GYRO_CHECK_EQ(harness.Renderer.Records, 0);
	GYRO_CHECK_EQ(harness.Presenter.Presents, 0);
	GYRO_CHECK(wake == Wake::Never());
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

	const Wake wake = harness.Loop.Step();

	// Decision 84. The run is positional, and three wakes are not one output's schedule however
	// enthusiastic they are — indexing it would have this output run on a neighbour's cadence.
	GYRO_CHECK_EQ(harness.Presenter.Presents, 0);
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
