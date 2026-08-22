#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numeric>
#include <span>
#include <string_view>

#include "Core/Time.h"
#include "Seam/OutputConfiguration.h"
// See Docs/Architecture.md#outputs-are-independent-periodic-tasks,
// Docs/Architecture.md#admission-control, Docs/Architecture.md#vrr-as-a-scheduling-degree-of-freedom,
// and decisions 29, 30, 31, 34, and 66.

// Decision 29's schedulability test, solved for `C`.
//
// The frame loop runs a schedule it was handed and decides one thing per iteration; this is what
// hands it that schedule. It runs when an input to the test changes — an output added or removed, a
// mode set, a measured cost moving the floor, cadence authority passing to or from a client — and
// never per frame. `𝓛` is not a usefully bounded loop and the frame thread does not host it.
//
// ```
//     U    = Σᵢ Cᵢ / Pᵢ
//
//     h(L) = Σᵢ ⌊L / Pᵢ⌋ · Cᵢ            work with a deadline at or before L
//     B(L) = max{ qₖ : Pₖ > L }          the longest non-preemptible chunk of any output
//                                         whose deadline falls after L
//
//     feasible  ⟺  U ≤ 1  and  ∀ L ∈ 𝓛 :  h(L) + B(L) ≤ L
// ```
//
// **It is read in the direction decision 29 revised it to.** `P` and the floor composite are given
// and the test is solved for the `C` each output may spend, because `C` is a property of the output
// *and what is on screen* and the peak — an overview transition with a dozen blurred thumbnails — is
// exactly what configuration time cannot know. So the answer is an allocation rather than a verdict,
// and `Admit` returns a degraded plan and never a refusal. A configuration the user asked for is
// always presented; infeasibility selects how much is given up and where.
//
// **`P` is the shortest interval in which an output may demand a frame, and it arrives already
// decided.** Decision 66's table is a question about who controls arrivals — a fixed output's
// measured period, a variable one's commanded period while the cadence is gyro's, the panel's
// minimum period while a client free-runs — and none of that is visible from here. The caller states
// `Period` and `Authority` and this treats the pair as fact. That is also what makes the `2ⁿ` plans
// decision 66 asks for a matter of calling this `2ⁿ` times rather than a mode inside it: control of
// each variable-refresh output is binary, so the composition root solves every combination at
// configuration change and the transition is a plan swap.
//
// **One `C` per output, and the composition that produced it is the caller's.** `Budget` holds two
// figures and refuses to sum them; `Timing::Reserve` composes them for one output as a pipeline that
// today computes a sum. That is the figure to pass, and passing it is an assertion about the
// configuration gyro is built first: one queue, unchunked, recording and execution in series.
// // SPEC: the honest generalisation is two resources — the CPU figures accumulate on one thread and
// the GPU figures on one queue, and decision 29's single `C` does not distinguish them. A set that is
// feasible on each device separately and infeasible on the pipeline is not expressible here, and the
// term stops being a sum in the same release chunking does. Until then the sum is conservative for
// one device and exact for the machine this runs on.
//
// **Two rungs of decision 30's ladder are built, and the gap in the enumeration below is the gate.**
// Rung 1 lengthens a variable-refresh output's period, which is free and changes an input to the
// test. Rung 2 reduces an output's allocation, which decision 34 spends as a quality tier step at the
// next commit. Rungs 3 and 4 — chunking, and speculative early rendering — are contingencies gated on
// measurement rather than first-cut work, so what is here for them is room: `qₖ` is a term of its own
// in the blocking bound rather than `Cₖ` spelled twice, and an allocation names a period the plan
// commits to rather than assuming the output's own. A configuration that rung 2 cannot rescue has to
// be observed before either is worth building.
//
// **Phase is not an input and cannot become a rung.** This is a processor-demand test over interval
// lengths rather than a simulation of a schedule, so a set that passes meets every deadline at every
// phase relationship. Drift guarantees every phase relationship eventually occurs, which is why
// aligning two outputs' vblanks may improve an already-feasible set and may never be the reason one
// is admitted.
//
// **Integer nanoseconds throughout, so the floors are exact.** The one place a rational is
// unavoidable is `U`, which is carried in fixed point and rounded *up* at every term — a utilization
// that reads high refuses a set that would have fitted, where one that reads low admits a set that
// misses. The `0.95` ceiling is already headroom for estimation error; this rounds into it rather
// than out of it.

