#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "Core/Clock.h"
#include "Core/FrameSection.h"
#include "Core/Result.h"
#include "Core/Signal.h"
#include "Core/Time.h"
#include "Core/Trace.h"
#include "Core/Wake.h"
#include "Frame/Admission.h"
#include "Frame/Assign.h"
#include "Frame/Budget.h"
#include "Frame/Capture.h"
#include "Frame/Evaluator.h"
#include "Frame/FrameClock.h"
#include "Frame/Timing.h"
#include "Geometry/Region.h"
#include "Geometry/Space.h"
#include "Publication/Reader/Reader.h"
#include "Publication/Return.h"
#include "Publication/Ring.h"
#include "Seam/Capture.h"
#include "Seam/EventSource.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/PresentationInfo.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
// See Docs/Architecture.md#the-frame-loop and decisions 29, 30, 35, 36, 80, 82, 83, and 84.

// One iteration of the frame thread, and the `while` above it belongs to the composition root.
//
// Decision 80 is the whole of this object's shape. The step takes no `now` — it holds `IClock` and
// reads it once, which is Core/Clock.h's discipline — and it takes no readiness set, because the
// ordering that matters is *drain before evaluate* and a shim handed that responsibility would hold it
// in the one part of the loop the schedulability sweep never runs. So every source is drained on every
// iteration, the shim is permitted to wake spuriously, and what comes back is a `Wake`.
//
// **The order below is an argument and not a sequence.** A `Presented` that lands after `FrameClock`
// was read leaves the entire iteration scheduled against a prediction one frame stale, so the drain is
// first. Acquisition is once per iteration rather than once per output, because a window straddling
// two outputs would otherwise show two different client frames in one iteration — visible on exactly
// the configuration decision 28 exists to serve, while the staleness it trades away is bounded by one
// iteration. Costs are collected before anything is assessed, since a budget that moved is a budget
// this iteration should be deciding against. The watermark is posted last, because it is the statement
// that the frame thread has finished reading and it is not true until it has.
//
// **The device instant threads through the per-output pass, and that is decision 29 arriving as a
// loop variable.** GPU work from two outputs serialises on one queue however it was recorded, so an
// output admitted second starts executing when the first finishes rather than now. `Timing` takes that
// as `deviceFreeAt` and returns the next value of it on the decision; this holds one per rendering
// device and hands each output the figure for the queue it is actually on. Outputs are visited
// earliest deadline first, which is the schedule decision 61 says is gyro's rather than the kernel's.
//
// **Nothing here decides how much an output may spend.** That is admission control, it is solved at
// configuration change, and its output is an allocation. This runs every iteration and executes the
// plan it was given — the one runtime timing decision is `Timing::Assess`, and it is called rather
// than reimplemented.
//
// **Damage accumulates per output since its last *successful* present**, which is the invariant that
// makes decision 35's skip safe. A skipped frame that dropped its damage would corrupt the next one,
// so the region is cleared where the present succeeded and nowhere else — including on a refused
// record and on a refused present, both of which leave the output owing exactly what it owed before.
//
// **That is one of two damage regions and it is the one that answers whether to draw at all.** The
// other is per target, and a ring deeper than two is why: `AcquireTarget` hands back an image that was
// on the glass two frames ago, so everything in it outside this frame's damage is two frames old.
// Scissoring to what changed since the last present leaves the rest of that image as it was — which is
// a window that moved leaving a crisp copy of itself behind on every third frame. So each target
// carries a backlog, `Seam/Renderer.h`'s *union of every damage since it was last drawn*, and what the
// renderer is scissored to is the pending region joined with the backlog of the target being drawn.
//
// **Keyed to when a target was last *drawn complete*, not to when it was last shown, and that is what
// makes the two independent of every way a frame can be lost.** A skip, a refused record, a refused
// commit: none of them drew anything, so no backlog moves. A frame the host accepted and discarded
// *did* draw its target completely, so that target's backlog is honest and only the glass is wrong —
// which `OnMissed` already answers by damaging the whole output. The backlogs fold forward on the one
// exit that presented, and nowhere else.
//
// **What is emphatically not merged is `PresentLayer::Damage`.** The screen holds the last frame that
// reached it, so what differs from the screen is the pending region alone — the backlog is repair to
// an image nobody has seen, and reporting it would hand a plane or a host more damage than the
// picture actually changed by.
//
// **A frame that was wanted and never reached the glass is counted and named.** The three ways it can
// happen — a target the presenter listed but does not hold, a record the renderer would not make, a
// commit the presenter would not take — are all a return with nothing said, and a run of them is a
// black screen with every counter above it reporting success. That is the failure this loop is worst
// placed to explain and best placed to notice, because the reason arrives as an `Error` from whoever
// refused and is thrown away one line later. So the first one is kept and the rest are tallied.
//
// **Kept rather than logged, because Docs/Architecture.md#why-io_uring forbids the log call here.**
// spdlog on the frame path is the blocking operation the whole thread exists to avoid, and an `Error`
// is a code beside a `string_view` over a literal — trivially copyable, allocation-free, and legal
// inside the frame section for exactly that reason. The composition root reads it afterwards, which
// is the same route `Compositor.cpp` already carries a dispatch failure out on.
//
// **What turns a snapshot into something to draw is Frame/Evaluator.h**, which decision 82 puts on
// this side of the render seam and which this loop calls once per output it serves. The interface is
// inside this module rather than at the seam because it has no second implementation that is not a
// test: `Scene` publishes, `Frame` evaluates, and no backend is ever on the other end of it.
//
// **An evaluator reports what it cost, and the loop files it where no tier can take it away.**
// Decision 94 puts the walk in `Budget::IrreducibleCpu()` rather than in either mode's figure, and
// the measurement comes back through `DrawList` for the reason `Submission::RecordCost` comes back
// through `Submission` — the party that knows where the work started and stopped is the one that did
// it, and a loop timing it from outside would read the clock twice per output where Core/Clock.h
// asks for once per iteration.

// SPEC: sized rather than measured. Four rendering devices is past any configuration gyro has been
// pointed at, and it is fixed capacity because the frame section forbids growing it. Its counterpart
// `MaxOutputs` is Admission.h's, because the capacity is the admission set's size and these outputs
// are that set.
inline constexpr std::size_t MaxDevices = 4;

// SPEC: how many commits one output may have outstanding, and therefore how many published sequences
// this loop remembers on its behalf.
//
// `IPresenter::CommitDepth` already says it is *bounded in practice by the target ring, since every
// outstanding commit is holding one*. This is that sentence made a bound rather than an observation,
// because the loop now keeps a queue per output and a presenter answering a larger number would run
// off the end of it. An output whose presenter is more generous than the ring simply stops one short,
// which is where `AcquireTarget` would have stopped it anyway.
inline constexpr std::uint32_t MaxCommitsInFlight = MaxTargets;

// The report speaks for a fixed number of outputs and the loop schedules a fixed number, and the two
// constants live in modules that may not name each other — see `OutputsPerReport`. This is the one
// place both are visible, so it is where the agreement is checked; being wrong about it would be a run
// of outputs whose flips are never reported, which reads as clients that stop drawing.
static_assert(MaxOutputs <= OutputsPerReport, "Every output the loop schedules must fit in one report");
static_assert(MaxOutputs <= TracedOutputs, "Every output the loop schedules must have rows of its own");

// One output's frame-thread state: the two figures `Timing` composes, and the three facts about this
// output that neither of them can see.
//
// It observes its own presenter rather than being told what happened, because the signals are the
// backend's way of reporting and a loop that polled them would be asking a question the source has
// already answered. The connections are why this is neither copyable nor movable, and why the
// composition root constructs these in place.
class FrameOutput
{
public:
	FrameOutput() = default;

	FrameOutput(const FrameOutput&) = delete;
	FrameOutput& operator=(const FrameOutput&) = delete;

	// `device` indexes the queue this output's work serialises on, which is what makes decision 29's
	// term per device rather than per output. Two outputs on one GPU share an index; a headless output
	// beside a real one does not.
	void Bind(
		IPresenter& presenter,
		IRenderer& renderer,
		std::size_t device,
		const OutputConfiguration& configuration,
		FrameClockPolicy clockPolicy = {},
		BudgetPolicy budgetPolicy = {}
	)
	{
		m_Presenter = &presenter;
		m_Renderer = &renderer;
		m_Device = device < MaxDevices ? device : 0;

		m_Clock = FrameClock{ clockPolicy };
		m_Cost = Budget{ budgetPolicy };
		Adopt(configuration);

		m_OnPresented.ConnectTo<&FrameOutput::OnPresented>(presenter.Presented, *this);
		m_OnMissed.ConnectTo<&FrameOutput::OnMissed>(presenter.Missed, *this);
		m_OnReconfigured.ConnectTo<&FrameOutput::OnReconfigured>(presenter.Reconfigured, *this);
		m_OnTargetsInvalidated.ConnectTo<&FrameOutput::OnTargetsInvalidated>(presenter.TargetsInvalidated, *this);
	}

	[[nodiscard]] bool IsBound() const noexcept { return m_Presenter != nullptr && m_Renderer != nullptr; }

	[[nodiscard]] const FrameClock& Clock() const noexcept { return m_Clock; }

	[[nodiscard]] const Budget& Cost() const noexcept { return m_Cost; }

	[[nodiscard]] const OutputConfiguration& Configuration() const noexcept { return m_Configuration; }

	// The last frame this output spoke for — recorded, presented, and awaiting its flip. It is what
	// `Timing::Assess` will not let default, because the permissive reading draws frame eight twice.
	[[nodiscard]] std::uint64_t Committed() const noexcept { return m_Committed; }

	// Whether anything this output committed is still unanswered. The count behind it is what
	// `IPresenter::CommitDepth` is compared against; this is the question every other caller asks.
	[[nodiscard]] bool IsFlipPending() const noexcept { return m_InFlight != 0; }

	// How many commits are outstanding, and how many this output is allowed. A presenter that has
	// vanished allows none, which is the same answer as being full.
	[[nodiscard]] std::uint32_t InFlight() const noexcept { return m_InFlight; }

	// **Whether a client's own buffer is on a plane a display engine may still be reading** — the
	// commits queued behind the glass, and the one that reached it.
	//
	// It is the question Seam/Capture.h's texture half has to ask before it touches one. A readback
	// acquires a client's dmabuf from `VK_QUEUE_FAMILY_FOREIGN_EXT`, which is a claim that the foreign
	// user has finished with it — and a display engine scanning that buffer out has not. Forcing the
	// promotion ceiling to zero on a capturing frame is not enough on its own, because it changes what
	// *this* frame commits and says nothing about the planes the last one left on the screen.
	//
	// **A flip is where a plane starts being read rather than where it stops**, which is what the first
	// version of this guard had backwards. A commit is in flight from the present until the flip; the
	// buffers it named are being scanned out from the flip until a *later* commit that does not name
	// them flips over it. So the queue says where a buffer is about to be and `m_GlassPromoted` says
	// where one is now, and a capture is owed both — asking only the queue waits for the exact instant
	// the hazard begins and then reads. On a presenter at `CommitDepth` one, which is every DRM output
	// today, it is worse than a frame early: the queue is empty by construction on the iteration a frame
	// is drawn, so that half never fires at all and the press read a client's buffer out from under a
	// live scanout every time.
	// The glass half on its own, which only the trace asks for: the two waits are the same refusal and
	// wildly different situations, and a row that cannot tell them apart is a row that says a capture
	// was deferred without saying what it is waiting for.
	[[nodiscard]] bool ArePlanesOnGlass() const noexcept { return m_GlassPromoted; }

	[[nodiscard]] bool ArePlanesLive() const noexcept
	{
		if (m_GlassPromoted)
		{
			return true;
		}

		for (std::uint32_t index = 0; index < m_InFlight; ++index)
		{
			if (m_InFlightPromoted[index])
			{
				return true;
			}
		}

		return false;
	}

	[[nodiscard]] bool IsCommitFull() const noexcept
	{
		return m_Presenter == nullptr || m_InFlight >= std::min(m_Presenter->CommitDepth(), MaxCommitsInFlight);
	}

	[[nodiscard]] const Region<DeviceSpace>& Damage() const noexcept { return m_Damage; }

	// What the last iteration decided, kept so that a sweep or a log line can read the slack without
	// the loop having to report through a channel that does not otherwise exist.
	[[nodiscard]] const FrameDecision& Last() const noexcept { return m_Last; }

	// How many frames this output was owed, was admitted for, and never got. Not a missed deadline and
	// not a dropped frame: `Timing` said yes and something downstream said no.
	[[nodiscard]] std::uint64_t Refused() const noexcept { return m_Refused; }

	// Why the first one was refused, which is the one worth printing — a refusal is almost always a
	// standing condition rather than an event, so the hundredth carries no information the first did
	// not and the first is the one that names what the run started doing wrong.
	[[nodiscard]] const std::optional<Error>& FirstRefusal() const noexcept { return m_FirstRefusal; }

	// Damage from outside the scene — a backend that lost its targets, a console that drew over the
	// output, a first frame with nothing behind it.
	void AddDamage(const Region<DeviceSpace>& region) noexcept { m_Damage.Add(region); }

