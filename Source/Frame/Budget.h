#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>

#include "Core/Time.h"
#include "Seam/Renderer.h"
// See Docs/Architecture.md#budgets, Docs/Architecture.md#the-floor-tier, and decisions 29, 34, and 35.
//
// The Seam edge is one enum. `RenderMode` was declared here while there was nowhere better and now
// lives with the interface that is told which composite to draw, which is where the observations
// below come from: a renderer reports its record cost and its resolved GPU costs, and this is what
// files them.

// What an output's frames have been costing, which is the other half of the pair the frame loop
// schedules with.
//
// `FrameClock` answers *when* a frame is owed and refuses to hold anything about gyro's own cost,
// naming the loop as the holder — `WakeupAt(sequence, reserve)` and `SequenceAfter(now + C)` both take
// a quantity from here. This is that quantity, and the whole of decision 35's runtime timing check is
// the two objects read together:
//
//     now + C_planned <= deadline   ->  render the planned tier
//     now + C_min     <= deadline   ->  render the floor tier
//     otherwise                     ->  skip this output's frame
//
// **The two devices are two figures and are never summed here.** A single `C` is recoverable from the
// pair and the pair is not recoverable from it, and every axis the frame loop composes along composes
// them differently: within one output a frame records then executes and they add; under chunking
// execution starts before recording finishes and they overlap; across outputs the GPU figures add on
// one queue while the CPU figures add on one thread, and the two never add to each other. The sum is
// correct only for one output, unchunked, which is the configuration gyro is built first and the one
// it will not stay at. So the composition belongs to the schedule, which knows those things, rather
// than to the record, which knows none of them.
//
// **There is a third figure on the CPU device, and what separates it is that no mode reduces it.**
// Decision 94. The frame thread walks the published scene to build the draw list, and it walks the
// same scene whichever composite follows — decision 35's floor tier is a cheaper shader over the
// same items rather than a smaller scene — so the walk sits outside the pair above and is added to
// both of them. Folding it into `FloorCpu` instead would make the floor target scene-dependent,
// which is the one thing that target cannot be: it is set at configuration change, and the session
// acquires windows afterwards. It is measured rather than policy for the reason the planned pair is,
// and it varies along a different axis from either of them — pixels and effects size those, and how
// many nodes exist sizes this.
//
// **`C_planned` is measured and `C_min` is policy, and that asymmetry is the type's shape.**
// Architecture.md#the-floor-tier states it directly: the floor composite's cost is a design target
// rather than a residue, because it is the size of the shock decision 35 can absorb. So it is a figure
// the floor composite is *built to meet*, and floor-tier frames exist here to falsify it rather than
// to produce it. The accessors are named for that — `FloorCpu()` is the target the record-time check
// reads, `MeasuredFloorCpu()` is what a floor frame actually did.
//
// **Which is also why measuring it costs nothing.** Obtaining `C_min` by observation would mean
// rendering a floor composite nothing asked for, since on a healthy machine the second branch never
// fires. Verifying a target does not: the per-machine number comes from the startup capability probe,
// which already runs the real pass chain off the frame path, and the samples that arrive here are from
// composites that were running regardless. Decision 62 forecloses the cheaper-sounding route —
// fusion folds pointwise effects into the composite pipeline, so a planned frame contains no
// separable base-composite span to subtract the effect passes from, and the floor composite is a
// different shader rather than a prefix.
//
// **A floor frame's measurement never sizes the planned mark.** They describe different work, and
// mixing the populations fails in the direction that compounds: a system in trouble renders the floor
// tier often, which would dilute the planned mark's window with frames that did no planned work, which
// makes admission optimistic exactly when the cascade decision 35's third branch exists to stop is
// already running.
//
// **A mark is a maximum over a window, not a percentile and not a running maximum.**
// Architecture.md#budgets rejects the percentile: drift guarantees every phase relationship eventually
// occurs, so the one frame in a hundred a p99 admits over budget eventually lands on the critical
// instant, and the figure looks healthy in aggregate while missing periodically forever. The window is
// what the running maximum lacks — without it the worst moment the machine has ever had is reserved
// for as long as the machine runs, and a budget that only rises is one that never recovers from a
// thermal event.
//
// **There is one liveness obligation on the caller, and it is load-bearing rather than tidy.** A
// planned mark comes down only when planned frames push samples through the window, and an output
// whose mark is too high to admit a planned frame renders the floor tier instead — which files
// nothing, leaves the mark where it was, and does the same next frame. Decision 35's answer to a
// systematic overrun is that it steps the quality tier down (decision 34), and **that step has to
// reach `Invalidate()`** or the output holds at the floor tier for as long as it runs. The floor tier
// is the terminal case and needs no escape, since there is nothing below it to be held out of.