// SPEC: sized rather than measured. Sixteen outputs is past any configuration gyro has been pointed
// at, and it is fixed capacity because the frame section forbids growing it. It lives here rather than
// with the loop because the capacity is the admission set's, and the loop's outputs are that set.
inline constexpr std::size_t MaxOutputs = 16;

// How far down decision 30's ladder the plan had to go. Ordered by what it costs the person looking
// at the screen, so the deepest rung a plan reached is a maximum.
//
// **Three and four are missing on purpose.** They are chunking and early rendering, they are
// contingencies gated on measurement, and a reader who looks for them should find the gap rather than
// a value that quietly means something else.
enum class Rung : std::uint8_t
{
	// Every output was allocated what it asked for at the period it already had.
	None = 0,

	// A variable-refresh output's period was lengthened. Free — no visual cost, and the only response
	// to an infeasible set that costs the user nothing at all.
	Period = 1,

	// An output was allocated less than it asked for, which decision 34 spends as a quality tier step
	// at the next commit.
	Tier = 2,

	// Every output is at its floor composite and the set still does not fit. Decision 30's last rung —
	// a known drop pattern — is the frame loop's rather than the plan's, since decision 35's skip is
	// already the mechanism; what this says is that it will be taken.
	Drop = 5,
};

[[nodiscard]] constexpr std::string_view Name(Rung rung) noexcept
{
	switch (rung)
	{
		case Rung::None:
			return "none";
		case Rung::Period:
			return "period";
		case Rung::Tier:
			return "tier";
		case Rung::Drop:
			return "drop";
	}

	return "invalid";
}

// One output as the test sees it: a period, what it would like to spend, and what it cannot go below.
struct OutputTask
{
	// The shortest interval in which this output may demand a frame. Decided by the caller from
	// decision 66's table; a non-positive period is an output that demands nothing and is not in the
	// set.
	Duration Period{};

	// `C` at the planned tier — what the output spends if nothing takes it away. Held up to `Floor`,
	// since an output cannot be allocated less than the composite it is guaranteed.
	Duration Want{};

	// `C_min`, the floor composite. Decision 34 makes it a design target rather than a residue, and
	// this is the one place that matters to a caller: it is the point below which a configuration is
	// infeasible rather than merely degraded.
	Duration Floor{};

	// `qₖ`, the largest non-preemptible chunk. Zero means *the whole composite*, which is every output
	// today. It is a field rather than a use of `Cₖ` because chunking enters the test here and only
	// here — it reduces `B` and never `h` — and leaving room for it costs one term.
	Duration Chunk{};

	// The range the panel will accept, which is learned at modeset rather than requested. Rung 1 moves
	// `Period` inside it and nowhere else.
	VariableRefresh Refresh{};

	// Whether the cadence is gyro's to command. Decision 66: a client whose next arrival gyro cannot
	// predict has taken it, and rung 1 is unavailable for as long as that is true. False is the
	// pessimistic reading and the one that cannot produce a miss.
	bool Authority = false;

	// The output under someone's hands. It is the last to give anything up, at every rung that costs
	// something, because that is where latency and staleness are perceived.
	bool Focused = false;
};

// What one output may spend, and at what period.
struct Allocation
{
	// The period the plan commits to. Equal to the task's unless rung 1 lengthened it — and decision
	// 31's servo may not command shorter than this, which is the whole of what committing to it means.
	Duration Period{};

	// `C` — how much this output may spend per frame. Decision 34's cut point is computed against it at
	// commit time, in declared priority order, one frame before the frame is recorded.
	Duration Cost{};

	// What the task asked for, carried so that the comparison a caller wants is a field rather than a
	// second lookup into the request.
	Duration Want{};

	Duration Floor{};

	// The output was told to spend less than it asked for. Decision 34's tier step is what it spends
	// the difference as, and that step has to reach `Budget::Invalidate` or the output holds where it
	// is; see Budget.h's liveness obligation.
	[[nodiscard]] constexpr bool Reduced() const noexcept { return Cost < Want; }

	// Nothing further to give. Every rung is gone below this.
	[[nodiscard]] constexpr bool AtFloor() const noexcept { return Cost <= Floor; }

