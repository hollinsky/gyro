#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>

#include "Core/Time.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/PresentationInfo.h"

// The prediction every deadline in the system is derived from, one per output.
//
// Decision 28 gives each output a clock of its own and refuses one for the machine: a frame is
// `(output, predicted presentation time)`, and because animation is closed form the same scene is
// evaluated at two of those in a single iteration and both answers are exact. This is the object
// that supplies the second half of that pair, and it is the only thing in the system that turns a
// page-flip timestamp into an instant anything else is allowed to schedule against.
//
// **It is keyed by sequence and not by now.** Its entire state is an anchor — frame `S` reached the
// glass at `T`, and the output is running at period `P` — so every question it answers is arithmetic
// on that anchor. The next frame is `S + 1` at `T + P`; the frame speculative early rendering wants
// is `S + k`. None of it needs a clock, which is decision 36's discipline applied one level below
// the render path: not merely that animation is never evaluated against an ambient now, but that the
// thing computing the deadlines does not read one either.
//
// **`SequenceAfter` is the one question that cannot be answered that way, and it is the recovery
// path.** When the anchor is several periods old — the output was idle, or a stall ate frames —
// which frame is next depends on an instant, and that is decision 35's `ceil(overrun / P)` rather
// than an incidental. Refusing the question does not remove it: a loop coming out of idle would ask
// for `S + 1`, be handed a deadline in the deep past, fail both timing branches, skip, ask for
// `S + 2`, and skip forever. So the instant enters here, once, at the call where it means something.
// The frame loop already reads `Now()` once per iteration and hands it to whoever is entitled to it
// (Core/Clock.h), and `SequenceAfter(now + C)` reads as *the frame I can still make if I finish
// then* — which is the timing policy's own question with the clock's arithmetic behind it.
//
// **Everything this clock knows, it learned from the output.** Observations arrive from the backend
// through `Observe`, the nominal period and the variable-refresh range from the configuration a
// reconfiguration achieved, and nothing here is a fact about gyro. That is why `WakeupAt` takes the
// reserve rather than holding it: `renderBudget` is a high-water mark of gyro's own CPU and GPU time
// and `safety` is a margin on gyro's own wakeups, both of which the loop already holds for decision
// 35's record-time check — `now + C_planned <= deadline`, else `now + C_min <= deadline`, else skip.
// A copy here would be a second place the same number can be wrong, written by admission control
// while the backend writes everything else in the object, and the two would disagree exactly when a
// budget moved. Architecture.md's sketch spelled `NextWakeup()` with no arguments and the phrase it
// is quoted with — *per-output budget as real data* — says where that data lives rather than that
// the clock is the one holding it.
//
// **A deadline is a presentation minus the latch lead.** The two are separate questions because the
// hardware makes them separate: a commit has to be programmed some way before the vblank that
// latches it, and how far before is the driver's business rather than gyro's. Nested and headless
// have no such requirement and pass zero, which makes the two instants equal there and keeps the
// distinction from being something a backend has to opt into. SPEC: the figure for KMS wants
// measuring against a real driver; the seam is here so that measuring it changes one number.
//
// **Three periods, and conflating any two of them is a servo reporting a rate nothing achieved.**
// `Period()` is what the panel did, `CommandedPeriod()` is what gyro is presently asking it for, and
// the target is where the servo is heading. They differ throughout every ramp and permanently on a
// panel that will not comply. Prediction takes the commanded one on a variable-refresh output and
// the observed one on a fixed one, because on a variable panel the vblank happens *when gyro
// submits* — decision 31's reading of both drivers is that flip cadence is not one lever among
// several but the only one there is — while on a fixed panel the vblank happens when the panel says
// and gyro's opinion is not an input.
//
// **The servo converges inside `Observe`, and that is where it belongs rather than where it fits.**
// `Command` states a target; each observation moves the commanded period toward it by a bounded step,
// clamped into the range the panel reported and held clear of the bottom so that low-framerate
// compensation never engages underneath it. Decision 31 requires the loop to close on an observation
// rather than an acknowledgement, because panels misreport their ranges — and an observation is the
// only moment a new measurement exists, so it is the only moment a step is owed. A `Command` that
// took the per-frame value instead would need its caller to read the period back and step it every
// single frame, which is the same arithmetic in a place that silently stops converging the first time
// somebody does not call it.
//
// **An invalid clock answers `Unscheduled`, and the value is chosen rather than convenient.** The
// failure this type exists to prevent is a stale anchor answering with an instant in the deep past,
// which arms nothing and fails every branch of the timing policy forever. Saturating the other way
// inverts both halves into the behaviour the design already asks for: nothing is ever due, so no
// timer is armed and an idle output costs nothing, while any frame damage demands passes the
// record-time check immediately and presents as soon as it is ready. That is decision 31's account
// of the first frame after an idle VRR output verbatim, and it falls out of the sentinel rather than
// needing a mode.
//
// **There is no filter over the observed period, deliberately.** Every observation re-anchors, so
// prediction error is bounded by one period of extrapolation rather than by however long ago the
// estimate was formed, and a filter would buy accuracy in a term that is already re-measured every
// frame at the cost of lag in the one quantity the servo closes its loop on. What is here instead is
// a fallback ladder for a backend that does not report a period at all: difference the anchor, which
// divides by the sequence advance and is therefore right across skipped frames, and fall back to the
// configured nominal when even that is unavailable.
//
// See Docs/Architecture.md#the-frame-clock and decisions 28, 31, 35, and 57.

