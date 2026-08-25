#include "Frame/Timing.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <string>

#include "Core/Time.h"
#include "Core/Wake.h"
#include "Frame/Budget.h"
#include "Frame/FrameClock.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/PresentationInfo.h"
#include "Testing/Test.h"

// The arithmetic is asserted in the header. What is worth a test here is the branch structure — which
// of decision 35's three answers comes back for a given (clock, budget, now, committed), and which
// frame the answer is about — because every one of those is a state the frame loop has to behave correctly in
// and none of them is reachable from a running compositor on purpose.
//
// Every case below runs against the same output: a 100 Hz panel anchored with frame 7 on the glass at
// t = 1000ms, so frame 8 is owed at 1010ms and each frame after it is ten milliseconds later. The
// numbers are chosen so a reader can check a verdict by hand.

namespace
{
using namespace std::chrono_literals;

constexpr Instant At(std::int64_t milliseconds) noexcept
{
	return Monotonic::FromNanoseconds(milliseconds * 1'000'000);
}

FrameClock Anchored()
{
	FrameClock clock;

	OutputConfiguration configuration;
	configuration.Resolution = { 2560, 1440 };
	configuration.Period = 10ms;
	clock.Configure(configuration);

	clock.Observe({ .PresentedAt = At(1000), .Period = 10ms, .Sequence = 7 });

	return clock;
}

Budget Costing(
	Duration plannedCpu,
	Duration plannedGpu,
	Duration floorCpu = {},
	Duration floorGpu = {},
	Duration irreducibleCpu = {}
)
{
	return Budget{ BudgetPolicy{ .FloorCpu = floorCpu,
		                         .FloorGpu = floorGpu,
		                         .InitialCpu = plannedCpu,
		                         .InitialGpu = plannedGpu,
		                         .InitialIrreducibleCpu = irreducibleCpu } };
}
} // namespace

GYRO_TEST(Timing, AdmitsThePlannedTierWithRoomToSpare)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	const FrameDecision decision = timing.Assess(clock, budget, At(1002), FrameClock::NoSequence);

	GYRO_CHECK(decision.Renders());
	GYRO_CHECK(decision.Verdict == Admission::Planned);
	GYRO_CHECK(decision.Mode() == RenderMode::Planned);
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 8 });
	GYRO_CHECK_EQ(decision.Presentation, At(1010));
	GYRO_CHECK_EQ(decision.Deadline, At(1010));
	GYRO_CHECK_EQ(decision.Finish, At(1007));
	GYRO_CHECK_EQ(decision.Slack(), 3ms);
}

GYRO_TEST(Timing, FallsToTheFloorWhenThePlannedTierWillNotFit)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	// The planned tier finishes at 1011ms and the frame is owed at 1010ms; the floor tier finishes at
	// 1008ms. Same frame, cheaper composite, which is decision 35's second promise.
	const FrameDecision decision = timing.Assess(clock, budget, At(1006), FrameClock::NoSequence);

	GYRO_CHECK(decision.Renders());
	GYRO_CHECK(decision.Verdict == Admission::Floor);
	GYRO_CHECK(decision.Mode() == RenderMode::Floor);
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 8 });
	GYRO_CHECK_EQ(decision.Finish, At(1008));
	GYRO_CHECK_EQ(decision.Slack(), 2ms);
}

// Decision 94. The walk that builds the draw list is CPU work neither tier reduces, so it lands in
// both reserves — and the two cases below are the same budget read from either side of that.
GYRO_TEST(Timing, BothTiersReserveTheWalk)
{
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms, 500us);
	const Timing timing;

	GYRO_CHECK_EQ(timing.Reserve(budget, RenderMode::Planned), 5500us);
	GYRO_CHECK_EQ(timing.Reserve(budget, RenderMode::Floor), 2500us);
}