// The numbers this record needs and nobody has measured, `// SPEC:` throughout for the reason
// `FrameClockPolicy` is. See Open.md, *`C_min` as a number*, *the capability probe*, and *scheduling
// policy constants*.
//
// The safety margin is deliberately not here. It is a margin on gyro's own wakeups rather than a
// measurement of its work, so it belongs to the timing policy that composes these figures; a copy in
// the record would be a number with no reader on the side that writes everything else in the object.
struct BudgetPolicy
{
	// `C_min`, split the way every cost here is split, because the second branch of the record-time
	// check has to compose exactly as the first does.
	//
	// Zero means *not yet chosen* rather than *free*, and both of its consequences are the recoverable
	// direction: the floor branch becomes maximally permissive, so an unmeasured floor admits a frame
	// it may miss rather than skipping one it could have made, and an unchosen target cannot be
	// exceeded, so nothing reports a defect against a figure nobody set.
	Duration FloorCpu{};
	Duration FloorGpu{};

	// What the planned marks hold before any frame has been measured. The capability probe runs the
	// real pass chain at startup and is what will eventually supply these.
	//
	// **Zero is optimistic on purpose.** The seed is a sample rather than a floor under one, so a
	// pessimistic seed reserves time no frame has asked for for a whole window, during which the
	// record-time check may hold the output at the floor tier. Decision 35 prices the other direction
	// at exactly one frame, which is the cheaper mistake by a window's worth of frames.
	Duration InitialCpu{};
	Duration InitialGpu{};

	// What the irreducible mark holds before any frame has been measured, on the terms the pair above
	// is seeded on and optimistic for the same reason. This is the one of the three whose figure
	// belongs to the *scene* rather than to the machine, so a probe can only seed the empty session
	// and everything after that is measurement.
	Duration InitialIrreducibleCpu{};

	// How many measurements a mark is the maximum over. Long enough that a scene which is expensive
	// every second frame is not forgotten between occurrences, short enough that a thermal event ends
	// when the thermals do. Clamped into `[1, WindowCapacity]` on use.
	std::int64_t Window = 256;
};

namespace Detail
{
// The window's capacity, which bounds the storage rather than choosing the length. Two of these per
// output is four kilobytes, which is worth spending to keep the length a number a test can vary.
inline constexpr std::size_t WindowCapacity = 256;

// The maximum over the last N measurements.
//
// **Eviction is the only case that costs more than two comparisons, and it is bounded rather than
// amortised.** A sample leaving the window matters only when it was the maximum, and then the
// replacement has to be looked for — O(N) on the frame where the worst measurement in a window ages
// out, O(1) on every other. Decision 36 asks for a worst case rather than an average, and a structure
// with an O(1) amortised push has the same O(N) worst case as this one for a good deal more code.
class HighWater
{
public:
	constexpr HighWater() noexcept = default;

	constexpr explicit HighWater(std::int64_t length) noexcept
		: m_Length{
			  static_cast<std::size_t>(std::clamp<std::int64_t>(length, 1, static_cast<std::int64_t>(WindowCapacity)))
		  }
	{}

	// Whether the mark moved, in either direction. Both directions are a configuration change: more
	// room repacks the schedule as surely as less does.
	constexpr bool Push(Duration sample) noexcept
	{
		const Duration before = m_Mark;
		bool evictedTheMark = false;

		if (m_Count == m_Length)
		{
			evictedTheMark = m_Samples[m_Head] == m_Mark;
			m_Samples[m_Head] = sample;
			m_Head = Offset(1);
		}
		else
		{
			m_Samples[Offset(m_Count)] = sample;
			++m_Count;
		}

		if (sample >= m_Mark)
		{
			m_Mark = sample;
		}
		else if (evictedTheMark)
		{
			// The rescan is also what makes a duplicated maximum survive its own eviction, which is why
			// it is unconditional rather than guarded on the mark being unique.
			m_Mark = Duration::zero();

			for (std::size_t index = 0; index < m_Count; ++index)
			{
				m_Mark = std::max(m_Mark, m_Samples[Offset(index)]);
			}
		}

		return m_Mark != before;
	}

