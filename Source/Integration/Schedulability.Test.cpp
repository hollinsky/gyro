#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "Core/Clock.h"
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

// **What the sweep has found is recorded here rather than asserted**, because a test that asserts a
// defect is one that has to be inverted before the defect can be fixed.
//
// *An output that falls a whole frame behind never renders again.* `Timing::Assess` admits work only
// where `SequenceAfter(finish) == owed`, and `owed` is derived from the frame clock's anchor, which
// only a flip advances. Once `now` is more than a period past that anchor the reach is at least
// `owed + 1`, the equality cannot hold again, and the output skips forever — so nothing is presented,
// so no flip arrives, so the anchor never moves. `AnOverAllocatedSetStarvesThePanelItBlocks` reaches
// that state through blocking, which is why the panel it blocks misses *every* vblank rather than a
// share of them. The same state is reached with no contention at all by idling: a `Wake::Never()`
// fold, time passing, and then damage — which is the path every output takes before the first thing
// that ever wants a frame arrives. `FrameClock::SequenceAfter`'s own contract calls itself the
// recovery path a loop that "skipped, stalled, or was idle" asks; what is missing is that the frame it
// names is not the frame the next iteration goes on to owe.

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

	[[nodiscard]] Result<void> BindTargets(std::span<const RenderTarget> targets) override
	{
		return m_Inner.BindTargets(targets);
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
	Machine(std::span<const PanelSpec> panels, std::span<const Allocation> allocations)
		: m_Count{ std::min({ panels.size(), allocations.size(), kMaxPanels }) }
	{
		for (std::size_t index = 0; index < m_Count; ++index)
		{
			Attach(index, panels[index], allocations[index]);
		}

		// Every output wants every frame, which is the steady state the schedulability claim is about:
		// a scene that settles is a scene whose deadlines are trivially met. Published once, since the
		// ring holds what it was given and the frame side re-reads nothing it already holds.
		std::array<Wake, kMaxPanels> wakes{};
		wakes.fill(Wake::EveryFrame());

		SnapshotPublisher publisher;
		publisher.PutWakes({ wakes.data(), m_Count });
		publisher.Build(m_Snapshot, 1);

		GYRO_CHECK(m_Ring.Publish(m_Snapshot.Bytes(), 0));

		m_Loop.Bind({ m_Outputs.data(), m_Count });
		m_Loop.Listen({ m_Sources.data(), 1 });
	}

	// The shim of decision 80: wake at or after the instant the step asked for, or earlier when a
	// source has something to say. The device's next event is that second condition — a headless flip
	// makes no file readable, so what a real shim learns from `io_uring` this one asks the backend.
	void Run(Duration duration)
	{
		const Instant until = Advanced(m_Clock.Now(), duration);
		std::size_t stalled = 0;

		while (m_Clock.Now() < until)
		{
			const Instant entered = m_Clock.Now();
			const Wake wake = m_Loop.Step();

			Sample();

			const Instant now = m_Clock.Now();

			// A job ran, which is progress however the wake below lands.
			stalled = now == entered ? stalled : 0;

			Instant next = m_Device.NextEvent();

			if (wake.Which != Wake::Kind::Settled)
			{
				next = std::min(next, wake.When);
			}

			if (next <= now)
			{
				// **The step asked for an instant the machine has already passed**, which happens on
				// every iteration between a commit and its flip once the next frame's record point is
				// behind: the output is owed a frame, so the fold prices one, and the flip it is
				// waiting for has not landed. A shim that armed that timer would wake immediately, do
				// nothing, and arm it again. Waking *at or after* the instant asked for is the whole of
				// the contract, so this waits for the next thing that can change anything instead.
				next = m_Device.NextEvent();
			}

			if (next == Instant{ Duration::max() })
			{
				// Nothing armed and no panel owing anything. Under a scene that wants every frame this
				// is unreachable, and it is here so that a machine which does go idle ends rather than
				// spins.
				break;
			}

			if (next <= now)
			{
				// Even the panel has nothing further to say at an instant the machine has not reached,
				// so another step can only repeat this one. Bounded rather than trusted: it is worth
				// failing on rather than hanging a test run.
				++stalled;
				GYRO_REQUIRE(stalled < kStallLimit);

				continue;
			}

			stalled = 0;
			m_Clock.Set(next);
		}

		// The last vblank inside the window may fall after the final step, and a frame that reached the
		// glass unreported would read as a miss. Draining here is what makes `Vblanks` and the frames
		// counted against it describe the same interval.
		(void)m_Device.Drain();
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

	[[nodiscard]] const Witness& Panel(std::size_t index) const noexcept { return m_Witnesses[index]; }

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
	// There is deliberately no counter for `Admission::Skip` beside this one. The verdict is not a
	// miss signal: the loop assesses every bound output on every iteration, so an output that has
	// already committed its next frame and is waiting for the flip reports a skip on each visit until
	// that flip lands. Not yet due and dropping this frame are the same enumerator, and only the panel
	// can tell them apart.
	[[nodiscard]] std::uint64_t Floors(std::size_t index) const noexcept { return m_Floors[index]; }

	// Make the next frame this output records cost `by` more than it was allocated, once. The transient
	// of Docs/Architecture.md#headless: a scene that is expensive for one frame, which is what proves a
	// miss costs one frame rather than starting a cascade.
	void Overrun(std::size_t index, Duration by) noexcept { m_Renderers[index]->Inner().Overrun = by; }

private:
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
		GYRO_CHECK(m_Renderers[index]->BindTargets(output->Targets()));

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
GYRO_TEST(Schedulability, AnOverAllocatedSetStarvesThePanelItBlocks)
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

		// Every vblank of the blocked panel, at every phase. See the note in this file's header: the
		// figure is total rather than occasional, and that is a defect in the loop's recovery rather
		// than a property of the task set.
		GYRO_CHECK(unchecked.Missed(0) > 0);

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