// The failure the term exists to stop, run rather than argued: the composite fits and the frame still
// misses, because the part that overran was the list it was given. On screen that is a workspace with
// enough windows in it that the walk itself no longer fits the gap — and with the walk invisible to
// the check, the loop would admit the floor tier here and hand KMS a frame two milliseconds late.
GYRO_TEST(Timing, AWalkTheDeadlineCannotHoldTakesTheFloorTierWithIt)
{
	const FrameClock clock = Anchored();
	const Timing timing;

	// Without the walk this is exactly `FallsToTheFloorWhenThePlannedTierWillNotFit`: at 1006ms the
	// floor composite finishes at 1008ms against a deadline of 1010ms.
	GYRO_CHECK(timing.Assess(clock, Costing(2ms, 3ms, 1ms, 1ms), At(1006), FrameClock::NoSequence).Sequence == 8);

	// Three milliseconds of walk is more than the two the floor tier had spare, so frame 8 is abandoned
	// and frame 9 is what the work is not late for.
	const FrameDecision decision =
		timing.Assess(clock, Costing(2ms, 3ms, 1ms, 1ms, 3ms), At(1006), FrameClock::NoSequence);

	GYRO_CHECK(decision.Renders());
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 9 });
	GYRO_CHECK_EQ(decision.Presentation, At(1020));
}

GYRO_TEST(Timing, DropsTheFrameOwedAndDrawsTheNextWhenNeitherTierFits)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	// 1009ms leaves one millisecond, which does not cover even the floor composite. Frame 8 is
	// abandoned rather than submitted late — that is the branch that stops the cascade — and what is
	// drawn instead is frame 9, which both tiers reach and which the work is not late for. The tie goes
	// to the planned tier, since the two land in the same frame's window.
	const FrameDecision decision = timing.Assess(clock, budget, At(1009), FrameClock::NoSequence);

	GYRO_CHECK(decision.Renders());
	GYRO_CHECK(decision.Verdict == Admission::Planned);
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 9 });
	GYRO_CHECK_EQ(decision.Presentation, At(1020));
	GYRO_CHECK_EQ(decision.Deadline, At(1020));
	GYRO_CHECK_EQ(decision.Finish, At(1014));
	GYRO_CHECK_EQ(decision.Slack(), 6ms);
}

GYRO_TEST(Timing, TheTierDrawnIsTheOneThatReachesTheEarliestFrame)
{
	const FrameClock clock = Anchored();

	// A planned composite far too expensive for one period, against a floor composite that fits in a
	// fifth of one. The planned tier's next reachable frame is 11; the floor tier's is 9. Taking the
	// planned tier would sleep straight through the frame the floor tier could have made and price one
	// missed frame at three, which is decision 35's second promise the wrong way round.
	const Budget budget = Costing(10ms, 15ms, 1ms, 1ms);
	const Timing timing;

	const FrameDecision decision = timing.Assess(clock, budget, At(1009), FrameClock::NoSequence);

	GYRO_CHECK(decision.Renders());
	GYRO_CHECK(decision.Verdict == Admission::Floor);
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 9 });
	GYRO_CHECK_EQ(decision.Presentation, At(1020));
	GYRO_CHECK_EQ(decision.Finish, At(1011));
}

GYRO_TEST(Timing, ABusyDeviceRefusesAFrameTheRecordAloneWouldMake)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	// Identical to the admitted case above except that the previous frame's work runs until 1008ms.
	// Recording still starts now and finishes at 1004ms; execution cannot start until 1008ms and takes
	// three more, so the planned tier lands a millisecond late on a frame a summed figure would have
	// admitted with three to spare.
	const FrameDecision decision = timing.Assess(clock, budget, At(1002), FrameClock::NoSequence, At(1008));

	GYRO_CHECK(decision.Verdict == Admission::Floor);
	GYRO_CHECK_EQ(decision.Finish, At(1009));

	GYRO_CHECK(timing.Assess(clock, budget, At(1002), FrameClock::NoSequence).Verdict == Admission::Planned);
}

GYRO_TEST(Timing, ACommittedFrameIsNotDrawnASecondTime)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	// Frame 8 has been recorded and handed to the presenter and its flip has not landed, so the clock is
	// still anchored on 7 and says 8 is owed. The loop knows better, and without it being told, this is
	// the admitted case above a second time — the same composite drawn again for a frame already spoken
	// for. Told, the answer is that there is nothing to do until 9's window.
	const FrameDecision decision = timing.Assess(clock, budget, At(1002), 8);

	GYRO_CHECK(!decision.Renders());
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 9 });
	GYRO_CHECK_EQ(decision.Deadline, At(1020));

	GYRO_CHECK(timing.Assess(clock, budget, At(1002), FrameClock::NoSequence).Verdict == Admission::Planned);
}