// The numbers this clock needs and nobody has measured.
//
// Grouped by provenance rather than by kind, which is worth saying because they are not the same kind
// of thing: the lead is a hardware fact, and the other two are policy about what a panel and a person
// will tolerate. What they share is that all three are `// SPEC:` — read from documentation and
// driver source rather than from a panel — so all three want to be data a test can vary rather than
// constants compiled into the arithmetic. See Open.md, *scheduling policy constants*.
struct FrameClockPolicy
{
	// How far clear of the longest period the panel admits the servo stays, so that low-framerate
	// compensation never engages underneath a period gyro commanded.
	Duration RangeClearance{};

	// The bound on one convergence step, as a divisor of the current commanded period — sixteen is
	// a little over six percent per presented frame, which walks a 144 Hz panel to 100 Hz in about
	// eight frames. A fraction rather than an absolute so the bound means the same thing on a 60 Hz
	// panel as on a 240 Hz one, since both the flicker and the perceptibility it answers to are
	// relative. Clamped to at least one on use.
	std::int64_t ServoStepDivisor = 16;
};

namespace Detail
{
// `period * count`, saturating, for a period that is strictly positive.
//
// The bare multiplication is undefined on overflow and the caller naming the sequence is speculative
// early rendering, which asks about frames that have not happened — so an absurd sequence is a
// question to answer badly rather than a precondition to assume. Saturating lands on `Unscheduled`,
// which is the answer that arms nothing.
[[nodiscard]] constexpr Duration Scaled(Duration period, std::int64_t count) noexcept
{
	const std::int64_t unit = period.count();

	if (unit <= 0 || count == 0)
	{
		return Duration::zero();
	}

	// Magnitude first and unsigned, so that the negative extreme has a representation. Negating
	// int64 min is the undefined behaviour this exists to avoid rather than an edge case it happens
	// to survive.
	const bool negative = count < 0;
	const std::uint64_t magnitude =
		negative ? std::uint64_t{ 0 } - static_cast<std::uint64_t>(count) : static_cast<std::uint64_t>(count);

	const std::uint64_t limit =
		static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / static_cast<std::uint64_t>(unit);

	if (magnitude > limit)
	{
		return negative ? Duration::min() : Duration::max();
	}

	const std::int64_t scaled = static_cast<std::int64_t>(magnitude * static_cast<std::uint64_t>(unit));

	return Duration{ negative ? -scaled : scaled };
}

// Ceiling division for a strictly positive divisor. Integer division truncates toward zero, which is
// already the ceiling for a non-positive numerator, so only the positive case adjusts — and it cannot
// overflow, because a divisor of one leaves no remainder to adjust for.
[[nodiscard]] constexpr std::int64_t CeilDivide(std::int64_t numerator, std::int64_t divisor) noexcept
{
	const std::int64_t quotient = numerator / divisor;

	return (numerator > 0 && numerator % divisor != 0) ? quotient + 1 : quotient;
}
} // namespace Detail

class FrameClock
{
public:
	// What every instant-shaped question answers while the clock names no frame. See the note above:
	// saturating forward arms no timer and admits a damage-driven frame, and saturating backward would
	// do the opposite of both.
	static constexpr Instant Unscheduled{ Duration::max() };

	// The sequence answered by a clock that names no frame. Unambiguous rather than merely reserved:
	// every answer `SequenceAfter` gives is strictly after an anchor, so a valid one is never zero.
	static constexpr std::uint64_t NoSequence = 0;

	constexpr FrameClock() noexcept = default;

