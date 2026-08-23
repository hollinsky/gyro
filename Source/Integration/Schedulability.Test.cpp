#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "Core/Clock.h"
#include "Core/ColorState.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Frame/Admission.h"
#include "Frame/Loop.h"
#include "Headless/Device.h"
#include "Headless/Output.h"
#include "Headless/Renderer.h"
#include "Publication/Publisher/Publisher.h"
#include "Publication/Return.h"
#include "Publication/Ring.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/PresentationInfo.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Testing/Test.h"

// Decision 29's claim, run rather than argued.
//
// > If the task set passes the test, every deadline is met at every phase relationship, and the
// > outputs never influence each other at all. If it fails, no amount of phase modelling saves it.
//
// That sentence in Docs/Architecture.md#outputs-are-independent-periodic-tasks is the whole of what
// this file exists to falsify. It names both `Frame` and `Headless`, which no module may, so it lives
// here for the reason the publication soak beside it does — and it is the first test in the tree that
// is about the *system* rather than about a component's arithmetic. `FrameClock`, `Budget`, `Timing`,
// and `Admission` each assert their own terms where they live; nothing below re-derives one. What is
// asserted here is what a panel did.
//
// **Time is virtual and the sweep owns it, which is the one thing that makes any of this mean
// something.** A `ManualClock` that only ever jumps between wakes would make every prediction fit and
// every assertion vacuous, so the harness below is a machine rather than a driver:
//
//   - **The clock is the GPU queue.** A composite is non-preemptible and two outputs' work serialises
//     on one queue however it was recorded, so `Record` advances the clock by the job it just ran.
//     That is the uniprocessor the section above models, spelled as the only resource in the machine.
//   - **`C` is charged to the GPU half.** `Timing::Project` composes the two device figures as a
//     pipeline — record on the frame thread while the device finishes what it has, execute once it is
//     free — so it is the GPU term that accumulates across outputs and the CPU term that overlaps.
//     Charging the composite to the half that serialises is what makes the loop's model and this
//     machine's model the same model; charging it to the CPU half would have the loop predict
//     `max(C₁, C₂)` where the machine ran `C₁ + C₂`, and the sweep would report the difference as a
//     scheduling defect.
//   - **The commit is issued when the pixels exist.** `HeadlessOutput::Present` latches against the
//     clock, and the clock has by then advanced through the job, so a frame that ran past its vblank
//     latches onto the next one. That is the miss, and it is the panel's own arithmetic rather than
//     an assertion about a prediction.
//
// **What is swept is phase, because phase is the one input the test does not take.** Vblank zero of
// the first output sits at the origin and every other output's is walked across its period, which
// visits every alignment the two grids can have — including the critical instant, where every output
// is released at once and the blocking term is the whole of what stands between the set and a miss.
// Docs/Architecture.md#phase-drift-is-not-something-to-track is why this is a sweep rather than a
// case: real panels drift, so every relationship eventually happens on somebody's desk.
//
// **A missed deadline is a vblank that carried no new frame**, counted from the presentations the
// panel reported. Nothing else is a miss: an output that renders the floor composite hit its deadline,
// and decision 35's whole argument is that the tier step is how a deadline is *met* under load.
//
// What this cannot prove is in Docs/Architecture.md#what-nested-can-and-cannot-prove's spirit: `C`
// here is a number a test chose, not a cost a GPU incurred, so this says nothing about whether an
// allocation is achievable — only that the schedule built on it holds. The measurement half arrives
// with a renderer.

namespace
{
using namespace std::chrono_literals;

// Past any set the sweep drives. Three is the interesting number, because it is what
// Docs/Architecture.md needs to break the two-output simplification, and four is headroom.
constexpr std::size_t kMaxPanels = 4;

// Architecture.md's own configuration, quoted to the number: a 144 Hz panel beside a 60 Hz projector.
constexpr Duration kFast = PeriodFromHertz(144.0);
constexpr Duration kSlow = PeriodFromHertz(60.0);

// How many alignments of one grid against another are walked. Sixteen is not a divisor of the 12:5
// ratio those two periods have, which is deliberate — a step that shared a factor with the beat would
// visit a sublattice of the relationships and report the rest as untested.
constexpr std::size_t kPhaseSteps = 32;

// Long enough for every clock to anchor, every ring to reach its steady state, and the first
// allocation's worth of cost samples to be filed. Nothing before this instant is measured.
constexpr Duration kWarmup = 20 * kSlow;

// Two seconds of virtual time, which is twenty-four beats of the 83.3 ms realignment the two nominal
// periods have, so a failure that happens at one point in the beat has somewhere to happen. The whole
// sweep costs under a second of wall clock, because nothing here draws anything.
constexpr Duration kMeasured = 120 * kSlow;

// Reaching this means the loop asked to be woken at an instant it had already passed, repeatedly, with
// nothing in the machine able to move — a livelock to report rather than a run to wait out.
constexpr std::size_t kStallLimit = 16;

// One simulated panel, as the sweep configures it. The allocation it runs against is separate, because
// the point of half these tests is to run a panel against an allocation admission control did not give
// it.
struct PanelSpec
{
	// `C_min`, which the panel is charged whenever the loop draws the floor composite. The period is
	// not here: it is the allocation's, because rung 1 lengthens it and a panel running at the period
	// it was configured with rather than the one the plan committed to would be a different machine.
	Duration Floor{};

	// Where vblank zero sits. This is what the sweep varies.
	Duration Phase{};
};

// A phase, as a fraction of a period.
[[nodiscard]] constexpr Duration PhaseAt(Duration period, std::size_t step, std::size_t steps) noexcept
{
	return period * static_cast<std::int64_t>(step) / static_cast<std::int64_t>(steps);
}

// `SimulatedRenderer`, plus the one thing a sweep needs that a unit test does not: the job runs.
//
// It is a decorator rather than a change to the module, because consuming time is the *harness's*
// model of execution and not a property of a renderer. Headless charges a simulated `C` and reports
// it, which is what its own tests want; what turns a reported cost into an occupied queue is the
// composition root, and here that is this file. Core/Clock.h's discipline is satisfied for the same
// reason — the clock is handed in by whoever owns it.
class ExecutingRenderer final : public IRenderer
{
public:
	ExecutingRenderer(ManualClock& clock, SimulatedRendererPolicy policy) noexcept
		: m_Clock{ &clock }, m_Policy{ policy }, m_Inner{ policy }
	{}