	// Rung 1 was applied to this output.
	[[nodiscard]] constexpr bool Lengthened(const OutputTask& task) const noexcept { return Period > task.Period; }

	friend constexpr bool operator==(Allocation, Allocation) noexcept = default;
};

namespace Detail
{
// The fixed point `U` is carried in. A power of two so the divisions the compiler can see are shifts,
// and large enough that a term rounded up costs less than a microsecond of a millisecond period.
inline constexpr std::int64_t UtilizationScale = 1 << 20;

// `U ≤ 0.95`. `L*` diverges as `U → 1`, and a set with no headroom for estimation error is not one to
// admit anyway — so the ceiling is a rejection before the interval loop runs rather than a bound on it.
inline constexpr std::int64_t UtilizationCeiling = (UtilizationScale * 19) / 20;

// SPEC: how many interval lengths one feasibility check will examine. `𝓛` is bounded by `L*` and by
// the hyperperiod, and on any configuration gyro has been pointed at it is a few hundred points; this
// is the guard against a pathological mix of periods turning a configuration change into a visible
// stall. Exhausting it answers *infeasible*, which is the direction that cannot produce a miss.
inline constexpr std::size_t MaxIntervals = 4096;

// The longest duration the test will reason about, which is what keeps every product below overflow
// without a saturating multiply in the inner loop. Ten seconds is a tenth of a hertz, well past any
// period a panel reports and any composite that is not already a hang.
inline constexpr Duration MaxSchedulable = std::chrono::seconds{ 10 };

[[nodiscard]] constexpr std::int64_t Nanoseconds(Duration value) noexcept
{
	return std::clamp(value, Duration::zero(), MaxSchedulable).count();
}

// Rounded up, for non-negative numerators and positive denominators. Every use of it is a term of `U`
// or of `L*`, and both round away from admitting a set that misses.
[[nodiscard]] constexpr std::int64_t CeilDiv(std::int64_t numerator, std::int64_t denominator) noexcept
{
	return (numerator + denominator - 1) / denominator;
}

// The admission set in the units the test is computed in, built once and then mutated by the ladder.
// It is a parallel-array working set rather than a span of `OutputTask` because the two things the
// rungs move — a period and a cost — are the only two the test reads, and copying them out is what
// lets a candidate be evaluated without disturbing the request it came from.
struct TaskSet
{
	std::array<std::int64_t, MaxOutputs> Period{};
	std::array<std::int64_t, MaxOutputs> Cost{};
	std::array<std::int64_t, MaxOutputs> Chunk{};
	std::size_t Count = 0;

	// An output with no period demands nothing and is not in the set. It still receives an allocation,
	// because a caller indexes the plan by the request and an absent row would be a hole.
	[[nodiscard]] constexpr bool Participates(std::size_t index) const noexcept
	{
		return Period[index] > 0 && Cost[index] > 0;
	}