	constexpr explicit FrameClock(FrameClockPolicy policy) noexcept : m_Policy{ policy }
	{
		m_Policy.RangeClearance = std::max(m_Policy.RangeClearance, Duration::zero());
		m_Policy.ServoStepDivisor = std::max<std::int64_t>(m_Policy.ServoStepDivisor, 1);
	}

	// What the output is programmed to be, from the configuration a reconfiguration *achieved* — the
	// mode's nominal period, and the variable-refresh window that decision 31 records can only be
	// learned and never asked for. Wired to `IPresenter::Reconfigured`.
	//
	// **It invalidates, unconditionally.** A mode set is one of the three events that make the last
	// observation describe a world that no longer exists, and a configuration achieved is exactly
	// that event reporting itself. Comparing fields to decide whether this one really counted would
	// buy one frame of prediction and would be wrong the first time a backend reported a field this
	// clock does not read.
	constexpr void Configure(const OutputConfiguration& configuration) noexcept
	{
		const Duration nominal = Positive(configuration.Period);

		// **Read here rather than held as policy, because it is a fact about this output.** It is the
		// panel's blanking interval plus the driver's commit path, so it changes when the mode does and
		// two outputs on one machine do not have to share one — which a policy field could not express
		// and which measurement says they do not: 303 us of blanking on one mode against 800 on another
		// of the same panel.
		m_LatchLead = std::max(configuration.LatchLead, Duration::zero());

		m_Range = configuration.Refresh;
		m_Observed = nominal;
		m_Commanded = ClampToRange(nominal);
		m_Target = m_Commanded;

		Invalidate();
	}

	// A frame reached the glass. Re-anchors, re-measures, and advances the servo one bounded step.
	// Wired to `IPresenter::Presented`.
	constexpr void Observe(const PresentationInfo& info) noexcept
	{
		const Duration measured = Measure(info);

		if (measured > Duration::zero())
		{
			m_Observed = measured;
		}

		m_AnchoredAt = info.PresentedAt;
		m_Sequence = info.Sequence;
		m_Precise = info.HardwareClock;
		m_Valid = true;

		Converge();
	}

	// The anchor described a world that no longer exists: a resume, a mode set, a device migration, a
	// variable-refresh output going idle.
	//
	// **The anchor goes and the rate estimate stays**, which is the split that matters. What a resume
	// falsifies is *where in the cadence the output is*, not how fast it runs, and admission control
	// still needs a `P` for an output that has not presented since. The variable case is the one that
	// looks like an exception and is not: an idle VRR panel has genuinely changed period, so its last
	// observation is wrong rather than stale — but prediction on a variable output reads the commanded
	// period rather than the observed one, so dropping the anchor is already the whole of the repair.
	constexpr void Invalidate() noexcept
	{
		m_Valid = false;
		m_Precise = false;
		m_AnchoredAt = Instant{};
		m_Sequence = NoSequence;
	}

	// The period gyro asks the panel for, as a target the servo converges on rather than a value it
	// takes. A no-op on a fixed-refresh output, where cadence is not gyro's to set and a stored request
	// would be a number with no reader — and equally on a variable one whose reported window is
	// nonsense, where the safe reading is that rung 1 is unavailable and admission control falls
	// through to the quality tier, rather than that the servo may command anything it likes.
	constexpr void Command(Duration target) noexcept
	{
		if (!HasServoAuthority())
		{
			return;
		}

		m_Target = ClampToRange(Positive(target));
	}

	// Observed since the last invalidation. Prediction is `Unscheduled` until this is true, and every
	// other accessor keeps answering — an output that has not presented since a resume still has a
	// rate, and admission control still has to size it.
	[[nodiscard]] constexpr bool IsValid() const noexcept { return m_Valid; }

	// The deadline may be deliberately deferred, because the vblank is gyro's to place.
	[[nodiscard]] constexpr bool IsVariable() const noexcept { return m_Range.Enabled; }

	// The anchor came from the display hardware rather than from software noticing afterwards. Per
	// clock rather than per machine: a virtual output's timestamps are flow control and are never
	// precise while the panel beside it is. What reads it downgrades — lower log severity, no latency
	// assertions — rather than reporting numbers with no error bar.
	[[nodiscard]] constexpr bool IsPrecise() const noexcept { return m_Valid && m_Precise; }

	// What the panel last did. Falls back to the configured nominal before the first observation,
	// which is the only estimate that exists then and is what supplies `P` to decision 29's test for
	// an output that has just been configured.
	[[nodiscard]] constexpr Duration Period() const noexcept { return m_Observed; }