	// **Every backlog is dropped here, and this is the only place they are dropped.** Damaging the whole
	// output subsumes them by definition — a backlog is a subset of the output's own extent, so the join
	// the renderer is scissored to is the whole output whatever any of them held. Clearing here rather
	// than in `Discard` is what keeps that unconditional: `Discard`'s callers all damage the whole output
	// today, and a fourth that did not would quietly hand back the ghost this exists to prevent.
	void DamageWholeOutput() noexcept
	{
		m_Damage.Add(PixelRect<DeviceSpace>{ {}, m_Configuration.Resolution });

		for (Region<DeviceSpace>& backlog : m_Backlog)
		{
			backlog.Clear();
		}
	}

private:
	friend class FrameLoop;

	// One commit answered, not all of them. Saturating rather than asserting, because a host that
	// speaks twice about one frame is a host, and an underflow here would leave this output believing
	// it may never commit again.
	//
	// **This is also where the return leg's half of the frame is staged**, and it is why the loop keeps
	// a queue of published sequences rather than the one number `m_Committed` holds. The oldest
	// outstanding commit is the one this answers, so what comes off the front is the snapshot *that*
	// frame was drawn from — an output two commits deep would otherwise report the newer scene as
	// having reached a glass it has not, which on the far side is a frame callback handed to a client
	// whose pixels are still in the queue.
	//
	// Staged rather than posted, because the report crosses once per iteration and this arrives in the
	// drain at the top of one. Two flips in one drain keep the later, for `ReturnChannel::Stage`'s
	// reason: the sequence is monotone, so the newer one's derivation already contains the older's.
	void OnPresented(const PresentationInfo& info) noexcept
	{
		m_Clock.Observe(info);

		// The oldest outstanding commit is the one this flip answers, so these are the scene it drew and
		// the frame it was, and both are zero where the host answered a commit this loop never made.
		const std::uint64_t scene = m_InFlight > 0 ? m_InFlightSnapshots[0] : 0;
		const std::uint64_t frame = m_InFlight > 0 ? m_InFlightFrames[0] : 0;

		// **The glass row, at the host's timestamp, and it is a slice rather than a mark.** The stamp is
		// `info.PresentedAt` and not the one the feedback was drained at, because the flip happened at
		// the panel's vblank and a row stamped where it was *learned* moves with every late wake of this
		// loop: a stutter of the reader would read as a lateness of the host, which is the opposite of
		// what the row is for.
		//
		// **It is an extent and it merges, which is the difference between this and the mark it
		// replaces.** A mark said *a flip happened* and left a reader counting ticks to decide whether
		// the cadence was even. A slice that runs until the picture *changes* says how long a person was
		// looking at one thing — so every slice is one refresh wide on a compositor that is keeping up,
		// and a stutter is a wide block seen without measuring anything. That is the row
		// Docs/Experience.md's promise is read off, and it is the only row in the trace that is about
		// what happened rather than about what gyro did.
		//
		// **The name is the frame the flip showed, and keying it on the scene instead made this row
		// state the opposite of the truth.** A snapshot carries coefficient runs rather than pixels and
		// the evaluator solves them at each frame's own presentation instant, so a dozen refreshes from
		// one publication are a dozen *different pictures* — an animation running exactly as it should.
		// Merged on the scene, the row drew them as one block and claimed the screen had been frozen for
		// a fifth of a second: a seven-second capture of a smoothly animating gym came out as
		// thirty-three still frames. Keyed on the frame it merges only where the same frame was scanned
		// out twice, which is a repeat — and a repeat is exactly the stutter this row exists to show.
		//
		// **It is also the number every other row of that frame is named for**, so a search for
		// `frame 142` now reaches the pixels as well as the work, and the question *when was this on the
		// screen* is answered by one slice rather than by the far edge of a flight lane. A vblank that
		// showed a frame gyro did not draw names nothing and says so.
		if (frame != m_Glass)
		{
			TraceCloseAt(info.PresentedAt, m_TraceGlass);
			TraceOpenAt("frame", info.PresentedAt, m_TraceGlass, frame != 0 ? TraceTag(frame) : TraceLabel{});

			// **The two numbers that belong to this slice and must not be joined to it by name.** The
			// scene is dispatch's count and the refresh is the panel's, and printing either into the
			// name would put three identities in one string and make a search for any of them light up
			// the wrong rows; giving either a row of its own would be a counter stepping at flips, which
			// is the *two spellings of one fact* habit these rows were rewritten to lose. As arguments
			// they cost a click and answer two questions nothing else in the picture can: which
			// publication a person is actually looking at, and whether the frame landed on the refresh
			// it was aimed at — which is a subtraction against the name, done by the reader, on numbers
			// the loop had all along and used to discard.
			if (scene != 0)
			{
				TraceAttributeAt("scene", info.PresentedAt, scene, m_TraceGlass);
			}

			if (info.Sequence != 0)
			{
				TraceAttributeAt("refresh", info.PresentedAt, info.Sequence, m_TraceGlass);
			}

			// The interval the host reported on this flip — the number the clock schedules the next
			// deadline from, and the one field of the observation the trace otherwise discards. A value
			// far from the panel's real cadence explains a stall, and nothing else in the picture can
			// say the host promised one.
			if (info.Period > Duration::zero())
			{
				TraceAttributeAt(
					"host period", info.PresentedAt, static_cast<std::uint64_t>(info.Period.count()), m_TraceGlass
				);
			}

			m_Glass = frame;
		}

		// **The reader's subtraction, done once, because the reader cannot be watching.** The glass
		// slice's name is the refresh this frame was aimed at and the attribute beside it is the one it
		// landed on, and the difference being nonzero is the miss Open.md is hunting — one frame in some
		// fifteen hundred, which recurs about as often as the ring is long, so a person cannot catch it
		// by reaching for `SIGUSR1` in time. The mark makes the bad vblank findable by name, in this
		// trace and in a system trace merged over it; the trigger asks the recorder to keep the file,
		// where `--trace-on-miss` armed it to.
		//
		// Greater rather than unequal, deliberately. The retirement comment below describes a panel
		// whose counter steps per *flip* rather than per refresh, and on that machine the reported
		// sequence runs behind the name on every landing — a hunt armed there would spend its snapshot
		// on the driver's counting before the first real miss.
		if (frame != 0 && info.Sequence > frame)
		{
			TraceMarkAt("landed late", info.PresentedAt, m_Trace, TraceTag(info.Sequence - frame));
			TraceTrigger();
		}

		if (m_InFlight == 0)
		{
			return;
		}

		// **The far end of the flight lane, and it closes the wait nothing used to draw.** Between the
		// present that handed this frame over and this instant the frame was in no row at all, and on a
		// sixty hertz panel that gap is a whole refresh a person is trying to account for. The lane is
		// the commit slot, so how many lanes are occupied here *is* the queue depth.
		TraceCloseAt(info.PresentedAt, m_InFlightRows[0]);

		// **No counter beside it, which is the row earning its keep.** What a `shown` counter said was
		// *the scene on the glass is 12*, held between samples and stepped at each flip — which is
		// exactly what the slice above draws, with the number in the name and the duration as its
		// width. Two spellings of one fact is the thing that made these charts hard to read.
		// **The panel's own three ride along unchanged**, which is the whole of this side's part in
		// serving `wp_presentation`: the retrace counter, the period the output was measured at and the
		// flags saying how much of that was hardware. Copied rather than interpreted — a backend that
		// left a field alone has said it does not know, and turning that into a plausible number here is
		// exactly what `Seam/PresentationInfo.h` forbids.
		if (scene >= m_Presented.Sequence)
		{
			m_Presented = { .Sequence = scene,
				            .At = info.PresentedAt,
				            .Vblank = info.Sequence,
				            .Period = info.Period,
				            .Vsync = info.Vsync,
				            .HardwareClock = info.HardwareClock,
				            .ZeroCopy = info.ZeroCopy };
		}

		// **What left the queue is what the display engine is now reading.** This is the fact
		// `ArePlanesLive` cannot get from the queue, because the flip that removes a commit from it is
		// the same event that starts its planes being scanned out — and they stay that way until
		// something else flips over them, which may be many refreshes later or never.
		m_GlassPromoted = m_InFlightPromoted[0];

		--m_InFlight;

		for (std::uint32_t index = 0; index < m_InFlight; ++index)
		{
			m_InFlightSnapshots[index] = m_InFlightSnapshots[index + 1];
			m_InFlightFrames[index] = m_InFlightFrames[index + 1];
			m_InFlightRows[index] = m_InFlightRows[index + 1];
			m_InFlightPromoted[index] = m_InFlightPromoted[index + 1];
		}

		m_InFlightSnapshots[m_InFlight] = 0;
		m_InFlightFrames[m_InFlight] = FrameClock::NoSequence;
		m_InFlightRows[m_InFlight] = TraceThread;
		m_InFlightPromoted[m_InFlight] = false;

		// **Retired here, because a frame that has flipped is not one this output is still speaking
		// for.** `m_Committed` was only ever cleared by `Discard`, on the reading that the anchor would
		// catch up to it on its own — every observation moves `LastSequence` forward, so `Owed`'s
		// `max(anchor, committed) + 1` converges the moment the flip lands and the field is harmless
		// until then. That holds where the reported sequence counts refreshes, and it is false where it
		// counts gyro's own flips — which is what an msm laptop panel was measured doing, four counts
		// per flip whether the flip came a refresh or six seconds after the last one, while the display
		// engine ran a continuous sixty hertz underneath. A loop that slept through forty refreshes
		// therefore names its next frame forty ahead and is answered with the fourth, the anchor never
		// reaches what was committed, `Owed` stays pinned to a frame the panel is tens of seconds from,
		// and every iteration after it fails both tiers and skips. What that looks like is a compositor
		// that draws four frames in twenty seconds and then stops — not a stutter, a screen that stops
		// moving while the loop is still awake and still deciding, once per wake, not to draw.
		//
		// SPEC: *why* the counter behaves that way is not settled — a vblank interrupt refcounted off
		// while nothing is presenting, on a CRTC with no hardware frame counter to catch up from, is the
		// candidate and wants reading rather than asserting. The repair below does not depend on the
		// answer: it is wrong to be owed a frame past one the panel has already shown, whatever the
		// driver counts.
		//
		// So the number is derived from the queue rather than left to expire: the newest frame still
		// awaiting a flip, and nothing where none is. That is what `Committed()` already says it is, and
		// on a panel whose counter free-runs it changes nothing — the last flip retires the last commit
		// and `Owed` reads the anchor, which is the same frame the stale field named.
		m_Committed = m_InFlight > 0 ? m_InFlightFrames[m_InFlight - 1] : FrameClock::NoSequence;
	}

	// The frame was accepted and never shown, which Seam/Presenter.h argues has to be its own signal.
	//
	// **The clock is invalidated rather than left alone, and that is the half worth reading twice.**
	// `FrameClock` predicts the next deadline from a run of observations, and a discarded frame is not
	// a late observation — it is a boundary that produced none. Left to run, the prediction would
	// carry a hole it has no way to see, so every deadline after it is derived from a cadence that
	// never happened. `Invalidate` is exactly the state that says *start again from the next thing
	// that really lands*, and this is its second caller after a mode set.
	//
	// The whole output is damaged for `OnTargetsInvalidated`'s reason: what was drawn is not on a
	// screen, and the next frame's damage would otherwise be relative to a picture nobody saw.
	void OnMissed() noexcept
	{
		// Decision 124's fourth signal, and the one worth seeing on a timeline: a frame that was drawn,
		// accepted and never shown leaves every counter above it reporting success.
		TraceMark("discarded", m_Trace);

		m_Clock.Invalidate();
		Discard();
		DamageWholeOutput();
	}

	// Decision 73: the transition completes as an event some time later, and this is that event. The
	// clock re-anchors from what was achieved rather than from what was asked for, and the record is
	// invalidated because a cost measured under the old mode is a cost from another configuration —
	// which is what `Budget`'s generation exists to say.
	//
	// **The whole output is damaged again, after the adoption rather than before it, and the ordering is
	// the entire content of the line.** `TargetsInvalidated` has already damaged the whole output — but
	// it ran while this held the *old* configuration, so what it accumulated was the old mode's extent.
	// Seam/Presenter.h fixes that order: the images go first and the transition completes second, since
	// the old target descriptors stop being valid before the new set exists. So a mode that grew leaves
	// the difference between the two extents outside the damage region, and the first frame after the
	// mode set scissors everything past the old resolution away — on a target that was just reallocated
	// and holds nothing. Re-damaging here costs one rectangle on a path that already invalidated
	// everything, and the alternative is a band of undefined pixels that appears only on a resolution
	// increase.
	void OnReconfigured(const OutputConfiguration& achieved) noexcept
	{
		Adopt(achieved);
		m_Clock.Invalidate();
		m_Clock.Configure(achieved);
		m_Cost.Invalidate();
		Discard();
		DamageWholeOutput();
	}

	// The targets are gone, so anything recorded against one is gone with it. The output owes a whole
	// frame afterwards, because there is no longer a previous frame for damage to be relative to.
	void OnTargetsInvalidated() noexcept
	{
		Discard();
		DamageWholeOutput();
	}