	constexpr void Clear() noexcept
	{
		m_Head = 0;
		m_Count = 0;
		m_Mark = Duration::zero();
	}

	[[nodiscard]] constexpr Duration Mark() const noexcept { return m_Mark; }

	[[nodiscard]] constexpr std::size_t Length() const noexcept { return m_Length; }

private:
	// The ring index `offset` places past the head. Both terms are less than the length, so the wrap is
	// one conditional subtraction rather than a division.
	[[nodiscard]] constexpr std::size_t Offset(std::size_t offset) const noexcept
	{
		const std::size_t index = m_Head + offset;

		return index >= m_Length ? index - m_Length : index;
	}

	std::array<Duration, WindowCapacity> m_Samples{};
	std::size_t m_Head = 0;
	std::size_t m_Count = 0;
	std::size_t m_Length = WindowCapacity;

	// Zero rather than saturated, which is the opposite of what `FrameClock` starts at and is the
	// recoverable direction for the same reason its choice was: a clock that owes no frame arms no
	// timer and costs nothing, while a budget that reserves everything renders nothing and — since
	// only a planned frame can bring a mark down — would have no way back.
	Duration m_Mark = Duration::zero();
};
} // namespace Detail

class Budget
{
public:
	// The longest window a policy can ask for, exposed because a caller choosing one wants to know what
	// bounds it.
	static constexpr std::size_t WindowCapacity = Detail::WindowCapacity;

	constexpr Budget() noexcept : Budget{ BudgetPolicy{} } {}

	constexpr explicit Budget(BudgetPolicy policy) noexcept
		: m_Policy{ policy }, m_Cpu{ policy.Window }, m_Gpu{ policy.Window }, m_Irreducible{ policy.Window }
	{
		m_Policy.FloorCpu = NonNegative(m_Policy.FloorCpu);
		m_Policy.FloorGpu = NonNegative(m_Policy.FloorGpu);
		m_Policy.InitialCpu = NonNegative(m_Policy.InitialCpu);
		m_Policy.InitialGpu = NonNegative(m_Policy.InitialGpu);
		m_Policy.InitialIrreducibleCpu = NonNegative(m_Policy.InitialIrreducibleCpu);

		Seed();
	}

	// A frame's CPU record time, filed the moment recording ends.
	//
	// **No generation, where the GPU half takes one**, and the asymmetry is the latency rather than an
	// oversight: this measurement exists on the frame thread before the frame is submitted and is filed
	// before the iteration that produced it returns, so there is no interval in which a reconfiguration
	// can arrive between the work and its cost.
	constexpr bool ObserveCpu(RenderMode mode, Duration cost) noexcept
	{
		return File(mode, NonNegative(cost), m_Cpu, m_MeasuredFloorCpu);
	}

	// A frame's GPU execution time, from timestamps resolved some frames after the submission that
	// wrote them.
	//
	// **The generation is what makes a late sample safe.** The query cannot be read back before the
	// next frame is submitted without stalling the thread that must not stall, so a measurement
	// outlives the configuration it was taken under — and a 4K frame's execution filed into a 1080p
	// output's mark reserves time nothing will ask for, for a whole window. The submitter stamps its
	// query with `Generation()` and hands that back here; anything from a superseded one is dropped.
	//
	// Within a generation nothing is policed. Samples arriving out of order, or the same one arriving
	// twice, cannot change a maximum, and a rule against them would be a rule whose only effect is to
	// discard evidence.
	constexpr bool ObserveGpu(RenderMode mode, std::uint32_t generation, Duration cost) noexcept
	{
		if (generation != m_Generation)
		{
			return false;
		}

		return File(mode, NonNegative(cost), m_Gpu, m_MeasuredFloorGpu);
	}

	// What producing this frame's draw list cost, before any composite was recorded.
	//
	// **No mode, and the absence is the whole of decision 94.** A cost that takes one is a cost the
	// ladder can step down, and this one cannot be: the same tree is walked and the same springs are
	// evaluated whichever composite the verdict named. Filing it against a mode would split one
	// population in two and leave each half describing work both halves did.
	//
	// No generation either, for `ObserveCpu`'s reason exactly — it is measured and filed inside the
	// iteration that produced it, so nothing can arrive between the work and its cost.
	constexpr bool ObserveIrreducibleCpu(Duration cost) noexcept { return m_Irreducible.Push(NonNegative(cost)); }

