#include "Frame/FrameClock.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <string>

#include "Core/Time.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/PresentationInfo.h"
#include "Testing/Test.h"

// What is worth testing here is not the arithmetic, which the header already asserts at compile time.
// It is the four states a clock passes through that nothing else in the system can be put into on
// purpose: seeded, stale, recovering, and mid-ramp. Each one is a shape the frame loop has to behave
// correctly in and none of them is reachable from a running compositor on demand.

namespace
{
using namespace std::chrono_literals;

constexpr Instant At(std::int64_t nanoseconds) noexcept
{
	return Monotonic::FromNanoseconds(nanoseconds);
}

OutputConfiguration Fixed(Duration period, Duration latch = Duration::zero())
{
	OutputConfiguration configuration;
	configuration.Resolution = { 2560, 1440 };
	configuration.Period = period;
	configuration.LatchLead = latch;

	return configuration;
}

OutputConfiguration Variable(Duration period, Duration shortest, Duration longest)
{
	OutputConfiguration configuration = Fixed(period);
	configuration.Refresh = { true, shortest, longest };

	return configuration;
}

PresentationInfo Flip(Instant presentedAt, Duration period, std::uint64_t sequence, bool hardwareClock = true)
{
	return { .PresentedAt = presentedAt, .Period = period, .Sequence = sequence, .HardwareClock = hardwareClock };
}
} // namespace