	// Allocation-free by construction and deliberately so: this is called from inside the frame
	// section, and `std::optional<Error>` holds an int and a `string_view` over a literal.
	void Refuse(const Error& why) noexcept
	{
		++m_Refused;

		if (!m_FirstRefusal)
		{
			m_FirstRefusal = why;
		}
	}

	void Adopt(const OutputConfiguration& configuration) noexcept
	{
		m_Configuration = configuration;
		m_Clock.Configure(configuration);
	}

	// Everything in flight, dropped. `Committed` goes with it: a frame nobody will flip is not one this
	// output has spoken for, and leaving it would have `Timing` schedule around a frame that will never
	// arrive.
	void Discard() noexcept
	{
		// **Instrumented because it is a silent reset of the very number a refused commit is read
		// against.** Tagged with what it is dropping: a non-zero here is a commit the panel may still be
		// holding, forgotten by the only party that was counting it.
		TraceMark("queue discarded", m_Trace, TraceTag(m_InFlight));

		// **Closed here rather than left to the writer, because these are the one unbalanced end that is
		// not the window's fault.** A truncated span at either edge of the ring is the last thirty
		// seconds honestly reported; a flight lane whose frame was dropped by a mode set is a claim that
		// the panel is still working on it, and left open it runs to the end of the trace as the widest
		// slice in the picture. Closed at now, which is when the frame stopped being in flight.
		for (std::uint32_t index = 0; index < m_InFlight; ++index)
		{
			TraceClose(m_InFlightRows[index]);
		}

		m_InFlight = 0;
		m_Committed = FrameClock::NoSequence;

		// The sequences those commits were drawn from go with them, because a frame nobody will flip is
		// one this output never presented. `m_Presented` deliberately stays: it is a flip that already
		// happened and is still owed to dispatch, and dropping it here would lose a frame callback on
		// the iteration a mode set landed in. `m_GlassPromoted` stays for the opposite reason — it is
		// what this output can no longer vouch for, and the answer that costs a deferred capture is the
		// one to be wrong with.
		m_InFlightSnapshots = {};
		m_InFlightFrames = {};
		m_InFlightRows = {};
		m_InFlightPromoted = {};

		// The ruler restarts rather than bridging the discontinuity. See `m_Tiled`.
		m_Tiled = {};

		// An index into a set that no longer exists. `OnTargetsInvalidated` is the case this is here for
		// — the images are released before the new ones exist, so a held index names memory that is gone
		// and the next attempt has to ask the new set for one of its own.
		m_Acquired.reset();
	}

	IPresenter* m_Presenter = nullptr;
	IRenderer* m_Renderer = nullptr;
	std::size_t m_Device = 0;

	// Which row this output's work is drawn on. Assigned by `FrameLoop::Bind` rather than taken by
	// `Bind` above, because an output's *position in the loop's set* is the loop's knowledge and not
	// this object's — the same reason `m_Device` is passed in and this is not.
	std::uint16_t m_Trace = TraceThread;

	// The GPU work this output's composites cost, which is not on any thread and so cannot be a slice
	// on one. Travels out on the record request because the timestamps that fill it resolve frames
	// later, by which point the renderer has drawn for other outputs.
	std::uint16_t m_TraceGpu = TraceThread;

	// The refresh this output is aiming at, and what is on its glass.
	std::uint16_t m_TraceGrid = TraceThread;

	// The backend's stretch of the flight, handed out on every present because the backend serves one
	// output and does not know which — Seam/Presenter.h's `PresentTrace`, which is Seam/Renderer.h's
	// `RecordRequest` reason.
	std::uint16_t m_TraceCommit = TraceThread;
	std::uint16_t m_TraceGlass = TraceThread;

	// Where this output's refresh ruler has been drawn up to, so the next tile abuts it exactly. The
	// epoch is *no chain yet*, which is the state a mode set returns it to — the deadlines on the far
	// side of one are not continuous with the deadlines before it, and a tile bridging the two would
	// draw a refresh that never happened.
	Instant m_Tiled{};

	// The first of this output's flight lanes; the rest are the ones after it, which Core/Trace.h
	// numbers consecutively for exactly this. A lane is picked by rotation rather than by queue
	// position, because a queue that pops from the front renumbers everything behind it and a slice
	// has to close on the row it opened on.
	std::uint16_t m_TraceFlight = TraceThread;
	std::uint32_t m_FlightLane = 0;

	OutputConfiguration m_Configuration{};
	FrameClock m_Clock{};
	Budget m_Cost{};

	std::uint64_t m_Committed = FrameClock::NoSequence;

	// Commits accepted and not yet answered. A count rather than the flag it was, because the rule it
	// enforces is the presenter's — see `IPresenter::CommitDepth`, and decision 135.
	std::uint32_t m_InFlight = 0;

	// What each outstanding commit was drawn from, oldest first, as a queue of exactly `m_InFlight`
	// entries. It is the published sequence rather than the frame clock's, which is the one distinction
	// this whole field exists to keep: `m_Committed` is what the schedule reasons about and this is what
	// the dispatch side turns back into surfaces.
	std::array<std::uint64_t, MaxCommitsInFlight> m_InFlightSnapshots{};

	// And which frame each of them *is*, shifted with them. The two counts are not interchangeable and
	// keeping only the first is what left the glass row unable to say what it was showing: this is the
	// frame clock's sequence, which every row of that frame is named for, and the one above is the
	// dispatch thread's, which is what a client is owed a callback against.
	std::array<std::uint64_t, MaxCommitsInFlight> m_InFlightFrames{};

	// The flight lane each of those commits was drawn on, shifted with them. Kept beside the queue
	// rather than derived from a position in it for the reason `m_TraceFlight` gives.
	std::array<std::uint16_t, MaxCommitsInFlight> m_InFlightRows{};

	// Whether each of those commits carried a client's buffer on a plane, shifted with them. One bit
	// rather than which ids, because the only question ever asked of it is `ArePlanesLive` — a capture
	// waits for *every* plane to stop being read rather than for the one window it wants, since the
	// buffer it wants is exactly the one most likely to have been promoted. The bit outlives the queue:
	// the shift-out in the drain is what sets `m_GlassPromoted`.
	std::array<bool, MaxCommitsInFlight> m_InFlightPromoted{};

	// The most recent flip this output has not yet reported, staged in the drain and cleared by the post
	// that carries it.
	PresentedFrame m_Presented{};

	// Which frame the glass row is currently showing, so that a flip repeating it extends the slice
	// instead of starting another. It survives a `Discard` deliberately: a mode set does not change what
	// is on the panel, and closing the slice there would draw a gap where a person saw a picture.
	std::uint64_t m_Glass = 0;

	// Whether the commit on the glass put a client's buffer on a plane, which is the half of
	// `ArePlanesLive` the queue cannot hold. It survives a `Discard` for `m_Glass`'s reason turned into
	// a safety: a mode set is the moment this output stops being able to say what the display engine is
	// holding, and of the two answers available the conservative one costs a capture deferred until the
	// next flip while the optimistic one costs the panel.
	bool m_GlassPromoted = false;

	Region<DeviceSpace> m_Damage{};

	// Last frame's partition, kept for one reason: what the composite is responsible for changed.
	//
	// **A promotion has no state and its consequences do.** Decision 152's partition is recomputed from
	// nothing every frame, which is what makes it glitch-free — but the composite that drew a window
	// last frame and does not draw it this frame has to repaint where it was, and the composite that is
	// about to stop being covered by a promoted layer has to repaint what was under it. Neither is
	// visible in this frame's damage, because nothing in the *scene* changed.
	//
	// So a partition that differs from the last one damages the whole output. That is deliberately the
	// blunt answer: promotion changes when a window starts or stops moving, or when a menu opens over a
	// video, which is a handful of frames in a session — and each costs one full repaint, which is
	// decision 35's budget exactly. Tightening it to the union of the quads that changed sides is an
	// Open.md entry rather than a silence.
	Partition m_Partition{};

	// The published sequence this output was last evaluated against, or zero before it has drawn
	// anything.
	//
	// **It is what makes a scene that changed while the loop was idle get drawn**, and it is here rather
	// than left to the evaluator because the evaluator only runs on an output the loop has already
	// decided to serve. Damage is what a frame is owed *for*, and until an output is evaluated there is
	// none — so an idle loop asking *is a frame wanted?* from damage and the scene's own wake alone
	// answers no, whatever arrived in the ring. The scene wake cannot cover it either: a still window
	// appearing on a still desktop publishes a schedule that says *nothing falls due*, which is true and
	// is not the question.
	//
	// What that cost before this existed was the whole feature: a client opened a window on a settled
	// desktop and nothing was ever composited again. It is cheap to be right about because a publication
	// happens only where the world changed — dispatch steps when something wakes it — so this is never
	// the reason a quiet machine draws.
	std::uint64_t m_Drawn = 0;

	// What each target is stale by *over and above* `m_Damage`, which is what makes the two disjoint
	// questions rather than two spellings of one: `m_Damage` is owed to the glass and decides whether a
	// frame is wanted, and this is repair owed to an image and decides only how much of it to redraw. A
	// `Wants` that consulted these would find the two targets it is not presenting permanently dirty
	// with each other's history and never let the output idle.
	//
	// So an output that settles and then wakes on one small change redraws more than moved for the first
	// few frames, as each target in turn cashes in what it accumulated before the scene went quiet. That
	// is bounded by the ring depth and it is the bill for the frames that were skipped, not a leak.
	std::array<Region<DeviceSpace>, MaxTargets> m_Backlog{};

	FrameDecision m_Last{};

	std::uint64_t m_Refused = 0;
	std::optional<Error> m_FirstRefusal{};

	// Held between iterations only while an attempt is owed one: acquired above, released by a present
	// or by a discard, and empty everywhere else.
	std::optional<std::uint32_t> m_Acquired{};

	Connection<const PresentationInfo&> m_OnPresented;
	Connection<> m_OnMissed;
	Connection<const OutputConfiguration&> m_OnReconfigured;
	Connection<> m_OnTargetsInvalidated;
};

class FrameLoop
{
public:
	FrameLoop(
		const IClock& clock,
		const SnapshotRing& ring,
		ReturnChannel& returns,
		IEvaluator& evaluator,
		Timing timing = {}
	) noexcept
		: m_Clock{ &clock }, m_Ring{ &ring }, m_Returns{ &returns }, m_Evaluator{ &evaluator }, m_Timing{ timing }
	{}

	FrameLoop(const FrameLoop&) = delete;
	FrameLoop& operator=(const FrameLoop&) = delete;

	// The outputs the loop schedules, owned by the composition root because it owns their lifetime and
	// their connections. Replaced rather than mutated on a hotplug, which is the same statement
	// decision 84 makes about the snapshot's runs from the other side.
	void Bind(std::span<FrameOutput> outputs) noexcept
	{
		m_Outputs = outputs.first(std::min(outputs.size(), MaxOutputs));

		for (std::size_t index = 0; index < m_Outputs.size(); ++index)
		{
			m_Outputs[index].m_Trace = TraceOutput(index);
			m_Outputs[index].m_TraceGpu = TraceGpu(index);
			m_Outputs[index].m_TraceGrid = TraceGrid(index);
			m_Outputs[index].m_TraceGlass = TraceGlass(index);
			m_Outputs[index].m_TraceCommit = TraceCommit(index);
			m_Outputs[index].m_TraceFlight = TraceFlight(index, 0);
		}
	}

	// The sources the loop drains, at whatever granularity the backend's descriptors actually have.
	// Decision 83 puts dispatch's publication among them: it is the one that wakes an idle frame
	// thread, and it is a source rather than a mechanism precisely so that this signature does not
	// change to accommodate it.
	void Listen(std::span<IEventSource* const> sources) noexcept { m_Sources = sources; }

	// Where a debug capture's pixels go, or null where the run was not asked for captures.
	//
	// **Owned by the composition root, because the slab and the writer thread are.** Seam/Capture.h
	// carries the whole argument for why this is not the day-to-day screencapture and must not become
	// it; what the loop contributes is the two things only it can — forcing the captured frame to
	// composite the entire screen, and reading the target back between the record and the present,
	// which is the last moment the pixels are provably the ones about to be latched.
	void Capture(ICaptureSink* sink) noexcept { m_Capture = sink; }

	// **`--no-planes`: refuse decision 152's partition for the whole run.** The promotion is recomputed
	// per frame with nothing carried over, so switching it off is a ceiling of zero rather than a mode
	// — the same lever a captured frame already pulls for itself, held for every frame instead of one.
	//
	// It is here rather than in `Timing` because it is not a policy about when to draw; it is a claim
	// about which hardware draws, and the only party that can state it is the one that asks the
	// presenter for its ceiling.
	void Planes(bool promote) noexcept { m_Planes = promote; }

