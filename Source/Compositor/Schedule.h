#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

#include "Core/Time.h"
#include "Frame/Admission.h"
#include "Frame/Budget.h"
#include "Frame/Timing.h"
#include "Seam/OutputConfiguration.h"

// Admission control's answer, turned back into the things the frame loop is actually configured with.
//
// `Admit` takes a `C` per output and returns an allocation per output, and both are one number.
// Everything downstream of it is two: Frame/Budget.h keeps a CPU mark and a GPU mark, and
// Frame/Timing.h composes them as a *pipeline* rather than a sum, because the record runs on the frame
// thread while the device finishes what it has. So something has to travel in both directions across
// that difference, and this is it. It is in `Compositor` rather than in `Frame` for the reason
// Docs/Structure.md#orchestration gives: admission runs when a *configuration* changes, and the
// configuration is the root's.
//
// **Going in, `C` is the composed reserve**, which is `Timing::Reserve` — the two device figures plus
// the safety margin. Composing it any other way would admit a set against a number the record-time
// check never evaluates, which is the one way this bridge could be wrong without ever looking wrong.
//
// **Coming back, a reduced allocation scales both halves and leaves the margin alone.** The margin is
// gyro's own wakeup latency and is not the scene's to give up; what a tier step actually buys is a
// cheaper pass chain, and a chain with fewer passes is cheaper on both devices roughly in proportion.
// SPEC: *roughly in proportion* is the assertion here, and it is the crude half of this file. A real
// tier table will supply the pair per tier and this ratio goes away — see
// Docs/Architecture.md#quality-tiers, where the tier is chosen rather than solved for.

// One output's cost, split across the two devices the pipeline composes.
struct CostSplit
{
	Duration Cpu{};
	Duration Gpu{};

	[[nodiscard]] constexpr Duration Total() const noexcept { return Detail::Sum(Cpu, Gpu); }

	friend constexpr bool operator==(CostSplit, CostSplit) noexcept = default;
};

// What the root knows about one output before anything has been admitted.
struct OutputDemand
{
	Duration Period{};

	CostSplit Planned{};
	CostSplit Floor{};

	VariableRefresh Refresh{};

	bool Authority = false;
	bool Focused = false;
};

// What one output ends up configured with.
struct OutputPlan
{
	// The period the plan committed to, which is the task's unless rung 1 lengthened it. The
	// configuration handed to `Reconfigure` carries this rather than what the panel was asked for.
	Duration Period{};

	// What this output may actually spend, after any rung was applied. It seeds `Budget` *and* it is
	// what the simulated renderer is told to charge — the same figure in both places, because a tier
	// step that reduced the allowance without making the work cheaper is a plan the frame would then
	// miss against.
	CostSplit Planned{};
	CostSplit Floor{};

	bool Reduced = false;
	bool Lengthened = false;

	[[nodiscard]] constexpr BudgetPolicy Budget() const noexcept
	{
		return { .FloorCpu = Floor.Cpu,
			     .FloorGpu = Floor.Gpu,
			     .InitialCpu = Planned.Cpu,
			     .InitialGpu = Planned.Gpu,
			     .Window = BudgetPolicy{}.Window };
	}
};

namespace Detail
{

// `cost * numerator / denominator`, in integer nanoseconds and rounded down.
//
// Rounded down rather than up, which is the opposite of every rounding in Frame/Admission.h and is
// deliberate: there the answer is a *demand* and rounding up is what keeps the floors safe, here it is
// a *permission* and rounding up would hand back a nanosecond the test did not allocate. Both round in
// the direction that cannot produce a miss.
[[nodiscard]] constexpr Duration Scale(Duration cost, std::int64_t numerator, std::int64_t denominator) noexcept
{
	if (denominator <= 0 || numerator <= 0 || cost <= Duration::zero())
	{
		return Duration::zero();
	}

	if (numerator >= denominator)
	{
		return cost;
	}

	// Nanoseconds of a composite against nanoseconds of a period: some 10^7 by 10^7 at the outside,
	// which is 14 digits and a long way inside int64. Stated rather than asserted because the two
	// bounds are Frame/Admission.h's MaxSchedulable and are not this file's to enforce.
	return Duration{ (cost.count() * numerator) / denominator };
}

} // namespace Detail

