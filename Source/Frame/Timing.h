#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <string_view>

#include "Core/Time.h"
#include "Core/Wake.h"
#include "Frame/Budget.h"
#include "Frame/FrameClock.h"
// See Docs/Architecture.md#the-frame-loop, Docs/Architecture.md#admission-control, and decisions 29,
// 31, 34, and 35.

// The one timing decision gyro takes at runtime, and the place the clock and the record meet.
//
// `FrameClock` says when a frame is owed and holds nothing about gyro's cost; `Budget` says what
// frames have cost and composes nothing. Neither can answer the question the frame loop asks once per
// output per iteration, because the question is both of them at once — decision 35's record-time
// check, which is the whole of the runtime timing policy and is stated there in three lines:
//
//     now + C_planned <= deadline   ->  render the planned tier
//     now + C_min     <= deadline   ->  render the floor tier
//     otherwise                     ->  skip this output's frame, target the next deadline
//
// Everything else about timing happens elsewhere and on a different schedule. Which output loses a
// rung, how much each may spend, whether a set is feasible at all — that is admission control, it is
// solved for `C` at configuration change, and its output is an allocation rather than a decision. This
// runs every iteration and decides one thing.
//
// **The composition of the two device figures is here because here is where it will stop being
// addition.** `Budget` refuses the sum and gives three reasons, all of them about knowledge it does
// not have: how many outputs share a queue, whether the work is chunked, whether recording and
// execution overlap. The frame loop is the object that will know those things and this is that
// knowledge's first resident. So the arithmetic below is written as a pipeline rather than as a sum
// even though today it computes a sum, because the case it grows into is then a change of terms and
// not a change of shape.
//
// **The frame being recorded is one past the later of two counters, and the second one is the
// caller's.** `FrameClock::LastSequence()` is the last frame *presented*; `committed` is the last
// frame *spoken for* — recorded, handed to `IPresenter::Present`, and awaiting its flip. Under a
// pipeline one frame deep the two differ only between a submit and its vblank, which is why a
// hardcoded `LastSequence() + 1` reads as right and is not: a loop visiting a 60 Hz output on every
// 144 Hz iteration lands in that interval two or three times a period, and from the clock alone it is
// indistinguishable from the frame never having been drawn. `deviceFreeAt` does not cover it either,
// because decision 35's `t_done` is the device finishing *execution*, and by then the device is idle.
// What the loop would be told is to draw frame 8 again, and the cost is three allocations spent on the
// frame admission control priced at one. So it is the same argument `deviceFreeAt` makes and the
// second resident of the same knowledge — state the loop holds, that neither the clock nor the record
// can see, and that a timing answer is wrong without.
//
// **`WakeFor` is where that floor is load-bearing.** `SequenceAfter` names the earliest frame the work
// could reach and refuses only frames already *observed*, so for a frame already recorded it names
// that frame — and the wake it yields is that frame's own record point, an instant the loop has
// already served. Without the floor it is handed that same instant back on every iteration until the
// flip lands, and the frame after it is never armed for at all.
//
// **The decision names the earliest frame the work can reach, and an equality there was a deadlock.**
// `SequenceAfter(finish)` is that frame and `owed` is the frame after the later of what reached the
// glass and what has been spoken for. Admitting only where the two are *equal* reads as conservative
// and is not, because nothing else moves `owed`: only a flip advances the anchor, and only a present
// produces a flip. So an output whose reach is one past `owed` — a loop coming out of idle, or one
// whose queue another output held for longer than a period — refuses, presents nothing, is handed a
// reach one larger on the next iteration, and never renders again. `FrameClock::SequenceAfter` calls
// itself the recovery path for a loop that "skipped, stalled, or was idle" and names the frame
// correctly; what was missing was that the answer was discarded and `owed` targeted regardless.
//
// **So the target is `max(reach, owed)`, which is what `WakeFor` already computes, and the verdict is
// which of the two that maximum was.** A reach *ahead* of the frame owed is that frame being genuinely
// unreachable: it is dropped, and the frame that *is* reachable is drawn. A reach *behind* it is the
// opposite failure and takes the opposite answer — the loop is awake before the owed frame's window,
// where drawing the reach would repeat a frame already committed and drawing `owed` would start it
// more than a period before its own deadline. That second one is a lead, decision 30 grants a lead
// from a plan, and this is not the object that grants it. What bounds the lead otherwise is the
// presenter's ring — `AcquireTarget()` answers nothing while two targets are held, so a
// double-buffered output cannot run ahead at all, and decision 30's third target is that same bound
// stated from the other side.
//
// **Which makes decision 35's three lines a fixpoint rather than one application of itself.** The
// third line's *target the next deadline* is the first line's input, so `now + C <= deadline` is
// always tested against the deadline the decision actually targets. Dropping `ceil(overrun / P)`
// frames and rendering the one after is decision 35's third promise stated exactly, and
// `SequenceAfter` is where that ceiling is computed — so the promise falls out of the branch that
// refuses the owed frame instead of being a rung beside it. The cascade the skip exists to stop is
// untouched, because work aimed at its own reach is by construction work that is **not** late: what
// decision 35 refuses is submitting for a frame already missed, and that is exactly the frame this
// stops naming.
//
// **The release constraint is unchanged, which is the property that makes the above safe.** `reach` is
// the earliest frame whose deadline is at or after the finish, so `deadline(reach) - P < finish` and
// work never starts more than one period before the deadline it is aimed at — whichever frame that
// is. The equality admitted precisely the same window for the frame owed; Source/Integration/
// Schedulability.Test.cpp's header already calls that "decision 30's relaxed release constraint,
// arriving for free out of the equality test". Nothing here widens it, no output's demand per period
// changes, and decision 29's test therefore still covers the schedule this produces.
//
// **Which leaves exactly the half of decision 30 this object can hold, and it holds it now.** The
// target is no longer pinned to the anchor, so `committed = anchor + k` is a pipeline `k + 1` deep and
// `Decide` supplies that frame's presentation and deadline rather than the next one's — which makes
// decision 30's claim that animation state is exact under early rendering `Evaluate(PresentationAt(S +
// k))`, with no new term anywhere. What is absent is everything on the other side of the record, and
// all of it belongs elsewhere: where in the cycle the work is *placed*, which is admission control's
// output; the end of that placement, which binds an early frame sooner than its own latch deadline
// does; and the commit held back to the frame's own window, since a flip issued at the record point
// lands a period early. Until a plan supplies the first of those, a lead is something the ring
// prevents rather than something this refuses.
//
// **The split buys one thing immediately, and it is the term the pseudocode spells as a boolean.**
// Architecture.md#the-frame-loop names *previous frame still in flight* as a disqualifier; decision 35
// states the same condition as arithmetic, `t_done + C_min <= D`. With one summed figure only the
// boolean is expressible. With two it is exact: recording is the frame thread's and begins now
// whatever the device is doing, execution begins when the device is free, and a frame refused because
// the GPU has two hundred microseconds of somebody else's work left is refused for a reason a boolean
// cannot see. `deviceFreeAt` is the *device's* instant and not the output's, because GPU work from two
// outputs serialises on one queue however it was recorded — which is decision 29's reason for
// rejecting parallel recording as a rescue, arriving here as a parameter.
//
// **The CPU term is a sum of two figures and the GPU term is one, which is decision 94 landing in a
// single line.** Producing the draw list is CPU work that happens before either composite is
// recorded and is the same work whichever one follows, so `Budget::IrreducibleCpu()` is added to the
// planned and floor figures alike. Both tiers therefore reserve it, which is what stops the ladder
// from promising a step down it cannot take: on a large scene the floor composite may fit a deadline
// the walk that fed it does not, and a check that could not see the walk would admit that frame and
// miss.
//
// **The check is spelled through `SequenceAfter` rather than through comparisons, and the two are the
// same statement.** `now + C <= deadline(S + 1)` holds exactly when `SequenceAfter(now + C)` answers
// `S + 1`, since that call names the earliest frame whose deadline is at or after an instant and never
// names one already observed. Writing it the second way means the dropped frames' `ceil(overrun / P)` is
// the *same call* rather than a second piece of arithmetic derived from the same anchor — decision
// 35's third promise falls out of the branch that refuses instead of being computed beside it, and the
// division stays in the object holding the anchor it is measured from.
//
// **The tier drawn is the one that reaches the earliest frame, and the planned tier takes a tie.**
// Decision 35's second promise is that an overrun of up to `P - C_min` costs exactly one frame, and
// the mechanism is the floor tier recovering at the next deadline. Taking the planned tier's reach
// would sleep straight through the frame the floor tier could have made and price a one-frame overrun
// at however many frames the planned tier happens to be behind. It is a minimum over the two rather
// than the floor's reach outright, because nothing in the system enforces that the floor composite is
// cheaper than the planned one — if a policy ever inverts them the earliest reachable frame is still
// the right answer, and the ordering is then an expectation rather than a load-bearing assumption. The
// tie goes to the planned tier because the two then reach the same frame and one of them looks better;
// that this now decides what is *drawn* rather than only what is waited for is the whole of what the
// paragraph above changed about it.
//
// **An invalid clock renders and does not wait, and it is a branch rather than a consequence.**
// `Unscheduled` carries that behaviour through every comparison the check could have been written as,
// since `now + C <= Unscheduled` is true for any cost. It does not carry through `SequenceAfter`,
// which answers `NoSequence` and would read here as a wait — so the one place the sentinel does not
// carry itself is the one place this has an explicit branch. What it produces is decision 31's account
// of the first frame after an idle variable-refresh output, verbatim: render at once, present as soon
// as it is ready, and let the flip that results anchor the clock.
//
// **That same branch is why the evaluation instant is not the deadline sentinel.** A frame with no
// anchor has no predicted presentation, and the least wrong instant to evaluate animations at is the
// one the work is predicted to finish, because a present with no deadline flips as soon as it is
// ready. Naming `Unscheduled` there would evaluate the scene at the end of time and settle every
// animation in a single frame. A sentinel that is exactly right for a deadline is catastrophic for an
// evaluation instant, which is why the two fields are populated from different sources in that case
// and from the same clock in every other.
//
// **Both margins live here.** `Budget` declined them on the grounds that they are margins on gyro's
// own scheduling rather than measurements of gyro's work, and named this object as the holder; a copy
// in the record would be a number written by the schedule sitting in the object the backend writes.
// The pair is `TimingPolicy::Margin` and `TimingPolicy::Lead`, and which of `Reserve` and `Arming`
// carries which is the whole of what that type's header is about.
//
// What is deliberately not here: admission control's allocation, which is a cap on what an output may
// spend rather than a record of what it did; the quality tier step decision 35 requires of a
// systematic overrun, which is decision 34's and reaches `Budget` through `Invalidate`; and the fold
// that decides whether an output is owed a frame at all, which is the scene's `Wake` and never asks
// what anything costs.