	[[nodiscard]] Result<void> BindTargets(std::span<const RenderTarget> targets, ColorState) override
	{
		return m_Inner.BindTargets(targets, ColorState::Srgb());
	}

	void ReleaseTargets() noexcept override { m_Inner.ReleaseTargets(); }

	[[nodiscard]] Result<Submission> Record(const RecordRequest& request) override
	{
		const Result<Submission> submission = m_Inner.Record(request);

		if (!submission)
		{
			return submission;
		}

		// The whole of the machine. `RecordCost` is what the inner renderer says this frame's CPU half
		// cost — including an injected overrun, which is why the advance is read back from the answer
		// rather than recomputed from the policy — and the GPU half is the term that serialises.
		m_Clock->Advance(submission->RecordCost + Gpu(request.Mode));

		return submission;
	}

	[[nodiscard]] bool IsComplete(SyncPoint point) const override { return m_Inner.IsComplete(point); }

	[[nodiscard]] std::size_t CollectCosts(std::span<GpuCost> into) override { return m_Inner.CollectCosts(into); }

	[[nodiscard]] SimulatedRenderer& Inner() noexcept { return m_Inner; }

private:
	[[nodiscard]] Duration Gpu(RenderMode mode) const noexcept
	{
		return mode == RenderMode::Planned ? m_Policy.PlannedGpu : m_Policy.FloorGpu;
	}

	ManualClock* m_Clock = nullptr;
	SimulatedRendererPolicy m_Policy{};
	SimulatedRenderer m_Inner;
};

// What the panel did.
//
// It observes the presenter rather than the loop on purpose: the loop's verdict is what gyro *decided*
// and a vblank with no new frame on it is what the glass *got*, and a sweep that only checked the
// first would pass on a system that predicted every frame correctly and presented none of them.
//
// It counts frames rather than misses, and the machine subtracts. A gap between two presented
// sequences is the obvious measure and it cannot see the failure that matters most — an output that
// presents *nothing* has no two sequences to have a gap between, so total starvation reads as a clean
// run. What the machine does instead is compare against the vblanks that actually happened.
class Witness
{
public:
	void Watch(IPresenter& presenter) { m_OnPresented.ConnectTo<&Witness::OnPresented>(presenter.Presented, *this); }

	void Arm() noexcept
	{
		m_Armed = true;
		m_Seen = false;
		m_Frames = 0;
		m_LongestGap = 0;
	}

	[[nodiscard]] std::uint64_t Frames() const noexcept { return m_Frames; }

	// The longest run of vblanks between two presented frames, which is the difference between a miss
	// that cost one frame and a cascade.
	[[nodiscard]] std::uint64_t LongestGap() const noexcept { return m_LongestGap; }

private:
	void OnPresented(const PresentationInfo& info) noexcept
	{
		if (!m_Armed)
		{
			return;
		}

		if (m_Seen && info.Sequence > m_Last + 1)
		{
			m_LongestGap = std::max(m_LongestGap, info.Sequence - m_Last - 1);
		}

		m_Last = info.Sequence;
		m_Seen = true;
		++m_Frames;
	}

	Connection<const PresentationInfo&> m_OnPresented;
	std::uint64_t m_Last = 0;
	std::uint64_t m_Frames = 0;
	std::uint64_t m_LongestGap = 0;
	bool m_Armed = false;
	bool m_Seen = false;
};

// The composition root, in virtual time: the clock, the backend, the publication channel, the outputs,
// and the `while` that Docs/Structure.md says belongs up here rather than inside the step.
class Machine
{
public:
	// `scene` is what every output's contributor folded to. `Wake::EveryFrame()` is the steady state
	// the schedulability claim is about — a scene that settles is a scene whose deadlines are trivially
	// met — and `Wake::Never()` is the other end of the same axis: the state every output is in before
	// the first thing that ever wants a frame arrives, where the only thing that asks for one is damage.
	Machine(std::span<const PanelSpec> panels, std::span<const Allocation> allocations, Wake scene = Wake::EveryFrame())
		: m_Count{ std::min({ panels.size(), allocations.size(), kMaxPanels }) }
	{
		std::array<Wake, kMaxPanels> wakes{};
		wakes.fill(scene);

		Build(panels, allocations, { wakes.data(), m_Count });
	}

	// The same machine with the scene stated per output, which is what decision 69's associativity is
	// for. The fold is partitioned by output precisely so that a blinking cursor on one panel does not
	// wake the other, and a scene that says the same thing about every panel cannot tell whether it was.
	//
	// An empty span publishes nothing at all rather than publishing an empty scene. That is a different
	// state and it is the one the frame thread is actually in first: dispatch has never built a
	// snapshot, so the reader is invalid, `Wakes()` is empty, and decision 84's rule reads that as no
	// information rather than as partial information. It has to fold to idle by the same route.
	Machine(std::span<const PanelSpec> panels, std::span<const Allocation> allocations, std::span<const Wake> scene)
		: m_Count{ std::min({ panels.size(), allocations.size(), kMaxPanels }) }
	{
		Build(panels, allocations, scene);
	}

	// What one turn of the shim did, which is the whole of what `Run` and `RunUntilIdle` disagree
	// about. Both drive the same iteration; one stops at an instant and the other stops at `Idle`.
	enum class Turn : std::uint8_t
	{
		Slept,   // the clock moved on to the next thing that can change anything
		Ran,     // a job ran, but nothing is scheduled past the instant it finished at
		Stalled, // nothing happened and nothing is scheduled past now
		Idle,    // no timer to arm and no panel owing a flip: the composition root would block forever
	};