	// The cost record describes a world that no longer exists: a mode set, a device migration, a
	// quality tier step. See the liveness obligation above — the tier step is the one that has to reach
	// here, and it is the only caller whose omission is silent.
	constexpr void Invalidate() noexcept
	{
		++m_Generation;

		m_Cpu.Clear();
		m_Gpu.Clear();
		m_Irreducible.Clear();
		m_MeasuredFloorCpu = Duration::zero();
		m_MeasuredFloorGpu = Duration::zero();

		Seed();
	}

	// What an in-flight GPU query is stamped with. See `ObserveGpu`.
	[[nodiscard]] constexpr std::uint32_t Generation() const noexcept { return m_Generation; }

	// `C_planned`, measured, one figure per device. There is deliberately no accessor that sums them;
	// see the note above for the three ways that sum is wrong.
	[[nodiscard]] constexpr Duration PlannedCpu() const noexcept { return m_Cpu.Mark(); }

	[[nodiscard]] constexpr Duration PlannedGpu() const noexcept { return m_Gpu.Mark(); }

	// What a frame costs before it costs anything a tier can take away. Frame/Timing.h adds it to
	// whichever of the two pairs below the verdict is composing, which is what makes it irreducible in
	// the only sense the schedule cares about.
	[[nodiscard]] constexpr Duration IrreducibleCpu() const noexcept { return m_Irreducible.Mark(); }

	// `C_min`, which is policy where the pair above is measurement. The record-time check's second
	// branch reads these exactly as its first branch reads those, so the timing policy composes one
	// pair the way it composes the other and never has to know which of them was measured.
	[[nodiscard]] constexpr Duration FloorCpu() const noexcept { return m_Policy.FloorCpu; }

	[[nodiscard]] constexpr Duration FloorGpu() const noexcept { return m_Policy.FloorGpu; }

	// The worst floor-tier frame since the last invalidation. Falsification rather than sizing, and it
	// accumulates differently for that reason: unwindowed, because a figure whose job is to contradict
	// a target must not forget the contradiction, where a figure whose job is to size a reservation
	// must.
	[[nodiscard]] constexpr Duration MeasuredFloorCpu() const noexcept { return m_MeasuredFloorCpu; }

	[[nodiscard]] constexpr Duration MeasuredFloorGpu() const noexcept { return m_MeasuredFloorGpu; }

	// The floor composite did not meet the target it is built to. A defect rather than a condition to
	// handle — decision 35's second promise is conditional on this being false, and nothing downstream
	// has a smaller composite to fall back to.
	[[nodiscard]] constexpr bool FloorExceedsTarget() const noexcept
	{
		return (m_Policy.FloorCpu > Duration::zero() && m_MeasuredFloorCpu > m_Policy.FloorCpu) ||
		       (m_Policy.FloorGpu > Duration::zero() && m_MeasuredFloorGpu > m_Policy.FloorGpu);
	}

	// How many measurements a mark is the maximum over, after clamping.
	[[nodiscard]] constexpr std::size_t Window() const noexcept { return m_Cpu.Length(); }

private:
	// A measurement that came out negative is a broken timestamp rather than a fast frame, and the
	// direction it breaks in is the one that matters: a negative reservation admits a frame with less
	// time than it takes.
	[[nodiscard]] static constexpr Duration NonNegative(Duration cost) noexcept
	{
		return std::max(cost, Duration::zero());
	}

	// The return says *the figure admission control sizes with has changed*, which is why a floor
	// observation answers false however far over the target it lands. It moves no reservation; what it
	// moves is `FloorExceedsTarget`, and a caller or-folding this across a drain wants the two
	// questions kept apart.
	constexpr bool File(RenderMode mode, Duration cost, Detail::HighWater& planned, Duration& floor) noexcept
	{
		if (mode == RenderMode::Floor)
		{
			floor = std::max(floor, cost);

			return false;
		}

		return planned.Push(cost);
	}