// The numbers this policy needs and nobody has measured, `// SPEC:` for the reason `FrameClockPolicy`
// and `BudgetPolicy` are. See Open.md, *scheduling policy constants*.
//
// **Two figures rather than one, because a single one cancels.** Both were `Safety` until a capture
// showed what that costs. The wake was armed at `deadline - Reserve` and the record-time check then
// asked whether `now + Reserve` cleared the same deadline, so the margin appeared on both sides of one
// comparison and subtracted out: the verdict reduced to `now <= armedAt`, and a loop woken by its own
// timer — which arrives at the armed instant and *then* drains its sources — could not pass it. What a
// person saw was the blur behind a window switching off for single frames while nothing was under
// load, because the frame that could not clear the check fell to the floor composite. The two figures
// below are the two ends of one frame, and they are held apart so that the arming end can be moved
// without making the check harder.
struct TimingPolicy
{
	// What is held above the measured cost between the predicted finish and the deadline.
	//
	// It covers the error in the cost model and nothing else: the marks in `Budget` are already a
	// maximum over their window, so this is the tail beyond that window — a composite that costs more
	// than the most expensive one recently seen. It is added once to a composed reserve and not once
	// per device, because it is one margin on one frame.
	//
	// **The failure it prevents is the one that cannot be taken back.** A frame whose composite
	// overruns after the loop committed to it has already been recorded and presented; the flip misses
	// its vblank and the frame lands a refresh late, which is a visible hitch and which no tier can
	// undo. That asymmetry is why this one is sized at a high quantile of the residual and `Lead` below
	// is not.
	//
	// This is the figure admission control is given, because a set admitted against a smaller margin
	// than the record-time check holds is a set that passes the test and misses the frame. `Lead` is
	// not: it is a fact about when gyro wakes up rather than about whether a set of outputs fits in a
	// period, and an allocator holding it would refuse configurations gyro can serve.
	Duration Margin{};