	// One iteration. Returns the wake the next one is owed at; `Wake::Never()` arms nothing.
	[[nodiscard]] Wake Step()
	{
		// **The iteration is one slice and everything below is inside it**, which is what makes the trace
		// answer the question it exists for: a frame that arrived late is either an iteration that
		// started late or one that took too long, and those are opposite problems with opposite fixes.
		// The span begins before the drain because a source that blocks is the first of the two.
		const TraceSpan iteration{ "iteration" };

		{
			const TraceSpan drain{ "drain" };

			// First, and for correctness rather than tidiness. See this file's header.
			for (IEventSource* const source : m_Sources)
			{
				if (source != nullptr)
				{
					(void)source->Drain();
				}
			}
		}

		const Instant now = m_Clock->Now();

		// **How much of the lead survived, which is the only figure that sizes the lead.**
		// `TimingPolicy::Lead` is what the previous iteration held back from the deadline so that this
		// one would be running before the work had to start; this is what is left of it now that the
		// kernel has dispatched the thread and the drain above has finished. Positive is the lead doing
		// its job and negative is the loop already late, which is the sample that says the figure is too
		// small — and because the record-time check is measured from this instant, a negative one here
		// is a frame at the floor composite a few lines below.
		//
		// **It is the loop's own request rather than the ring's timeout**, which is the honest end to
		// measure: the composition root reduces this against the backend's next event and may sleep for
		// less, so the ring's number would answer *did the timer fire on time* where this answers *was I
		// running when I said I had to be*. Only the second one is what the policy is a promise about.
		//
		// Nothing is emitted when nothing was armed. A settled world names no instant, so there is no
		// lead to have spent, and a zero there would be a sample of a promise nobody made.
		if (m_Armed.Which == Wake::Kind::Timed)
		{
			TraceElapsed("lead", Elapsed(now, m_Armed.When));
		}

		std::array<Instant, MaxDevices> deviceFree{};
		std::array<std::uint8_t, MaxOutputs> order{};
		std::size_t due = 0;

		{
			// Core/FrameSection.h names this span exactly: from acquiring the published snapshot through
			// `Present`. The drain is outside it because a backend reading its own descriptor is not on
			// the frame path in the sense decision 36 means, and the fold is outside it because it is
			// arithmetic over state the iteration has already finished producing.
			const FrameSection guard;

			Acquire();
			CollectCosts();

			TraceCount("held", static_cast<std::int64_t>(m_Held));

			due = OrderByDeadline(order);

			// **The whole device's schedule, before any of it is served.** Each output is held to when
			// its *queue* must begin rather than to when it alone could, and that is one instant shared
			// by everything on the card — so it cannot be composed as each output is reached, because a
			// member folded in later moves the instant the members before it are held to. Costs are
			// collected above this for the same reason they are collected before anything is assessed: a
			// budget that moved is a budget this iteration should be reserving against.
			Schedule({ order.data(), due });

			for (std::size_t position = 0; position < due; ++position)
			{
				Serve(m_Outputs[order[position]], order[position], now, deviceFree);
			}

			// Last: it is the statement that the frame thread has finished reading, and it is what
			// releases the dispatch thread to reclaim. Buffer releases join it when there is a protocol
			// layer to mint the ids — they are a different lifetime, since a buffer recorded into a
			// texture is held by GPU work rather than by the snapshot, and posting them here would
			// release them a frame early.
			//
			// **The presented run rides beside it and is not the same statement**, which is the whole
			// reason decision 75 keeps two numbers rather than one. The watermark is what the frame
			// thread has finished *reading*; a presented sequence is what an output has finished
			// *showing*, and the second trails the first by however deep that output's commits are. A
			// dispatch side that derived frame callbacks from the watermark would send them to clients
			// whose pixels are still in a queue.
			(void)m_Returns->Post(m_Held, {}, Presentations());
		}

		// **Folded again, because the frames it is about are not the frames that were just served.** The
		// pass above reserved for what each output owed on the way in; the outputs that drew now owe the
		// frame after, and the alarm has to name that batch rather than the one it has already run. The
		// order is untouched by any of it — `OrderByDeadline` reads the anchor, and only a flip moves an
		// anchor — so what changed between the two calls is exactly the frames, which is what the second
		// call is for.
		Schedule({ order.data(), due });

		m_Armed = Fold(now, deviceFree);

		return m_Armed;
	}

	[[nodiscard]] std::uint64_t Held() const noexcept { return m_Held; }

	// How many frames this loop has reported as having reached the glass, over every output.
	//
	// **It is the doorbell's condition and nothing else reads it.** The return channel carries no
	// descriptor — Dispatch/Loop.h's `PublishRetryInterval` is the same hole seen from the other side —
	// so the composition root is what wakes the dispatch thread when a report has something in it that a
	// client is waiting on, and this counter moving is how the root knows this iteration was one of
	// those. A count rather than a flag because the root compares it across a step, exactly as it
	// already compares dispatch's publication count to decide whether to ring decision 83's doorbell in
	// the other direction.
	[[nodiscard]] std::uint64_t Shown() const noexcept { return m_Shown; }

	[[nodiscard]] const SnapshotReader& Snapshot() const noexcept { return m_Snapshot; }

private:
	std::uint64_t m_Shown = 0;

	// Gather what each output staged in the drain, and clear it as it goes.
	//
	// **Clearing here is what makes a report say *no news* rather than repeating itself.** A report goes
	// out every iteration whether or not anything flipped, so an output that has not presented since the
	// last one leaves a zero — which `ReturnChannel::Stage` will never let overwrite a real sequence and
	// which the reader takes as silence. Repeating the last pair instead would be indistinguishable from
	// a second flip of the same scene, and the timestamp would drift a frame further from the truth on
	// every iteration that did nothing.
	//
	// It is mutable and allocation-free by construction: a fixed array of sixteen pairs on the stack, in
	// the same shape as the device instants above it, because the frame section forbids anything else.
	[[nodiscard]] std::span<const PresentedFrame> Presentations() noexcept
	{
		for (std::size_t index = 0; index < m_Outputs.size(); ++index)
		{
			m_Presentations[index] = m_Outputs[index].m_Presented;
			m_Outputs[index].m_Presented = {};

			m_Shown += m_Presentations[index].Sequence != 0 ? std::uint64_t{ 1 } : std::uint64_t{ 0 };
		}

		return { m_Presentations.data(), m_Outputs.size() };
	}

	void Acquire() noexcept
	{
		const AcquiredSnapshot acquired = m_Ring->Acquire(m_Held);

		if (!acquired.IsNewer())
		{
			return;
		}

		m_Held = acquired.Sequence;
		m_Snapshot = SnapshotReader{ acquired.Bytes };

		// **The flow the whole two-thread picture hangs off.** The sequence is the same number dispatch
		// stamped on the publish that produced this scene, so a reader following the arrow lands on the
		// `author` slice that wrote what is about to be drawn — which is how *the frame is stale* and
		// *the frame is late* stop looking alike.
		//
		// **The one arrow left in the whole picture, and it is here because it joins two counts.** A
		// publication is numbered by the dispatch thread and a frame by the panel it is shown on, so
		// nothing in the name of one can find the other — which is exactly the join a line is for.
		// Every other flow this trace used to carry said only *these slices are the same frame*, and
		// they are all tags now: the number is spelled into the name and a reader joins the rows by
		// reading them.
		//
		// **Inside the check rather than above it, and that is the difference between an arrow and a
		// thicket.** Perfetto chains every event carrying a flow id, so a mark re-emitted on an
		// iteration that acquired nothing links the same publish to itself again: a ten-second trace
		// held one snapshot for a hundred and eighteen iterations and drew a hundred and eighteen
		// arrows, which is a picture with no information in it and one a reader has to disbelieve
		// before they can read anything else. What the scene *is* between acquisitions is the `held`
		// counter, sampled every iteration precisely because it is a state and not an event.
		TraceMark("acquired", TraceThread, TraceFlow(TraceDomain::Scene, m_Held));
	}

	// Every renderer reports what its finished work actually cost, filed against the generation it was
	// measured under. A cost from a superseded configuration is dropped by `Budget` rather than tested
	// for here, which is the whole reason that generation exists.
	void CollectCosts() noexcept
	{
		for (FrameOutput& output : m_Outputs)
		{
			if (!output.IsBound())
			{
				continue;
			}

			std::array<GpuCost, MaxCostsPerIteration> costs{};
			const std::size_t collected = output.m_Renderer->CollectCosts(costs);

			for (std::size_t index = 0; index < std::min(collected, costs.size()); ++index)
			{
				(void)output.m_Cost.ObserveGpu(costs[index].Mode, costs[index].Generation, costs[index].Cost);
			}
		}
	}

	// Earliest deadline first, over the outputs' own next deadlines. Insertion sort because the set is
	// tiny, fixed, and nearly ordered from one iteration to the next — and because nothing inside the
	// frame section may allocate a comparator's scratch.
	[[nodiscard]] std::size_t OrderByDeadline(std::array<std::uint8_t, MaxOutputs>& order) const noexcept
	{
		std::size_t count = 0;

		for (std::size_t index = 0; index < m_Outputs.size(); ++index)
		{
			if (!m_Outputs[index].IsBound() || !m_Outputs[index].m_Configuration.Powered)
			{
				continue;
			}

			const Instant deadline = m_Outputs[index].m_Clock.NextDeadline();
			std::size_t position = count;

			while (position > 0 && m_Outputs[order[position - 1]].m_Clock.NextDeadline() > deadline)
			{
				order[position] = order[position - 1];
				--position;
			}

			order[position] = static_cast<std::uint8_t>(index);
			++count;
		}

		return count;
	}

