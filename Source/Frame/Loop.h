#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "Core/Clock.h"
#include "Core/FrameSection.h"
#include "Core/Signal.h"
#include "Core/Wake.h"
#include "Frame/Budget.h"
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
// **What the loop does not have yet is anything to draw.** Decision 82 settles what crosses the render
// seam — a flat span of evaluated draw items produced by `Frame` into its own arena — and the thing
// that turns a snapshot into that span is the evaluator below. It is an interface inside this module
// rather than at the seam because it has no second implementation that is not a test: `Scene` will
// publish, `Frame` will evaluate, and no backend is ever on the other end of it. Until there is a
// scene to evaluate, `NullEvaluator` draws nothing and every ordering above is still exercised.

// SPEC: sized rather than measured. Sixteen outputs and four rendering devices are past any
// configuration gyro has been pointed at, and both are here as fixed capacity because the frame
// section forbids growing either.
inline constexpr std::size_t MaxOutputs = 16;
inline constexpr std::size_t MaxDevices = 4;

// What one output's frame is, once the snapshot has been evaluated at its predicted presentation.
//
// Damage is the new damage this evaluation produced, in device space; the loop unions it into what the
// output has been accumulating rather than replacing it, for the reason in this file's header.
struct DrawList
{
	std::span<const DrawItem> Items;
	Region<DeviceSpace> Damage;
};

struct EvaluateRequest
{
	const SnapshotReader& Snapshot;

	// Which output, as an index into the loop's outputs. It is the snapshot's index too — decision 84
	// has per-output runs cross positionally under a set generation — so this is what an evaluator
	// resolves the output's runs with.
	std::size_t Output = 0;

	// Decision 36 in one parameter: animations are evaluated at a named instant and never against an
	// ambient now. It is the predicted presentation of the frame being drawn, which is per output
	// because there is no global clock to make it anything else.
	Instant Presentation{};

	RenderMode Mode = RenderMode::Planned;
};

class IEvaluator
{
public:
	IEvaluator() = default;

	virtual ~IEvaluator() = default;

	IEvaluator(const IEvaluator&) = delete;
	IEvaluator& operator=(const IEvaluator&) = delete;
	IEvaluator(IEvaluator&&) = delete;
	IEvaluator& operator=(IEvaluator&&) = delete;

	// Called inside the frame section, so the items must come from storage the evaluator already holds.
	// The span is read before the next call and never after it.
	[[nodiscard]] virtual DrawList Evaluate(const EvaluateRequest& request) = 0;
};

// Draws nothing, which is what this module's own tests want and what the composition root binds until
// there is a `Scene`. It is not a stub in the sense that it is waiting to be replaced by a real
// implementation of the same thing — it is the floor case, and an output with nothing to draw is one
// the loop must still schedule, present, and idle correctly.
class NullEvaluator final : public IEvaluator
{
public:
	[[nodiscard]] DrawList Evaluate(const EvaluateRequest&) override { return {}; }
};

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

	[[nodiscard]] bool IsFlipPending() const noexcept { return m_FlipPending; }

	[[nodiscard]] const Region<DeviceSpace>& Damage() const noexcept { return m_Damage; }

	// What the last iteration decided, kept so that a sweep or a log line can read the slack without
	// the loop having to report through a channel that does not otherwise exist.
	[[nodiscard]] const FrameDecision& Last() const noexcept { return m_Last; }

	// Damage from outside the scene — a backend that lost its targets, a console that drew over the
	// output, a first frame with nothing behind it.
	void AddDamage(const Region<DeviceSpace>& region) noexcept { m_Damage.Add(region); }

	void DamageWholeOutput() noexcept { m_Damage.Add(PixelRect<DeviceSpace>{ {}, m_Configuration.Resolution }); }