	// What is held *ahead* of the armed instant, so that the loop is already running when the work has
	// to start.
	//
	// It covers everything between the ring's timeout expiring and the first instruction of the record:
	// the kernel's wakeup latency for a `SCHED_FIFO` thread on an absolute `hrtimer`, the drain of every
	// event source, and the snapshot acquisition. It appears in the arming alone — `Reserve` does not
	// carry it — which is the whole of the split described above.
	//
	// **It is also what decides whether gyro's own schedule is what wakes gyro.** The frame thread
	// sleeps on `min(timeout, descriptors)`, so a lead shorter than the host's event cadence leaves the
	// instant a frame starts at set by the other end of the socket rather than by this policy. Sized to
	// beat that, the timer wins the race and the start time becomes gyro's own.
	//
	// **It does not delay the frame it leads, which is the thing to get right before sizing it.** The
	// work moves to the start of the refresh window rather than the end, and the frame reaches the same
	// vblank from either place — so a lead costs nothing on the way to the glass and *saves* a refresh
	// wherever the window's end left a composite finishing with no room for the flip in front of it.
	// Animation is unaffected in both cases, being evaluated at the predicted presentation whatever
	// instant the work began at.
	//
	// **What bounds it from above is the period.** A lead longer than one refresh arms before the
	// previous frame's window and asks the loop to render against a scene it has not been handed, which
	// is a frame of latency paid for nothing. Below that the figure is free, so it wants to be the
	// smallest that covers the wakeup rather than the smallest that fits.
	//
	// Zero is the permissive reading and is the recoverable direction: an unset lead arms at the last
	// instant the planned composite still fits and takes the floor composite whenever it is not early,
	// where an overstated one pays latency on every frame forever.
	Duration Lead{};
};

