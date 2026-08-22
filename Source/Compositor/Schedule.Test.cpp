#include "Compositor/Schedule.h"

#include <array>
#include <chrono>
#include <span>

#include "Core/Time.h"
#include "Frame/Admission.h"
#include "Frame/Timing.h"
#include "Testing/Test.h"

// This file is a bridge across a difference in arity, and every way it can be wrong is a way the two
// sides disagree without either being wrong on its own. Admission control reasons about one `C` per
// output; `Frame/Timing.h` composes two. So the test worth writing is not that the arithmetic works —
// it is that the number going in is the number the record-time check will actually evaluate, and that
// the number coming back is one the renderer is also told about.

namespace
{
using namespace std::chrono_literals;

constexpr Duration Margin = 250us;

OutputDemand Panel(Duration period, Duration planned, Duration floor, bool focused = false)
{
	return OutputDemand{ .Period = period,
		                 .Planned = { .Cpu = planned / 4, .Gpu = planned - planned / 4 },
		                 .Floor = { .Cpu = floor / 4, .Gpu = floor - floor / 4 },
		                 .Refresh = {},
		                 .Authority = true,
		                 .Focused = focused };
}

Schedule Build(std::initializer_list<OutputDemand> demands)
{
	return Schedule::Build(std::span<const OutputDemand>{ demands.begin(), demands.size() }, Margin);
}
} // namespace

// The load-bearing assertion of the whole file. `Timing::Reserve` is CPU plus GPU plus the safety
// margin, and admission has to be given that same figure — a set admitted against a smaller number
// than the check holds is a set that passes the test and misses the frame, and nothing about it would
// look wrong from either side.
GYRO_TEST(Schedule, WhatIsAdmittedIsWhatTimingReserves)
{
	const CostSplit split{ .Cpu = 1ms, .Gpu = 3ms };

	Budget budget{ BudgetPolicy{ .InitialCpu = split.Cpu, .InitialGpu = split.Gpu } };
	const Timing timing{ TimingPolicy{ .Safety = Margin } };

	GYRO_CHECK_EQ(Schedule::Composed(split, Margin), timing.Reserve(budget, RenderMode::Planned));
}

GYRO_TEST(Schedule, AFeasibleSetIsHandedBackUntouched)
{
	const Schedule schedule = Build({ Panel(16'666'667ns, 4ms, 1ms) });

	GYRO_REQUIRE_EQ(schedule.Count(), std::size_t{ 1 });
	GYRO_CHECK(schedule.Admitted().IsFeasible());

	const OutputPlan& plan = schedule.Plans()[0];

	GYRO_CHECK(!plan.Reduced);
	GYRO_CHECK(!plan.Lengthened);
	GYRO_CHECK_EQ(plan.Period, 16'666'667ns);
	GYRO_CHECK_EQ(plan.Planned.Cpu, 1ms);
	GYRO_CHECK_EQ(plan.Planned.Gpu, 3ms);
	GYRO_CHECK_EQ(plan.Floor.Cpu, 250us);
}

// Docs/Architecture.md#outputs-are-independent-periodic-tasks quotes this configuration by name: a
// 144 Hz panel beside a 60 Hz projector, where the set is utilization-feasible and non-preemptively
// infeasible, and the slack is in the wrong place. What this test is about is not that admission
// reduces it — that is Frame/Admission.h's own test — but that the reduction reaches both places it
// has to reach.
GYRO_TEST(Schedule, AReducedAllocationScalesBothDevices)
{
	const Schedule schedule = Build({ Panel(6'944'444ns, 4ms, 1ms, true), Panel(16'666'667ns, 5ms, 1ms) });

	GYRO_REQUIRE_EQ(schedule.Count(), std::size_t{ 2 });

	const OutputPlan& projector = schedule.Plans()[1];

	GYRO_REQUIRE(projector.Reduced);

	// Both halves moved, and in proportion. A reduction that reached only one of them would leave a
	// composed cost the record-time check still fails, on an output admission believed it had rescued.
	GYRO_CHECK(projector.Planned.Cpu < 1'250'000ns);
	GYRO_CHECK(projector.Planned.Gpu < 3'750'000ns);
	GYRO_CHECK(projector.Planned.Cpu > Duration::zero());
	GYRO_CHECK(projector.Planned.Gpu > Duration::zero());

	// The unfocused output is the one that gives ground, which is Architecture.md's rule and is what
	// makes this the projector rather than the panel under someone's hands.
	GYRO_CHECK(!schedule.Plans()[0].Reduced);
}

// The scaled figures are still a *composed* cost that fits what was allocated. Asserting the
// composition rather than the two terms is what keeps this test honest if the split ever stops being
// a ratio — which the SPEC note in Compositor/Schedule.h says it will.
GYRO_TEST(Schedule, TheScaledSplitFitsTheAllocation)
{
	const Schedule schedule = Build({ Panel(6'944'444ns, 4ms, 1ms, true), Panel(16'666'667ns, 5ms, 1ms) });

	for (std::size_t index = 0; index < schedule.Count(); ++index)
	{
		GYRO_CHECK(Schedule::Composed(schedule.Plans()[index].Planned, Margin) <= schedule.Admitted()[index].Cost);
	}
}

// The floor is what decision 35's second branch renders and admission already refused to allocate
// below it. Scaling it with everything else would produce a floor composite the set was never
// admitted against, which is the one reduction that cannot be recovered from.
GYRO_TEST(Schedule, TheFloorIsNeverScaled)
{
	const OutputDemand panel = Panel(6'944'444ns, 4ms, 1ms, true);
	const OutputDemand projector = Panel(16'666'667ns, 5ms, 1ms);

	const Schedule schedule = Build({ panel, projector });

	GYRO_REQUIRE(schedule.Plans()[1].Reduced);
	GYRO_CHECK_EQ(schedule.Plans()[1].Floor.Cpu, projector.Floor.Cpu);
	GYRO_CHECK_EQ(schedule.Plans()[1].Floor.Gpu, projector.Floor.Gpu);
}

// A budget seeded with what the output may spend rather than with nothing, which is Budget.h's
// sanctioned use of `Initial*`: the figure a probe supplies. Seeding it with the allocation is what
// makes the first frame's assessment mean something instead of discovering the cost a frame later.
GYRO_TEST(Schedule, ThePlanSeedsTheBudgetItAllocated)
{
	const Schedule schedule = Build({ Panel(16'666'667ns, 4ms, 1ms) });
	const OutputPlan& plan = schedule.Plans()[0];
	const BudgetPolicy policy = plan.Budget();

	GYRO_CHECK_EQ(policy.InitialCpu, plan.Planned.Cpu);
	GYRO_CHECK_EQ(policy.InitialGpu, plan.Planned.Gpu);
	GYRO_CHECK_EQ(policy.FloorCpu, plan.Floor.Cpu);
	GYRO_CHECK_EQ(policy.FloorGpu, plan.Floor.Gpu);
}

GYRO_TEST(Schedule, AnEmptySetIsAnEmptyPlan)
{
	const Schedule schedule = Schedule::Build({}, Margin);

	GYRO_CHECK_EQ(schedule.Count(), std::size_t{ 0 });
	GYRO_CHECK(schedule.Plans().empty());
	GYRO_CHECK(schedule.Admitted().IsFeasible());
}
