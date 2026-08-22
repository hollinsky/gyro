#include "Frame/Admission.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <span>
#include <string>

#include "Core/Time.h"
#include "Seam/OutputConfiguration.h"
#include "Testing/Test.h"

// The arithmetic of the test itself is asserted in the header, against the configurations
// Architecture.md quotes. What is worth a test here is the ladder — which rung is spent, on which
// output, and how far — because every one of those is a policy decision that a reader can check by
// hand and that nothing else in the system will notice going wrong.
//
// Two outputs recur below: a 144 Hz panel at 6944444 ns and a 60 Hz projector at 16666666 ns. Where a
// number is quoted rather than derived it is `P_fast - C_fast`, which is the whole of the simple form.

namespace
{
using namespace std::chrono_literals;

constexpr Duration Fast = PeriodFromHertz(144.0);
constexpr Duration Slow = PeriodFromHertz(60.0);

constexpr OutputTask Panel(Duration want, Duration floor = 1ms)
{
	return { .Period = Fast, .Want = want, .Floor = floor, .Focused = true };
}

constexpr OutputTask Projector(Duration want, Duration floor = 1ms)
{
	return { .Period = Slow, .Want = want, .Floor = floor };
}

constexpr OutputTask Variable(Duration want, Duration longest, bool authority)
{
	return { .Period = Fast,
		     .Want = want,
		     .Floor = 1ms,
		     .Refresh = { true, Fast, longest },
		     .Authority = authority,
		     .Focused = true };
}
} // namespace

GYRO_TEST(Admission, AllocatesWhatWasAskedWhereTheSetFits)
{
	const std::array tasks{ Panel(2ms), Projector(2ms) };
	const Plan plan = Admit(tasks);

	GYRO_CHECK(plan.IsFeasible());
	GYRO_CHECK(plan.Deepest() == Rung::None);
	GYRO_CHECK(plan.Count() == 2);
	GYRO_CHECK(plan[0].Cost == 2ms);
	GYRO_CHECK(plan[1].Cost == 2ms);
	GYRO_CHECK(plan[0].Period == Fast);
	GYRO_CHECK(!plan[0].Reduced() && !plan[1].Reduced());
}

// The simple form, in the one configuration Architecture.md quotes a number for. Utilization is 0.88
// and the set is non-preemptively infeasible, so the slack exists and is in the wrong place.
GYRO_TEST(Admission, TheSlowOutputIsHeldToWhatTheFastPeriodLeaves)
{
	const std::array tasks{ Panel(4ms), Projector(5ms) };
	const Plan plan = Admit(tasks);

	GYRO_REQUIRE(plan.IsFeasible());
	GYRO_CHECK(plan.Deepest() == Rung::Tier);
	GYRO_CHECK(plan[0].Cost == 4ms);
	GYRO_CHECK(plan[1].Cost == Fast - 4ms);
	GYRO_CHECK(plan[1].Cost == Duration{ 2'944'444 });
	GYRO_CHECK(plan[1].Reduced() && !plan[1].AtFloor());
}

// Architecture.md#admission-control's rule, isolated from refresh rate: two identical outputs, and the
// one under someone's hands keeps what it asked for.
GYRO_TEST(Admission, FocusIsTheLastToGiveAnythingUp)
{
	std::array tasks{ Projector(9ms), Projector(9ms) };
	tasks[1].Focused = true;

	const Plan plan = Admit(tasks);

	GYRO_REQUIRE(plan.IsFeasible());
	GYRO_CHECK(plan[1].Cost == 9ms);
	GYRO_CHECK(plan[0].Reduced() && !plan[0].AtFloor());
}

// And the tie-break, isolated from focus: neither output is focused, so the slower one gives way.
GYRO_TEST(Admission, TiesAreBrokenTowardTheSlowerRefresh)
{
	std::array tasks{ Panel(4ms), Projector(5ms) };
	tasks[0].Focused = false;

	const Plan plan = Admit(tasks);

	GYRO_REQUIRE(plan.IsFeasible());
	GYRO_CHECK(!plan[0].Reduced());
	GYRO_CHECK(plan[1].Reduced());
}