GYRO_TEST(FrameClock, SeedsOnTheFirstObservation)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms));

	GYRO_CHECK(!clock.IsValid());
	GYRO_CHECK_EQ(clock.Period(), 10ms);
	GYRO_CHECK_EQ(clock.LastSequence(), FrameClock::NoSequence);

	clock.Observe(Flip(At(1'000'000'000), 10ms, 7));

	GYRO_CHECK(clock.IsValid());
	GYRO_CHECK(clock.IsPrecise());
	GYRO_CHECK_EQ(clock.LastSequence(), std::uint64_t{ 7 });
	GYRO_CHECK_EQ(clock.PresentationAt(7), At(1'000'000'000));
	GYRO_CHECK_EQ(clock.NextPresentation(), At(1'010'000'000));
}

// The anchor is what a resume falsifies. The rate estimate outlives it, because admission control
// still has to size an output that has not presented since.
GYRO_TEST(FrameClock, InvalidationDropsTheAnchorAndKeepsTheRate)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms));
	clock.Observe(Flip(At(1'000'000'000), 8ms, 7));

	clock.Invalidate();

	GYRO_CHECK(!clock.IsValid());
	GYRO_CHECK(!clock.IsPrecise());
	GYRO_CHECK_EQ(clock.Period(), 8ms);
	GYRO_CHECK_EQ(clock.LastSequence(), FrameClock::NoSequence);
}

// A mode set is one of the three events that make the last observation describe a world that no
// longer exists, and an achieved configuration is that event reporting itself.
GYRO_TEST(FrameClock, ConfigurationInvalidates)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms));
	clock.Observe(Flip(At(1'000'000'000), 10ms, 7));
	GYRO_REQUIRE(clock.IsValid());

	clock.Configure(Fixed(20ms));

	GYRO_CHECK(!clock.IsValid());
	GYRO_CHECK_EQ(clock.Period(), 20ms);
}

// The sentinel, and the two behaviours it was chosen for: no timer is armed, and a frame damage
// demands passes the record-time check rather than being refused forever.
GYRO_TEST(FrameClock, AnInvalidClockArmsNothingAndRefusesNothing)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms));

	GYRO_CHECK_EQ(clock.NextPresentation(), FrameClock::Unscheduled);
	GYRO_CHECK_EQ(clock.NextDeadline(), FrameClock::Unscheduled);
	GYRO_CHECK_EQ(clock.NextWakeup(4ms), FrameClock::Unscheduled);
	GYRO_CHECK_EQ(clock.PresentationAt(9'000), FrameClock::Unscheduled);
	GYRO_CHECK_EQ(clock.SequenceAfter(At(1'000'000'000)), FrameClock::NoSequence);

	// Decision 35's record-time check, run against a clock that names no frame: the planned tier is
	// admitted, which is decision 31's damage-driven first frame after an idle output.
	const Instant now = At(1'000'000'000);
	GYRO_CHECK(now + 4ms <= clock.NextDeadline());
}

// A backend that does not know its period says zero rather than filling the field in, so the estimate
// comes from the anchor difference — divided by the sequence advance, which is what makes it right
// across frames that were skipped rather than only across consecutive ones.
GYRO_TEST(FrameClock, AnUnreportedPeriodIsMeasuredFromTheAnchor)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms));

	clock.Observe(Flip(At(1'000'000'000), Duration::zero(), 7));
	GYRO_CHECK_EQ(clock.Period(), 10ms);

	// Three vblanks later, and the third of them is the one that presented.
	clock.Observe(Flip(At(1'024'000'000), Duration::zero(), 10));
	GYRO_CHECK_EQ(clock.Period(), 8ms);

	// A backend that reports one again is believed over the difference, since it is the only figure
	// here measured by something with a view of the hardware.
	clock.Observe(Flip(At(1'032'000'000), 16ms, 11));
	GYRO_CHECK_EQ(clock.Period(), 16ms);
}

// A sequence that does not advance is a backend fault, and the previous estimate survives it rather
// than being replaced by a division nobody can defend.
GYRO_TEST(FrameClock, AStalledSequenceLeavesTheEstimateAlone)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms));
	clock.Observe(Flip(At(1'000'000'000), 8ms, 7));

	clock.Observe(Flip(At(1'008'000'000), Duration::zero(), 7));

	GYRO_CHECK_EQ(clock.Period(), 8ms);
	GYRO_CHECK_EQ(clock.PresentationAt(8), At(1'016'000'000));
}

GYRO_TEST(FrameClock, PredictionAnswersForThePastAsReadilyAsTheFuture)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms));
	clock.Observe(Flip(At(1'000'000'000), 10ms, 100));

	GYRO_CHECK_EQ(clock.PresentationAt(103), At(1'030'000'000));
	GYRO_CHECK_EQ(clock.PresentationAt(97), At(970'000'000));
}

// The two verbs of the deadline: a lead the panel and its driver need, which arrives with the mode,
// and a reserve the caller measured of itself. Neither is the other's, and the clock holds only the
// first.
GYRO_TEST(FrameClock, TheDeadlineLeadsThePresentationAndTheWakeupLeadsTheDeadline)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms, 400us));
	clock.Observe(Flip(At(1'000'000'000), 10ms, 7));

	GYRO_CHECK_EQ(clock.NextPresentation(), At(1'010'000'000));
	GYRO_CHECK_EQ(clock.NextDeadline(), At(1'009'600'000));
	GYRO_CHECK_EQ(clock.NextWakeup(4ms), At(1'005'600'000));

	// A negative reserve is read as none rather than as a wakeup after the deadline it came from.
	GYRO_CHECK_EQ(clock.NextWakeup(-4ms), clock.NextDeadline());
}

// Nested and headless have no such requirement, and pass zero rather than opting out of the concept.
GYRO_TEST(FrameClock, WithoutALatchLeadTheTwoInstantsCoincide)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms));
	clock.Observe(Flip(At(1'000'000'000), 10ms, 7));

	GYRO_CHECK_EQ(clock.NextDeadline(), clock.NextPresentation());
}

// The recovery path, and the composition it exists for: hand it the instant the work would finish and
// it names the frame that work can still make.
GYRO_TEST(FrameClock, SequenceAfterNamesTheFrameWorkCanStillMake)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms));
	clock.Observe(Flip(At(1'000'000'000), 10ms, 7));

	// A deadline falling exactly on the instant asked about is one the work can still make.
	GYRO_CHECK_EQ(clock.SequenceAfter(At(1'010'000'000)), std::uint64_t{ 8 });
	GYRO_CHECK_EQ(clock.SequenceAfter(At(1'010'000'001)), std::uint64_t{ 9 });

	// Decision 35's ceil(overrun / P) after a stall of two and a half periods.
	GYRO_CHECK_EQ(clock.SequenceAfter(At(1'025'000'000)), std::uint64_t{ 10 });

	// A second of idle, and the answer is a hundred frames on rather than one.
	GYRO_CHECK_EQ(clock.SequenceAfter(At(2'000'000'000)), std::uint64_t{ 107 });

	// Never a frame already presented, whatever instant is asked about.
	GYRO_CHECK_EQ(clock.SequenceAfter(At(0)), std::uint64_t{ 8 });
}

// The lead moves the deadline, so it moves which frame is reachable — the point of asking the clock
// rather than counting periods at the call site.
GYRO_TEST(FrameClock, SequenceAfterCountsFromTheDeadlineRatherThanThePresentation)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms, 1ms));
	clock.Observe(Flip(At(1'000'000'000), 10ms, 7));

	// Frame 8's deadline is 1'009'000'000, so work finishing a microsecond later has missed it.
	GYRO_CHECK_EQ(clock.SequenceAfter(At(1'009'000'000)), std::uint64_t{ 8 });
	GYRO_CHECK_EQ(clock.SequenceAfter(At(1'009'000'001)), std::uint64_t{ 9 });
}

// Speculative early rendering asks about frames that have not happened, so an absurd sequence is a
// question to answer badly rather than a precondition to assume.
GYRO_TEST(FrameClock, AnAbsurdSequenceSaturatesRatherThanWrapping)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms));
	clock.Observe(Flip(At(1'000'000'000), 10ms, 7));

	GYRO_CHECK_EQ(clock.PresentationAt(std::numeric_limits<std::uint64_t>::max()), FrameClock::Unscheduled);
	GYRO_CHECK_EQ(clock.DeadlineAt(std::numeric_limits<std::uint64_t>::max()), FrameClock::Unscheduled);
	GYRO_CHECK_EQ(clock.PresentationAt(0), At(930'000'000));

	// And the same distance backwards, which is reachable only from an anchor no panel will ever
	// report and is here because the negation it guards is the undefined one. It saturates into the
	// deep past rather than wrapping into the future, which is the half that matters: a wrapped
	// instant is a deadline the timing policy would cheerfully admit.
	FrameClock late;
	late.Configure(Fixed(10ms));
	late.Observe(Flip(At(1'000'000'000), 10ms, std::numeric_limits<std::uint64_t>::max()));
	GYRO_CHECK(late.PresentationAt(0) < At(0));
}

// Precision is per clock rather than per machine, and it is a property of the anchor: a clock with no
// anchor is not precise about anything.
GYRO_TEST(FrameClock, PrecisionFollowsTheObservation)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms));

	GYRO_CHECK(!clock.IsPrecise());

	clock.Observe(Flip(At(1'000'000'000), 10ms, 7, false));
	GYRO_CHECK(clock.IsValid());
	GYRO_CHECK(!clock.IsPrecise());

	clock.Observe(Flip(At(1'010'000'000), 10ms, 8, true));
	GYRO_CHECK(clock.IsPrecise());

	clock.Invalidate();
	GYRO_CHECK(!clock.IsPrecise());
}

GYRO_TEST(FrameClock, AFixedOutputReportsADegenerateRange)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms));
	clock.Observe(Flip(At(1'000'000'000), 8ms, 7));

	GYRO_CHECK(!clock.IsVariable());
	GYRO_CHECK(clock.PeriodRange().IsDegenerate());
	GYRO_CHECK_EQ(clock.PeriodRange().Shortest, 8ms);
}

// Cadence is not gyro's to set on a fixed output, and a stored request would be a number with no
// reader — worse, one a later reader could mistake for authority.
GYRO_TEST(FrameClock, CommandIsANoOpOnAFixedOutput)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms));
	clock.Observe(Flip(At(1'000'000'000), 10ms, 7));

	clock.Command(16ms);

	GYRO_CHECK_EQ(clock.CommandedPeriod(), 10ms);
	GYRO_CHECK_EQ(clock.TargetPeriod(), 10ms);
	GYRO_CHECK_EQ(clock.NextPresentation(), At(1'010'000'000));
}

// The servo: a target, a bounded step per observation, and convergence. The divisor is four here so
// the arithmetic is legible; the shipped default is sixteen.
GYRO_TEST(FrameClock, TheServoConvergesInBoundedSteps)
{
	FrameClock clock{ FrameClockPolicy{ .ServoStepDivisor = 4 } };
	clock.Configure(Variable(10ms, 5ms, 20ms));
	clock.Observe(Flip(At(1'000'000'000), 10ms, 7));

	GYRO_CHECK_EQ(clock.CommandedPeriod(), 10ms);

	// A command with no observation behind it moves nothing: the loop closes on what the panel did
	// rather than on the fact of having asked.
	clock.Command(16ms);
	GYRO_CHECK_EQ(clock.TargetPeriod(), 16ms);
	GYRO_CHECK_EQ(clock.CommandedPeriod(), 10ms);

	clock.Observe(Flip(At(1'010'000'000), 10ms, 8));
	GYRO_CHECK_EQ(clock.CommandedPeriod(), 12'500us);

	clock.Observe(Flip(At(1'022'000'000), 12ms, 9));
	GYRO_CHECK_EQ(clock.CommandedPeriod(), 15'625us);

	clock.Observe(Flip(At(1'037'000'000), 15ms, 10));
	GYRO_CHECK_EQ(clock.CommandedPeriod(), 16ms);
	GYRO_CHECK_EQ(clock.TargetPeriod(), 16ms);

	// And what the panel did stays separately answerable throughout, which is the whole reason the two
	// are not one accessor.
	GYRO_CHECK_EQ(clock.Period(), 15ms);
}

// A servo that ramps toward a shorter period is the same mechanism and not a second one.
GYRO_TEST(FrameClock, TheServoRampsInBothDirections)
{
	FrameClock clock{ FrameClockPolicy{ .ServoStepDivisor = 4 } };
	clock.Configure(Variable(16ms, 5ms, 20ms));
	clock.Observe(Flip(At(1'000'000'000), 16ms, 7));

	clock.Command(8ms);
	clock.Observe(Flip(At(1'016'000'000), 16ms, 8));

	GYRO_CHECK_EQ(clock.CommandedPeriod(), 12ms);
}

// The panel's window bounds the servo's authority, and the clearance keeps it off the bottom so that
// low-framerate compensation never engages underneath a period gyro commanded.
GYRO_TEST(FrameClock, TheServoIsClampedToThePanelsRange)
{
	FrameClock clock{ FrameClockPolicy{ .RangeClearance = 2ms } };
	clock.Configure(Variable(10ms, 5ms, 20ms));

	clock.Command(1ms);
	GYRO_CHECK_EQ(clock.TargetPeriod(), 5ms);

	clock.Command(19ms);
	GYRO_CHECK_EQ(clock.TargetPeriod(), 18ms);
}

// A window that cannot be true is a backend saying nothing, so the servo is refused rather than
// unbounded: rung 1 is unavailable and admission control falls through to the quality tier, which is
// the direction that cannot produce a period no panel agreed to.
GYRO_TEST(FrameClock, ANonsenseRangeDisablesTheServo)
{
	FrameClock clock;
	clock.Configure(Variable(10ms, 20ms, 5ms));

	clock.Command(50ms);

	GYRO_CHECK(clock.IsVariable());
	GYRO_CHECK(clock.PeriodRange().IsDegenerate());
	GYRO_CHECK_EQ(clock.TargetPeriod(), 10ms);
	GYRO_CHECK_EQ(clock.CommandedPeriod(), 10ms);
}

// On a variable panel the vblank happens when gyro submits, so what gyro intends is the prediction.
// On a fixed one the panel decides and gyro's intention is not an input — which is the same clock
// reading two different periods, and the reason both are kept.
GYRO_TEST(FrameClock, PredictionFollowsTheCommandedPeriodOnlyWhereCadenceIsGyros)
{
	FrameClock variable{ FrameClockPolicy{ .ServoStepDivisor = 4 } };
	variable.Configure(Variable(10ms, 5ms, 20ms));
	variable.Command(20ms);
	variable.Observe(Flip(At(1'000'000'000), 10ms, 7));

	GYRO_CHECK_EQ(variable.Period(), 10ms);
	GYRO_CHECK_EQ(variable.CommandedPeriod(), 12'500us);
	GYRO_CHECK_EQ(variable.NextPresentation(), At(1'012'500'000));

	FrameClock fixed;
	fixed.Configure(Fixed(10ms));
	fixed.Command(20ms);
	fixed.Observe(Flip(At(1'000'000'000), 10ms, 7));

	GYRO_CHECK_EQ(fixed.NextPresentation(), At(1'010'000'000));
}

// Decision 31's idle VRR output, end to end: the observation is not stale but wrong, so going idle
// invalidates; nothing is due while it is invalid; the next frame is damage-driven and re-seeds.
GYRO_TEST(FrameClock, AnIdleVariableOutputInvalidatesAndRecoversOnDamage)
{
	FrameClock clock;
	clock.Configure(Variable(10ms, 5ms, 20ms));
	clock.Observe(Flip(At(1'000'000'000), 10ms, 7));

	clock.Invalidate();
	GYRO_CHECK_EQ(clock.NextWakeup(4ms), FrameClock::Unscheduled);

	// Damage arrives seconds later and the frame presents when it is ready rather than when a
	// prediction says. The flip that follows is what re-seeds the clock.
	clock.Observe(Flip(At(9'000'000'000), 14ms, 900));

	GYRO_CHECK(clock.IsValid());
	GYRO_CHECK_EQ(clock.Period(), 14ms);
	GYRO_CHECK_EQ(clock.LastSequence(), std::uint64_t{ 900 });
}

GYRO_TEST(FrameClock, ThePrintedFormReadsAsAClock)
{
	FrameClock clock;
	clock.Configure(Fixed(10ms));

	GYRO_CHECK_EQ(std::format("{}", clock), std::string{ "clock invalid period 10000000ns" });

	clock.Observe(Flip(At(1'000'000'000), 10ms, 7));
	GYRO_CHECK_EQ(std::format("{}", clock), std::string{ "clock seq 7 at 1000000000ns period 10000000ns hw-clock" });

	FrameClock variable;
	variable.Configure(Variable(10ms, 5ms, 20ms));
	variable.Command(16ms);

	GYRO_CHECK_EQ(
		std::format("{}", variable), std::string{ "clock invalid period 10000000ns vrr 10000000ns -> 16000000ns" }
	);
}