	void Serve(FrameOutput& output, std::size_t index, Instant now, std::array<Instant, MaxDevices>& deviceFree)
	{
		// **Nothing is opened before the decision, because a slice cannot be named for what it did not
		// know yet.** This row used to carry a `serve` span per wake, and two thirds of them were four
		// microseconds long with nothing inside: the loop wakes about three times a refresh, finds the
		// panel still busy with the frame in front, and goes back to sleep. Three identical blocks per
		// vblank of which one is the frame is the single thing that made this chart unreadable — a
		// reader had to open each one to find out which.
		//
		// So a wake that declines costs one mark that says *why*, and a wake that draws opens a slice
		// named for the frame. Six ways out of this function and every one of them now says something,
		// where before there were two marks and three silent returns.
		Instant& free = deviceFree[output.m_Device];
		const FrameDecision decision = m_Timing.Assess(output.m_Clock, output.m_Cost, now, output.m_Committed, free);

		output.m_Last = decision;

		// A commit the presenter cannot take yet is the disqualifier Architecture.md names, and it is
		// separate from the arithmetic one `Assess` answers: it is the backend's condition rather than
		// the schedule's, and how many it will take is the backend's to say. One is KMS's rule — a
		// second nonblocking commit on a CRTC that has not flipped is refused — and a nested window
		// answers two, because its completion arrives from the host a whole refresh after the frame it
		// is about.
		if (output.IsCommitFull())
		{
			// **A mark rather than the wait this used to open, because the flight lanes draw the wait
			// properly now.** The old span ran from the first blocked wake to the flip that freed the
			// slot, which meant a reader was shown the *waiting* and never the frame being waited on —
			// and it was gated behind `Wants` to stop an idle panel painting the lane solid, which in
			// practice meant it never fired at all: a seven-second capture with eight hundred blocked
			// wakes in it had an empty row where the explanation should have been. Core/Trace.h's
			// flight lanes hold the frame itself from submission to glass, so the length is drawn by
			// the thing that has it and this only has to say the loop came back too early.
			TraceMark("queue full", output.m_Trace);

			return;
		}

		if (!decision.Renders())
		{
			// Named apart from the wait above because they are different failures wearing one `return`.
			// A full commit queue is an output waiting on the host or the panel and is ordinary; a
			// verdict that declines is decision 35's third branch, which is gyro deciding it cannot fit
			// the frame it owes.
			TraceMark("over budget", output.m_Trace);

			return;
		}

		// **The arming enforced, rather than merely computed.** `Assess` says the work fits before the
		// deadline, and inside one refresh it fits from the instant the previous flip retires — so a
		// wake that came from a socket, an input device or a flip, rather than from this output's own
		// timer, recorded the frame as much as fourteen milliseconds before the point the policy named.
		// `Admission::Wait` cannot see it: that verdict is a reach behind the frame owed, and work
		// starting early in the same refresh still reaches exactly it.
		//
		// **What a person loses by recording early is a refresh of the pointer.** Anything a spring
		// solves is evaluated at the predicted presentation and does not care when the work ran, but a
		// pointer position and a client's committed pixels are model values held in the snapshot the
		// record reads: starting a refresh early publishes a cursor a refresh behind the hand that
		// moved it, with nothing downstream to recover it. The commit also lands next to the previous
		// flip, which is where the panel refuses a second one.
		//
		// So the frame waits for its own record point and the next `WakeFor` arms for exactly this
		// instant, which is why declining here does not spin.
		//
		// **The point is the device's rather than this output's, which is the whole of what two monitors
		// on one card changed.** A record point prices one composite against an idle GPU, and outputs
		// sharing a queue serialise — so this used to be enforced only where an output had its card to
		// itself, and a second monitor was served from wherever the loop happened to wake, drawing a
		// pointer position a whole refresh before the glass showed it. `Schedule` composes the instant
		// the *queue* must begin at instead, and every output on that queue is held to it: they are
		// served back to back as they always were, from a place the policy chose rather than from
		// whichever flip or key press got here first.
		const Instant recordAt = m_Gated[index] ?
		                             m_Timing.RecordPoint(output.m_Clock, m_Arming[index], decision.Sequence) :
		                             FrameClock::Unscheduled;

		if (recordAt != FrameClock::Unscheduled && now < recordAt)
		{
			TraceMark("early", output.m_Trace);

			return;
		}

		// **Whether this output owes a capture, asked once and used three times.** It decides the ceiling
		// below, it forces the damage to the whole screen, and it is what the readback after the record
		// is conditional on — and asking the sink once keeps those three from disagreeing inside one
		// frame.
		//
		// **Above the `Wants` gate rather than beside its other two uses**, because a screen nobody is
		// animating owes no frame and would otherwise never take the picture: the damage this adds is
		// what the gate reads. It is below the three gates above it on purpose — those are *can a frame
		// be made at all*, and a capture that jumped a full commit queue or an unscheduled clock would be
		// a debug verb rearranging the schedule it was pressed to look at.
		const bool capturing = m_Capture != nullptr && m_Capture->Wanted(static_cast<std::uint32_t>(index));

		if (capturing)
		{
			// The composite is only obliged to draw the damage, and a capture wants the picture rather
			// than the delta: without this the file is last frame's target with this frame's changes
			// painted into it, which is *nearly* right and therefore the worst kind of wrong in an image
			// somebody is about to reason about.
			output.m_Damage.Add(PixelRect<DeviceSpace>{ {}, output.m_Configuration.Resolution });
		}

		// `Assess` answers whether a frame *can* be made and never whether one is *wanted* — it is
		// decision 35's check and it never asks what anything costs to want. This is the other half, and
		// it is the scene's `Wake` rather than a second opinion about timing.
		if (!Wants(output, index, decision))
		{
			// **Two different things wear this return and only one of them is a defect**, which is the
			// same split as `queue full` against `over budget` above and was arrived at by measurement
			// rather than by argument. The comment that used to stand here called every one of these a
			// schedule that armed for nothing; a thirteen second capture of gyro driving a 60 Hz panel
			// through KMS recorded four hundred and twenty seven of them, every one of them within a
			// few tens of microseconds of a page flip, and the `lead` sample above says the alarm still
			// had twelve milliseconds to run on every one that had one.
			//
			// **A wake the schedule armed for that turns out to want nothing is the defect.** Nothing is
			// owed, nothing is animating, and the fold should therefore have answered `Wake::Never()`
			// and left the thread blocked — an alarm that fires to draw nothing is the exact shape
			// Architecture.md#doing-nothing-must-cost-nothing forbids, and what it costs is a laptop
			// showing a still screen running its frame thread anyway. It is rare, it is a bug in the
			// arming rather than in the drawing, and that is why it gets a name of its own instead of a
			// share of one.
			//
			// **A wake an event source caused before the alarm is a rest, and gyro cannot avoid it.** A
			// host compositor's presentation feedback arrives a whole refresh after the instant it
			// reports and a panel's page flip arrives at its vblank, both on descriptors the frame
			// thread is waiting on; the thread must wake and read them, or every deadline after this one
			// is computed against an anchor a refresh stale. Having read them and found the world
			// settled, drawing nothing is the correct answer and not a failure of anything.
			//
			// **It is still marked rather than suppressed**, for the reason the wait above records: a
			// mark gated until it never fires is a row a reader finds empty where the explanation should
			// be. How often a backend wakes this thread for nothing is the figure that says what that
			// backend costs to idle under, and it is only visible if the benign case draws.
			//
			// `m_Armed` is the previous iteration's answer and is the whole of what tells them apart.
			// Due means the instant the schedule named has arrived, so the schedule owns this wake
			// whatever else also happened to fire — the honest reading, since that instant did turn out
			// to want nothing. Not due means the loop is running ahead of its own alarm, and `Settled`
			// means it armed no alarm at all, so in both cases something other than the schedule is what
			// woke the thread.
			TraceMark(m_Armed.IsDue(now) ? "armed for nothing" : "idle", output.m_Trace);

			return;
		}

		// **The frame as a person means the word, and everything below is inside it.** One slice per
		// frame on this output's own row, named for the frame it is — so `frame 142` on this row, on the
		// GPU row, on the flight lane and on the refresh ruler are four drawings of one thing that a
		// reader joins by reading, or by searching for the words, rather than by following eight arrows
		// across four rows. It is a scope guard because every refusal below returns, and each of those
		// is a frame that was started and abandoned: the slice is short and the mark inside it says why.
		const TraceSpan frame{ "frame", output.m_Trace, TraceTag(decision.Sequence) };

		// **The ruler, and the row is only readable because it tiles.** The span runs from the previous
		// refresh's deadline to this one's, so consecutive frames abut and the row becomes the grid
		// every other row is read against. It used to start at `now` instead, which drew a four
		// millisecond sliver with a twelve millisecond gap after it — an extent that looked like a
		// budget, was actually the time left when the loop woke up, and made a composite that fit
		// comfortably inside its refresh read as an overrun on every single frame.
		//
		// A row of its own rather than a slice the work nests inside, because the case worth seeing is
		// the child outliving the parent and nesting cannot draw that.
		if (decision.Deadline != FrameClock::Unscheduled)
		{
			// **The tile starts where the last one ended, and it is remembered rather than recomputed.**
			// Asking the clock for `DeadlineAt(sequence - 1)` is the obvious spelling and it is wrong by
			// microseconds: the clock re-anchors on every flip, so the deadline it names for a frame is
			// not quite the one it named a refresh ago, and consecutive tiles overlap by the drift.
			// Perfetto has no *slightly overlapping* — a slice that starts before its neighbour ended is
			// a child of it, so the ruler came out as a staircase nesting deeper every frame.
			//
			// A tile wider than one refresh is a refresh gyro drew nothing for, which is the honest
			// reading and the one worth seeing.
			const bool chained = output.m_Tiled != Instant{} && output.m_Tiled < decision.Deadline;

			// **Named for the refresh it is rather than for the frame aimed at it**, which is the
			// distinction the row was quietly losing. Every other `frame N` in the picture is an extent
			// that happened; this one is a prediction, and it ends where the commit for that refresh is
			// due rather than where its pixels appear. Sharing the word made a reader join a forecast to
			// four observations and read the ruler as the screen — the tile that says `refresh 99` sits
			// two tiles left of the pixels frame 99 put on the glass.
			TraceSpanAt(
				"refresh",
				chained ? output.m_Tiled : std::min(now, decision.Deadline),
				decision.Deadline,
				output.m_TraceGrid,
				TraceTag(decision.Sequence)
			);

			output.m_Tiled = decision.Deadline;

			// **Slack is only a number once this output has a prediction to be early or late against.**
			// On the first frame of an output whose clock has not been seeded, `Deadline` is the
			// unscheduled sentinel and the subtraction saturates — one sample of two hundred and ninety
			// two years flattens every real one on the same axis to a flat line.
			TraceElapsed("slack", decision.Slack(), output.m_Trace);
		}

		// **What the machine has been costing, sampled on the frames that read it.** These used to be
		// sampled on every wake, which meant three identical samples per refresh of a figure that only
		// moves when a cost is observed — two thirds of the points on the chart were the loop repeating
		// itself. Their measured counterparts are `cpu cost` here and `gpu cost` on the GPU row, and the
		// pair is the whole point: a mark that climbs away from what frames actually cost is the picture
		// that says the next thing to go wrong is a dropped frame.
		TraceElapsed("cpu mark", output.m_Cost.PlannedCpu(), output.m_Trace);
		TraceElapsed("gpu mark", output.m_Cost.PlannedGpu(), output.m_Trace);

		// Closed by hand rather than by a scope, because what the span measures is the walk and what the
		// scope holds is its result.
		TraceSpan evaluate{ "evaluate", output.m_Trace };

		const DrawList list = m_Evaluator->Evaluate(
			{ .Snapshot = m_Snapshot,
		      .Output = index,
		      .Outputs = m_Outputs.size(),
		      .Resolution = output.m_Configuration.Resolution,
		      .Presentation = decision.Presentation,
		      .Pictured = m_Captures.Held(index),
		      .Target = output.m_Configuration.Color,
		      .Mode = decision.Mode() }
		);

		evaluate.Close();

		(void)output.m_Cost.ObserveIrreducibleCpu(list.EvaluateCost);
		output.m_Damage.Add(list.Damage);

		TraceCount("items", static_cast<std::int64_t>(list.Items.size()), output.m_Trace);

		// What of this frame's closing windows still owes a picture, which is all of them the first time
		// each is seen and none of them on the frames after. Taken before the partition below because
		// the partition depends on it, and before the request because the proposal and the item list it
		// indexes have to be the same frame's.
		const std::span<const SnapshotCapture> captures = m_Captures.Propose(list.Captures, index);

		TraceCount("captures", static_cast<std::int64_t>(captures.size()), output.m_Trace);

		// **A frame that is taking a window's picture is the same clause for the same reason.** The
		// picture is drawn out of this list, so anything the display engine took is a piece of that
		// window the snapshot cannot reach — and a window that faded out with its own middle missing is
		// worse than one that never promoted. Promotion is a power saving on a still screen, and a
		// screen with a window leaving it is not one.
		//
		// **It is the proposal and not everything the walk found**, which is the difference between one
		// frame per closing window and every frame of every exit. A window whose picture has already
		// been drawn is still in the walk's report — `Frame/Capture.h` says why it has to be — and
		// suppressing planes for the whole length of its fade would spend an exit's worth of scanout
		// bandwidth to protect a copy that already happened.
		const std::uint32_t ceiling =
			capturing || !m_Planes || !captures.empty() ? 0 : output.m_Presenter->LayerCeiling();

		// **Decision 152's partition: which of these items the display engine draws and which the GPU
		// does.** Recomputed from this frame's list alone, with nothing carried over — see Frame/Assign.h.
		// **A ceiling of zero where a capture is owed, which is how the whole screen ends up in one
		// image.** `Assign` promotes a suffix of the draw list onto planes, and anything it promotes is
		// a window the composite is *not* responsible for — read the target back on such a frame and the
		// picture has holes where the display engine was going to do the work. Zero says promote
		// nothing, so the GPU draws every item and the target is the screen.
		//
		// This is the perturbation Seam/Capture.h declares and the reason the verb is a debug hatch: the
		// captured frame is drawn by a different path from its neighbours, and if the two ever disagree
		// the disagreement is visible on the glass at the instant of capture.
		Partition partition = Assign(list.Items, ceiling, output.m_Configuration.Color);

		// **What the count below cannot say.** A frame that promoted nothing and a frame whose top window
		// grew a shadow read the same on a counter, and the second is the one somebody is looking for.
		// Recorded from the assigner's own walk rather than re-derived, so the row names the clause that
		// actually stopped it.
		if (partition.Stopped != PromotionRefusal::None)
		{
			TraceMark(Reason(partition.Stopped), output.m_Trace);
		}

		if (partition != output.m_Partition)
		{
			// The composite is responsible for a different part of the screen than it was last frame, and
			// nothing in the scene says so. See `m_Partition`.
			output.m_Damage.Add(PixelRect<DeviceSpace>{ {}, output.m_Configuration.Resolution });
		}

		// **The target, and only where the partition left something for the GPU to draw.** A frame whose
		// every item promoted needs no render pass, no image out of the presenter's free set and no queue
		// submission, which is the arrangement decision 152 exists for — see `Target` above for what this
		// order replaced.
		//
		// **An empty list is not this case and the predicate has to say so.** `NeedsComposite` is
		// `Composited != 0`, which a partition that promoted everything and a partition with nothing in it
		// at all both answer false to — and the second still owes the screen a clear. Skipping on that
		// spelling left a panel showing whatever was on it when the last window closed.
		std::optional<std::uint32_t> composite{};

		if (partition.NeedsComposite() || partition.Count == 0)
		{
			composite = Target(output);

			if (!composite)
			{
				return;
			}
		}

		// The layers this frame would commit, built before anything is recorded so that the hardware can
		// be asked while there is still time to change the answer. The composite's geometry is known
		// without drawing it — it is the whole target — and its acquire point is filled in below, after
		// the record that produces one. A test consumes no fence, which is what makes that order legal.
		//
		// The composite goes first because the partition's promoted set is a suffix and gyro's own drawing
		// is the bottom of it; where there is no composite the promoted layers start at zero and the
		// lowest of them lands on the primary plane, which is what keeps the CRTC showing something.
		std::array<PresentLayer, MaxLayers> layers{};
		std::uint32_t count = 0;

		// Fill `layers` from whatever `partition` currently says, taking the composite's target if the
		// partition has come to need one. False means this frame has to be abandoned, which is the one
		// thing the caller cannot do anything about.
		//
		// **A closure because the narrowing below runs it more than once**, and the two runs have to
		// build the array the same way — a second spelling of *the composite first, then the promoted
		// suffix* is a second place for the layer order to be wrong, and the order is what the plane
		// assignment rides on.
		const auto build = [&]() -> bool {
			// **The target the narrowed partition needs, which the frame may not be holding.** A partition
			// that promoted everything acquired nothing above, so giving a layer back here is the one path
			// that discovers it wants the GPU after deciding it did not. Asked for now rather than kept in
			// hand against the possibility: holding an image on every fully-promoted frame is the cost the
			// order above was changed to stop paying, and a free set with nothing in it means this output
			// waits a frame rather than that it draws wrong.
			if ((partition.NeedsComposite() || partition.Count == 0) && !composite)
			{
				composite = Target(output);

				if (!composite)
				{
					return false;
				}
			}

			count = 0;

			if (composite)
			{
				const PixelSize<DeviceSpace> size = output.m_Presenter->Targets()[*composite].Size;

				layers[count++] = PresentLayer{
					.Target = LayerSource{ *composite },
					.Blend = BlendMode::Opaque,
					.Acquire = SyncPoint::Immediate(),
					.Source = { {}, { static_cast<float>(size.Width), static_cast<float>(size.Height) } },
					.Destination = { {}, size },
					.Damage = {},
					.Color = output.m_Configuration.Color,
				};
			}

			for (std::uint32_t promoted = 0; promoted < partition.Count; ++promoted)
			{
				layers[count++] = Promoted(list.Items[partition.ItemIndexForPromoted(promoted)]);
			}

			return true;
		};

		if (!build())
		{
			return;
		}

		// **Asked only where something would be promoted**, so a machine that promotes nothing pays no
		// ioctl. A refusal is ordinary rather than a fault: a plane's format, its bandwidth, or a scaler
		// it shares with another pipe are all things only the driver knows, and the answer is that this
		// frame composites — which is decision 35's one frame and is why the fallback is silent.
		//
		// **A refused partition gives one layer back and asks again, rather than collapsing to a full
		// composite.** An atomic test is all-or-nothing — one layer the display engine will not take
		// refuses the whole set — so a partition that is one layer too wide, or that names one buffer a
		// plane cannot read, costs every other promotion in it. Narrowing turns that cliff into a step,
		// and it costs one test per layer given up on a frame that was going to be refused anyway. It
		// stops at zero rather than testing it: a lone composite on the primary plane is the commit this
		// output makes on every frame that promotes nothing, so there is nothing left to ask.
		//
		// **The layer given back is the one nearest the composite, which is forced rather than chosen.**
		// The promoted set is a suffix and the only end of it that can move is the bottom; giving back
		// the frontmost layer would leave a promoted item under a composited one, which is the hole in
		// the composite decision 152's suffix rule exists to avoid.
		//
		// **So this does nothing for a refusal caused by the frontmost item, and that is worth saying
		// out loud.** A pointer glyph the display engine will not take is the last layer to be given up
		// rather than the first, so the walk pays a test per step and ends where it started. What that
		// case wants is not a different narrowing order — there is no legal one — but for the item never
		// to have been proposed: a plane it fits, which is why the cursor plane is now in the inventory,
		// and the format pre-filter each backend applies before the ioctl.
		//
		// **Generic on purpose.** Nothing here knows which layer it is dropping or why the driver said
		// no — the refusal is the driver's whole answer, and proposing less of the same partition is the
		// only response that is right for a format, a bandwidth ceiling, a shared scaler and a plane with
		// a minimum width alike.
		while (partition.Count != 0)
		{
			const Result<void> testable = output.m_Presenter->TestLayers({ layers.data(), count });

			if (testable)
			{
				break;
			}

			// **Silent until now, and that was the gap worth closing.** A driver refusing every proposal —
			// a format the plane will not take, bandwidth it does not have, a scaler shared with another
			// pipe — reads exactly like a compositor that never tried, because both leave the count at
			// zero. The fallback stays silent to the *user*; it must not be silent to the instrument.
			//
			// **And the words rather than the fact, for the reason the two sites below it take them.** A
			// bare *planes refused* cannot tell the two halves of this call apart, and they are opposite
			// stories: a backend refuses a partition it cannot express *before* it asks the hardware —
			// an id that never imported for scanout, more layers than the output has planes — so a
			// capture full of that mark reads as *the driver will not take these planes* when what
			// happened is that gyro never issued the ioctl. The first is a machine to give up promoting
			// on and the second is a bug upstream of here, and only the sentence separates them.
			//
			// One mark per attempt rather than one per frame, which is what makes the narrowing legible:
			// a row with three of these and a `planes` count of two says the driver refused twice and
			// took the third proposal, and that is the shape somebody tuning a machine wants to see.
			TraceMark(
				testable.error().Sentence(),
				output.m_Trace,
				TraceTag(static_cast<std::uint64_t>(testable.error().Code()))
			);

			--partition.Count;
			++partition.Composited;

			// The comparison against last frame's partition happened before the driver was asked, so the
			// damage it decided on was for a partition this frame is no longer committing. The composite
			// is now responsible for a layer it was not, and nothing in the scene says so.
			output.m_Damage.Add(PixelRect<DeviceSpace>{ {}, output.m_Configuration.Resolution });

			if (!build())
			{
				return;
			}
		}

		// **The scene this output has now drawn from**, recorded once the frame has everything it needs to
		// draw. It sat beside the evaluator's walk while the target was taken before that walk, and it
		// cannot stay there now: an iteration that evaluates and then finds no free image has drawn
		// nothing, and saying otherwise leaves the output believing it is square with a scene it never
		// put on the glass.
		output.m_Drawn = m_Held;

		output.m_Partition = partition;

		// The shape this frame actually commits, recorded after the driver has been asked so that the
		// number is what reached the screen rather than what was proposed. It counts the composite too:
		// one is a screen the GPU drew whole, and the interesting frame is the one where it climbs.
		TraceCount("planes", static_cast<std::int64_t>(partition.Layers()), output.m_Trace);

		// **The GPU's whole half, skipped where the display engine took every item.** No backlog join, no
		// record and no submission: the promoted layers carry their own images and their own acquire points,
		// so a fully-promoted frame commits what the assigner built and nothing draws. This is the branch
		// decision 152 exists for, and what it saves is a composite per refresh on a panel that needed none.
		if (composite)
		{
			// The buffer-age join, and it is built after the evaluator has contributed so that this frame's
			// own damage is in it. A copy rather than a reference because `RecordRequest` takes the region by
			// value and the loop must not hand a target's backlog somewhere it could be cleared from — and it
			// is a fixed-size array of rectangles on the stack, which is what decision 36 asks of it.
			Region<DeviceSpace> stale = output.m_Damage;
			stale.Add(output.m_Backlog[*composite]);

			const RecordRequest request{ .Target = *composite,
				                         .Mode = decision.Mode(),
				                         .Trace = output.m_TraceGpu,
				                         .Frame = decision.Sequence,
				                         .CostGeneration = output.m_Cost.Generation(),
				                         // Decision 142's hint, and the translation of one sentinel into
				                         // another: an unscheduled clock has no instant to name, and the
				                         // seam says so as the epoch because Seam/Renderer.h may not reach
				                         // in here for `FrameClock`'s spelling of the same nothing.
				                         .Deadline = decision.Deadline == FrameClock::Unscheduled ? Instant{} :
				                                                                                    decision.Deadline,
				                         .Damage = stale,
				                         // The prefix, which is the whole list wherever nothing was promoted.
				                         // The suffix is on planes and the composite must not draw it twice — an
				                         // item drawn under an opaque plane is invisible, and one drawn under a
				                         // plane the driver later refuses is a window in two places.
				                         .Items = list.Items.first(partition.Composited),
				                         // The closing windows, each naming a run of the items above. They
				                         // index the whole list rather than the prefix, which the ceiling
				                         // clause up there makes the same span whenever there is one of
				                         // these to draw.
				                         .Captures = captures };

			TraceSpan record{ "record", output.m_Trace };

			const Result<Submission> submission = output.m_Renderer->Record(request);

			record.Close();

			if (!submission)
			{
				// **Named by the refusal's own sentence rather than by the word `refused`**, which is a mark
				// that says a frame was thrown away and never why. A capture with fifty of them in it left a
				// reader to guess between every branch that can decline, and the guessing was the whole cost:
				// a refusal is almost always a standing condition, so the one thing worth knowing is which
				// one, and that is a word this frame already has in its hand. Core/Result.h's sentence is a
				// literal with static storage, so the ring takes the pointer and copies nothing — legal
				// inside the frame section for the same reason every other mark is.
				//
				// **And the code beside it, because a sentence does not always choose one.** The first version
				// left the errno out on the argument that a reader holding the words has more than a number
				// would tell them, which is true of every refusal gyro authors — those sentences name their
				// own branch and the code is redundant. It is false of a refusal that is a *syscall's*: the
				// sentence names the call and the code names the failure, so `committing a page flip` reads
				// identically whether the kernel meant *the previous flip is still retiring, come back* or
				// *this atomic request is malformed*. Those are opposite stories, and a capture with
				// twenty-seven of them in it could not tell them apart.
				//
				// A `TraceTag` rather than a `TraceAttribute`: an attribute binds to the slice open on its row
				// and Trace/Perfetto.cpp's walk ends that eligibility at anything which is not a `Begin`, so
				// one emitted beside a mark binds to nothing and is dropped. The tag is printed into the name
				// and joined to nothing, which is what a code wants — it is read at a glance rather than
				// clicked, and it is not an identity anything else in the picture counts by.
				TraceMark(
					submission.error().Sentence(),
					output.m_Trace,
					TraceTag(static_cast<std::uint64_t>(submission.error().Code()))
				);

				// Seam/Renderer.h's *an item the renderer cannot express* arriving: the draw list held
				// something this backend refuses to draw wrong, so it drew none of it. The reason is the
				// renderer's own words and this is the only place they exist.
				output.Refuse(submission.error());

				return;
			}

			// The device is busy from here whatever happens to the present, so the next output on this queue
			// starts from the new figure even if the flip below is refused.
			free = decision.DeviceFreeAt;
			(void)output.m_Cost.ObserveCpu(decision.Mode(), submission->RecordCost);

			// A window whose picture is now ordered before anything that samples it, and therefore one
			// this output never draws again. Filed on the submission and not on the flip — see
			// `Frame/Capture.h` — and on the renderer's own count rather than on what was offered.
			m_Captures.Landed(submission->Captured);

			// **The fourth mark, and the one the other three cannot stand in for: what the renderer
			// actually took.** `captures` above says what this frame offered, and a renderer with no
			// snapshot path — `Blit`, today — accepts none of them without failing anything, so the walk
			// goes on proposing and `Held` stays empty and the window is never drawn from its picture.
			// That is a fade that works on one backend and not on another, which is the shape a person
			// debugging this would otherwise chase through both. On an output's own row rather than the
			// thread's, because whether a picture exists is a fact about one screen's atlas.
			if (!captures.empty() && submission->Captured == 0)
			{
				TraceMark("captures refused", output.m_Trace, TraceTag(captures.size()));
			}

			// What the record produced, filled into the layer the test was run against rather than a second
			// one built here: committing a partition assembled differently from the one the hardware accepted
			// asks a different question of it.
			layers[0].Acquire = submission->Point;
			layers[0].Damage = output.m_Damage;

			// **Between the record and the present, which is the only window where both facts hold**: the
			// pixels exist, and the target is not yet the thing a display engine is reading. Reading after
			// the present would be reading a framebuffer that has been handed to the panel and may already
			// have been handed back out of the ring.
			//
			// It stalls this thread on the composite's fence — see Seam/Capture.h — so the frame it
			// captures very likely misses its deadline. That is accepted rather than worked around: a
			// capture perturbs the frame it takes either way, and one late frame at an instant a person
			// chose is cheaper than a slab held across iterations and a target the presenter cannot reuse.
			// **The press waits for every promoted plane to stop being read before it reads anything.**
			// The ceiling above is zero while a capture is owed, so this frame promotes nothing and every
			// frame after it promotes nothing — but the last ordinary frame put client buffers on planes,
			// and the display engine is scanning those out until a frame that does not name them has
			// flipped over it. `CaptureTextures` acquires exactly such a buffer away from the display
			// engine, which on this machine's amdgpu wedges the display controller: DMCUB faults, the
			// flip never completes, and the panel is gone for the life of the process.
			//
			// So the capture is deferred rather than narrowed. The slot stays `Armed` because nothing
			// publishes it, `capturing` is therefore still true next iteration, the whole-output damage
			// above keeps the loop drawing, and the capturing frame's own flip is what clears the glass —
			// so the read happens one flip after the last promoting commit rather than at the instant it
			// landed. It converges because the only thing that could promote again is the ceiling, and
			// the ceiling is zero.
			//
			// **Both halves wait, not just the texture half.** Reading only the target on such a frame
			// would put a picture on disk with no buffers beside it, which reads as *no clients were
			// up* rather than as *the capture came a frame early* — and the two are the same directory
			// listing.
			if (capturing && output.ArePlanesLive())
			{
				// **Two marks rather than one tagged with a depth**, because on a presenter that holds one
				// commit the queue is always empty at this point and a zero on that row would read as a
				// wait against nothing — which is the shape of the bug this guard was written twice for.
				if (output.ArePlanesOnGlass())
				{
					TraceMark("capture waiting on the plane being scanned out", output.m_Trace);
				}
				else
				{
					TraceMark("capture waiting on a promoted commit", output.m_Trace, TraceTag(output.InFlight()));
				}
			}
			else if (capturing)
			{
				// The target first, so a slow disk holds up the picture rather than the windows in it.
				// The textures inherit its stall rather than repeating it: the composite that sampled
				// every one of these images is the work `CaptureTarget` has just seen land — which is
				// why they run only where it says it actually waited.
				if (CaptureTarget(output, index, *composite, submission->Point, decision.Sequence))
				{
					CaptureTextures(output, list.Items);
				}
			}
		}

		{
			const TraceSpan present{ "present", output.m_Trace };

			if (const Result<void> presented = output.m_Presenter->Present(
					{ layers.data(), count }, { .Trace = output.m_TraceCommit, .Frame = decision.Sequence }
				);
			    !presented)
			{
				// The presenter's own words and its code, for the reason the record refusal above gives — and
				// this is the site that wanted both. A composite that reaches here has already been paid for
				// in full and is discarded, so a run with a standing refusal burns a whole frame's GPU work
				// per refresh and draws the same refresh two and three times on the ruler. It is also where
				// the sentence is least likely to be gyro's own: Drm/Output.h hands back what the atomic
				// ioctl said, and *which* errno that was is the difference between a race to back off from
				// and a request to fix.
				TraceMark(
					presented.error().Sentence(),
					output.m_Trace,
					TraceTag(static_cast<std::uint64_t>(presented.error().Code()))
				);
				output.Refuse(presented.error());

				return;
			}
		}

		// Presented, so the presenter has it back and the loop is not holding one any more. Beside the
		// damage clear for the same reason: this is the one exit that put a frame on its way to the glass.
		output.m_Acquired.reset();

		output.m_Committed = decision.Sequence;

		// The published sequence this frame was composed from, queued behind whatever is already out.
		// `m_Held` is the one the evaluator walked above, and it is read here rather than remembered
		// per attempt because acquisition is once per iteration — every output served in this pass drew
		// from the same scene.
		output.m_InFlightSnapshots[output.m_InFlight] = m_Held;

		// And the frame it is, which the queue used to drop. Two counts ride out with one commit and
		// the return leg needs both: the scene is what dispatch turns back into surfaces, and this is
		// the number every row of this frame is named for — so it is what the glass row has to say when
		// the vblank arrives, and what tells a reader whether the frame landed on the refresh it was
		// aimed at.
		output.m_InFlightFrames[output.m_InFlight] = decision.Sequence;

		// **The lane the frame flies in, opened here and closed by the vblank that shows it.** Rotation
		// rather than queue position, because `OnPresented` pops from the front and shuffles everything
		// behind it down — a slice has to close on the row it opened on, and a row derived from a
		// position would move under it. The rotation cannot collide: at most `MaxCommitsInFlight` are
		// outstanding and there are exactly that many lanes.
		//
		// **Stamped here rather than at the iteration's clock read**, which is the correction the row
		// needed. Stamped at the wake, the lane opened before the frame slice on the row above it did —
		// a frame waiting in a queue microseconds before it began being drawn, on every frame of a
		// capture — and *how many lanes are occupied is the queue depth* stopped being true, since a
		// lane was also occupied while its frame was still being recorded. The end-to-end latency the
		// old stamp drew is not lost: it is this output's frame slice opening against this lane closing.
		const std::uint16_t lane =
			static_cast<std::uint16_t>(output.m_TraceFlight + output.m_FlightLane % TracedFlights);

		++output.m_FlightLane;
		output.m_InFlightRows[output.m_InFlight] = lane;

		// Whether this commit hands a client's own buffer to the display engine, which is what a later
		// capture has to wait out before it may read one back. `partition.Count` rather than the layer
		// count, because the composite is a layer too and it is gyro's own image.
		output.m_InFlightPromoted[output.m_InFlight] = partition.Count != 0;

		TraceOpen("frame", lane, TraceTag(decision.Sequence));

		++output.m_InFlight;

		// The backlogs fold forward, and this is the whole of the buffer-age bookkeeping. Every *other*
		// target now owes this frame's change on top of what it already owed, and the one just drawn owes
		// nothing — its content is complete as of now, which is true whether or not the glass ever shows
		// it. Before the clear below, because the region being folded is the one being cleared.
		for (std::size_t other = 0; other < MaxTargets; ++other)
		{
			if (!composite || other != *composite)
			{
				output.m_Backlog[other].Add(output.m_Damage);
			}
		}

		// **Every target owes this frame where none of them drew it.** A fully-promoted frame changed the
		// screen without touching an image in the ring, so there is nothing to mark complete and the debt
		// goes to all of them — which is what makes the composite that eventually happens repaint the part
		// the planes had been covering.
		if (composite)
		{
			output.m_Backlog[*composite].Clear();
		}

		output.m_Damage.Clear();
	}