	// `qₖ`, which is the whole composite unless the output is chunked, and never more than the
	// composite it is a chunk of.
	[[nodiscard]] constexpr std::int64_t Blocking(std::size_t index) const noexcept
	{
		return Chunk[index] > 0 ? std::min(Chunk[index], Cost[index]) : Cost[index];
	}
};

// `lcm` over the participating periods, stopped at `cap` rather than computed and compared.
//
// The hyperperiod is the other bound on `𝓛` and it almost never binds: real periods are measured in
// nanoseconds and two of them are coprime far more often than not, so the lcm is astronomical and `L*`
// is what caps the loop. It is here because decision 29 names it, and because the case where it does
// bind — nominal periods in a clean ratio, which is exactly what a test writes — is the case a reader
// checks the loop count of by hand.
[[nodiscard]] constexpr std::int64_t Hyperperiod(const TaskSet& set, std::int64_t cap) noexcept
{
	std::int64_t hyper = 1;

	for (std::size_t index = 0; index < set.Count; ++index)
	{
		if (!set.Participates(index))
		{
			continue;
		}

		const std::int64_t divisor = std::gcd(hyper, set.Period[index]);

		// Overflow is indistinguishable from *larger than the cap* for this purpose, and the cap is
		// what the caller will use either way.
		if (hyper > cap / (set.Period[index] / divisor))
		{
			return cap;
		}

		hyper = hyper / divisor * set.Period[index];
	}

	return std::min(hyper, cap);
}

// `U`, in `UtilizationScale` units, each term rounded up.
[[nodiscard]] constexpr std::int64_t Utilization(const TaskSet& set) noexcept
{
	std::int64_t utilization = 0;

	for (std::size_t index = 0; index < set.Count; ++index)
	{
		if (set.Participates(index))
		{
			utilization += CeilDiv(set.Cost[index] * UtilizationScale, set.Period[index]);
		}
	}

	return utilization;
}

// `h(L) + B(L) ≤ L`, for one interval length.
[[nodiscard]] constexpr bool Fits(const TaskSet& set, std::int64_t length) noexcept
{
	std::int64_t demand = 0;
	std::int64_t blocking = 0;

	for (std::size_t index = 0; index < set.Count; ++index)
	{
		if (!set.Participates(index))
		{
			continue;
		}

		if (set.Period[index] <= length)
		{
			demand += (length / set.Period[index]) * set.Cost[index];
		}
		else
		{
			blocking = std::max(blocking, set.Blocking(index));
		}
	}

	return demand + blocking <= length;
}

// Decision 29's test, whole.
//
// The interval loop walks each output's multiples rather than a merged ascending sequence, because the
// test is a conjunction and the order it is evaluated in changes nothing. Duplicates — a length that is
// a multiple of two periods — are checked twice, which costs a pass over a set of at most sixteen and
// saves the merge.
[[nodiscard]] constexpr bool Feasible(const TaskSet& set) noexcept
{
	const std::int64_t utilization = Utilization(set);

	if (utilization > UtilizationCeiling)
	{
		return false;
	}

	std::int64_t total = 0;

	for (std::size_t index = 0; index < set.Count; ++index)
	{
		if (set.Participates(index))
		{
			total += set.Cost[index];
		}
	}

	if (total == 0)
	{
		return true;
	}

	// `L* = Σ Cᵢ / (1 − U)`, the synchronous busy period, with the subtraction done in fixed point so
	// the denominator is the same rounding as the numerator. The ceiling above bounds it: `1 − U` is at
	// least a twentieth, so this is at most twenty times the total cost.
	const std::int64_t busy = CeilDiv(total * UtilizationScale, UtilizationScale - utilization);
	const std::int64_t limit = Hyperperiod(set, busy);

	std::size_t examined = 0;

	for (std::size_t index = 0; index < set.Count; ++index)
	{
		if (!set.Participates(index))
		{
			continue;
		}

		for (std::int64_t length = set.Period[index]; length <= limit; length += set.Period[index])
		{
			if (++examined > MaxIntervals)
			{
				return false;
			}

			if (!Fits(set, length))
			{
				return false;
			}
		}
	}

	return true;
}
} // namespace Detail

class Plan;

// Solve the set. Declared ahead of the plan it fills, which is the whole reason the plan can keep its
// fields private: one function authors a plan and everything else reads one.
[[nodiscard]] constexpr Plan Admit(std::span<const OutputTask> tasks) noexcept;

// What the frame loop is handed: one allocation per requested output, and how much was given up to
// produce it.
//
// Indexed by the request. An output that was not in the set — no period, or nothing to spend — still
// has a row, so a caller never has to reconcile two orderings.
class Plan
{
public:
	constexpr Plan() noexcept = default;

	[[nodiscard]] constexpr std::size_t Count() const noexcept { return m_Count; }

	[[nodiscard]] constexpr const Allocation& operator[](std::size_t index) const noexcept
	{
		return m_Allocations[index];
	}

	[[nodiscard]] constexpr std::span<const Allocation> Allocations() const noexcept
	{
		return { m_Allocations.data(), m_Count };
	}

	// False only when every output is at its floor composite and the set still does not fit — decision
	// 29's definition, and the one state where a frame is going to be dropped on purpose. It is not a
	// refusal: the allocations below are still the plan the loop runs.
	[[nodiscard]] constexpr bool IsFeasible() const noexcept { return m_Deepest != Rung::Drop; }

	// The deepest rung any output reached, which is the one number a log line about a configuration
	// change wants.
	[[nodiscard]] constexpr Rung Deepest() const noexcept { return m_Deepest; }