	// What gyro is presently asking for, which equals the target only once a ramp has converged and
	// equals `Period()` only once the panel has complied.
	[[nodiscard]] constexpr Duration CommandedPeriod() const noexcept { return m_Commanded; }

	// Where the servo is heading. The third of the three periods, and the one admission control wrote:
	// a ramp in flight is exactly the interval over which this and `CommandedPeriod()` disagree.
	[[nodiscard]] constexpr Duration TargetPeriod() const noexcept { return m_Target; }

	// The window the servo has to work in. A fixed-refresh output reports the degenerate range at its
	// own rate, which is the same shape rather than a special case — and so does a variable one whose
	// panel reported a window that cannot be true, since the answer to *how much room is there* is
	// none in both cases and a caller choosing a rung should not have to ask twice.
	//
	// The clearance the servo holds above the bottom is the servo's own restraint rather than the
	// panel's, so it is not subtracted here.
	[[nodiscard]] constexpr VariableRefresh PeriodRange() const noexcept
	{
		if (HasServoAuthority())
		{
			return m_Range;
		}

		return VariableRefresh{ m_Range.Enabled, m_Observed, m_Observed };
	}

	// The frame the anchor describes, which is the output's own counter — vblank sequence on KMS, the
	// feedback's sequence nested — and therefore the counter every other sequence here is in.
	[[nodiscard]] constexpr std::uint64_t LastSequence() const noexcept { return m_Sequence; }

	// When an arbitrary frame reaches the glass. Answers for the past as readily as the future, since
	// both are the same arithmetic and refusing one would make the caller subtract instead.
	[[nodiscard]] constexpr Instant PresentationAt(std::uint64_t sequence) const noexcept
	{
		if (!m_Valid)
		{
			return Unscheduled;
		}

		return Advanced(m_AnchoredAt, Detail::Scaled(PredictedPeriod(), Ahead(sequence)));
	}

	// When that frame's commit must have landed.
	[[nodiscard]] constexpr Instant DeadlineAt(std::uint64_t sequence) const noexcept
	{
		const Instant presentation = PresentationAt(sequence);

		if (presentation == Unscheduled)
		{
			return Unscheduled;
		}

		return Advanced(presentation, -m_LatchLead);
	}

	// When work reserving `reserve` must start to make that frame — `deadline - renderBudget -
	// safety`, with the caller naming the sum because the caller is the one measuring it. A negative
	// reserve is read as none rather than as a wakeup after the deadline it is derived from.
	[[nodiscard]] constexpr Instant WakeupAt(std::uint64_t sequence, Duration reserve) const noexcept
	{
		const Instant deadline = DeadlineAt(sequence);

		if (deadline == Unscheduled)
		{
			return Unscheduled;
		}

		return Advanced(deadline, -std::max(reserve, Duration::zero()));
	}

	// The earliest frame whose deadline is at or after `notBefore`, and never a frame that has already
	// been observed.
	//
	// This is the recovery path of the note above, and the composition it is written for is
	// `SequenceAfter(now + C)`: hand it the instant the work would be finished and it names the frame
	// that work can still make. A loop that skipped, stalled, or was idle asks this instead of
	// counting periods itself, which is what keeps decision 35's `ceil(overrun / P)` in the one object
	// holding the anchor it is measured from.
	//
	// `NoSequence` while the clock names no frame, which a caller distinguishes without a second
	// question because a real answer is always at least one past an anchor.
	[[nodiscard]] constexpr std::uint64_t SequenceAfter(Instant notBefore) const noexcept
	{
		if (!m_Valid)
		{
			return NoSequence;
		}

		const Duration ahead = Elapsed(Advanced(m_AnchoredAt, -m_LatchLead), notBefore);
		const std::int64_t steps =
			std::max<std::int64_t>(Detail::CeilDivide(ahead.count(), PredictedPeriod().count()), 1);
		const std::uint64_t advance = static_cast<std::uint64_t>(steps);

		if (m_Sequence > std::numeric_limits<std::uint64_t>::max() - advance)
		{
			return std::numeric_limits<std::uint64_t>::max();
		}

		return m_Sequence + advance;
	}

	// The frame this output owes next, meaning the one after the anchor. A loop in steady state asks
	// these; a loop that has just lost time asks `SequenceAfter` first and then asks these about what
	// it named.
	[[nodiscard]] constexpr Instant NextPresentation() const noexcept { return PresentationAt(m_Sequence + 1); }