	// The target this frame will composite into, taken from the presenter's free set.
	//
	// **Asked for only where a composite is going to happen, which is the whole of decision 152's
	// sleeping-GPU case.** This used to run before the scene was evaluated, so a frame whose every item
	// went on a plane was still holding an image it had no use for and no way to give back — and the
	// loop's answer was to hand the bottom layer back to the composite and draw it. That cost the
	// arrangement the mechanism exists for: a fullscreen window under nothing, or under a pointer, with
	// the GPU asleep. It also hid the measurement, because the layer handed back is the bottom of the
	// suffix and so the *window* never reached `IPresenter::TestLayers` at all.
	//
	// **Acquired once and held until it is presented, which reads as an omission until you follow the
	// other path out of here.** This takes a target *out* of the presenter's free set, and the only way
	// one goes back is by being presented. So an iteration that acquires and then refuses has taken an
	// image nobody can hand out again, and a standing refusal empties the set one frame at a time: on a
	// triple-buffered output the third refusal is the last thing that output ever attempts. What that
	// looks like on screen is a panel that goes black and stays black, and what it looks like in the log
	// is a refusal count of three for a run of thousands — a number small enough to read as a hiccup.
	//
	// Holding it is not a workaround for a release verb `IPresenter` does not have. The loop *does* own
	// this image, it will render into it on the next attempt, and the bug was only ever that it forgot.
	// `Discard` drops it, because a target set that was invalidated took this index with it.
	//
	// Nothing is an ordinary answer and the caller's response is to leave this output for the next
	// iteration, which is why the two defects below are refused here rather than reported: all three
	// exits mean the same thing to a step that cannot draw.
	[[nodiscard]] std::optional<std::uint32_t> Target(FrameOutput& output) noexcept
	{
		if (!output.m_Acquired)
		{
			output.m_Acquired = output.m_Presenter->AcquireTarget();
		}

		if (!output.m_Acquired)
		{
			// Decision 30's lead, bounded from the other side: an output with no free target cannot run
			// ahead, so a double-buffered one cannot run ahead at all. Not an error, and not damage lost.
			return std::nullopt;
		}

		const std::uint32_t target = *output.m_Acquired;

		if (target >= output.m_Presenter->Targets().size())
		{
			// A presenter handing out an index outside the set it published is the one refusal here that is
			// a defect rather than a limit, and it is the one that would otherwise be indistinguishable
			// from an output that simply had nothing to draw.
			//
			// Dropped rather than held, unlike every refusal below it: those keep an index that is good and
			// will be drawn into next time, and this one is an index that names nothing. Holding it would
			// retry the same bad number forever and never ask the presenter again.
			output.Refuse(Error{ ERANGE, "the presenter acquired a target outside the set it lists" });
			output.m_Acquired.reset();

			return std::nullopt;
		}

		if (target >= MaxTargets)
		{
			// A ring deeper than this loop carries a backlog for. Refused rather than drawn without one,
			// because drawing it is the buffer-age bug arriving silently — a stale band on one target in
			// the rotation, which reads as a renderer fault and is a capacity fault. Dropped for the same
			// reason as above: the index is one this loop will never accept, so holding it retries it
			// forever where releasing it lets the presenter offer one inside the set.
			output.Refuse(Error{ ERANGE, "the presenter's target ring is deeper than the frame loop tracks" });
			output.m_Acquired.reset();

			return std::nullopt;
		}

		return target;
	}