namespace Detail
{
// `left + right` for non-negative durations, saturating.
//
// The same discipline Core/Time.h applies to the instant arithmetic, one level down and for the same
// reason it gives: neither bound is reachable from figures this process measures, and a saturated
// answer is one the caller can compare where an overflowed one is a value the optimiser is entitled to
// assume cannot exist. `BudgetPolicy` is caller-authored and nothing clamps a target from above.
[[nodiscard]] constexpr Duration Sum(Duration left, Duration right) noexcept
{
	const Duration headroom = Duration::max() - left;

	return right > headroom ? Duration::max() : left + right;
}
} // namespace Detail

// Whether the frame thread records a composite for this output on this iteration, and which one.
//
// **What "admitted" means here is *let start now*, and it is not admission control's sense of the
// word.** Frame/Admission.h runs at configuration change and answers *how much may this output spend
// on a frame* — its answer is an allocation, and it is a cap. This runs every iteration and answers
// *does work on a frame begin at this instant, and at which composite* — its answer is one of the
// three below, and it is a start signal. The allocation is an input to the second question by way of
// `Budget`, so the two cannot disagree, and nothing here is entitled to spend more than the plan
// granted. The unfortunate part is that both are called admission; the two live one `#include` apart
// and only this one is per iteration.
//
// Ordered by the work it lets through, so that the more conservative of two answers is `std::min`.
// That reduction is what a device-wide degradation looks like — several outputs sharing one queue,
// each assessed and the set held to the weakest — and having the order mean something costs an
// enumerator value here against writing the comparison out wherever it is first needed. It is the same
// argument `Wake::Kind` makes one direction up.
enum class Admission : std::uint8_t
{
	// **Record nothing for this output this iteration.** Not a frame being dropped, and the name says
	// so: what the loop does here is wait.
	//
	// Every tier's reach is *behind* the frame owed — which means this output has a frame already spoken
	// for whose window has not come, so there is nothing to record until it does, and the next thing to
	// happen is somebody else's. A frame deliberately dropped is one of the two render verdicts below
	// with a `Sequence` past the frame owed, and the frames between the two are what decision 35's third
	// promise prices at `ceil(overrun / P)`.
	//
	// **It still names a frame**, with that frame's deadline and presentation, because the wake the
	// loop arms is derived from them. A verdict carrying no frame would make the recovery path ask a
	// second question to find out what it was waiting for.
	Wait = 0,

	// **Record the frame at the floor composite**, which is the tier decision 34 guarantees exists and
	// the one admission control checks an allocation against rather than the one it hands out.
	//
	// Chosen where the planned composite would not have landed in the same frame's window and this one
	// does: either the planned tier overshoots the frame owed and the floor tier makes it, which is
	// decision 35's second promise and the mechanism that prices an overrun of up to `P - C_min` at
	// exactly one frame; or both overshoot and the floor tier reaches an earlier frame, which is the
	// same promise once the frame owed is already gone. A deadline met on the lower rung is a deadline
	// met, so this is not a miss and nothing downstream counts it as one.
	Floor = 1,

	// **Record the frame at the planned composite**, which is what admission control allocated for and
	// what a set meeting its deadlines answers on every iteration.
	//
	// Also the answer where both tiers reach the same frame, since the output then pays the same frame
	// either way and one of the two looks better. And the answer for an output whose clock has no
	// anchor, where there is no deadline to be measured against at all — decision 31's first frame
	// after idle renders at once, presents as soon as it is ready, and lets the flip that results
	// anchor the clock.
	Planned = 2,
};

[[nodiscard]] constexpr std::string_view Name(Admission admission) noexcept
{
	switch (admission)
	{
		case Admission::Wait:
			return "wait";
		case Admission::Floor:
			return "floor";
		case Admission::Planned:
			return "planned";
	}

	return "invalid";
}