	// `U` for the admitted plan, in thousandths. A reported figure rather than one
	// anything decides against — the decision was taken in `UtilizationScale` units, and this is that
	// number in the form a person reads.
	[[nodiscard]] constexpr std::int64_t UtilizationPerMille() const noexcept { return m_Utilization; }

private:
	friend constexpr Plan Admit(std::span<const OutputTask>) noexcept;

	std::array<Allocation, MaxOutputs> m_Allocations{};
	std::size_t m_Count = 0;
	Rung m_Deepest = Rung::None;
	std::int64_t m_Utilization = 0;
};

namespace Detail
{
// The order outputs give ground in, which is Architecture.md#admission-control's rule verbatim: the
// one without input focus, ties broken toward the slower refresh. Stable in the index, so two
// identical outputs are not reordered by a rerun and a plan does not change under a caller that
// changed nothing.
//
// Insertion sort because the set is at most sixteen and nothing here may allocate a comparator's
// scratch — the same argument the frame loop's deadline ordering makes, one level up the schedule.
[[nodiscard]] constexpr std::size_t
VictimOrder(std::span<const OutputTask> tasks, std::array<std::uint8_t, MaxOutputs>& order) noexcept
{
	std::size_t count = 0;

	for (std::size_t index = 0; index < tasks.size(); ++index)
	{
		std::size_t position = count;

		while (position > 0)
		{
			const OutputTask& seated = tasks[order[position - 1]];

			// Focus outranks everything: the panel under someone's hands is last to lose a rung
			// whatever its refresh rate is. Below that, the slower output gives way first, because a
			// sixtieth of a second of staleness on a projector is close to invisible.
			const bool yieldsFirst =
				seated.Focused != tasks[index].Focused ? seated.Focused : seated.Period < tasks[index].Period;

			if (!yieldsFirst)
			{
				break;
			}

			order[position] = order[position - 1];
			--position;
		}

		order[position] = static_cast<std::uint8_t>(index);
		++count;
	}

	return count;
}

// Rung 1 is available on this output.
[[nodiscard]] constexpr bool CanLengthen(const OutputTask& task) noexcept
{
	return task.Refresh.Enabled && task.Authority && task.Refresh.Longest > task.Period && Nanoseconds(task.Period) > 0;
}

// The shortest period in `(current, longest]` that makes the whole set fit, or zero if none does.
//
// Binary search, and it is exact rather than a sampling of a continuous range: the periods are integer
// nanoseconds and `h(L) + B(L)` is weakly decreasing in any one period, so the feasible half is a
// suffix. That a term moving out of `h` and into `B` cannot make the sum worse is what makes it weakly
// decreasing — `⌊L / P⌋ · C ≥ C` for every `P ≤ L`, so the additive term it leaves behind is at least
// the maximum it joins.
//
// **Shortest rather than longest, and this is the one policy choice the documents do not settle.**
// Architecture.md calls rung 1 free because it costs no visual quality, which is true and is not the
// whole of what a person perceives — a panel servoed from 144 Hz to 48 Hz when 100 Hz would have done
// is motion given away for nothing. So the rung takes the smallest step that works.
[[nodiscard]] constexpr std::int64_t
ShortestFeasiblePeriod(TaskSet& set, std::size_t index, std::int64_t longest) noexcept
{
	const std::int64_t original = set.Period[index];
	std::int64_t low = original + 1;
	std::int64_t high = longest;
	std::int64_t found = 0;

	while (low <= high)
	{
		const std::int64_t middle = low + (high - low) / 2;

		set.Period[index] = middle;

		if (Feasible(set))
		{
			found = middle;
			high = middle - 1;
		}
		else
		{
			low = middle + 1;
		}
	}

	set.Period[index] = original;

	return found;
}

// The largest cost in `[floor, want]` that makes the whole set fit. Zero is never returned, because
// the floor is the bottom of the range and an infeasible floor is the caller's answer rather than this
// one's — it is the state that means decision 30's last rung.
//
// Monotone for the same reason the period search is, from the other side: reducing a cost weakly
// reduces every term of `h`, of `B`, and of `U` at once.
[[nodiscard]] constexpr std::int64_t
LargestFeasibleCost(TaskSet& set, std::size_t index, std::int64_t floor, std::int64_t want) noexcept
{
	const std::int64_t original = set.Cost[index];
	std::int64_t low = floor;
	std::int64_t high = want;
	std::int64_t found = floor;

	while (low <= high)
	{
		const std::int64_t middle = low + (high - low) / 2;

		set.Cost[index] = middle;

		if (Feasible(set))
		{
			found = middle;
			low = middle + 1;
		}
		else
		{
			high = middle - 1;
		}
	}

	set.Cost[index] = original;

	return found;
}
} // namespace Detail