	// The shim of decision 80: wake at or after the instant the step asked for, or earlier when a
	// source has something to say. The device's next event is that second condition — a headless flip
	// makes no file readable, so what a real shim learns from `io_uring` this one asks the backend.
	void Run(Duration duration)
	{
		const Instant until = Advanced(m_Clock.Now(), duration);
		std::size_t stalled = 0;

		while (m_Clock.Now() < until)
		{
			const Turn turn = Advance();

			if (turn == Turn::Idle)
			{
				// Nothing armed and no panel owing anything. Under a scene that wants every frame this
				// is unreachable, and it is here so that a machine which does go idle ends rather than
				// spins.
				break;
			}

			// Bounded rather than trusted: a machine that can no longer move is worth failing on rather
			// than hanging a test run.
			stalled = turn == Turn::Stalled ? stalled + 1 : 0;
			GYRO_REQUIRE(stalled < kStallLimit);
		}

		// The last vblank inside the window may fall after the final step, and a frame that reached the
		// glass unreported would read as a miss. Draining here is what makes `Vblanks` and the frames
		// counted against it describe the same interval.
		(void)m_Device.Drain();
	}

	// The same shim stopped at a *state* rather than at an instant: iterate until the composition root
	// would block indefinitely, and answer how many iterations that took. Empty where it never got
	// there, which is the interesting failure — a loop that keeps arming a timer with nothing to draw
	// is Docs/Architecture.md#doing-nothing-must-cost-nothing violated, and it costs a wakeup per
	// period forever rather than showing up as a wrong pixel.
	//
	// Idle is `Wake::Never()` *and* a quiet backend, and both halves are load-bearing. A fold of
	// `Never()` while a flip is still outstanding is not idle — it is an iteration that has not
	// happened yet — and the count would then depend on where in a frame the caller happened to start.
	[[nodiscard]] std::optional<std::size_t> RunUntilIdle(std::size_t limit)
	{
		std::size_t stalled = 0;

		for (std::size_t iterations = 1; iterations <= limit; ++iterations)
		{
			const Turn turn = Advance();

			if (turn == Turn::Idle)
			{
				return m_Fold == Wake::Never() ? std::optional{ iterations } : std::nullopt;
			}

			stalled = turn == Turn::Stalled ? stalled + 1 : 0;

			if (stalled >= kStallLimit)
			{
				return std::nullopt;
			}
		}

		return std::nullopt;
	}

	// Begin measuring. Everything before this is the warm-up: unanchored clocks, unseeded budgets, and
	// an output whose phase puts its first vblank most of a period away.
	void Arm() noexcept
	{
		for (std::size_t index = 0; index < m_Count; ++index)
		{
			m_Witnesses[index].Arm();
			m_ArmedAt[index] = m_Panels[index]->Vblank().After(m_Clock.Now());
			m_Floors[index] = 0;
		}

		m_Armed = true;
	}

	// One iteration, sampled. `Run` is the shim around this; a test that wants to look at a single
	// decision calls it directly.
	Wake Step()
	{
		m_Fold = m_Loop.Step();

		Sample();

		return m_Fold;
	}

	// What the last step folded to, so that a driver which consumed the answer can still be asked what
	// it was.
	[[nodiscard]] Wake Fold() const noexcept { return m_Fold; }

	[[nodiscard]] Instant Now() const noexcept { return m_Clock.Now(); }

	// Time passing with the loop asleep, which is not `Run`: there is nothing to run, and what the idle
	// case is about is the state the clock is left in when something finally asks for a frame.
	void Idle(Duration duration) noexcept { m_Clock.Advance(duration); }

	// Damage from outside the scene, which is the only thing that asks a settled output for a frame.
	void Damage(std::size_t index) noexcept { m_Outputs[index].DamageWholeOutput(); }

	// What the loop decided about this output on the iteration that last visited it.
	[[nodiscard]] const FrameDecision& Decision(std::size_t index) const noexcept { return m_Outputs[index].Last(); }

	[[nodiscard]] const Witness& Panel(std::size_t index) const noexcept { return m_Witnesses[index]; }

	// Commits *issued*, which is the other side of what the witness counts. A frame that reached the
	// glass is the measure everywhere else in this file; the idle invariant is about work gyro did at
	// all, so what it asserts on is `Present` having been called and not the flip that followed.
	[[nodiscard]] std::uint64_t Commits(std::size_t index) const noexcept { return m_Panels[index]->Commits; }

	// How many vblanks this panel has had since the measurement was armed. `After` is the first vblank
	// strictly past an instant, so the difference of two of them is the count of vblanks between, and
	// `Run` drains once more on the way out so the last of them has been reported.
	[[nodiscard]] std::uint64_t Vblanks(std::size_t index) const noexcept
	{
		return m_Panels[index]->Vblank().After(m_Clock.Now()) - m_ArmedAt[index];
	}

	// **The measurement.** A vblank that carried no new frame is a missed deadline and nothing else is:
	// an output that drew the floor composite met its deadline, which is the whole of decision 35's
	// argument, and an output the schedule declined to visit this iteration has not missed anything
	// until a vblank goes by without it.
	[[nodiscard]] std::uint64_t Missed(std::size_t index) const noexcept
	{
		const std::uint64_t vblanks = Vblanks(index);
		const std::uint64_t frames = m_Witnesses[index].Frames();

		return vblanks > frames ? vblanks - frames : 0;
	}

	[[nodiscard]] std::uint64_t Missed() const noexcept
	{
		std::uint64_t missed = 0;

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			missed += Missed(index);
		}

		return missed;
	}

	// Frames made at the floor tier. Not a miss — the deadline was met — but the rung it was met on.
	//
	// There is deliberately no counter for `Admission::Wait` beside this one, and the reason is now in
	// the enumerator's name: the verdict is not a miss signal because it is not a frame drop at all.
	// `Timing::Assess` reaches it only where every tier's reach is *behind* the frame owed, which is an
	// output that has already committed its next frame and is waiting for the flip — so it answers on
	// each visit until that flip lands, and it means *not yet due* and nothing else. A frame
	// deliberately dropped is a render verdict whose sequence jumped past the frame owed, and it is
	// still only the panel that says whether the glass got one.
	[[nodiscard]] std::uint64_t Floors(std::size_t index) const noexcept { return m_Floors[index]; }