GYRO_TEST(Timing, ADeeperPipelineTargetsAFrameTheAnchorHasNotReached)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	// Decision 30's early rendering, in the only terms this object has for it: 8 is committed and has not
	// presented, so 9 is being recorded fourteen milliseconds before it reaches the glass rather than the
	// five its record point calls for. Everything downstream follows the target rather than the anchor,
	// which is what makes the animation evaluated for 1020ms exact rather than a frame early.
	const FrameDecision decision = timing.Assess(clock, budget, At(1006), 8);

	GYRO_CHECK(decision.Verdict == Admission::Planned);
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 9 });
	GYRO_CHECK_EQ(decision.Presentation, At(1020));
	GYRO_CHECK_EQ(decision.Deadline, At(1020));
	GYRO_CHECK_EQ(decision.Slack(), 9ms);
}

GYRO_TEST(Timing, AnOutputComingOutOfIdleRendersTheFrameItCanStillMake)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	// Forty-five milliseconds past the anchor, which is four and a half periods, with nothing committed
	// and nothing executing. Counting periods from the last frame would name 8 and be wrong by four;
	// the clock names 12, and 12 is a frame the work makes with nothing to spare and nothing to lose.
	//
	// **Refusing it is the state nothing recovers from.** Only a flip moves the anchor and only a
	// present produces a flip, so an output that declines here declines again a period later against a
	// reach one larger, for ever. That is the whole of the defect, and it is reached with no contention
	// at all by an output that idled — which every output does before the first thing that ever wants a
	// frame arrives.
	const FrameDecision decision = timing.Assess(clock, budget, At(1045), FrameClock::NoSequence);

	GYRO_CHECK(decision.Renders());
	GYRO_CHECK(decision.Verdict == Admission::Planned);
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 12 });
	GYRO_CHECK_EQ(decision.Presentation, At(1050));
	GYRO_CHECK_EQ(decision.Deadline, At(1050));

	// The instant animations are evaluated at is in the future, which is the reason the target moves
	// with the verdict rather than staying at the frame owed. Decision 36 makes this the only time an
	// animation ever sees, and 8's presentation is thirty-five milliseconds in the past.
	GYRO_CHECK(decision.Presentation > At(1045));
}

GYRO_TEST(Timing, AnUnsetFloorTargetAdmitsRatherThanSkips)
{
	const FrameClock clock = Anchored();

	// A budget whose floor target nobody has chosen. Budget documents zero as maximally permissive, and
	// this is the consequence downstream: a frame that may miss rather than one that certainly does not
	// happen, which decision 35 prices at exactly one frame.
	const Budget budget = Costing(2ms, 3ms);
	const Timing timing;

	const FrameDecision decision = timing.Assess(clock, budget, At(1009), FrameClock::NoSequence);

	GYRO_CHECK(decision.Verdict == Admission::Floor);
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 8 });
}

GYRO_TEST(Timing, TheCompletionMarginIsHeldOnceOverTheWholeFrame)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing{ TimingPolicy{ .Margin = 1ms } };

	GYRO_CHECK_EQ(timing.Reserve(budget, RenderMode::Planned), 6ms);
	GYRO_CHECK_EQ(timing.Reserve(budget, RenderMode::Floor), 3ms);

	// The margin is what refuses this one: without it the planned tier finishes exactly on the deadline.
	const FrameDecision decision = timing.Assess(clock, budget, At(1005), FrameClock::NoSequence);

	GYRO_CHECK(decision.Verdict == Admission::Floor);
	GYRO_CHECK(timing.Assess(clock, budget, At(1004), FrameClock::NoSequence).Verdict == Admission::Planned);
}

GYRO_TEST(Timing, TheWakeIsTheDeadlineLessTheReserve)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	GYRO_CHECK(timing.WakeFor(clock, budget, At(1000), FrameClock::NoSequence) == Wake::At(At(1005)));

	// Woken late, the same call names the later frame it can still make rather than one already gone.
	GYRO_CHECK(timing.WakeFor(clock, budget, At(1009), FrameClock::NoSequence) == Wake::At(At(1015)));
}