// The plan, and the allocation it came from.
//
// Both are kept because they answer different questions: the allocations are what admission decided
// and what a log line or a sweep wants to read, and the plans are what the loop is configured with.
class Schedule
{
public:
	Schedule() = default;

	// `safety` is the same margin the loop's `Timing` was constructed with. Passing it rather than
	// reading it back off a `Timing` keeps this callable before one exists, which is the order the root
	// actually builds things in.
	[[nodiscard]] static Schedule Build(std::span<const OutputDemand> demands, Duration safety) noexcept
	{
		Schedule schedule;

		std::array<OutputTask, MaxOutputs> tasks{};
		const std::size_t count = std::min(demands.size(), tasks.size());

		for (std::size_t index = 0; index < count; ++index)
		{
			const OutputDemand& demand = demands[index];

			tasks[index] = OutputTask{ .Period = demand.Period,
				                       .Want = Composed(demand.Planned, safety),
				                       .Floor = Composed(demand.Floor, safety),
				                       .Chunk = Duration::zero(),
				                       .Refresh = demand.Refresh,
				                       .Authority = demand.Authority,
				                       .Focused = demand.Focused };
		}

		schedule.m_Admitted = Admit(std::span<const OutputTask>{ tasks.data(), count });
		schedule.m_Count = count;

		for (std::size_t index = 0; index < count; ++index)
		{
			schedule.m_Plans[index] = Apply(demands[index], schedule.m_Admitted[index], tasks[index], safety);
		}

		return schedule;
	}

	[[nodiscard]] std::size_t Count() const noexcept { return m_Count; }

	[[nodiscard]] std::span<const OutputPlan> Plans() const noexcept { return { m_Plans.data(), m_Count }; }

	[[nodiscard]] const Plan& Admitted() const noexcept { return m_Admitted; }

	// The composed reserve, which is what `Timing::Reserve` computes and therefore what admission has
	// to be given. Named here so that the two cannot drift apart silently.
	[[nodiscard]] static constexpr Duration Composed(CostSplit split, Duration safety) noexcept
	{
		return Detail::Sum(split.Total(), safety);
	}

private:
	[[nodiscard]] static constexpr OutputPlan
	Apply(const OutputDemand& demand, const Allocation& allocation, const OutputTask& task, Duration safety) noexcept
	{
		OutputPlan plan{ .Period = allocation.Period,
			             .Planned = demand.Planned,
			             .Floor = demand.Floor,
			             .Reduced = allocation.Reduced(),
			             .Lengthened = allocation.Lengthened(task) };

		if (!plan.Reduced)
		{
			return plan;
		}

		// The scene's share of each figure, which is the whole of it less the margin that is not being
		// given up. Both are clamped at zero: an allocation at or below the margin is a configuration
		// where gyro's own latency exceeds the frame, and the honest answer there is that the scene gets
		// nothing rather than that it gets a negative amount.
		const Duration allowed = std::max(allocation.Cost - safety, Duration::zero());
		const Duration wanted = std::max(allocation.Want - safety, Duration::zero());

		plan.Planned = { .Cpu = Detail::Scale(demand.Planned.Cpu, allowed.count(), wanted.count()),
			             .Gpu = Detail::Scale(demand.Planned.Gpu, allowed.count(), wanted.count()) };

		// The floor is a floor. It is what decision 35's second branch renders and admission already
		// refused to allocate below it, so it is carried through untouched — scaling it would produce a
		// floor composite the set was never admitted against.
		return plan;
	}

	std::array<OutputPlan, MaxOutputs> m_Plans{};
	std::size_t m_Count = 0;
	Plan m_Admitted{};
};