	// Make the next frame this output records cost `by` more than it was allocated, once. The transient
	// of Docs/Architecture.md#headless: a scene that is expensive for one frame, which is what proves a
	// miss costs one frame rather than starting a cascade.
	void Overrun(std::size_t index, Duration by) noexcept { m_Renderers[index]->Inner().Overrun = by; }

private:
	void Build(std::span<const PanelSpec> panels, std::span<const Allocation> allocations, std::span<const Wake> scene)
	{
		for (std::size_t index = 0; index < m_Count; ++index)
		{
			Attach(index, panels[index], allocations[index]);
		}

		if (!scene.empty())
		{
			// Published once, since the ring holds what it was given and the frame side re-reads nothing
			// it already holds. Exactly one wake per bound output: decision 84 has a schedule of the
			// wrong length read as no information, so a short span would quietly test the unpublished
			// case instead of the one it named.
			GYRO_CHECK(scene.size() >= m_Count);

			SnapshotPublisher publisher;
			publisher.PutWakes(scene.first(std::min(scene.size(), m_Count)));
			publisher.Build(m_Snapshot, 1);

			GYRO_CHECK(m_Ring.Publish(m_Snapshot.Bytes(), 0));
		}

		m_Loop.Bind({ m_Outputs.data(), m_Count });
		m_Loop.Listen({ m_Sources.data(), 1 });
	}

	// One turn of the shim: a step, and the sleep the composition root would have taken after it.
	Turn Advance()
	{
		const Instant entered = m_Clock.Now();
		const Wake wake = Step();
		const Instant now = m_Clock.Now();

		Instant next = m_Device.NextEvent();

		if (wake.Which != Wake::Kind::Settled)
		{
			next = std::min(next, wake.When);
		}

		if (next <= now)
		{
			// **The step asked for an instant the machine has already passed**, which happens on every
			// iteration between a commit and its flip once the next frame's record point is behind: the
			// output is owed a frame, so the fold prices one, and the flip it is waiting for has not
			// landed. A shim that armed that timer would wake immediately, do nothing, and arm it again.
			// Waking *at or after* the instant asked for is the whole of the contract, so this waits for
			// the next thing that can change anything instead.
			next = m_Device.NextEvent();
		}

		if (next == Instant{ Duration::max() })
		{
			return Turn::Idle;
		}

		if (next > now)
		{
			m_Clock.Set(next);

			return Turn::Slept;
		}

		// Even the panel has nothing further to say at an instant the machine has not reached, so
		// another step can only repeat this one — unless the step itself consumed time, which is a job
		// having run and is progress however this turn is scored.
		return now == entered ? Turn::Stalled : Turn::Ran;
	}

	void Attach(std::size_t index, const PanelSpec& panel, const Allocation& allocation)
	{
		OutputConfiguration configuration;

		// Small on purpose. Nothing is drawn here — `C` is a number the renderer is told rather than a
		// cost pixels incurred — and a headless target is a real mapping, so a panel-sized one would
		// have the sweep spend most of its wall clock zeroing thirty megabytes per machine it builds.
		// The resolution reaches the plane catalog and the layer rectangles and nothing else.
		configuration.Resolution = { 320, 180 };
		configuration.Period = allocation.Period;

		HeadlessOutput* output = m_Device.Add(configuration, { .Targets = 2, .Phase = panel.Phase });

		if (output == nullptr)
		{
			return;
		}

		// The costs the simulated scene incurs, and the budget the loop schedules against, from the one
		// allocation. They are seeded rather than learned so that the first measured frame is already in
		// the steady state the plan describes; the samples that arrive afterwards agree with the seed,
		// which is what makes an injected overrun the only thing that ever moves a mark.
		const SimulatedRendererPolicy costs{ .PlannedGpu = allocation.Cost, .FloorGpu = panel.Floor };
		const BudgetPolicy budget{ .FloorGpu = panel.Floor, .InitialGpu = allocation.Cost };

		m_Renderers[index].emplace(m_Clock, costs);
		GYRO_CHECK(m_Renderers[index]->BindTargets(output->Targets(), ColorState::Srgb()));

		// One device for every output, because that is the coupling decision 29 is about: two outputs
		// are independent except that they share one frame thread and one queue.
		m_Outputs[index].Bind(*output, *m_Renderers[index], 0, configuration, {}, budget);
		m_Witnesses[index].Watch(*output);
		m_Panels[index] = output;
	}

	// The loop's own view, taken once per iteration and keyed on the frame it decided about, since an
	// output the schedule did not visit this iteration still holds the answer it gave last time.
	void Sample() noexcept
	{
		if (!m_Armed)
		{
			return;
		}

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			const FrameDecision& decision = m_Outputs[index].Last();

			if (decision.Sequence == m_Assessed[index])
			{
				continue;
			}

			m_Assessed[index] = decision.Sequence;
			m_Floors[index] += decision.Verdict == Admission::Floor ? 1 : 0;
		}
	}

	ManualClock m_Clock{};
	SnapshotRing m_Ring{};
	ReturnChannel m_Returns{};
	NullEvaluator m_Evaluator{};
	SnapshotBuffer m_Snapshot{};
	HeadlessDevice m_Device{ m_Clock };

	std::array<std::optional<ExecutingRenderer>, kMaxPanels> m_Renderers{};
	std::array<FrameOutput, kMaxPanels> m_Outputs{};
	std::array<Witness, kMaxPanels> m_Witnesses{};

	FrameLoop m_Loop{ m_Clock, m_Ring, m_Returns, m_Evaluator };
	std::array<IEventSource*, 1> m_Sources{ &m_Device };

	std::array<HeadlessOutput*, kMaxPanels> m_Panels{};
	std::array<std::uint64_t, kMaxPanels> m_ArmedAt{};
	std::array<std::uint64_t, kMaxPanels> m_Assessed{};
	std::array<std::uint64_t, kMaxPanels> m_Floors{};

	Wake m_Fold = Wake::Never();
	std::size_t m_Count = 0;
	bool m_Armed = false;
};