	[[nodiscard]] constexpr Instant NextDeadline() const noexcept { return DeadlineAt(m_Sequence + 1); }

	[[nodiscard]] constexpr Instant NextWakeup(Duration reserve) const noexcept
	{
		return WakeupAt(m_Sequence + 1, reserve);
	}

	[[nodiscard]] constexpr const FrameClockPolicy& Policy() const noexcept { return m_Policy; }

private:
	// A period of zero divides and a negative one runs the output backwards, and both are reachable
	// from a configuration nobody validated — `PeriodFromHertz` already answers a nonsense rate with
	// `Duration::max()`, which saturates prediction into `Unscheduled` and is the recoverable
	// direction.
	[[nodiscard]] static constexpr Duration Positive(Duration period) noexcept
	{
		return std::max(period, Duration{ 1 });
	}

	// What the panel did, in preference order. The backend's own figure first, since it is the one
	// thing here measured by something with a view of the hardware; the anchor difference second,
	// which divides by the sequence advance and is therefore correct across frames that were skipped
	// rather than merely across consecutive ones; and the caller's previous estimate last, by this
	// returning zero and `Observe` leaving what it had.
	[[nodiscard]] constexpr Duration Measure(const PresentationInfo& info) const noexcept
	{
		if (info.Period > Duration::zero())
		{
			return info.Period;
		}

		if (!m_Valid || info.Sequence <= m_Sequence)
		{
			return Duration::zero();
		}

		const std::uint64_t advanced = info.Sequence - m_Sequence;
		const Duration elapsed = Elapsed(m_AnchoredAt, info.PresentedAt);

		if (elapsed <= Duration::zero() ||
		    advanced > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
		{
			return Duration::zero();
		}

		return elapsed / static_cast<std::int64_t>(advanced);
	}

	// One bounded step toward the target, on the observation that is the only new evidence there is.
	constexpr void Converge() noexcept
	{
		if (!HasServoAuthority() || m_Commanded == m_Target)
		{
			return;
		}

		// At least a nanosecond, so that a divisor larger than the period still converges rather than
		// standing still while reporting a target it will never reach.
		const Duration bound = std::max(m_Commanded / m_Policy.ServoStepDivisor, Duration{ 1 });

		m_Commanded += std::clamp(m_Target - m_Commanded, -bound, bound);
	}

	// Whether there is a window to servo inside. Decision 31 declines to validate a *requested* period
	// against a panel's own documentation, and this is the other thing: a window that cannot be true
	// whatever the panel is — disabled, inverted, or non-positive — is a backend saying nothing rather
	// than saying something to distrust. Nothing is what it is believed to have said.
	[[nodiscard]] constexpr bool HasServoAuthority() const noexcept
	{
		return m_Range.Enabled && m_Range.Shortest > Duration::zero() && m_Range.Longest >= m_Range.Shortest;
	}

	// The panel's window, held clear of its bottom so that low-framerate compensation never engages
	// underneath a period gyro commanded.
	[[nodiscard]] constexpr Duration ClampToRange(Duration period) const noexcept
	{
		if (!HasServoAuthority())
		{
			return period;
		}

		const Duration longest = std::max(m_Range.Longest - m_Policy.RangeClearance, m_Range.Shortest);

		return std::clamp(period, m_Range.Shortest, longest);
	}

	// The commanded period on a variable output and the observed one on a fixed output. See the note
	// above: on a variable panel the vblank happens when gyro submits, so what gyro intends is the
	// prediction, while on a fixed one the panel decides and gyro's intention is not an input.
	[[nodiscard]] constexpr Duration PredictedPeriod() const noexcept
	{
		return Positive(m_Range.Enabled ? m_Commanded : m_Observed);
	}

	// How far a sequence is from the anchor, saturated into the signed domain the arithmetic needs.
	[[nodiscard]] constexpr std::int64_t Ahead(std::uint64_t sequence) const noexcept
	{
		constexpr std::uint64_t Limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

		if (sequence >= m_Sequence)
		{
			const std::uint64_t forward = sequence - m_Sequence;

			return forward > Limit ? std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(forward);
		}

		const std::uint64_t backward = m_Sequence - sequence;

		return backward > Limit ? std::numeric_limits<std::int64_t>::min() : -static_cast<std::int64_t>(backward);
	}

	FrameClockPolicy m_Policy{};

	// What this output's own configuration said, which is the mode's arithmetic rather than a number
	// chosen for every panel at once. Survives an invalidation: a stale anchor is a fact that expired,
	// and the blanking interval is one that did not.
	Duration m_LatchLead{};

	// The anchor, and the whole of what an invalidation drops.
	Instant m_AnchoredAt{};
	std::uint64_t m_Sequence = NoSequence;
	bool m_Valid = false;
	bool m_Precise = false;

	// The three periods, and the window the last two are held inside. They start saturated rather than
	// zero, for the reason Core/Time.h gives where `PeriodFromHertz` answers a nonsense rate the same
	// way: an output that never owes a frame is recoverable and one that owes infinitely many is not.
	Duration m_Observed = Duration::max();
	Duration m_Commanded = Duration::max();
	Duration m_Target = Duration::max();
	VariableRefresh m_Range{};
};

// Prints as clock seq 4210 at 12500000ns period 6944444ns hw-clock, or clock invalid period 6944444ns,
// with vrr 8333333ns -> 10000000ns naming the commanded period and its target on a variable output.
template<>
struct std::formatter<FrameClock>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const FrameClock& clock, Context& context) const
	{
		auto out = clock.IsValid() ? std::format_to(
										 context.out(),
										 "clock seq {} at {}",
										 clock.LastSequence(),
										 clock.PresentationAt(clock.LastSequence())
									 ) :
		                             std::format_to(context.out(), "clock invalid");

		out = std::format_to(out, " period {}", clock.Period());

		if (clock.IsVariable())
		{
			out = std::format_to(out, " vrr {} -> {}", clock.CommandedPeriod(), clock.TargetPeriod());
		}

		if (clock.IsPrecise())
		{
			out = std::format_to(out, " hw-clock");
		}

		return out;
	}
};

