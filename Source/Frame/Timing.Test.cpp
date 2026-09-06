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

// Decision 191. The frame owed cannot be made at full quality, so it is given up and the work is aimed
// at the frame that can be — one refresh of latency and no change to the picture. The floor composite
// would have reached 1010ms and used to be taken here; what that cost is
// `TheFrameOwedIsGivenUpRatherThanDrawnWithoutItsMaterials` below.
GYRO_TEST(Timing, ADeadlineThePlannedTierCannotMakeSelectsTheNextFrame)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	// The planned tier finishes at 1011ms and frame 8 is owed at 1010ms, so 8 is gone and 9 is what the
	// work is not late for.
	const FrameDecision decision = timing.Assess(clock, budget, At(1006), FrameClock::NoSequence);

	GYRO_CHECK(decision.Renders());
	GYRO_CHECK(decision.Verdict == Admission::Planned);
	GYRO_CHECK(decision.Mode() == RenderMode::Planned);
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 9 });
	GYRO_CHECK_EQ(decision.Presentation, At(1020));
	GYRO_CHECK_EQ(decision.Finish, At(1011));
	GYRO_CHECK_EQ(decision.Slack(), 9ms);
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
// the check, the loop would hand KMS a frame three milliseconds late.
GYRO_TEST(Timing, AWalkTheDeadlineCannotHoldCostsTheFrameOwed)
{
	const FrameClock clock = Anchored();
	const Timing timing;

	// Without the walk, 1005ms leaves exactly the five the planned composite wants against a deadline
	// of 1010ms, so frame 8 is made.
	GYRO_CHECK(timing.Assess(clock, Costing(2ms, 3ms, 1ms, 1ms), At(1005), FrameClock::NoSequence).Sequence == 8);

	// Three milliseconds of walk is more than the nothing that was spare, so frame 8 is abandoned and
	// frame 9 is what the work is not late for.
	const FrameDecision decision =
		timing.Assess(clock, Costing(2ms, 3ms, 1ms, 1ms, 3ms), At(1005), FrameClock::NoSequence);

	GYRO_CHECK(decision.Renders());
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 9 });
	GYRO_CHECK_EQ(decision.Presentation, At(1020));
}

GYRO_TEST(Timing, DropsTheFrameOwedAndDrawsTheNextWhenItWillNotFit)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	// 1009ms leaves one millisecond against a composite wanting five. Frame 8 is abandoned rather than
	// submitted late — that is what stops the cascade — and what is drawn instead is frame 9, which the
	// work is not late for.
	const FrameDecision decision = timing.Assess(clock, budget, At(1009), FrameClock::NoSequence);

	GYRO_CHECK(decision.Renders());
	GYRO_CHECK(decision.Verdict == Admission::Planned);
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 9 });
	GYRO_CHECK_EQ(decision.Presentation, At(1020));
	GYRO_CHECK_EQ(decision.Deadline, At(1020));
	GYRO_CHECK_EQ(decision.Finish, At(1014));
	GYRO_CHECK_EQ(decision.Slack(), 6ms);
}

// **What decision 191 gives up, priced.** A planned composite far too expensive for one period reaches
// frame 11; the floor composite would have reached 9, and used to be taken here — two frames of
// cadence bought with the flat tint, which is the one case decision 35's second promise was strongest
// about. That case is real and it is now the quality ladder's to answer by stepping the tier down and
// leaving it down, which is the mechanism that does not flicker. Until that ladder is built this is a
// machine that judders, and this test is where that shows.
GYRO_TEST(Timing, ACompositeTooExpensiveForAPeriodReachesTheFrameItReaches)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(10ms, 15ms, 1ms, 1ms);
	const Timing timing;

	const FrameDecision decision = timing.Assess(clock, budget, At(1009), FrameClock::NoSequence);

	GYRO_CHECK(decision.Renders());
	GYRO_CHECK(decision.Verdict == Admission::Planned);
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 11 });
	GYRO_CHECK_EQ(decision.Presentation, At(1040));
	GYRO_CHECK_EQ(decision.Finish, At(1034));
}