// Never refuses; see the header.
//
// The ladder is walked in order and each rung stays applied when the next one runs, which is what
// *falls through to the next rung* means: a period already lengthened is a period the tier step does
// not have to pay for again. Rung 1 first because it is the only one that costs nothing, rung 2 taken
// from the output that can least afford it, and the terminal state — everyone at their floor and the
// set still over — reported rather than resolved, since the mechanism for it is decision 35's skip and
// that already exists.
[[nodiscard]] constexpr Plan Admit(std::span<const OutputTask> tasks) noexcept
{
	using namespace Detail;

	Plan plan;
	plan.m_Count = std::min(tasks.size(), MaxOutputs);
	tasks = tasks.first(plan.m_Count);

	TaskSet set;
	set.Count = plan.m_Count;

	for (std::size_t index = 0; index < set.Count; ++index)
	{
		// A want below the floor is not a smaller request, it is a request for less than the output is
		// guaranteed. The floor wins, and the set is then infeasible on its own terms if it must be.
		const std::int64_t floor = Nanoseconds(tasks[index].Floor);

		set.Period[index] = Nanoseconds(tasks[index].Period);
		set.Cost[index] = std::max(Nanoseconds(tasks[index].Want), floor);
		set.Chunk[index] = Nanoseconds(tasks[index].Chunk);

		plan.m_Allocations[index] = { .Period = Duration{ set.Period[index] },
			                          .Cost = Duration{ set.Cost[index] },
			                          .Want = Duration{ set.Cost[index] },
			                          .Floor = Duration{ floor } };
	}

	if (!Feasible(set))
	{
		// Rung 1. Every eligible output in shortest-period-first order, because that is where the
		// leverage is: at `L = P_fast` the fast output's demand appears at the highest multiplicity of
		// any term in the test. An output that cannot finish the job on its own is left at its longest
		// period and the next one is tried, so what rung 2 inherits is the most room rung 1 can make.
		std::array<std::uint8_t, MaxOutputs> byPeriod{};
		std::size_t eligible = 0;

		for (std::size_t index = 0; index < set.Count; ++index)
		{
			if (!CanLengthen(tasks[index]))
			{
				continue;
			}

			std::size_t position = eligible;

			while (position > 0 && set.Period[byPeriod[position - 1]] > set.Period[index])
			{
				byPeriod[position] = byPeriod[position - 1];
				--position;
			}

			byPeriod[position] = static_cast<std::uint8_t>(index);
			++eligible;
		}

		for (std::size_t position = 0; position < eligible && !Feasible(set); ++position)
		{
			const std::size_t index = byPeriod[position];
			const std::int64_t longest = Nanoseconds(tasks[index].Refresh.Longest);
			const std::int64_t shortest = ShortestFeasiblePeriod(set, index, longest);

			set.Period[index] = shortest > 0 ? shortest : longest;
			plan.m_Allocations[index].Period = Duration{ set.Period[index] };
			plan.m_Deepest = Rung::Period;
		}
	}

	if (!Feasible(set))
	{
		// Rung 2. The allocation comes down on the output that can least afford it, as far as it has to
		// and no further, and the next output is asked only when the previous one has reached its floor.
		std::array<std::uint8_t, MaxOutputs> order{};
		const std::size_t victims = VictimOrder(tasks, order);

		for (std::size_t position = 0; position < victims && !Feasible(set); ++position)
		{
			const std::size_t index = order[position];

			if (!set.Participates(index))
			{
				continue;
			}

			const std::int64_t floor = Nanoseconds(tasks[index].Floor);
			const std::int64_t want = set.Cost[index];

			if (floor >= want)
			{
				continue;
			}

			set.Cost[index] = LargestFeasibleCost(set, index, floor, want);
			plan.m_Allocations[index].Cost = Duration{ set.Cost[index] };
			plan.m_Deepest = Rung::Tier;
		}
	}

	if (!Feasible(set))
	{
		plan.m_Deepest = Rung::Drop;
	}

	// Rounded to nearest rather than up, and it is the one figure here that is. Every term of `U` was
	// already rounded up on the way in; rounding up again on the way out would report 0.401 for a set
	// that is exactly two fifths, which reads as a defect in a number nothing decides against.
	plan.m_Utilization = (Utilization(set) * 1000 + UtilizationScale / 2) / UtilizationScale;

	return plan;
}