// Rung 1 is spent before rung 2 and nothing else is spent at all — the only response to an infeasible
// set that costs the user nothing.
GYRO_TEST(Admission, AVariableRefreshPeriodIsTheFirstThingSpent)
{
	const std::array tasks{ Variable(4ms, PeriodFromHertz(48.0), true), Projector(5ms) };
	const Plan plan = Admit(tasks);

	GYRO_REQUIRE(plan.IsFeasible());
	GYRO_CHECK(plan.Deepest() == Rung::Period);
	GYRO_CHECK(!plan[0].Reduced() && !plan[1].Reduced());
	GYRO_CHECK(plan[0].Lengthened(tasks[0]));
}

// And it is spent minimally. `4 + 5 <= P` is the binding constraint, so the panel servoes to exactly
// nine milliseconds — 111 Hz — rather than to the 48 Hz the panel would also have accepted.
GYRO_TEST(Admission, TheSmallestStepThatWorksIsTheOneTaken)
{
	const std::array tasks{ Variable(4ms, PeriodFromHertz(48.0), true), Projector(5ms) };
	const Plan plan = Admit(tasks);

	GYRO_CHECK(plan[0].Period == 9ms);
}

// Decision 66's failure, as the difference between two plans that differ in one bool: a client whose
// arrivals gyro cannot predict has taken the rate, rung 1 is gone, and the projector pays.
GYRO_TEST(Admission, LosingCadenceAuthorityCostsTheOtherOutputARung)
{
	const std::array held{ Variable(4ms, PeriodFromHertz(48.0), true), Projector(5ms) };
	const std::array taken{ Variable(4ms, PeriodFromHertz(48.0), false), Projector(5ms) };

	GYRO_CHECK(Admit(held).Deepest() == Rung::Period);
	GYRO_CHECK(Admit(held)[1].Cost == 5ms);

	const Plan fallen = Admit(taken);

	GYRO_CHECK(fallen.Deepest() == Rung::Tier);
	GYRO_CHECK(fallen[0].Period == Fast);
	GYRO_CHECK(fallen[1].Cost == Fast - 4ms);
}

// The range is learned rather than requested, and a panel with nowhere to go is a panel rung 1 skips.
GYRO_TEST(Admission, ARangeWithNoRoomIsNotARung)
{
	const std::array tasks{ Variable(4ms, Fast, true), Projector(5ms) };
	const Plan plan = Admit(tasks);

	GYRO_CHECK(plan.Deepest() == Rung::Tier);
	GYRO_CHECK(plan[0].Period == Fast);
}

// Rung 1 stays applied when it cannot finish the job on its own, which is what *falls through to the
// next rung* means: the panel goes as far as its range allows and the tier step pays only the rest.
GYRO_TEST(Admission, AnInsufficientPeriodStillCountsAgainstTheTierStep)
{
	const std::array partial{ Variable(6ms, 9ms, true), Projector(9ms) };
	const std::array none{ Panel(6ms), Projector(9ms) };

	const Plan lengthened = Admit(partial);
	const Plan fixed = Admit(none);

	GYRO_REQUIRE(lengthened.IsFeasible() && fixed.IsFeasible());
	GYRO_CHECK(lengthened.Deepest() == Rung::Tier);
	GYRO_CHECK(lengthened[0].Period == 9ms);
	GYRO_CHECK(lengthened[1].Cost > fixed[1].Cost);
}

// Decision 30's chunking enters the test in exactly one place: it reduces `B` and never `h`. The
// configuration below is the one quoted above, rescued by splitting the projector's blur chain.
GYRO_TEST(Admission, ChunkingReducesTheBlockingTermAndNothingElse)
{
	std::array tasks{ Panel(4ms), Projector(5ms) };
	tasks[1].Chunk = 2'900'000ns;

	const Plan plan = Admit(tasks);

	GYRO_CHECK(plan.Deepest() == Rung::None);
	GYRO_CHECK(plan[1].Cost == 5ms);
}