// One output's answer for one iteration.
//
// Every instant here describes the *targeted* frame, which is the one being rendered when the verdict
// renders and the one being waited for when it waits. A wait is not an absence of an answer — it names
// the frame the loop is now aiming at, which is what stops the recovery path from asking a second
// time.
struct FrameDecision
{
	Admission Verdict = Admission::Wait;

	// Never a frame already observed, and `NoSequence` only when the clock names none.
	std::uint64_t Sequence = FrameClock::NoSequence;

	// What to evaluate animations at. Architecture.md#the-frame-loop keeps this inside the per-output
	// loop, which is the whole of per-output evaluation in one line: there is no global predicted
	// presentation time because there is no global clock.
	Instant Presentation = FrameClock::Unscheduled;

	// When the commit must have landed for that frame to reach the glass.
	Instant Deadline = FrameClock::Unscheduled;

	// When the admitted work is predicted to be done. Carried rather than recomputed because the log
	// line that explains a verdict is the difference between this and the deadline and nothing else.
	Instant Finish = FrameClock::Unscheduled;

	// When the device is predicted to be free, which is `Finish` without the margin over it. It is here
	// because it is the *next* output's input: decision 29 has GPU work from two outputs serialise on
	// one queue however it was recorded, so a loop admitting outputs in deadline order threads this
	// forward and hands it to the following `Assess` as its `deviceFreeAt`. Recovering it by subtracting
	// `Policy().Margin` back off `Finish` would be the same number reached by arithmetic beside the call
	// that already had it, and it would silently stop being the same number the moment the composition
	// below stops being addition. The margin is not subtracted because it was never the device's: it is
	// a margin on gyro's own wakeups, so a second output that inherited it would pay it twice.
	Instant DeviceFreeAt = FrameClock::Unscheduled;

	[[nodiscard]] constexpr bool Renders() const noexcept { return Verdict != Admission::Wait; }

	// Which composite to draw, and which population its cost is filed into afterwards. Meaningful only
	// when `Renders()`; a wait draws nothing and files nothing, and the caller that asks anyway is one
	// that has already ignored the verdict.
	[[nodiscard]] constexpr RenderMode Mode() const noexcept
	{
		return Verdict == Admission::Planned ? RenderMode::Planned : RenderMode::Floor;
	}

	// How much room the prediction left, negative by exactly the overrun when it did not fit. The
	// figure a schedulability sweep reports and the one an assertion is written against, which is why
	// it is a subtraction here rather than at each of them.
	[[nodiscard]] constexpr Duration Slack() const noexcept { return Elapsed(Finish, Deadline); }
};

// What `Finish` computes on the way to its answer, returned whole because both halves have callers.
//
// The pipeline has two ends that mean different things — when the *device* stops being busy, and when
// *this frame* is safely done — and `Assess` needs both in the same breath: the second decides the
// verdict, the first is what the next output on the same queue starts from.
struct Projection
{
	Instant DeviceFreeAt{};
	Instant Finish{};
};

class Timing
{
public:
	constexpr Timing() noexcept = default;

	constexpr explicit Timing(TimingPolicy policy) noexcept : m_Policy{ policy }
	{
		m_Policy.Margin = std::max(m_Policy.Margin, Duration::zero());
		m_Policy.Lead = std::max(m_Policy.Lead, Duration::zero());
	}