// The two runs every test below is a sweep of: settle the machine, then measure it.
void Settle(Machine& machine)
{
	machine.Run(kWarmup);
	machine.Arm();
	machine.Run(kMeasured);
}
} // namespace

// The claim itself. Docs/Architecture.md#outputs-are-independent-periodic-tasks says an admitted set
// meets every deadline at every phase relationship; this walks the relationships.
GYRO_TEST(Schedulability, AnAdmittedSetMissesNothingAtAnyPhase)
{
	// Feasible with room: 3 + 3 against a 6.944 ms period leaves just under a millisecond, so the set
	// passes at rung none and nothing is given up. The interesting arrangement is still the critical
	// instant, which the sweep reaches on its own.
	constexpr std::array<OutputTask, 2> tasks{
		OutputTask{ .Period = kFast, .Want = 3ms, .Floor = 1ms, .Focused = true },
		OutputTask{ .Period = kSlow, .Want = 3ms, .Floor = 1ms },
	};

	const Plan plan = Admit(tasks);

	GYRO_REQUIRE(plan.IsFeasible());
	GYRO_REQUIRE(plan.Deepest() == Rung::None);

	for (std::size_t step = 0; step < kPhaseSteps; ++step)
	{
		const std::array<PanelSpec, 2> panels{
			PanelSpec{ .Floor = 1ms },
			PanelSpec{ .Floor = 1ms, .Phase = PhaseAt(kSlow, step, kPhaseSteps) },
		};

		Machine machine{ panels, plan.Allocations() };
		Settle(machine);

		// A machine that presented nothing misses nothing, so the frames are asserted before the misses
		// are.
		GYRO_CHECK(machine.Panel(0).Frames() > 0);
		GYRO_CHECK(machine.Panel(1).Frames() > 0);

		GYRO_CHECK_EQ(machine.Missed(), std::uint64_t{ 0 });
		GYRO_CHECK_EQ(machine.Floors(0) + machine.Floors(1), std::uint64_t{ 0 });
	}
}

// The number Docs/Architecture.md quotes the configuration against: with a 4 ms composite on the
// 144 Hz panel, the projector's entire composite must fit in under 2.9 ms.
GYRO_TEST(Schedulability, AnInfeasibleSetIsCutToWhatTheFastPanelLeaves)
{
	constexpr std::array<OutputTask, 2> tasks{
		OutputTask{ .Period = kFast, .Want = 4ms, .Floor = 1ms, .Focused = true },
		OutputTask{ .Period = kSlow, .Want = 5ms, .Floor = 1ms },
	};

	const Plan plan = Admit(tasks);

	GYRO_REQUIRE(plan.IsFeasible());
	GYRO_REQUIRE(plan.Deepest() == Rung::Tier);

	// The panel under someone's hands keeps what it asked for, and the projector gives it up. That is
	// the tie-break rather than a coincidence of these numbers.
	GYRO_CHECK(!plan[0].Reduced());
	GYRO_CHECK(plan[1].Reduced());
	GYRO_CHECK(plan[0].Cost + plan[1].Cost <= kFast);
}

// The other half of the claim, and the half that gives the assertion above its teeth: a set that does
// not pass the test drops frames, and the sweep is what sees them.
//
// **The blocking job is longer than the fast panel's whole period**, which is the failure mode the
// non-preemptive term exists for and the only one this loop is actually exposed to. A shorter blocker
// is absorbed — see the note on the three-output set below — so a pair chosen to fail the arithmetic
// by a little would have proved nothing here.
//
// **What the blocked panel loses is a share of its vblanks and never two in a row**, which is the
// shape a scheduling failure is supposed to have and is what this test is named for. It read
// *starves* while `Timing::Assess` admitted work only where the frame it could reach was the frame it
// owed: a panel blocked for longer than its period reaches `owed + 1`, refused, presented nothing,
// and so never got the flip that would have moved the anchor it was being judged against. Missing
// every single vblank at every single phase was that deadlock rather than the task set, and an
// over-allocated set is now what decision 35 says a set gyro cannot keep up with is — frames dropped,
// each one costing exactly itself.
GYRO_TEST(Schedulability, AnOverAllocatedSetCostsThePanelItBlocksAShareOfItsVblanks)
{
	constexpr std::array<OutputTask, 2> tasks{
		OutputTask{ .Period = kFast, .Want = 3ms, .Floor = 1ms, .Focused = true },
		OutputTask{ .Period = kSlow, .Want = 8ms, .Floor = 1ms },
	};

	const Plan plan = Admit(tasks);

	GYRO_REQUIRE(plan.IsFeasible());
	GYRO_REQUIRE(plan.Deepest() == Rung::Tier);
	GYRO_REQUIRE(plan[1].Reduced());

	// What the two outputs wanted, handed to them without the test being run. Utilization is
	// 3/6.944 + 8/16.667 = 0.91, comfortably below one — the set is utilization-feasible and
	// non-preemptively infeasible, which is the distinction the whole of decision 29 rests on. The
	// slack exists; it is in the wrong place.
	constexpr std::array<Allocation, 2> greedy{
		Allocation{ .Period = kFast, .Cost = 3ms, .Want = 3ms, .Floor = 1ms },
		Allocation{ .Period = kSlow, .Cost = 8ms, .Want = 8ms, .Floor = 1ms },
	};

	for (std::size_t step = 0; step < kPhaseSteps; ++step)
	{
		const std::array<PanelSpec, 2> panels{
			PanelSpec{ .Floor = 1ms },
			PanelSpec{ .Floor = 1ms, .Phase = PhaseAt(kSlow, step, kPhaseSteps) },
		};

		Machine unchecked{ panels, greedy };
		Settle(unchecked);

		// The set is infeasible and the panel it blocks is the one that pays, so there are misses to
		// find — but they are a fraction of its vblanks rather than all of them, and no two of them are
		// adjacent. The sweep measures between an eighth and a sixth of the panel's vblanks lost across
		// the thirty-two phases; a quarter is the bound asserted, so that a change which doubles the
		// damage fails here rather than being read as noise.
		GYRO_CHECK(unchecked.Panel(0).Frames() > 0);
		GYRO_CHECK(unchecked.Missed(0) > 0);
		GYRO_CHECK(unchecked.Missed(0) * 4 < unchecked.Vblanks(0));

		// Decision 35's second promise, holding on a set admission control would have refused: a frame
		// dropped costs exactly itself, and the floor tier has the panel back on its own cadence by the
		// next vblank.
		GYRO_CHECK_EQ(unchecked.Panel(0).LongestGap(), std::uint64_t{ 1 });

		Machine admitted{ panels, plan.Allocations() };
		Settle(admitted);

		GYRO_CHECK(admitted.Panel(0).Frames() > 0);
		GYRO_CHECK_EQ(admitted.Missed(), std::uint64_t{ 0 });
	}
}