// The failure that split `TimingPolicy` in two, written as the arithmetic that produced it. One figure
// served both ends, so the alarm was set for `deadline - reserve` and the verdict then asked whether
// `now + reserve` cleared that same deadline — which is `now <= armedAt`, an inequality a wake cannot
// satisfy once it has travelled through the kernel and drained a socket. Every frame the loop woke
// itself for took the floor composite, and what a person saw was the blur behind a window switching
// off for single frames on a machine doing nothing.
GYRO_TEST(Timing, TheWakeLeadsTheInstantTheVerdictMeasuresAgainst)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing{ TimingPolicy{ .Margin = 1ms, .Lead = 2ms } };

	// Frame 8's deadline is 1010ms and the reserve is 6ms, so the work has to start by 1004ms and the
	// loop has to be *running* by 1002ms.
	GYRO_CHECK_EQ(timing.Reserve(budget, RenderMode::Planned), 6ms);
	GYRO_CHECK_EQ(timing.Arming(budget), 8ms);
	GYRO_CHECK(timing.WakeFor(clock, budget, At(1000), FrameClock::NoSequence) == Wake::At(At(1002)));

	// And the point of the lead: woken at the instant it armed for, the loop still clears the check with
	// the whole lead to spend on the kernel and the drain. Under one figure this was exactly zero.
	const FrameDecision decision = timing.Assess(clock, budget, At(1002), FrameClock::NoSequence);

	GYRO_CHECK(decision.Verdict == Admission::Planned);
	GYRO_CHECK_EQ(decision.Slack(), 2ms);
}

// The lead is spent before the check rather than by it: a loop that used its whole lead getting here
// is on time, and one that overran it by a microsecond takes the floor composite. That boundary is the
// sample `Frame/Loop.h` records as `lead`.
GYRO_TEST(Timing, ALoopThatOverrunsItsLeadFallsToTheFloor)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing{ TimingPolicy{ .Margin = 1ms, .Lead = 2ms } };

	GYRO_CHECK(timing.Assess(clock, budget, At(1004), FrameClock::NoSequence).Verdict == Admission::Planned);
	GYRO_CHECK(timing.Assess(clock, budget, At(1004) + 1ns, FrameClock::NoSequence).Verdict == Admission::Floor);
}

GYRO_TEST(Timing, ACommittedFrameArmsTheNextRecordPointRatherThanItsOwn)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	// The clock has not seen 8 present, so the frame the work could reach is 8, and 8's record point is
	// 1005ms — an instant the loop has already served. Armed, it would be handed that instant again on
	// every iteration until the flip lands. The floor is what makes the answer 9's record point instead.
	GYRO_CHECK(timing.WakeFor(clock, budget, At(1002), 8) == Wake::At(At(1015)));
	GYRO_CHECK(timing.WakeFor(clock, budget, At(1002), FrameClock::NoSequence) == Wake::At(At(1005)));
}

GYRO_TEST(Timing, AnUnanchoredOutputArmsNothingAndRendersOnDemand)
{
	const FrameClock clock;
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	GYRO_CHECK(timing.WakeFor(clock, budget, At(1000), FrameClock::NoSequence) == Wake::Never());

	// Nothing is due, so nothing is armed — but a frame the scene asks for is admitted at once and
	// evaluated for the instant it will be ready.
	const FrameDecision decision = timing.Assess(clock, budget, At(1000), FrameClock::NoSequence);

	GYRO_CHECK(decision.Verdict == Admission::Planned);
	GYRO_CHECK_EQ(decision.Sequence, FrameClock::NoSequence);
	GYRO_CHECK_EQ(decision.Presentation, At(1005));
	GYRO_CHECK_EQ(decision.Deadline, FrameClock::Unscheduled);
}

GYRO_TEST(Timing, ADecisionPrintsItsVerdictAndItsFrame)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	GYRO_CHECK_EQ(
		std::format("{}", timing.Assess(clock, budget, At(1002), FrameClock::NoSequence)),
		std::string{ "planned frame 8 slack 3000000ns" }
	);
	GYRO_CHECK_EQ(
		std::format("{}", timing.Assess(FrameClock{}, budget, At(1002), FrameClock::NoSequence)),
		std::string{ "planned unanchored" }
	);
}
