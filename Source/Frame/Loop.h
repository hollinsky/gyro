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
#include "Core/Wake.h"
#include "Frame/Admission.h"
#include "Frame/Budget.h"
#include "Frame/Evaluator.h"
#include "Frame/FrameClock.h"
#include "Frame/Timing.h"
#include "Geometry/Region.h"
#include "Geometry/Space.h"
#include "Publication/Reader/Reader.h"
#include "Publication/Return.h"
#include "Publication/Ring.h"
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

		if (m_InFlight == 0)
		{
			return;
		}

		if (m_InFlightSnapshots[0] >= m_Presented.Sequence)
		{
			m_Presented = { .Sequence = m_InFlightSnapshots[0], .At = info.PresentedAt };
		}

		--m_InFlight;

		for (std::uint32_t index = 0; index < m_InFlight; ++index)
		{
			m_InFlightSnapshots[index] = m_InFlightSnapshots[index + 1];
		}

		m_InFlightSnapshots[m_InFlight] = 0;
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
		m_InFlight = 0;
		m_Committed = FrameClock::NoSequence;

		// The sequences those commits were drawn from go with them, because a frame nobody will flip is
		// one this output never presented. `m_Presented` deliberately stays: it is a flip that already
		// happened and is still owed to dispatch, and dropping it here would lose a frame callback on
		// the iteration a mode set landed in.
		m_InFlightSnapshots = {};

		// An index into a set that no longer exists. `OnTargetsInvalidated` is the case this is here for
		// — the images are released before the new ones exist, so a held index names memory that is gone
		// and the next attempt has to ask the new set for one of its own.
		m_Acquired.reset();
	}

	IPresenter* m_Presenter = nullptr;
	IRenderer* m_Renderer = nullptr;
	std::size_t m_Device = 0;

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

	// The most recent flip this output has not yet reported, staged in the drain and cleared by the post
	// that carries it.
	PresentedFrame m_Presented{};

	Region<DeviceSpace> m_Damage{};

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
	}

	// The sources the loop drains, at whatever granularity the backend's descriptors actually have.
	// Decision 83 puts dispatch's publication among them: it is the one that wakes an idle frame
	// thread, and it is a source rather than a mechanism precisely so that this signature does not
	// change to accommodate it.
	void Listen(std::span<IEventSource* const> sources) noexcept { m_Sources = sources; }

	// One iteration. Returns the wake the next one is owed at; `Wake::Never()` arms nothing.
	[[nodiscard]] Wake Step()
	{
		// First, and for correctness rather than tidiness. See this file's header.
		for (IEventSource* const source : m_Sources)
		{
			if (source != nullptr)
			{
				(void)source->Drain();
			}
		}

		const Instant now = m_Clock->Now();
		std::array<Instant, MaxDevices> deviceFree{};

		{
			// Core/FrameSection.h names this span exactly: from acquiring the published snapshot through
			// `Present`. The drain is outside it because a backend reading its own descriptor is not on
			// the frame path in the sense decision 36 means, and the fold is outside it because it is
			// arithmetic over state the iteration has already finished producing.
			const FrameSection guard;

			Acquire();
			CollectCosts();

			std::array<std::uint8_t, MaxOutputs> order{};
			const std::size_t due = OrderByDeadline(order);

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

		return Fold(now, deviceFree);
	}

	[[nodiscard]] std::uint64_t Held() const noexcept { return m_Held; }

	[[nodiscard]] const SnapshotReader& Snapshot() const noexcept { return m_Snapshot; }

private:
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
		Instant& free = deviceFree[output.m_Device];
		const FrameDecision decision = m_Timing.Assess(output.m_Clock, output.m_Cost, now, output.m_Committed, free);

		output.m_Last = decision;

		// A commit the presenter cannot take yet is the disqualifier Architecture.md names, and it is
		// separate from the arithmetic one `Assess` answers: it is the backend's condition rather than
		// the schedule's, and how many it will take is the backend's to say. One is KMS's rule — a
		// second nonblocking commit on a CRTC that has not flipped is refused — and a nested window
		// answers two, because its completion arrives from the host a whole refresh after the frame it
		// is about.
		if (output.IsCommitFull() || !decision.Renders())
		{
			return;
		}

		// `Assess` answers whether a frame *can* be made and never whether one is *wanted* — it is
		// decision 35's check and it never asks what anything costs to want. This is the other half, and
		// it is the scene's `Wake` rather than a second opinion about timing.
		if (!Wants(output, index, decision))
		{
			return;
		}

		// **Acquired once and held until it is presented, which is the whole of the fix and reads as an
		// omission until you follow the other path out of here.** `AcquireTarget` is the loop taking a
		// target *out* of the presenter's free set, and the only way one goes back is by being presented.
		// So an iteration that acquires and then refuses has taken an image nobody can hand out again,
		// and a standing refusal empties the set one frame at a time: on a triple-buffered output the
		// third refusal is the last thing that output ever attempts. What that looks like on screen is a
		// panel that goes black and stays black, and what it looks like in the log is a refusal count of
		// three for a run of thousands — a number small enough to read as a hiccup.
		//
		// Holding it is not a workaround for a release verb `IPresenter` does not have. The loop *does*
		// own this image, it will render into it on the next attempt, and the bug was only ever that it
		// forgot. `Discard` drops it, because a target set that was invalidated took this index with it.
		if (!output.m_Acquired)
		{
			output.m_Acquired = output.m_Presenter->AcquireTarget();
		}

		if (!output.m_Acquired)
		{
			// Decision 30's lead, bounded from the other side: an output with no free target cannot run
			// ahead, so a double-buffered one cannot run ahead at all. Not an error, and not damage lost.
			return;
		}

		const std::uint32_t target = *output.m_Acquired;
		const std::span<const RenderTarget> targets = output.m_Presenter->Targets();

		if (target >= targets.size())
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

			return;
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

			return;
		}

		const DrawList list = m_Evaluator->Evaluate(
			{ .Snapshot = m_Snapshot,
		      .Output = index,
		      .Outputs = m_Outputs.size(),
		      .Resolution = output.m_Configuration.Resolution,
		      .Presentation = decision.Presentation,
		      .Mode = decision.Mode() }
		);

		(void)output.m_Cost.ObserveIrreducibleCpu(list.EvaluateCost);
		output.m_Damage.Add(list.Damage);

		// The buffer-age join, and it is built after the evaluator has contributed so that this frame's
		// own damage is in it. A copy rather than a reference because `RecordRequest` takes the region by
		// value and the loop must not hand a target's backlog somewhere it could be cleared from — and it
		// is a fixed-size array of rectangles on the stack, which is what decision 36 asks of it.
		Region<DeviceSpace> stale = output.m_Damage;
		stale.Add(output.m_Backlog[target]);

		const RecordRequest request{ .Target = target,
			                         .Mode = decision.Mode(),
			                         .CostGeneration = output.m_Cost.Generation(),
			                         .Damage = stale,
			                         .Items = list.Items };

		const Result<Submission> submission = output.m_Renderer->Record(request);

		if (!submission)
		{
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

		const PixelSize<DeviceSpace> size = targets[target].Size;
		const PresentLayer layer{
			.Target = target,
			.Blend = BlendMode::Opaque,
			.Acquire = submission->Point,
			.Source = { {}, { static_cast<float>(size.Width), static_cast<float>(size.Height) } },
			.Destination = { {}, size },
			.Damage = output.m_Damage,
			.Color = output.m_Configuration.Color,
		};

		if (const Result<void> presented = output.m_Presenter->Present({ &layer, 1 }); !presented)
		{
			output.Refuse(presented.error());

			return;
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
		++output.m_InFlight;

		// The backlogs fold forward, and this is the whole of the buffer-age bookkeeping. Every *other*
		// target now owes this frame's change on top of what it already owed, and the one just drawn owes
		// nothing — its content is complete as of now, which is true whether or not the glass ever shows
		// it. Before the clear below, because the region being folded is the one being cleared.
		for (std::size_t other = 0; other < MaxTargets; ++other)
		{
			if (other != target)
			{
				output.m_Backlog[other].Add(output.m_Damage);
			}
		}

		output.m_Backlog[target].Clear();
		output.m_Damage.Clear();
	}

	// Whether this output is owed the frame `Assess` says it could make.
	//
	// Damage outranks everything, since it is pixels that have not reached the glass. Otherwise it is the
	// scene's answer, and the comparison is against the *presentation* rather than against now: the loop
	// wakes a reserve ahead of the frame it is serving, so an instant that has not arrived yet is
	// precisely the case this exists to admit. Testing `IsDue(now)` here would refuse every frame at
	// exactly the moment the schedule woke up to draw it.
	[[nodiscard]] bool Wants(const FrameOutput& output, std::size_t index, const FrameDecision& decision) const noexcept
	{
		if (!output.m_Damage.IsEmpty())
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

		// Owed a frame as soon as one can be made: damage that has not reached the glass, a flip whose
		// completion the clock is still waiting for, or a scene contributor that wants every frame.
		const bool immediate = !output.m_Damage.IsEmpty() || output.IsFlipPending() ||
		                       (scene.Which == Wake::Kind::Continuous && scene.Interval == Duration::zero());

		if (immediate)
		{
			return m_Timing.WakeFor(
				output.m_Clock, output.m_Cost, now, output.m_Committed, deviceFree[output.m_Device]
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

		const Instant at = output.m_Clock.WakeupAt(sequence, m_Timing.Reserve(output.m_Cost, RenderMode::Planned));

		return at == FrameClock::Unscheduled ? Wake::Never() : Wake::At(at);
	}

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
};