// Docs/Architecture.md's counterexample to the two-output simplification: two 144 Hz panels at 3 ms
// beside a 60 Hz projector at 2 ms satisfies `C_fast + max(C_other) <= P_fast` and is caught by the
// general form, since `h(P_fast) + B(P_fast) = (3 + 3) + 2 = 8 > 6.944`. This is the set where the two
// forms disagree, and what is asserted is that the plan the general form produces survives every phase
// relationship the two grids can have.
//
// **The set the general form rejects does not actually miss on this loop**, which is worth recording
// rather than asserting. `Timing::Assess` renders the frame it owes as soon as it can finish before
// that frame's vblank, so an output's effective release is a whole period before its deadline rather
// than `C` before it — decision 30's relaxed release constraint, arriving for free out of the equality
// test rather than as the rung it is described as. The processor-demand test's blocking term is
// pessimistic against that, and only a job longer than the blocked output's entire period is beyond
// it. That makes the test conservative, which is the safe direction, and it means the sweep cannot
// falsify admission control by finding a rejected set that ran.
GYRO_TEST(Schedulability, ThreeOutputsWhereTheSimpleFormWouldHaveAdmittedTheSet)
{
	constexpr std::array<OutputTask, 3> tasks{
		OutputTask{ .Period = kFast, .Want = 3ms, .Floor = 1ms, .Focused = true },
		OutputTask{ .Period = kFast, .Want = 3ms, .Floor = 1ms },
		OutputTask{ .Period = kSlow, .Want = 2ms, .Floor = 1ms },
	};

	const Plan plan = Admit(tasks);

	GYRO_REQUIRE(plan.IsFeasible());
	GYRO_REQUIRE(plan.Deepest() == Rung::Tier);

	// Two phases to walk rather than one, so the grid is coarser. Both are swept against the first
	// output, whose vblank zero sits at the origin.
	constexpr std::size_t kGrid = 8;

	for (std::size_t second = 0; second < kGrid; ++second)
	{
		for (std::size_t third = 0; third < kGrid; ++third)
		{
			const std::array<PanelSpec, 3> panels{
				PanelSpec{ .Floor = 1ms },
				PanelSpec{ .Floor = 1ms, .Phase = PhaseAt(kFast, second, kGrid) },
				PanelSpec{ .Floor = 1ms, .Phase = PhaseAt(kSlow, third, kGrid) },
			};

			Machine admitted{ panels, plan.Allocations() };
			Settle(admitted);

			GYRO_CHECK(admitted.Panel(0).Frames() > 0);
			GYRO_CHECK_EQ(admitted.Missed(), std::uint64_t{ 0 });
		}
	}
}