	// What each output must hold back from its own deadline, which is every composite the device it is
	// on owes this refresh rather than only its own.
	//
	// **It is a duration per output and an instant per device, and the two are the same statement.**
	// `Batch::RecordAt` is one instant for the whole queue; read back against a member's own deadline it
	// is a reserve, and a reserve is what both readers want — `Serve` applies it to the frame `Assess`
	// decided on and `Timing::WakeFor` applies it to whichever frame its own floors name. Handing either
	// of them the instant instead would pin them to the frame this fold happened to use, which is the
	// frame already committed on every iteration between a submit and its flip.
	//
	// **Two passes, because a member joining moves the instant the members before it are held to.** The
	// fold produces a grouping and the finished batches are read afterwards; assigning as it walks would
	// give the first output on a card an arming composed before its neighbour was known.
	//
	// Every output starts at the arming it composes for itself, so an output in no batch — settled, or
	// on a clock with no anchor — is held to exactly what an output alone on its device has always been
	// held to.
	void Schedule(std::span<const std::uint8_t> order) noexcept
	{
		std::array<Batch, MaxOutputs> batches{};
		std::array<std::uint64_t, MaxOutputs> owed{};
		std::array<std::uint8_t, MaxOutputs> group{};
		std::array<std::uint8_t, MaxDevices> open{};
		std::array<Duration, MaxDevices> demand{};
		std::array<Duration, MaxDevices> shortest{};
		std::uint8_t count = 0;

		open.fill(Unbatched);
		shortest.fill(Duration::max());

		for (std::size_t index = 0; index < m_Outputs.size(); ++index)
		{
			m_Arming[index] = m_Timing.Arming(m_Outputs[index].m_Cost);
			m_Gated[index] = false;
			group[index] = Unbatched;
			owed[index] = FrameClock::NoSequence;
		}

		for (const std::uint8_t index : order)
		{
			const FrameOutput& output = m_Outputs[index];

			if (!Owes(output, index))
			{
				continue;
			}

			owed[index] = Timing::OwedFrame(output.m_Clock, output.m_Committed);

			// No anchor is no deadline, so there is no demand to place and nothing to place it before.
			// Decision 31 has such an output render on demand, and it is charged to no queue while it
			// does — which is the same answer `Timing::Fold` gives and is tested here so that the fold's
			// refusal cannot be mistaken for a batch that stayed as it was.
			if (output.m_Clock.DeadlineAt(owed[index]) == FrameClock::Unscheduled)
			{
				owed[index] = FrameClock::NoSequence;

				continue;
			}

			demand[output.m_Device] =
				Detail::Sum(demand[output.m_Device], m_Timing.Reserve(output.m_Cost, RenderMode::Planned));
			shortest[output.m_Device] = std::min(shortest[output.m_Device], output.m_Clock.Period());
		}

		for (const std::uint8_t index : order)
		{
			const FrameOutput& output = m_Outputs[index];

			if (owed[index] == FrameClock::NoSequence || !MayHold(demand, shortest, output.m_Device))
			{
				continue;
			}

			const std::uint8_t held = open[output.m_Device];
			const Batch before = held == Unbatched ? Batch{} : batches[held];
			const Batch after = m_Timing.Fold(before, output.m_Clock, output.m_Cost, owed[index]);

			if (after.Members == 1)
			{
				// Either the device had nothing open or this output's own start clears everything already
				// queued on it, and those are the same statement about what happens next: the batch open
				// on this device from here is one whose first member is this output.
				batches[count] = after;
				open[output.m_Device] = count;
				++count;
			}
			else
			{
				batches[open[output.m_Device]] = after;
			}

			group[index] = open[output.m_Device];
		}

		for (std::size_t index = 0; index < m_Outputs.size(); ++index)
		{
			if (group[index] != Unbatched)
			{
				m_Arming[index] =
					Elapsed(batches[group[index]].RecordAt, m_Outputs[index].m_Clock.DeadlineAt(owed[index]));
				m_Gated[index] = true;
			}
		}
	}

	// Whether this device has any idle time to hold a frame back *with*.
	//
	// **A record point is a latency optimisation and it is spent out of slack the device has.** Holding a
	// composite until the last instant that still meets its deadline is free exactly while the queue has
	// somewhere to put the work it is not doing yet; on a queue whose composites already fill the fastest
	// panel's period there is no such place, and the hold pushes the work it delayed into the next
	// refresh's window. What that costs is a frame per beat of the two panels' phase — which the sweep
	// measured the moment the batch was enforced on a set admission control had cut to exactly
	// `C_fast + C_slow = P_fast`, and which decision 30's relaxed release was quietly paying for before:
	// an output rendering as soon as the work fits is released a whole period early, and a queue that
	// never idles needs every bit of that.
	//
	// So a saturated device keeps the behaviour it has always had — served back to back from wherever the
	// loop woke, work-conserving — and the hold applies where there is genuinely room for it. The
	// comparison is against the *shortest* member's period because that is the panel whose next release
	// the held work would collide with, and it is strict because equality is a queue with exactly no
	// idle time, which is the case that fails.
	//
	// **It is a fact about the device rather than a rung**, which is why it is not admission control's:
	// the plan is unchanged, every output spends what it was allocated, and what varies is only whether
	// gyro chooses to start early. A set this refuses to hold is a set that still meets every deadline.
	[[nodiscard]] bool MayHold(
		const std::array<Duration, MaxDevices>& demand,
		const std::array<Duration, MaxDevices>& shortest,
		std::size_t device
	) const noexcept
	{
		if (shortest[device] == Duration::max())
		{
			return false;
		}

		return Detail::Sum(demand[device], m_Timing.Policy().Lead) < shortest[device];
	}