	// The seed goes in as a sample rather than as a floor under one, which is what gives it a life of
	// exactly one window: it ages out the moment there is a window's worth of evidence, and until then
	// it is the only evidence there is.
	constexpr void Seed() noexcept
	{
		m_Cpu.Push(m_Policy.InitialCpu);
		m_Gpu.Push(m_Policy.InitialGpu);
		m_Irreducible.Push(m_Policy.InitialIrreducibleCpu);
	}

	BudgetPolicy m_Policy{};

	Detail::HighWater m_Cpu{};
	Detail::HighWater m_Gpu{};
	Detail::HighWater m_Irreducible{};

	Duration m_MeasuredFloorCpu{};
	Duration m_MeasuredFloorGpu{};

	// Wraps, and wrapping is harmless: what it protects against is a sample from a superseded
	// configuration, and a sample cannot be four billion invalidations in flight.
	std::uint32_t m_Generation = 0;
};

// Prints as budget gen 3 planned cpu 2000000ns gpu 5000000ns floor cpu 1000000ns gpu 2000000ns
// irreducible cpu 300000ns, with the floor's measured pair appended only when it contradicts the
// target it is printed beside.
template<>
struct std::formatter<Budget>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const Budget& budget, Context& context) const
	{
		auto out = std::format_to(
			context.out(),
			"budget gen {} planned cpu {} gpu {} floor cpu {} gpu {} irreducible cpu {}",
			budget.Generation(),
			budget.PlannedCpu(),
			budget.PlannedGpu(),
			budget.FloorCpu(),
			budget.FloorGpu(),
			budget.IrreducibleCpu()
		);

		if (budget.FloorExceedsTarget())
		{
			out =
				std::format_to(out, " over-floor cpu {} gpu {}", budget.MeasuredFloorCpu(), budget.MeasuredFloorGpu());
		}

		return out;
	}
};

// The contract everything downstream assumes. The behaviour is swept in Budget.Test.cpp; what is here
// is the part a wrong answer changes silently.
static_assert(std::formattable<Budget, char>, "A report prints the budget rather than <unprintable>");

// A budget nobody has configured reserves nothing and contradicts nothing, which is what makes
// construction before the first frame an ordinary state rather than one the composition root has to
// sequence around.
static_assert(Budget{}.PlannedCpu() == Duration::zero());
static_assert(Budget{}.PlannedGpu() == Duration::zero());
static_assert(Budget{}.FloorCpu() == Duration::zero());
static_assert(Budget{}.IrreducibleCpu() == Duration::zero(), "A scene nobody has walked costs nothing to walk");
static_assert(!Budget{}.FloorExceedsTarget());

// The mark is the maximum over the window, and the window is what brings it back down.
static_assert(
	[] {
		Budget budget{ BudgetPolicy{ .Window = 2 } };

		const bool rose = budget.ObserveCpu(RenderMode::Planned, std::chrono::milliseconds{ 4 });
		const bool held = budget.ObserveCpu(RenderMode::Planned, std::chrono::milliseconds{ 1 });
		const bool fell = budget.ObserveCpu(RenderMode::Planned, std::chrono::milliseconds{ 1 });

		return rose && !held && fell && budget.PlannedCpu() == std::chrono::milliseconds{ 1 };
	}(),
	"A mark rises on the sample and falls when that sample leaves the window"
);

// The failure this type exists to prevent: a floor frame, filed while the system is already in
// trouble, quietly shrinking the reservation the planned tier is admitted against.
static_assert(
	[] {
		Budget budget;
		budget.ObserveCpu(RenderMode::Planned, std::chrono::milliseconds{ 8 });
		budget.ObserveCpu(RenderMode::Floor, std::chrono::milliseconds{ 1 });

		return budget.PlannedCpu() == std::chrono::milliseconds{ 8 } &&
	           budget.MeasuredFloorCpu() == std::chrono::milliseconds{ 1 };
	}(),
	"A floor frame falsifies the target and never sizes the planned mark"
);

// A GPU sample outliving its configuration, which is the whole reason the generation exists.
static_assert(
	[] {
		Budget budget;
		const std::uint32_t stamped = budget.Generation();
		budget.Invalidate();

		return !budget.ObserveGpu(RenderMode::Planned, stamped, std::chrono::milliseconds{ 9 }) &&
	           budget.PlannedGpu() == Duration::zero();
	}(),
	"A measurement from a superseded configuration reserves nothing"
);