// The same clause one frame in, which is where it stops being worth what it costs.
//
// **What the floor tier spends here is a picture and what it buys is one refresh.** Both tiers have
// missed the frame owed, so nothing is being held to a cadence — the loop has been idle and is picking
// which future frame to draw, not saving one in a moving picture. Taking the cheap composite to be a
// single refresh earlier drops every glass surface on the screen to the flat tint Seam/Dressing.h's
// third rung draws and brings it back on the next frame, which a person sees as the panel flashing.
//
// **A capture of the run bar is where this came from.** The loop had been idle three refreshes and woke
// 1.9 ms before a vblank because a hand moved the pointer; the floor tier reached that vblank and the
// planned tier the one after it. gyro gave up the glass to be one refresh earlier — and the cheap
// composite still took longer than the time that was left, so the frame landed a refresh late anyway.
// The numbers below are that frame's, scaled onto this file's 100 Hz panel.
GYRO_TEST(Timing, OneFrameIsNotWorthAMaterialWhereNeitherTierMakesTheFrameOwed)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	// Frame 8 is owed and 1027ms is seventeen milliseconds past its deadline, so it is gone at either
	// tier. The floor composite finishes at 1029ms and reaches frame 10; the planned one finishes at
	// 1032ms and reaches 11. One frame between them.
	const FrameDecision decision = timing.Assess(clock, budget, At(1027), FrameClock::NoSequence);

	GYRO_CHECK(decision.Renders());
	GYRO_CHECK(decision.Verdict == Admission::Planned);
	GYRO_CHECK(decision.Mode() == RenderMode::Planned);
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 11 });
	GYRO_CHECK_EQ(decision.Presentation, At(1040));
	GYRO_CHECK_EQ(decision.Finish, At(1032));
}

// **Decision 191's boundary, and the case it was written from.** The frame owed is reachable by the
// cheap composite and not by the full one, which is where the floor tier used to fire — and on a run
// bar that is a blurred backdrop going flat for one refresh and back, to buy a refresh of latency on
// content nobody has seen yet. A tier is a judgement about what a machine can sustain; this is a
// judgement about one deadline, and it may not move the picture. So the frame owed is given up.
//
// This is the boundary rather than the interior, so a change that reintroduces a second tier on this
// branch fails here rather than in a schedulability sweep.
GYRO_TEST(Timing, TheFrameOwedIsGivenUpRatherThanDrawnWithoutItsMaterials)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	// The floor composite reaches frame 8, which is owed; the planned one reaches 9, one frame later.
	const FrameDecision decision = timing.Assess(clock, budget, At(1006), FrameClock::NoSequence);

	GYRO_CHECK(decision.Verdict == Admission::Planned);
	GYRO_CHECK(decision.Mode() == RenderMode::Planned);
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 9 });
}

GYRO_TEST(Timing, ABusyDeviceRefusesAFrameTheRecordAloneWouldMake)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing;

	// Identical to the admitted case above except that the previous frame's work runs until 1008ms.
	// Recording still starts now and finishes at 1004ms; execution cannot start until 1008ms and takes
	// three more, so the composite lands a millisecond late on a frame a summed figure would have
	// admitted with three to spare — and frame 9 is what it is aimed at instead.
	const FrameDecision decision = timing.Assess(clock, budget, At(1002), FrameClock::NoSequence, At(1008));

	GYRO_CHECK(decision.Verdict == Admission::Planned);
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 9 });
	GYRO_CHECK_EQ(decision.Finish, At(1011));

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

// The floor target used to decide this branch, and an unset one — which `Budget` documents as
// maximally permissive — admitted a frame on a reserve of nothing. Decision 191 takes the floor off
// this path entirely, so the figure is no longer an input: the two budgets below differ only in their
// floor pair and the verdict may not notice. It still sizes the pinned arming, which is
// `AFloorFrameOverItsTargetMovesThePinnedArming`.
GYRO_TEST(Timing, TheFloorTargetIsNotAnInputToAnUnpinnedVerdict)
{
	const FrameClock clock = Anchored();
	const Timing timing;

	const FrameDecision unset = timing.Assess(clock, Costing(2ms, 3ms), At(1009), FrameClock::NoSequence);
	const FrameDecision set = timing.Assess(clock, Costing(2ms, 3ms, 1ms, 1ms), At(1009), FrameClock::NoSequence);

	GYRO_CHECK(unset.Verdict == Admission::Planned);
	GYRO_CHECK(unset.Verdict == set.Verdict);
	GYRO_CHECK_EQ(unset.Sequence, set.Sequence);
	GYRO_CHECK_EQ(unset.Sequence, std::uint64_t{ 9 });
	GYRO_CHECK_EQ(unset.Finish, set.Finish);
}

GYRO_TEST(Timing, TheCompletionMarginIsHeldOnceOverTheWholeFrame)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing{ TimingPolicy{ .Margin = 1ms } };

	GYRO_CHECK_EQ(timing.Reserve(budget, RenderMode::Planned), 6ms);
	GYRO_CHECK_EQ(timing.Reserve(budget, RenderMode::Floor), 3ms);

	// The margin is what refuses frame 8 here: without it the composite finishes exactly on the
	// deadline, and with it the work is aimed a frame later.
	const FrameDecision decision = timing.Assess(clock, budget, At(1005), FrameClock::NoSequence);

	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 9 });
	GYRO_CHECK_EQ(timing.Assess(clock, budget, At(1004), FrameClock::NoSequence).Sequence, std::uint64_t{ 8 });
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