	// Decision 35's record-time check, for one output at its record point.
	//
	// `committed` is the last frame this output has spoken for — recorded, handed to
	// `IPresenter::Present`, and awaiting its flip — and `NoSequence` where nothing is outstanding past
	// the anchor. **It has no default**, because the permissive reading is the defect described above
	// rather than a conservative answer: the loop owns its pipeline depth, and this is where it says so.
	//
	// `deviceFreeAt` is when the device finishes what it is already executing, which the loop obtains
	// by polling the previous frame's timeline semaphore non-blockingly. The epoch means *nothing in
	// flight*, and it is the identity of the maximum below rather than a sentinel to test for.
	[[nodiscard]] constexpr FrameDecision Assess(
		const FrameClock& clock,
		const Budget& budget,
		Instant now,
		std::uint64_t committed,
		Instant deviceFreeAt = Instant{}
	) const noexcept
	{
		const Projection planned = Project(now, deviceFreeAt, budget, RenderMode::Planned);

		if (!clock.IsValid())
		{
			// No anchor, so no prediction and no deadline. See the note above for why the evaluation
			// instant is the finish rather than the sentinel beside it.
			return { .Verdict = Admission::Planned,
				     .Sequence = FrameClock::NoSequence,
				     .Presentation = planned.Finish,
				     .Deadline = FrameClock::Unscheduled,
				     .Finish = planned.Finish,
				     .DeviceFreeAt = planned.DeviceFreeAt };
		}

		const std::uint64_t owed = Owed(clock, committed);
		const std::uint64_t plannedReach = clock.SequenceAfter(planned.Finish);

		if (plannedReach == owed)
		{
			return Decide(clock, Admission::Planned, owed, planned);
		}

		const Projection floor = Project(now, deviceFreeAt, budget, RenderMode::Floor);
		const std::uint64_t floorReach = clock.SequenceAfter(floor.Finish);

		if (floorReach == owed)
		{
			return Decide(clock, Admission::Floor, owed, floor);
		}

		// Neither tier lands in the owed frame's own window, and past this point the two ways of
		// failing that are opposite in both cause and answer. See the header: a reach past the frame
		// owed is that frame being unreachable, and the frame that is reachable is the one to draw; a
		// reach behind it is the loop awake early, where every frame it could name is either a repeat
		// or a lead.
		const bool takeFloor = floorReach > owed && (plannedReach < owed || floorReach < plannedReach);

		if (takeFloor)
		{
			return Decide(clock, Admission::Floor, floorReach, floor);
		}

		if (plannedReach > owed)
		{
			return Decide(clock, Admission::Planned, plannedReach, planned);
		}

		// Both tiers reach only a frame already spoken for, so there is nothing to record until that
		// frame's successor comes due. The wait names *that* frame rather than the one either tier
		// reached, which is what stops the recovery path asking a second time — and it reports the
		// planned prediction, so the slack printed beside it is measured against the deadline it
		// named.
		//
		// Time flies when you're having fun.
		return Decide(clock, Admission::Wait, owed, planned);
	}

	// When the loop must be running again for this output to make its next frame at the planned tier.
	//
	// One contribution to the fold Architecture.md#doing-nothing-must-cost-nothing describes, and the
	// only one that prices anything: the scene's contributors say a frame is *wanted*, this says when
	// work on it has to start. An output whose clock names no frame contributes `Never()`, which is
	// that section's invariant reached from the timing side — nothing is due, so nothing is armed, and
	// the damage that eventually arrives is admitted by `Assess` on its own merits.
	[[nodiscard]] constexpr Wake WakeFor(
		const FrameClock& clock,
		const Budget& budget,
		Instant now,
		std::uint64_t committed,
		Instant deviceFreeAt = Instant{}
	) const noexcept
	{
		const Duration arming = Arming(budget);
		const std::uint64_t reach = clock.SequenceAfter(Project(now, deviceFreeAt, budget, RenderMode::Planned).Finish);

		if (reach == FrameClock::NoSequence)
		{
			return Wake::Never();
		}

		// **A third floor, and it is the only one that is about the alarm rather than about the frame.**
		// The two above name a frame; this names the earliest frame whose *record point* has not already
		// gone by, which is the one property an instant handed to a timer has to have. Without it the
		// loop armed the past — `IORING_TIMEOUT_ABS` fires an expired deadline the moment it is
		// submitted, so the iteration woke, found whatever it found, computed the same instant again and
		// armed it again: a spin at twelve microseconds a turn on the `SCHED_FIFO` frame thread, for as
		// long as the condition holding the frame up lasted.
		//
		// **What put the record point in the past is an arming longer than a period**, which is ordinary
		// rather than exotic: a composite costing most of a refresh plus the margin plus the lead
		// reaches back past the start of the previous one, and the frame the output is owed is then a
		// frame whose work should have started before the frame in front of it finished. The honest
		// answer is the next frame gyro can still be early for, and it is what decision 35's *target the
		// next deadline* already says one rung down — a deadline that cannot be met is dropped and the
		// one after it is aimed at, and an alarm is no different.
		//
		// `SequenceAfter(now + arming)` is that frame by construction: it is the earliest whose deadline
		// clears the reserve measured from now, so its record point is at or after now and every earlier
		// frame's is behind. In steady state it is below both floors above and changes nothing.
		const std::uint64_t armable = clock.SequenceAfter(Advanced(now, arming));

		// The floor is the whole of what `committed` is for here. A frame recorded and awaiting its
		// flip is one the clock has not observed, so the reach names it and the wake becomes its own
		// record point — an instant the loop has already served, and one it would be handed back on
		// every iteration until the flip lands. One frame past it is the answer.
		const Instant wakeAt = clock.WakeupAt(std::max({ reach, Owed(clock, committed), armable }), arming);

		return wakeAt == FrameClock::Unscheduled ? Wake::Never() : Wake::At(wakeAt);
	}