// The contract everything downstream assumes. The behaviour is swept in FrameClock.Test.cpp; what is
// here is the part a wrong answer changes silently.
static_assert(std::formattable<FrameClock, char>, "A report prints the clock rather than <unprintable>");

// A clock nobody has configured names no frame and arms nothing, which is what makes construction
// before the first mode set an ordinary state rather than one the composition root has to sequence
// around.
static_assert(!FrameClock{}.IsValid());
static_assert(FrameClock{}.NextDeadline() == FrameClock::Unscheduled);
static_assert(FrameClock{}.SequenceAfter(Monotonic::FromNanoseconds(1'000'000'000)) == FrameClock::NoSequence);

// Prediction is arithmetic on the anchor, and the deadline is the presentation less the latch lead.
static_assert(
	[] {
		FrameClock clock;
		OutputConfiguration configuration{ .Period = PeriodFromHertz(100.0),
		                                   .LatchLead = std::chrono::microseconds{ 400 } };
		clock.Configure(configuration);
		clock.Observe(
			{ .PresentedAt = Monotonic::FromNanoseconds(1'000'000'000),
	          .Period = std::chrono::milliseconds{ 10 },
	          .Sequence = 7 }
		);

		return clock.IsValid() && clock.Period() == std::chrono::milliseconds{ 10 } &&
	           clock.NextPresentation() == Monotonic::FromNanoseconds(1'010'000'000) &&
	           clock.NextDeadline() == Monotonic::FromNanoseconds(1'009'600'000) &&
	           clock.PresentationAt(107) == Monotonic::FromNanoseconds(2'000'000'000) &&
	           clock.NextWakeup(std::chrono::milliseconds{ 4 }) == Monotonic::FromNanoseconds(1'005'600'000);
	}(),
	"The anchor plus the period is the next frame, and a deadline leads its presentation"
);

// Decision 35's ceil(overrun / P), which is the whole reason an instant enters this interface.
static_assert(
	[] {
		FrameClock clock;
		OutputConfiguration configuration{ .Period = std::chrono::milliseconds{ 10 } };
		clock.Configure(configuration);
		clock.Observe(
			{ .PresentedAt = Monotonic::FromNanoseconds(1'000'000'000),
	          .Period = std::chrono::milliseconds{ 10 },
	          .Sequence = 7 }
		);

		// Twenty-five milliseconds of stall lands on the third frame after the anchor; a deadline that
	    // falls exactly on the instant asked about is still one the work can make.
		return clock.SequenceAfter(Monotonic::FromNanoseconds(1'025'000'000)) == 10 &&
	           clock.SequenceAfter(Monotonic::FromNanoseconds(1'020'000'000)) == 9 &&
	           clock.SequenceAfter(Monotonic::FromNanoseconds(500'000'000)) == 8;
	}(),
	"An instant names the frame it can still make, and never one already presented"
);