	// Whether this output is owed a frame as soon as one can be made: damage that has not reached the
	// glass, a scene it has not drawn, a flip whose completion the clock is still waiting for, or a
	// contributor that wants every frame.
	//
	// **It is the batch's membership and the fold's contribution at once, and one predicate rather than
	// two is the point.** A settled panel beside an animating one must not hold the queue — a batch that
	// counted it would drag its neighbour's record point earlier by a composite that is never going to
	// be recorded, which is pointer latency bought to reserve for nothing. The two questions have to
	// answer together, because an output the fold declines to arm for is an output the batch must not be
	// reserving for.
	//
	// What is deliberately outside it is a full commit queue. Such an output records nothing *this*
	// iteration and is still owed the frame after its flip, so leaving it in reserves for a composite
	// that is merely early rather than absent — and the error is in the recoverable direction, since a
	// larger batch only ever moves the record point earlier and can therefore cost latency but never a
	// frame.
	[[nodiscard]] bool Owes(const FrameOutput& output, std::size_t index) const noexcept
	{
		const Wake scene = SceneWake(index);

		return !output.m_Damage.IsEmpty() || output.m_Drawn != m_Held || output.IsFlipPending() ||
		       (scene.Which == Wake::Kind::Continuous && scene.Interval == Duration::zero());
	}

	// Whether this output is owed the frame `Assess` says it could make.
	//
	// Damage outranks everything, since it is pixels that have not reached the glass. Otherwise it is the
	// scene's answer, and the comparison is against the *presentation* rather than against now: the loop
	// wakes a reserve ahead of the frame it is serving, so an instant that has not arrived yet is
	// precisely the case this exists to admit. Testing `IsDue(now)` here would refuse every frame at
	// exactly the moment the schedule woke up to draw it.
	// Read the composite back and hand it over. Everything that could allocate or open a file is the
	// sink's and happens on the sink's own thread; what runs here is a reserve, a copy and a publish.
	// **Answers whether the composite's own work was waited on**, which is the precondition every
	// texture read after it inherits rather than repeats. Both of the early returns below leave that
	// work outstanding, so a caller that read textures anyway would be copying images the GPU may
	// still be sampling — and the two paths that get there are a mode set since `Remember` and a
	// format the slab was not sized for, neither of which is rare enough to leave unsaid.
	[[nodiscard]] bool CaptureTarget(
		FrameOutput& output,
		std::size_t index,
		std::uint32_t target,
		SyncPoint point,
		std::uint64_t sequence
	) noexcept
	{
		const PixelSize<DeviceSpace> size = output.m_Presenter->Targets()[target].Size;
		const PixelFormat format = output.m_Presenter->Targets()[target].Format;
		const std::uint32_t bytesPerPixel = DecodableBytesPerPixel(format.Code);

		if (bytesPerPixel == 0)
		{
			return false;
		}

		// Tight, which is the stride Virtual/Pam.h writes and the one a reader of the slab can derive
		// from the format without being told.
		const std::uint32_t stride = static_cast<std::uint32_t>(size.Width) * bytesPerPixel;
		const std::uint32_t at = static_cast<std::uint32_t>(index);
		const std::span<std::byte> into = m_Capture->Reserve(at, size, format, stride);

		if (into.empty())
		{
			// An ordinary answer rather than a fault, per Seam/Capture.h: the frame has already been
			// forced to composite and the honest thing left is to draw it.
			return false;
		}

		const Result<void> read =
			output.m_Renderer->ReadTarget({ .Target = target, .After = point, .Into = into, .Stride = stride });

		if (!read)
		{
			// The refusal's own words, for the reason every other mark on this row takes them: *the
			// capture failed* cannot tell a device with no readback usage from a copy that timed out, and
			// those are a flag to pass at startup and a wedged GPU.
			TraceMark(
				read.error().Sentence(), output.m_Trace, TraceTag(static_cast<std::uint64_t>(read.error().Code()))
			);
		}

		m_Capture->Publish(at, sequence, read.has_value());

		// **The success of the read is the signal, because only success implies the stall.** Two of
		// `ReadTarget`'s refusals — a device built without readback, a target that is not bound — come
		// back *before* it waits on `point`, so a caller that read textures on any refusal would be
		// copying images the composite may still be sampling. The reads that follow this are ordered by
		// that wait and by nothing else, so anything short of a clean return has to stop them.
		return read.has_value();
	}

	// Read back every client image this frame drew, for Seam/Capture.h's texture half.
	//
	// **The draw list is the walk rather than the texture space**, which bounds this to what the
	// composite actually sampled and is the honest limit: a window on another output, occluded, or
	// off-screen is one the frame never touched and cannot vouch for. It also makes the cost
	// proportional to the picture rather than to how many windows the machine is holding open.
	//
	// The sink deduplicates rather than this walk: two items naming one texture — a window and a
	// thumbnail of it — reach `ReserveTexture` twice, and the second ask comes back empty because the
	// first already took the slot. Doing it here would need a set on the frame thread, which is either
	// an allocation Core/FrameSection.h aborts on or a fixed array duplicating the one the sink has.
	void CaptureTextures(FrameOutput& output, std::span<const DrawItem> items) noexcept
	{
		for (const DrawItem& item : items)
		{
			const DrawTexture* const content = std::get_if<DrawTexture>(&item.Content);

			if (content == nullptr || content->Texture.IsNull() || !m_Capture->WantsTexture(content->Texture))
			{
				continue;
			}

			const TextureSlab slab = m_Capture->ReserveTexture(content->Texture);

			if (!slab.IsValid())
			{
				// Ordinary, per Seam/Capture.h: a texture already read on this press, or a sink whose
				// writer is still busy. Nothing has been perturbed, so there is nothing to say.
				continue;
			}

			const Result<void> read = output.m_Renderer->ReadTexture(content->Texture, slab.Into, slab.Stride);

			if (!read)
			{
				// The refusal's own words, for `CaptureTarget`'s reason — and this one has a second
				// failure worth telling apart by hand: a renderer with no readback usage and a client
				// whose modifier the device would not copy out of read very differently.
				TraceMark(
					read.error().Sentence(), output.m_Trace, TraceTag(static_cast<std::uint64_t>(read.error().Code()))
				);
			}

			m_Capture->PublishTexture(content->Texture, read.has_value());
		}
	}

	[[nodiscard]] bool Wants(const FrameOutput& output, std::size_t index, const FrameDecision& decision) const noexcept
	{
		if (!output.m_Damage.IsEmpty() || output.m_Drawn != m_Held)
		{
			return true;
		}

		const Wake scene = SceneWake(index);

		if (scene.Which == Wake::Kind::Settled)
		{
			return false;
		}

		return (scene.Which == Wake::Kind::Continuous && scene.Interval == Duration::zero()) ||
		       scene.When <= decision.Presentation;
	}

	// Decision 84's rule, and the whole of the check that is available before the header carries a set
	// generation: a run that is not this output set's is no information rather than partial information.
	[[nodiscard]] Wake SceneWake(std::size_t index) const noexcept
	{
		const std::span<const Wake> schedule = m_Snapshot.Wakes();

		return schedule.size() == m_Outputs.size() ? schedule[index] : Wake::Never();
	}

	// **The loop only ever arms for the next iteration**, so every contribution is `Never` or a single
	// instant and the `Continuous` kind never has to be returned. That is not a simplification of
	// decision 69's monoid — it is what the monoid is for, one level up: the scene's contributors fold
	// to a wake per output on the dispatch side, and what this reduces is that answer against what the
	// schedule can actually serve it with.
	[[nodiscard]] Wake Fold(Instant now, const std::array<Instant, MaxDevices>& deviceFree) const noexcept
	{
		Wake wake = Wake::Never();

		for (std::size_t index = 0; index < m_Outputs.size(); ++index)
		{
			wake = Sooner(wake, Contribution(m_Outputs[index], index, now, deviceFree));
		}

		return wake;
	}

	[[nodiscard]] Wake Contribution(
		const FrameOutput& output,
		std::size_t index,
		Instant now,
		const std::array<Instant, MaxDevices>& deviceFree
	) const noexcept
	{
		if (!output.IsBound() || !output.m_Configuration.Powered)
		{
			return Wake::Never();
		}

		const Wake scene = SceneWake(index);

		if (Owes(output, index))
		{
			// **Armed for when the queue must begin and not for when this output alone could.** The
			// arming is `Schedule`'s, so an output sharing a card wakes the loop early enough for
			// everything in front of it to run first — without which the alarm names the instant one
			// output could have started at and whatever is served after it is late by exactly that
			// output's execution.
			return m_Timing.WakeFor(
				output.m_Clock, output.m_Cost, now, output.m_Committed, deviceFree[output.m_Device], m_Arming[index]
			);
		}

		if (scene.Which == Wake::Kind::Settled)
		{
			// Architecture.md#doing-nothing-must-cost-nothing, reached from both sides at once: nothing
			// wants a frame and nothing is outstanding, so nothing is armed.
			return Wake::Never();
		}

		// A frame wanted later. The one to serve it is the earliest the clock can still place at or after
		// the instant named, and the wake is that frame's record point rather than the instant itself —
		// arming at the instant would start the work a whole frame after it was wanted.
		const std::uint64_t sequence = output.m_Clock.SequenceAfter(scene.When);

		if (sequence == FrameClock::NoSequence)
		{
			// No anchor to place it against, so there is nothing to derive and the instant asked for is
			// the honest answer. An unanchored output renders on demand; see decision 31.
			return Wake::At(scene.When);
		}

		// `m_Arming[index]` is this output's own composed arming here rather than a batch's: `Owes`
		// declined it above, so `Schedule` left it out of every queue and out of the fill it starts from.
		// A frame wanted later is one no device is reserving for yet.
		const Instant at = output.m_Clock.WakeupAt(sequence, m_Arming[index]);

		return at == FrameClock::Unscheduled ? Wake::Never() : Wake::At(at);
	}

	// No output's place in a batch, which is what an output the fold declined occupies. `MaxOutputs` is
	// sixteen, so a byte says this without a second array of flags beside the grouping.
	static constexpr std::uint8_t Unbatched = 0xFF;

	// SPEC: how many finished frames a renderer may report in one iteration. Two per output per
	// iteration is the steady state under decision 30's pipeline; eight is slack for a device coming
	// back from a stall.
	static constexpr std::size_t MaxCostsPerIteration = 8;

	const IClock* m_Clock = nullptr;
	const SnapshotRing* m_Ring = nullptr;
	ReturnChannel* m_Returns = nullptr;
	IEvaluator* m_Evaluator = nullptr;

	Timing m_Timing{};

	std::span<FrameOutput> m_Outputs{};
	std::span<IEventSource* const> m_Sources{};

	std::uint64_t m_Held = 0;
	SnapshotReader m_Snapshot{};

	// Scratch for the run `Presentations` hands the return channel, a member rather than a local so that
	// the span it returns outlives the call. Nothing reads it between iterations.
	std::array<PresentedFrame, MaxOutputs> m_Presentations{};

	// What each output holds back from its own deadline, which is its whole device's batch. Recomputed
	// twice per iteration by `Schedule` and read by the two parties that need it in between, so it is a
	// member for the reason the array above is one rather than because anything survives the step.
	std::array<Duration, MaxOutputs> m_Arming{};

	// And whether the record point is enforced at all, which is `MayHold` per output. A device with no
	// idle time to spend keeps the arming above for its *alarm* — the loop still wants waking at a
	// sensible instant — and is not held to it, because on that queue the honest answer is to start as
	// soon as there is anything to start.
	std::array<bool, MaxOutputs> m_Gated{};

	// What the previous iteration answered, kept only so that the next one can say how much of its lead
	// it still had. It is not read by anything that decides: the loop recomputes the fold from scratch
	// every iteration, and a wake remembered and acted upon would be a schedule with two authors.
	// Null on every run that was not asked for captures, which is almost all of them — the check is one
	// predictable branch per output per frame and buys a loop that carries no capture state at all.
	ICaptureSink* m_Capture = nullptr;

	// Which closing windows have already had their picture taken, per output. The loop holds it rather
	// than the evaluator because the answer is about submissions and the walk knows nothing of those.
	ExitCaptures m_Captures{};

	// Whether a client may reach the glass on a plane at all. True unless `--no-planes` said otherwise.
	bool m_Planes = true;

	Wake m_Armed = Wake::Never();
};