	// The composed cost of one frame at one tier, which is what `FrameClock::WakeupAt` means by its
	// reserve. The degenerate form of `Finish` with an idle device, and the two agree by construction —
	// a wake is computed for a future in which this frame's predecessor is no longer executing, so the
	// pipelining term has nothing to bite on there.
	[[nodiscard]] constexpr Duration Reserve(const Budget& budget, RenderMode mode) const noexcept
	{
		return Detail::Sum(Detail::Sum(Cpu(budget, mode), Gpu(budget, mode)), m_Policy.Margin);
	}

	// The same reserve with `TimingPolicy::Lead` on top, which is what an arming subtracts from a
	// deadline and the only place the lead appears.
	//
	// **`Reserve` is what the verdict measures against and this is what the alarm is set by, and the
	// two must differ or the alarm is set for the instant that is already too late.** Handing
	// `FrameClock::WakeupAt` the plain reserve arms at `deadline - Reserve`, and `Assess` then asks
	// whether `now + Reserve` clears the same deadline — an inequality that reduces to `now <=
	// armedAt`, which no wake that has to travel through the kernel and drain a socket can satisfy.
	// The lead is the difference between *when the work must start* and *when this thread must be
	// running*, and only the second is something the loop can ask a timer for.
	//
	// Planned rather than a parameter: the floor composite exists to be reachable from wherever the
	// loop happens to be, and arming for it would schedule gyro to arrive too late for the tier it
	// wants and exactly on time for the tier it settles for.
	[[nodiscard]] constexpr Duration Arming(const Budget& budget) const noexcept
	{
		return Detail::Sum(Reserve(budget, RenderMode::Planned), m_Policy.Lead);
	}

	// When work started now would be done: record on the frame thread while the device finishes what it
	// has, execute once it is free, and hold the margin over the whole of it.
	[[nodiscard]] constexpr Instant
	Finish(Instant now, Instant deviceFreeAt, const Budget& budget, RenderMode mode) const noexcept
	{
		return Project(now, deviceFreeAt, budget, mode).Finish;
	}

	// The same pipeline with both ends kept. `Finish` above is this with one of them dropped, so the two
	// cannot disagree by construction — which is the property worth having, since the loop reads one and
	// the verdict reads the other.
	[[nodiscard]] constexpr Projection
	Project(Instant now, Instant deviceFreeAt, const Budget& budget, RenderMode mode) const noexcept
	{
		const Instant recorded = Advanced(now, Cpu(budget, mode));
		const Instant executed = Advanced(std::max(recorded, deviceFreeAt), Gpu(budget, mode));

		return { .DeviceFreeAt = executed, .Finish = Advanced(executed, m_Policy.Margin) };
	}

	[[nodiscard]] constexpr const TimingPolicy& Policy() const noexcept { return m_Policy; }

private:
	// The frame this output owes: one past the later of what has reached the glass and what has been
	// spoken for. Saturating, because `SequenceAfter` answers the maximum on overflow and a floor
	// derived from that answer must not wrap around behind it.
	[[nodiscard]] static constexpr std::uint64_t Owed(const FrameClock& clock, std::uint64_t committed) noexcept
	{
		const std::uint64_t latest = std::max(clock.LastSequence(), committed);

		return latest == std::numeric_limits<std::uint64_t>::max() ? latest : latest + 1;
	}

	// The tier's own figure plus the part of the frame no tier reduces. Saturating for `Sum`'s reason:
	// both addends are figures this process measured, and an overflowed reservation is a value the
	// optimiser may assume away where a saturated one merely refuses every frame.
	[[nodiscard]] static constexpr Duration Cpu(const Budget& budget, RenderMode mode) noexcept
	{
		return Detail::Sum(
			budget.IrreducibleCpu(), mode == RenderMode::Planned ? budget.PlannedCpu() : budget.FloorCpu()
		);
	}

	[[nodiscard]] static constexpr Duration Gpu(const Budget& budget, RenderMode mode) noexcept
	{
		return mode == RenderMode::Planned ? budget.PlannedGpu() : budget.FloorGpu();
	}

	// The two instants come from the clock and never from the caller, so that a decision cannot name a
	// presentation the output was never going to make.
	[[nodiscard]] constexpr FrameDecision
	Decide(const FrameClock& clock, Admission verdict, std::uint64_t sequence, Projection projected) const noexcept
	{
		return { .Verdict = verdict,
			     .Sequence = sequence,
			     .Presentation = clock.PresentationAt(sequence),
			     .Deadline = clock.DeadlineAt(sequence),
			     .Finish = projected.Finish,
			     .DeviceFreeAt = projected.DeviceFreeAt };
	}

	TimingPolicy m_Policy{};
};