// Convenience for the ordinary call, which has the tasks in an array on the stack.
template<std::size_t N>
[[nodiscard]] constexpr Plan Admit(const std::array<OutputTask, N>& tasks) noexcept
{
	return Admit(std::span<const OutputTask>{ tasks });
}

// Prints as plan tier U 0.883 [6944444ns 4000000ns/4000000ns] [16666666ns 2944444ns/5000000ns], which
// is the rung, the utilization, and one bracket per output giving the period the plan committed to,
// what that output may spend, and what it asked for.
template<>
struct std::formatter<Plan>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const Plan& plan, Context& context) const
	{
		auto out = std::format_to(
			context.out(),
			"plan {} U {}.{:03}",
			Name(plan.Deepest()),
			plan.UtilizationPerMille() / 1000,
			plan.UtilizationPerMille() % 1000
		);

		for (const Allocation& allocation : plan.Allocations())
		{
			out = std::format_to(out, " [{} {}/{}]", allocation.Period, allocation.Cost, allocation.Want);
		}

		return out;
	}
};

// The contract everything downstream assumes. The behaviour is swept in Admission.Test.cpp; what is
// here is the part a wrong answer changes silently.
static_assert(std::formattable<Plan, char>, "A report prints the plan rather than <unprintable>");

// The order is what a caller reduces over, so the deepest rung a plan reached is a maximum rather than
// a rule somebody remembers to write.
static_assert(std::max(Rung::None, Rung::Period) == Rung::Period);
static_assert(std::max(Rung::Tier, Rung::Drop) == Rung::Drop);

// An empty set is feasible and allocates nothing, which is what makes a plan before the first output
// an ordinary state rather than one the composition root sequences around.
static_assert(Admit(std::span<const OutputTask>{}).IsFeasible());
static_assert(Admit(std::span<const OutputTask>{}).Deepest() == Rung::None);

// One output that fits gets exactly what it asked for, and `U` is its own ratio.
static_assert(
	[] {
		const std::array tasks{ OutputTask{ .Period = std::chrono::milliseconds{ 10 },
		                                    .Want = std::chrono::milliseconds{ 4 },
		                                    .Floor = std::chrono::milliseconds{ 1 } } };
		const Plan plan = Admit(tasks);

		return plan.IsFeasible() && plan.Deepest() == Rung::None && plan[0].Cost == std::chrono::milliseconds{ 4 } &&
	           !plan[0].Reduced() && plan.UtilizationPerMille() == 400;
	}(),
	"A set that fits is allocated what it asked for"
);

// Architecture.md's headline configuration: a 4 ms composite on a 144 Hz panel beside a 60 Hz
// projector. Utilization is 0.88 and the set is non-preemptively infeasible, so the projector — no
// focus, slower refresh — is held to what fits under the panel's period rather than the 5 ms it wanted.
// `h(P_fast) + B(P_fast) = 4 + C_slow <= 6944444`.
static_assert(
	[] {
		const std::array tasks{
			OutputTask{ .Period = PeriodFromHertz(144.0),
		                .Want = std::chrono::milliseconds{ 4 },
		                .Floor = std::chrono::milliseconds{ 1 },
		                .Focused = true },
			OutputTask{ .Period = PeriodFromHertz(60.0),
		                .Want = std::chrono::milliseconds{ 5 },
		                .Floor = std::chrono::milliseconds{ 1 } },
		};
		const Plan plan = Admit(tasks);

		return plan.IsFeasible() && plan.Deepest() == Rung::Tier && plan[0].Cost == std::chrono::milliseconds{ 4 } &&
	           !plan[0].Reduced() && plan[1].Reduced() &&
	           plan[1].Cost == Duration{ PeriodFromHertz(144.0) - std::chrono::milliseconds{ 4 } };
	}(),
	"The projector's whole composite must fit in what the panel's period leaves"
);