private:
	friend class FrameLoop;

	void OnPresented(const PresentationInfo& info) noexcept
	{
		m_Clock.Observe(info);
		m_FlipPending = false;
	}

	// Decision 73: the transition completes as an event some time later, and this is that event. The
	// clock re-anchors from what was achieved rather than from what was asked for, and the record is
	// invalidated because a cost measured under the old mode is a cost from another configuration —
	// which is what `Budget`'s generation exists to say.
	void OnReconfigured(const OutputConfiguration& achieved) noexcept
	{
		Adopt(achieved);
		m_Clock.Invalidate();
		m_Clock.Configure(achieved);
		m_Cost.Invalidate();
		Discard();
	}

	// The targets are gone, so anything recorded against one is gone with it. The output owes a whole
	// frame afterwards, because there is no longer a previous frame for damage to be relative to.
	void OnTargetsInvalidated() noexcept
	{
		Discard();
		DamageWholeOutput();
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
		m_FlipPending = false;
		m_Committed = FrameClock::NoSequence;
	}

	IPresenter* m_Presenter = nullptr;
	IRenderer* m_Renderer = nullptr;
	std::size_t m_Device = 0;

	OutputConfiguration m_Configuration{};
	FrameClock m_Clock{};
	Budget m_Cost{};

	std::uint64_t m_Committed = FrameClock::NoSequence;
	bool m_FlipPending = false;
	Region<DeviceSpace> m_Damage{};
	FrameDecision m_Last{};

	Connection<const PresentationInfo&> m_OnPresented;
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
			(void)m_Returns->Post(m_Held);
		}

		return Fold(now, deviceFree);
	}

	[[nodiscard]] std::uint64_t Held() const noexcept { return m_Held; }

	[[nodiscard]] const SnapshotReader& Snapshot() const noexcept { return m_Snapshot; }

private:
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

		// A flip still outstanding is the disqualifier Architecture.md names, and it is separate from
		// the arithmetic one `Assess` answers: KMS refuses a second nonblocking commit on a CRTC that
		// has not flipped, so this is the hardware's condition rather than the schedule's.
		if (output.m_FlipPending || !decision.Renders())
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

		const std::optional<std::uint32_t> target = output.m_Presenter->AcquireTarget();

		if (!target)
		{
			// Decision 30's lead, bounded from the other side: an output with no free target cannot run
			// ahead, so a double-buffered one cannot run ahead at all. Not an error, and not damage lost.
			return;
		}

		const std::span<const RenderTarget> targets = output.m_Presenter->Targets();

		if (*target >= targets.size())
		{
			return;
		}

		const DrawList list = m_Evaluator->Evaluate(
			{ .Snapshot = m_Snapshot, .Output = index, .Presentation = decision.Presentation, .Mode = decision.Mode() }
		);

		output.m_Damage.Add(list.Damage);

		const RecordRequest request{ .Target = *target,
			                         .Mode = decision.Mode(),
			                         .CostGeneration = output.m_Cost.Generation(),
			                         .Output = output.m_Configuration.Color,
			                         .Damage = output.m_Damage,
			                         .Items = list.Items };

		const Result<Submission> submission = output.m_Renderer->Record(request);

		if (!submission)
		{
			return;
		}

		// The device is busy from here whatever happens to the present, so the next output on this queue
		// starts from the new figure even if the flip below is refused.
		free = decision.DeviceFreeAt;
		(void)output.m_Cost.ObserveCpu(decision.Mode(), submission->RecordCost);

		const PixelSize<DeviceSpace> size = targets[*target].Size;
		const PresentLayer layer{
			.Target = *target,
			.Blend = BlendMode::Opaque,
			.Acquire = submission->Point,
			.Source = { {}, { static_cast<float>(size.Width), static_cast<float>(size.Height) } },
			.Destination = { {}, size },
			.Damage = output.m_Damage,
			.Color = output.m_Configuration.Color,
		};

		if (!output.m_Presenter->Present({ &layer, 1 }))
		{
			return;
		}

		output.m_Committed = decision.Sequence;
		output.m_FlipPending = true;
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
		const bool immediate = !output.m_Damage.IsEmpty() || output.m_FlipPending ||
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
};