// Decision 35 in one run: a frame that overruns costs one frame and does not start a cascade, and the
// output beside it is not the one that pays.
GYRO_TEST(Schedulability, AMissCostsOneFrameAndDoesNotCascade)
{
	constexpr std::array<OutputTask, 2> tasks{
		OutputTask{ .Period = kFast, .Want = 3ms, .Floor = 1ms, .Focused = true },
		OutputTask{ .Period = kSlow, .Want = 3ms, .Floor = 1ms },
	};

	const Plan plan = Admit(tasks);

	GYRO_REQUIRE(plan.Deepest() == Rung::None);

	const std::array<PanelSpec, 2> panels{
		PanelSpec{ .Floor = 1ms },
		PanelSpec{ .Floor = 1ms, .Phase = PhaseAt(kSlow, 3, kPhaseSteps) },
	};

	Machine machine{ panels, plan.Allocations() };

	machine.Run(kWarmup);
	machine.Arm();

	// A whole period more than the output was allocated, which is a frame that cannot reach its vblank
	// however early it started.
	machine.Overrun(0, kFast);
	machine.Run(kMeasured);

	GYRO_CHECK_EQ(machine.Missed(0), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(machine.Panel(0).LongestGap(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(machine.Missed(1), std::uint64_t{ 0 });

	// And it goes on meeting them at the floor tier, which is decision 35's second branch rather than
	// an afterthought: the overrun is filed into the planned mark and stays there for a window, so what
	// the output can afford afterwards is the composite it is guaranteed. A deadline met on the lower
	// rung is still a deadline met, which is why the count above is of vblanks and not of tiers.
	GYRO_CHECK(machine.Floors(0) > 0);
}

// Docs/Architecture.md#doing-nothing-must-cost-nothing, in the harness that section names: *when
// nothing is animating and nothing has committed, no timer is armed and the frame thread blocks
// indefinitely.*
//
// Two things go wrong here and neither shows up as a wrong pixel, which is why they are asserted
// rather than watched. A fold that answers `At` where it should answer `Never` costs a wakeup per
// period forever, on the machine most likely to be running on a battery — the section's own worry
// about the VRR keepalive is one instance of it. And a fold that is right but is reached only after a
// number of iterations that grows with how long the loop has been running is the same defect deferred:
// it settles in the test and spins on the desk.
//
// **What is asserted is `Step`'s answer and the commits the panel got, and never a symptom.** A
// thread's voluntary context-switch count is the obvious instrument and it is the wrong one: it
// measures the kernel's opinion of a scheduling decision this file does not make, it is perturbed by
// everything else on the machine, and it turns an invariant into a threshold somebody eventually
// raises. The invariant is structural, so the assertion is too. The shim's half — that a `Never()`
// fold arms no timeout on a real ring — is asserted where the ring is, in Source/Compositor/Uring.Test.cpp,
// because it is decision 80's other side and needs a kernel rather than a `ManualClock`.
GYRO_TEST(Schedulability, AnIdleSceneArmsNothingHoweverLongTheClockRuns)
{
	constexpr std::array<OutputTask, 2> tasks{
		OutputTask{ .Period = kFast, .Want = 3ms, .Floor = 1ms, .Focused = true },
		OutputTask{ .Period = kSlow, .Want = 3ms, .Floor = 1ms },
	};

	const Plan plan = Admit(tasks);

	GYRO_REQUIRE(plan.IsFeasible());

	constexpr std::array<PanelSpec, 2> panels{
		PanelSpec{ .Floor = 1ms },
		PanelSpec{ .Floor = 1ms, .Phase = PhaseAt(kSlow, 5, kPhaseSteps) },
	};

	// The two routes to the same answer, asserted together because they are different states and only
	// one of them is reachable at startup. `Never()` per output is a scene dispatch built and found
	// nothing in; the empty span is dispatch not having built one yet, which decision 84 reads as no
	// information. A fold that folded a missing schedule to anything but idle would have gyro composite
	// its way through boot before a client exists.
	constexpr std::array<Wake, 2> settled{ Wake::Never(), Wake::Never() };

	Machine published{ panels, plan.Allocations(), std::span<const Wake>{ settled } };
	Machine unpublished{ panels, plan.Allocations(), std::span<const Wake>{} };

	GYRO_CHECK_EQ(published.Step(), Wake::Never());
	GYRO_CHECK_EQ(unpublished.Step(), Wake::Never());

	// **Idle is a state and not a moment**, which is the half a single assertion after a single step
	// cannot see. Sixty-six seconds of virtual time, sampled once a slow period, with nothing to make
	// the answer change: a fold that recomputed a deadline from `now` rather than from a contributor
	// would come back with an instant on the very first iteration where the clock had moved past
	// something, and a fold that woke on a period would come back with one on all of them.
	for (std::size_t period = 0; period < 4'000; ++period)
	{
		published.Idle(kSlow);
		unpublished.Idle(kSlow);

		GYRO_REQUIRE(published.Step() == Wake::Never());
		GYRO_REQUIRE(unpublished.Step() == Wake::Never());
	}

	// And nothing was presented in any of it. The fold is what the composition root arms; this is what
	// the glass got, and an invariant about cost is about both.
	for (std::size_t index = 0; index < 2; ++index)
	{
		GYRO_CHECK_EQ(published.Commits(index), std::uint64_t{ 0 });
		GYRO_CHECK_EQ(unpublished.Commits(index), std::uint64_t{ 0 });
	}
}

// The invariant's other half: idle is a state the loop *returns to*, in a bounded number of iterations
// that does not grow.
//
// Damage is the only thing that asks a settled output for a frame, so a round here is the whole of
// what a static desktop does — something dirties a region, one frame per output reaches the glass, and
// the loop blocks again. What it costs is the number worth pinning: one iteration to render and
// commit, and one per flip to observe it and fold back to `Never()`. A count that crept up round over
// round would be state accumulating somewhere in the fold, which is exactly the shape of the defect
// that is invisible until a machine has been up for a week.
GYRO_TEST(Schedulability, DamageSettlesBackToIdleInABoundedNumberOfIterations)
{
	constexpr std::array<OutputTask, 2> tasks{
		OutputTask{ .Period = kFast, .Want = 3ms, .Floor = 1ms, .Focused = true },
		OutputTask{ .Period = kSlow, .Want = 3ms, .Floor = 1ms },
	};

	const Plan plan = Admit(tasks);

	GYRO_REQUIRE(plan.IsFeasible());

	constexpr std::array<PanelSpec, 2> panels{
		PanelSpec{ .Floor = 1ms },
		PanelSpec{ .Floor = 1ms, .Phase = PhaseAt(kSlow, 5, kPhaseSteps) },
	};

	Machine machine{ panels, plan.Allocations(), std::span<const Wake>{} };

	// One frame per output plus its flip is the expected shape, and it costs three iterations: one that
	// records and commits both outputs, one that observes the first flip, one that observes the second
	// and folds to `Never()`. Six is that with room, and it is deliberately not a dozen — a bound loose
	// enough to absorb a doubling is a bound that stops being an assertion.
	constexpr std::size_t kBound = 6;
	constexpr std::size_t kRounds = 24;

	std::optional<std::size_t> steady;

	for (std::size_t round = 0; round < kRounds; ++round)
	{
		machine.Damage(0);
		machine.Damage(1);

		const std::optional<std::size_t> iterations = machine.RunUntilIdle(kBound);

		GYRO_REQUIRE(iterations.has_value());
		GYRO_REQUIRE(machine.Fold() == Wake::Never());

		// Exactly one frame per output per round. A second commit would mean the loop drew a frame
		// nothing asked for, which is the same defect as an armed timer wearing different clothes.
		GYRO_CHECK_EQ(machine.Commits(0), round + 1);
		GYRO_CHECK_EQ(machine.Commits(1), round + 1);

		// The cold start is allowed to differ from the rest — the first round has no anchor to predict
		// against and decision 31 has an unanchored output render on demand — but every round after it
		// is the same round, and that is the claim.
		if (round == 1)
		{
			steady = iterations;
		}

		if (round > 1)
		{
			GYRO_CHECK_EQ(iterations, steady);
		}

		// Time passes between rounds, and deliberately not a whole number of either period: the anchor
		// each output is judged against goes stale by an amount that is different every round, so a
		// count that depended on how far behind the anchor had fallen would be visible as drift rather
		// than as a single wrong number.
		machine.Idle(37 * kSlow + 7ms);
	}

	GYRO_REQUIRE(steady.has_value());
}

// The fold is partitioned per output, which is decision 69's associativity spent rather than merely
// stated: *a blinking cursor on one panel must not wake the other.*
//
// A global fold over the whole scene would pass every assertion above — nothing is animating there, so
// there is nothing for a partition to get wrong. What separates the two is a scene where one output is
// animating and the other is not, and the question is whether the settled one pays for its neighbour.
// It is offered hundreds of iterations here, because the animating panel wakes the loop on every one
// of its vblanks and the loop visits every bound output on every iteration. It must decline all of
// them.
GYRO_TEST(Schedulability, ASettledOutputIsNotWokenByTheOneAnimatingBesideIt)
{
	constexpr std::array<OutputTask, 2> tasks{
		OutputTask{ .Period = kFast, .Want = 3ms, .Floor = 1ms, .Focused = true },
		OutputTask{ .Period = kSlow, .Want = 3ms, .Floor = 1ms },
	};

	const Plan plan = Admit(tasks);

	GYRO_REQUIRE(plan.IsFeasible());
	GYRO_REQUIRE(plan.Deepest() == Rung::None);

	constexpr std::array<PanelSpec, 2> panels{
		PanelSpec{ .Floor = 1ms },
		PanelSpec{ .Floor = 1ms, .Phase = PhaseAt(kSlow, 5, kPhaseSteps) },
	};

	// The fast panel is settled and the slow one wants every frame it can have. Which is which matters:
	// the settled output is the one whose deadlines come round most often, so it is the one a fold that
	// leaked across outputs would wake most.
	constexpr std::array<Wake, 2> scene{ Wake::Never(), Wake::EveryFrame() };

	Machine machine{ panels, plan.Allocations(), std::span<const Wake>{ scene } };

	// Damaged once, so that it is settled rather than merely never started: it has an anchor, a frame
	// behind it, and a clock predicting deadlines it could be woken for.
	machine.Damage(0);
	machine.Run(kWarmup);
	machine.Arm();
	machine.Run(kMeasured);

	// Its one frame, drawn during the warm-up, and nothing since.
	GYRO_CHECK_EQ(machine.Commits(0), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(machine.Panel(0).Frames(), std::uint64_t{ 0 });

	// While its vblanks went by in the hundreds and its neighbour met every one of its own. Both halves
	// are needed: a machine that stopped iterating would also have presented nothing on output 0.
	GYRO_CHECK(machine.Vblanks(0) > 100);
	GYRO_CHECK(machine.Panel(1).Frames() > 0);
	GYRO_CHECK_EQ(machine.Missed(1), std::uint64_t{ 0 });

	// And the mirror, which is what makes the count above a statement about the partition rather than
	// about output 0 having run out of damage: the same panel, the same contribution, alone, folds to
	// `Never()` and blocks. The scene answer is identical; only the neighbour is gone.
	Machine alone{ std::span{ panels }.first(1), plan.Allocations().first(1), std::span{ scene }.first(1) };

	alone.Damage(0);

	const std::optional<std::size_t> iterations = alone.RunUntilIdle(12);

	GYRO_REQUIRE(iterations.has_value());
	GYRO_CHECK_EQ(alone.Fold(), Wake::Never());
	GYRO_CHECK_EQ(alone.Commits(0), std::uint64_t{ 1 });
}

// The same recovery with no contention in it at all, which is the route every output takes before the
// first thing that ever wants a frame arrives.
//
// Nothing in the scene wants one, so the fold is `Wake::Never()`, the composition root arms no timer,
// and the loop sleeps for as long as the world lets it. What it wakes to is damage, and by then the
// anchor its own last flip left is tens of periods old. The frame it *owes* by that anchor was due
// half a second ago; the frame it can still *make* is the one the clock names, and the difference
// between admitting the second and insisting on the first is the difference between a compositor and a
// black screen.
GYRO_TEST(Schedulability, AnIdleOutputWokenByDamageRendersRatherThanStarving)
{
	constexpr std::array<OutputTask, 1> tasks{
		OutputTask{ .Period = kSlow, .Want = 3ms, .Floor = 1ms, .Focused = true },
	};

	const Plan plan = Admit(tasks);

	GYRO_REQUIRE(plan.IsFeasible());
	GYRO_REQUIRE(plan.Deepest() == Rung::None);

	constexpr std::array<PanelSpec, 1> panels{ PanelSpec{ .Floor = 1ms } };

	Machine machine{ panels, plan.Allocations(), Wake::Never() };

	machine.Arm();

	// The first frame has nothing behind it, which is the one `Compositor` damages the whole output for
	// at startup. It is also the only thing here that anchors the clock: an unanchored output renders on
	// demand whatever the schedule thinks, so a machine that never presented would prove nothing.
	machine.Damage(0);
	machine.Run(4 * kSlow);

	const std::uint64_t anchored = machine.Panel(0).Frames();

	GYRO_REQUIRE(anchored > 0);

	// And then it idles rather than spins. `Run` returns as soon as nothing is armed and no panel owes
	// anything, so the clock is still sitting on the vblank that anchored it.
	GYRO_REQUIRE(machine.Step() == Wake::Never());

	// Half a second of nothing, which is thirty periods past that anchor.
	machine.Idle(500ms);

	const Instant woken = machine.Now();

	machine.Damage(0);
	(void)machine.Step();

	// A frame, at a presentation instant that has not happened yet. Decision 36 makes that instant the
	// only time an animation ever sees, so targeting the frame the anchor says is owed would be worse
	// than dropping this one: it would evaluate every spring half a second in the past.
	GYRO_CHECK(machine.Decision(0).Renders());
	GYRO_CHECK(machine.Decision(0).Presentation > woken);

	machine.Run(4 * kSlow);

	GYRO_CHECK(machine.Panel(0).Frames() > anchored);
}