// Rung 1 before rung 2, and it costs nothing: the same set with the panel variable-refresh and its
// cadence gyro's is rescued by lengthening the panel instead of taking a tier off the projector.
static_assert(
	[] {
		const std::array tasks{
			OutputTask{ .Period = PeriodFromHertz(144.0),
		                .Want = std::chrono::milliseconds{ 4 },
		                .Floor = std::chrono::milliseconds{ 1 },
		                .Refresh = { true, PeriodFromHertz(144.0), PeriodFromHertz(48.0) },
		                .Authority = true,
		                .Focused = true },
			OutputTask{ .Period = PeriodFromHertz(60.0),
		                .Want = std::chrono::milliseconds{ 5 },
		                .Floor = std::chrono::milliseconds{ 1 } },
		};
		const Plan plan = Admit(tasks);

		return plan.IsFeasible() && plan.Deepest() == Rung::Period && plan[0].Period > tasks[0].Period &&
	           plan[0].Period <= tasks[0].Refresh.Longest && !plan[0].Reduced() && !plan[1].Reduced();
	}(),
	"A variable-refresh period is the first thing spent and the only one that is free"
);

// A client holding the panel's refresh rate takes rung 1 away without changing anything else, which is
// decision 66's failure written as an assertion: the same configuration now costs the projector a tier.
static_assert(
	[] {
		std::array tasks{
			OutputTask{ .Period = PeriodFromHertz(144.0),
		                .Want = std::chrono::milliseconds{ 4 },
		                .Floor = std::chrono::milliseconds{ 1 },
		                .Refresh = { true, PeriodFromHertz(144.0), PeriodFromHertz(48.0) },
		                .Authority = false,
		                .Focused = true },
			OutputTask{ .Period = PeriodFromHertz(60.0),
		                .Want = std::chrono::milliseconds{ 5 },
		                .Floor = std::chrono::milliseconds{ 1 } },
		};

		return Admit(tasks).Deepest() == Rung::Tier;
	}(),
	"Cadence authority is an input to the test, and losing it costs a rung"
);

// Decision 29's three-display case, which is the whole reason the general form is needed: two 144 Hz
// panels at 3 ms beside a 60 Hz projector at 2 ms satisfies `C_fast + max(C_other) <= P_fast` and still
// misses, because the second panel completes at 8 ms against a 6.944 ms deadline.
static_assert(
	[] {
		const std::array tasks{
			OutputTask{ .Period = PeriodFromHertz(144.0),
		                .Want = std::chrono::milliseconds{ 3 },
		                .Floor = std::chrono::milliseconds{ 1 },
		                .Focused = true },
			OutputTask{ .Period = PeriodFromHertz(144.0),
		                .Want = std::chrono::milliseconds{ 3 },
		                .Floor = std::chrono::milliseconds{ 1 } },
			OutputTask{ .Period = PeriodFromHertz(60.0),
		                .Want = std::chrono::milliseconds{ 2 },
		                .Floor = std::chrono::milliseconds{ 1 } },
		};
		const Plan plan = Admit(tasks);

		return plan.Deepest() == Rung::Tier && plan[2].Reduced();
	}(),
	"Accumulated demand catches what a single fast job plus blocking does not"
);

// The terminal state, which is decision 29's software-rendering case: every output at its floor and the
// set still over. Reported rather than refused, and the allocations are still the plan the loop runs.
static_assert(
	[] {
		const std::array tasks{
			OutputTask{ .Period = std::chrono::milliseconds{ 10 },
		                .Want = std::chrono::milliseconds{ 8 },
		                .Floor = std::chrono::milliseconds{ 8 } },
			OutputTask{ .Period = std::chrono::milliseconds{ 10 },
		                .Want = std::chrono::milliseconds{ 8 },
		                .Floor = std::chrono::milliseconds{ 8 } },
		};
		const Plan plan = Admit(tasks);

		return !plan.IsFeasible() && plan.Deepest() == Rung::Drop && plan.Count() == 2 &&
	           plan[0].Cost == std::chrono::milliseconds{ 8 } && plan[1].AtFloor();
	}(),
	"Infeasible is a degraded plan and never a refusal"
);