// **A wake is an instant handed to a timer, so the one thing it may never be is already gone.** The
// `Lead` is what put it there: the alarm is set at `deadline - reserve - lead` while the frame the
// output is owed is chosen without it, so a loop arriving inside that last lead's width computes a
// record point up to a whole lead behind itself. `IORING_TIMEOUT_ABS` fires an expired deadline the
// moment it is submitted, so the iteration woke, found the same conditions, computed the same instant
// and armed it again — a spin at twelve microseconds a turn on the `SCHED_FIFO` frame thread until
// whatever was holding the frame up let go. Measured on a nested `--gym=materials` capture as
// twenty-five of twenty-six iterations inside one six millisecond window, with the loop's own `lead`
// sample walking from -145us down past -519us and the frames beyond that boundary falling to the floor
// composite.
GYRO_TEST(Timing, TheWakeIsNeverAnInstantAlreadyGone)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing{ TimingPolicy{ .Margin = 1ms, .Lead = 3ms } };

	// Frame 8 is spoken for and awaiting its flip, so frame 9 is owed, its deadline is 1020ms, and with
	// a nine millisecond arming the loop should have been running by 1011ms. It is 1012ms.
	GYRO_CHECK_EQ(timing.Arming(budget), 9ms);

	const Wake wake = timing.WakeFor(clock, budget, At(1012), 8);

	GYRO_REQUIRE(wake.Which == Wake::Kind::Timed);
	GYRO_CHECK(wake.When > At(1012));

	// And the instant is the next record point rather than the next microsecond, which is decision 35's
	// *target the next deadline* arriving one rung up: a frame gyro is already too late to start is
	// dropped, and the alarm is set for the one it can still be early for.
	GYRO_CHECK(wake.When == At(1021));

	// One lead earlier and nothing has gone by, so the answer is unchanged — the floor is a bound rather
	// than a rounding, and every wake in steady state is decided by the two frame-shaped floors above it.
	GYRO_CHECK(timing.WakeFor(clock, budget, At(1008), 8) == Wake::At(At(1011)));
}

// The lead is spent before the check rather than by it: a loop that used its whole lead getting here is
// on time, and one that overran it by a microsecond has missed the frame it was woken for and is aimed
// at the next. That boundary is the sample `Frame/Loop.h` records as `lead`.
GYRO_TEST(Timing, ALoopThatOverrunsItsLeadTakesTheNextFrame)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing{ TimingPolicy{ .Margin = 1ms, .Lead = 2ms } };

	GYRO_CHECK_EQ(timing.Assess(clock, budget, At(1004), FrameClock::NoSequence).Sequence, std::uint64_t{ 8 });
	GYRO_CHECK_EQ(timing.Assess(clock, budget, At(1004) + 1ns, FrameClock::NoSequence).Sequence, std::uint64_t{ 9 });
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

// `--composite=planned`, which since decision 191 asks the pinned path for the answer the unpinned one
// already gives: the composite finishes at 1011ms, frame 8 is gone, and what is drawn is frame 9. It
// is kept because `Pinned` is a separate path from `Assess` and the two agreeing is the property worth
// holding — `ADeadlineThePlannedTierCannotMakeSelectsTheNextFrame` is the other half of it.
GYRO_TEST(Timing, PinningThePlannedTierDropsTheFrameInsteadOfSteppingDown)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing{ TimingPolicy{ .Composite = RenderMode::Planned } };

	const FrameDecision decision = timing.Assess(clock, budget, At(1006), FrameClock::NoSequence);

	GYRO_CHECK(decision.Verdict == Admission::Planned);
	GYRO_CHECK(decision.Mode() == RenderMode::Planned);
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 9 });
	GYRO_CHECK_EQ(decision.Deadline, At(1020));
	GYRO_CHECK_EQ(decision.Finish, At(1011));
}

// `--composite=floor`. The planned tier fits with three milliseconds to spare and the cheap composite
// is drawn regardless, which is the steady load the governor is read against.
GYRO_TEST(Timing, PinningTheFloorTierDrawsItWhereThePlannedTierWouldHaveFit)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing{ TimingPolicy{ .Composite = RenderMode::Floor } };

	const FrameDecision decision = timing.Assess(clock, budget, At(1002), FrameClock::NoSequence);

	GYRO_CHECK(decision.Verdict == Admission::Floor);
	GYRO_CHECK(decision.Mode() == RenderMode::Floor);
	GYRO_CHECK_EQ(decision.Sequence, std::uint64_t{ 8 });
	GYRO_CHECK_EQ(decision.Finish, At(1004));
}