// Decision 29's three-display case, with the allocation checked rather than the verdict: the second
// panel's demand accumulates into `h`, so the projector is quoted less than the simple form would.
GYRO_TEST(Admission, AccumulatedDemandBindsBeforeBlockingDoes)
{
	// Floors well under the answer, so the projector can absorb the whole of it and the assertion is
	// about the arithmetic rather than about which output runs out of room first.
	std::array tasks{ Panel(3ms, 500us), Panel(3ms, 500us), Projector(2ms, 500us) };
	tasks[1].Focused = false;

	const Plan plan = Admit(tasks);

	GYRO_REQUIRE(plan.IsFeasible());
	GYRO_CHECK(plan[0].Cost == 3ms);
	GYRO_CHECK(plan[1].Cost == 3ms);
	GYRO_CHECK(plan[2].Cost == Fast - 6ms);
}

// Decision 29's software-rendering case: every rung gone, the allocation below the floor composite,
// and the answer still a plan. The frame loop takes it from here, and decision 35's skip is the
// mechanism.
GYRO_TEST(Admission, InfeasibleIsADegradedPlanAndNeverARefusal)
{
	const std::array tasks{ Panel(9ms, 9ms), Projector(9ms, 9ms) };
	const Plan plan = Admit(tasks);

	GYRO_CHECK(!plan.IsFeasible());
	GYRO_CHECK(plan.Deepest() == Rung::Drop);
	GYRO_CHECK(plan.Count() == 2);
	GYRO_CHECK(plan[0].Cost == 9ms && plan[0].AtFloor());
	GYRO_CHECK(plan[1].Cost == 9ms && plan[1].AtFloor());
}

// A want below the floor is a request for less than the output is guaranteed, and the floor wins.
GYRO_TEST(Admission, TheFloorOutranksAWantBelowIt)
{
	const std::array tasks{ Projector(1ms, 3ms) };
	const Plan plan = Admit(tasks);

	GYRO_CHECK(plan[0].Cost == 3ms);
	GYRO_CHECK(plan[0].Want == 3ms);
	GYRO_CHECK(plan[0].AtFloor());
}

// The utilization ceiling is a rejection before the interval loop rather than a bound on it, because
// `L*` diverges as `U` approaches one. A single output is trivially schedulable at any `C <= P` and is
// still held to 0.95 of its period.
GYRO_TEST(Admission, HeadroomIsReservedEvenWhereTheIntervalTestWouldPass)
{
	const std::array tasks{ Projector(16ms) };
	const Plan plan = Admit(tasks);

	GYRO_REQUIRE(plan.IsFeasible());
	GYRO_CHECK(plan[0].Reduced());
	GYRO_CHECK(plan.UtilizationPerMille() <= 950);
	GYRO_CHECK(plan.UtilizationPerMille() >= 949);
}

// An output that demands nothing is not in the test and still has a row, so a caller indexes the plan
// by the request it made.
GYRO_TEST(Admission, AnOutputWithNoPeriodIsNotInTheSet)
{
	std::array tasks{ Panel(4ms), Projector(5ms), OutputTask{} };

	const Plan plan = Admit(tasks);

	GYRO_CHECK(plan.Count() == 3);
	GYRO_CHECK(plan[2].Cost == Duration::zero());
	GYRO_CHECK(plan[1].Cost == Fast - 4ms);
}

// Fixed capacity, and the excess is dropped rather than growing anything. The frame section forbids
// the alternative.
GYRO_TEST(Admission, TheSetIsBounded)
{
	std::array<OutputTask, MaxOutputs + 4> tasks{};
	tasks.fill(Projector(100us));

	GYRO_CHECK(Admit(std::span<const OutputTask>{ tasks }).Count() == MaxOutputs);
}

// Nothing here is stateful, so the same request answers the same plan. It matters because admission
// runs on a trigger stated as *any input to the test* — a rerun that changed the plan without an input
// changing would repack the schedule for no reason.
GYRO_TEST(Admission, TheSameRequestAnswersTheSamePlan)
{
	const std::array tasks{ Panel(4ms), Projector(5ms), Projector(3ms) };

	GYRO_CHECK(Admit(tasks).Allocations()[1] == Admit(tasks).Allocations()[1]);
	GYRO_CHECK(Admit(tasks).UtilizationPerMille() == Admit(tasks).UtilizationPerMille());
}

GYRO_TEST(Admission, Formats)
{
	const std::array tasks{ Projector(4ms) };

	GYRO_CHECK(std::format("{}", Admit(tasks)) == "plan none U 0.240 [16666666ns 4000000ns/4000000ns]");
	GYRO_CHECK(Name(Rung::Drop) == "drop");
}