// Prints as planned frame 108 slack 2400000ns, which is the verdict, what it is about, and the one
// number that explains it.
template<>
struct std::formatter<FrameDecision>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const FrameDecision& decision, Context& context) const
	{
		auto out = std::format_to(context.out(), "{}", Name(decision.Verdict));

		if (decision.Sequence == FrameClock::NoSequence)
		{
			return std::format_to(out, " unanchored");
		}

		return std::format_to(out, " frame {} slack {}", decision.Sequence, decision.Slack());
	}
};

// The contract everything downstream assumes. The behaviour is swept in Timing.Test.cpp; what is here
// is the part a wrong answer changes silently.
static_assert(std::formattable<FrameDecision, char>, "A report prints the decision rather than <unprintable>");

// The order is the reduction, so a set held to its weakest member is a minimum rather than a rule
// somebody remembers to write.
static_assert(std::min(Admission::Planned, Admission::Floor) == Admission::Floor);
static_assert(std::min(Admission::Floor, Admission::Wait) == Admission::Wait);

// A reserve is the two devices and the margin, and the pipelined form agrees with it when nothing is
// in flight. The invariant is what lets a wake be computed from the cheap one.
static_assert(
	[] {
		Budget budget{ BudgetPolicy{ .InitialCpu = std::chrono::milliseconds{ 2 },
		                             .InitialGpu = std::chrono::milliseconds{ 5 } } };
		const Timing timing{ TimingPolicy{ .Margin = std::chrono::milliseconds{ 1 } } };
		const Instant now = Monotonic::FromNanoseconds(1'000'000'000);

		return timing.Reserve(budget, RenderMode::Planned) == std::chrono::milliseconds{ 8 } &&
	           timing.Finish(now, Instant{}, budget, RenderMode::Planned) ==
	               Advanced(now, std::chrono::milliseconds{ 8 });
	}(),
	"The reserve is the pipeline with an idle device"
);

// **The alarm is set earlier than the instant the verdict measures against, and that gap is the lead.**
// The two were one figure, which made this an equality — and an equality here is a loop that arms for
// the instant it has already run out of time at, so every frame it woke itself for fell to the floor
// composite. Asserted rather than swept because the failure is silent: both spellings compile, both
// produce frames, and the difference is only visible as a blur that switches off for one frame at a
// time on a machine doing nothing in particular.
static_assert(
	[] {
		Budget budget{ BudgetPolicy{ .InitialCpu = std::chrono::milliseconds{ 2 },
		                             .InitialGpu = std::chrono::milliseconds{ 5 } } };
		const Timing timing{ TimingPolicy{ .Margin = std::chrono::milliseconds{ 1 },
		                                   .Lead = std::chrono::milliseconds{ 3 } } };

		return timing.Arming(budget) == std::chrono::milliseconds{ 11 } &&
	           timing.Arming(budget) > timing.Reserve(budget, RenderMode::Planned);
	}(),
	"An arming leads the reserve it is derived from"
);

// A busy device delays execution and not recording, which is the whole of what the two figures buy
// over one. Recording ends at 1002ms, the device is busy until 1006ms, so execution runs 1006 to 1011
// and the margin lands the finish at 1012 rather than the 1008 a summed figure would have predicted.
static_assert(
	[] {
		Budget budget{ BudgetPolicy{ .InitialCpu = std::chrono::milliseconds{ 2 },
		                             .InitialGpu = std::chrono::milliseconds{ 5 } } };
		const Timing timing{ TimingPolicy{ .Margin = std::chrono::milliseconds{ 1 } } };

		return timing.Finish(
				   Monotonic::FromNanoseconds(1'000'000'000),
				   Monotonic::FromNanoseconds(1'006'000'000),
				   budget,
				   RenderMode::Planned
			   ) == Monotonic::FromNanoseconds(1'012'000'000);
	}(),
	"Recording overlaps the device and execution does not"
);

// An output that has never presented renders at once and evaluates at the instant it will be ready,
// which is decision 31's first frame after idle and is the one case the deadline sentinel is not the
// evaluation instant.
static_assert(
	[] {
		const Budget budget{ BudgetPolicy{ .InitialCpu = std::chrono::milliseconds{ 3 } } };
		const Timing timing;
		const FrameDecision decision =
			timing.Assess(FrameClock{}, budget, Monotonic::FromNanoseconds(1'000'000'000), FrameClock::NoSequence);

		return decision.Verdict == Admission::Planned && decision.Renders() &&
	           decision.Sequence == FrameClock::NoSequence &&
	           decision.Presentation == Monotonic::FromNanoseconds(1'003'000'000) &&
	           decision.Deadline == FrameClock::Unscheduled;
	}(),
	"An unanchored output presents as soon as it is ready and evaluates for that instant"
);