// A pin arms for the tier it pinned — revised, decision 168. This asserted the opposite: that the
// arming stayed the planned reserve under either pin, so a sweep measured one schedule whichever tier
// it drew. What that actually produced was a compositor waking for work it was never going to do, and
// on a machine where the pinned floor composite costs more than the planned seed it is the whole
// frame late, every frame. The reachable set under a pin is one mode, and the arming is that mode's.
GYRO_TEST(Timing, APinArmsForTheTierItPinned)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing pinned{ TimingPolicy{ .Composite = RenderMode::Floor } };
	const Timing adaptive;

	GYRO_CHECK_EQ(pinned.Arming(budget), pinned.Reserve(budget, RenderMode::Floor));
	GYRO_CHECK(pinned.Arming(budget) < adaptive.Arming(budget));

	// The half of the old claim that was right, and the reason the pin is still honest about
	// admission: a frame already spoken for is waited for whichever tier will draw it.
	GYRO_CHECK(pinned.Assess(clock, budget, At(1002), 8).Verdict == Admission::Wait);
}

// The other pin, and it is the control: `Planned` is what the unpinned schedule arms for on every
// machine since decision 191, so pinning it moves nothing.
GYRO_TEST(Timing, PinningThePlannedTierArmsWhereTheUnpinnedScheduleDoes)
{
	const FrameClock clock = Anchored();
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing pinned{ TimingPolicy{ .Composite = RenderMode::Planned } };
	const Timing adaptive;

	GYRO_CHECK_EQ(pinned.Arming(budget), adaptive.Arming(budget));
	GYRO_CHECK_EQ(pinned.WakeFor(clock, budget, At(1002), 8), adaptive.WakeFor(clock, budget, At(1002), 8));
}

// Unpinned, the arming is the planned reserve and the floor pair may not move it — decision 191, since
// the record-time check draws the planned composite or waits and there is no floor frame to be armed
// for. The budget below is the one that used to invert it: a floor target measured well above an
// optimistic planned seed, which the old rule read as *the worst of the reachable set* and armed
// against. The reachable set no longer has a second member, so arming against it would be waking early
// for work gyro is never going to do.
GYRO_TEST(Timing, TheUnpinnedArmingIsThePlannedReserveWhateverTheFloorCosts)
{
	const Timing timing;

	GYRO_CHECK_EQ(timing.Arming(Costing(1ms, 1ms, 4ms, 4ms)), timing.Arming(Costing(1ms, 1ms)));
	GYRO_CHECK_EQ(
		timing.Arming(Costing(1ms, 1ms, 4ms, 4ms)), timing.Reserve(Costing(1ms, 1ms, 4ms, 4ms), RenderMode::Planned)
	);
}

// All three halves of decision 168 end to end: a floor frame over its target moves the evidence, the
// evidence sizes the reservation, and the reservation is what a pinned schedule arms for. Without any
// one of them the arming below is the target and the frame starts too late for work already measured.
GYRO_TEST(Timing, AFloorFrameOverItsTargetMovesThePinnedArming)
{
	Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing pinned{ TimingPolicy{ .Composite = RenderMode::Floor } };

	const Duration before = pinned.Arming(budget);

	budget.ObserveGpu(RenderMode::Floor, budget.Generation(), 6ms);

	GYRO_CHECK_EQ(pinned.Arming(budget), before + 5ms);
}

// Decision 31's first frame after idle, under a pin: no anchor, so no deadline to measure against and
// nothing to wait for — the pinned tier renders at once exactly as the planned one does.
GYRO_TEST(Timing, APinnedUnanchoredOutputStillRendersOnDemand)
{
	const Budget budget = Costing(2ms, 3ms, 1ms, 1ms);
	const Timing timing{ TimingPolicy{ .Composite = RenderMode::Floor } };

	const FrameDecision decision = timing.Assess(FrameClock{}, budget, At(1000), FrameClock::NoSequence);

	GYRO_CHECK(decision.Verdict == Admission::Floor);
	GYRO_CHECK_EQ(decision.Sequence, FrameClock::NoSequence);
	GYRO_CHECK_EQ(decision.Presentation, At(1002));
	GYRO_CHECK_EQ(decision.Deadline, FrameClock::Unscheduled);
}
