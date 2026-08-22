// sigaction, pthread scheduling, and the rest of what this file asks of the platform are POSIX rather
// than ISO C, and glibc hides them under -std=c++NN. Named here for the reason Core/Clock.cpp names
// its own: what is wanted from the platform is stated rather than inherited from a build flag.
#define _POSIX_C_SOURCE 200809L

#include "Compositor/Compositor.h"

#include <signal.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>

#include "Compositor/RealTime.h"
#include "Compositor/Schedule.h"
#include "Compositor/Uring.h"
#include "Core/Clock.h"
#include "Core/Signal.h"
#include "Core/Time.h"
#include "Frame/Evaluator.h"
#include "Frame/Loop.h"
#include "Geometry/Space.h"
#include "Headless/Device.h"
#include "Headless/Output.h"
#include "Headless/Renderer.h"
#include "Publication/Return.h"
#include "Publication/Ring.h"
#include "Seam/EventSource.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/RenderTarget.h"

namespace
{

// SPEC: gyro's own wakeup latency — everything between the ring's timeout expiring and the first
// instruction of the record. It is `TimingPolicy::Safety`, and it is the same figure admission control
// is given, because a set admitted against a smaller margin than the record-time check holds is a set
// that passes the test and misses the frame. A number rather than a measurement until there is a real
// thread on a real kernel to measure it from, which is among the first things the sweep will supply.
constexpr Duration WakeupMargin = std::chrono::microseconds{ 250 };

// SPEC: how the one `--cost` figure is split across the two devices `Timing` composes as a pipeline.
// A composite is GPU-dominated — Docs/Architecture.md#precision-and-why-the-blur-chain-is-affordable
// has the blur chain as most of what makes `C` large — so a quarter on the frame thread and the rest
// on the device is the shape rather than the measurement. It goes away when there is a renderer that
// reports both halves; until then it exists so that the pipelining term in `Timing::Project` has
// something to bite on outside a test.
constexpr std::int64_t CpuShareOf = 4;

[[nodiscard]] constexpr CostSplit Split(Duration cost) noexcept
{
	const Duration cpu = Duration{ cost.count() / CpuShareOf };

	return { .Cpu = cpu, .Gpu = cost - cpu };
}

// What the process is asked to stop by. A file-scope pointer because a signal handler takes no
// argument and has no `this`, and an atomic one because the handler may run on any thread from the
// moment the object exists.
std::atomic<Interrupt*> g_Stopping{ nullptr };

extern "C" void OnStopSignal(int)
{
	// Everything this reaches is async-signal-safe, and that is not incidental: `Interrupt::Raise` is a
	// lock-free atomic store and an 8-byte `write`, both of which are on the list. It is also why
	// shutdown is a handler and a join rather than a thread parked in `sigwait` — that thread would
	// have to be woken by the frame thread finishing on its own, which is a second stop path for
	// something that already has one.
	if (Interrupt* const interrupt = g_Stopping.load(std::memory_order_acquire))
	{
		interrupt->Raise();
	}
}

[[nodiscard]] Result<void> InstallStopHandlers()
{
	struct sigaction action = {};
	action.sa_handler = &OnStopSignal;
	action.sa_flags = 0;
	::sigemptyset(&action.sa_mask);

	if (::sigaction(SIGINT, &action, nullptr) != 0 || ::sigaction(SIGTERM, &action, nullptr) != 0)
	{
		return Failure(errno, "installing gyro's shutdown handlers");
	}

	return {};
}

// One output, and everything the root owns on its behalf.
//
// **The renderer is per output rather than per device**, and it is the one thing here worth reading
// twice. `IRenderer::BindTargets` is not a per-frame call — Seam/Renderer.h has it importing images and
// building pipelines — so a writer is bound to one presenter's target set for as long as that set
// exists, which makes it per output. The *device* is the queue the work serialises on, it is what
// `FrameOutput::Bind` is told, and every output here names device zero because there is one simulated
// device. Two outputs on one GPU is that same arrangement, and the serialisation is decision 29's
// blocking term.
//
// The renderer is optional-wrapped because `IRenderer` is neither copyable nor movable, by the
// contract Docs/Architecture.md#device-migration puts on it: a renderer is replaced by being destroyed
// and constructed. So it is emplaced once its policy is known rather than assigned into.
struct BoundOutput
{
	HeadlessOutput* Presenter = nullptr;
	std::optional<SimulatedRenderer> Renderer;
	Connection<> OnTargetsInvalidated;

	// The root's half of decision 41: the images went away, so whatever holds them has to be told before
	// the next record names one. `FrameOutput` has already damaged the whole output by the time this
	// runs; what is left is the binding, which is the root's because the root is what paired this
	// renderer with this presenter.
	void Rebind() noexcept
	{
		if (Presenter != nullptr && Renderer)
		{
			(void)Renderer->BindTargets(Presenter->Targets());
		}
	}
};

// Everything with a lifetime, in one object, constructed in place.
//
// Almost none of it is movable — `FrameOutput` holds signal connections, `HeadlessOutput` is a
// presenter, `FrameRing` holds mmapped regions — so the root is built rather than returned, and `Run`
// puts it on the heap because sixteen frame outputs and their target rings are more than a thread's
// stack should be asked to hold.
class Compositor
{
public:
	[[nodiscard]] Result<void> Open(const Options& options)
	{
		m_Options = options;
		m_RealTime = options.RealTime;

		if (const Result<void> ready = m_Interrupt.Open(); !ready)
		{
			return ready;
		}

		if (const Result<void> ready = Configure(); !ready)
		{
			return ready;
		}

		m_Sources[0] = &m_Device;
		m_Sources[1] = &m_Interrupt;

		m_Loop = std::make_unique<FrameLoop>(
			m_Clock, m_Snapshots, m_Returns, m_Evaluator, Timing{ TimingPolicy{ .Safety = WakeupMargin } }
		);
		m_Loop->Bind({ m_Outputs.data(), m_Count });
		m_Loop->Listen({ m_Sources.data(), m_Sources.size() });

		return {};
	}

	[[nodiscard]] Result<void> Run()
	{
		g_Stopping.store(&m_Interrupt, std::memory_order_release);

		if (const Result<void> installed = InstallStopHandlers(); !installed)
		{
			g_Stopping.store(nullptr, std::memory_order_release);

			return installed;
		}

		// The thread is the root's, per decision 80, and so is everything platform about it: its
		// priority, its page residency, and the `while` inside it. What it runs is one portable call in a
		// loop.
		std::thread frame{ [this] { m_Result = Iterate(); } };

		frame.join();

		g_Stopping.store(nullptr, std::memory_order_release);

		Report();

		return m_Result;
	}

private:
	// Everything the frame thread does, and it is decision 80's shape entire: step, and wait for the
	// wake the step returned.
	[[nodiscard]] Result<void> Iterate()
	{
		// Inside the thread, because a scheduling policy is one thread's property and this is the only
		// place its identity is unambiguous. A refusal is carried out and reported rather than fatal —
		// see Compositor/RealTime.h, and note that headless has already declined to ask.
		if (m_RealTime)
		{
			if (const Result<void> promoted = PromoteToRealTime({ .Priority = m_Options.Priority }); !promoted)
			{
				m_PriorityRefused = promoted.error();
			}
		}

		// **The ring is opened here rather than beside everything else the root constructs, and
		// `IORING_SETUP_SINGLE_ISSUER` is why.** That flag binds the ring to one task, and a ring created
		// on the thread that spawned the frame thread refuses every submission from it with `EEXIST`. So
		// the ring belongs to the thread that pumps it, which is decision 81's rule — *exactly one thread
		// pumps a source, for the whole of its life* — turning out to hold for the thing doing the
		// pumping as well. It is also the reason the flag is worth having: a ring only one task may touch
		// is a ring with no cross-thread ordering to get wrong.
		if (const Result<void> ready = m_Ring.Open(); !ready)
		{
			return ready;
		}

		// The order between them does not matter, because the loop drains every source on every iteration
		// whatever woke it. The headless device registers nothing — its descriptor is invalid by design —
		// so on this backend the timeout and the interrupt are the only things that can wake the ring,
		// which is exactly the case decision 80 says a readiness set could not have served.
		for (IEventSource* const source : m_Sources)
		{
			if (const Result<void> watched = m_Ring.Watch(*source); !watched)
			{
				return watched;
			}
		}

		while (!m_Interrupt.IsRaised())
		{
			// Two statements rather than one call, and the sequencing is the whole reason: argument
			// evaluation order is unspecified, so `Sooner(Step(), BackendWake())` is entitled to read the
			// backend *before* the iteration that presents into it. That produces a wake with no flip in
			// it, on the one frame — the first — where the loop has nothing else to arm.
			const Wake stepped = m_Loop->Step();
			const Wake wake = Sooner(stepped, BackendWake());

			++m_Iterations;

			// Bounded mode stops at idle rather than at the count, and the count is the guard rather than
			// the goal. A `Settled` wake *is* Docs/Architecture.md#doing-nothing-must-cost-nothing reached
			// — nothing owed, nothing outstanding, nothing to arm — so a run that gets there has shown the
			// thing worth showing, and waiting out the remaining iterations would only show that the
			// binary can block. The count is what catches the case where idle never arrives.
			if (m_Options.Iterations != 0)
			{
				m_Idled = wake.Which == Wake::Kind::Settled;

				if (m_Idled || m_Iterations >= m_Options.Iterations)
				{
					return {};
				}
			}

			if (m_Interrupt.IsRaised())
			{
				return {};
			}

			// The one place this thread blocks. Nothing is logged from here — spdlog on the frame path is
			// the blocking-operation hazard Docs/Architecture.md#why-io_uring names — so a failure is
			// carried out of the thread and reported by whoever joined it.
			if (const Result<void> waited = m_Ring.WaitFor(wake); !waited)
			{
				return waited;
			}
		}

		return {};
	}

	// When the *simulated hardware* next moves, folded into the wake the loop asked for.
	//
	// **This is the composition root standing in for a descriptor, and it is needed exactly once per
	// output.** On a real backend a page flip makes the DRM file readable and the ring's poll wakes the
	// loop; a headless flip is a function of the clock and no file becomes readable, which
	// Seam/EventSource.h makes an ordinary answer rather than a defect. Everywhere else that costs
	// nothing, because `FrameOutput` holds `m_FlipPending` and the loop's own contribution keeps it
	// awake. The one place it does not is the **first** frame: an unanchored `FrameClock` predicts
	// `Unscheduled`, so `Timing::WakeFor` contributes `Never()`, and the loop that just presented would
	// arm nothing and never drain the `Presented` that would have anchored it. The backend and the idle
	// fold would then each be waiting for the other.
	//
	// So the root supplies what the backend cannot: it wired a clock-driven presenter to a real clock,
	// which makes the consequence its own. Nothing in `Frame` changes and the shim's contract does not
	// either — this only ever makes a wake *earlier*, which the contract already permits.
	//
	// Whether `IEventSource` should be able to answer this itself, so that the root does not have to
	// know which backend it built, is a seam question and is [open](../../Docs/Open.md).
	[[nodiscard]] Wake BackendWake() const noexcept
	{
		const Instant next = m_Device.NextEvent();

		return next == Instant{ Duration::max() } ? Wake::Never() : Wake::At(next);
	}

	// Admission control, and then everything it decided applied to something. This is what runs again on
	// a configuration change; today it runs once.
	[[nodiscard]] Result<void> Configure()
	{
		const std::span<const OutputRequest> requested = m_Options.Requested();

		std::array<OutputDemand, MaxOutputs> demands{};
		const std::size_t count = std::min(requested.size(), demands.size());

		for (std::size_t index = 0; index < count; ++index)
		{
			demands[index] = OutputDemand{ .Period = PeriodFromHertz(requested[index].Refresh),
				                           .Planned = Split(m_Options.PlannedCost),
				                           .Floor = Split(m_Options.FloorCost),
				                           .Refresh = {},
				                           .Authority = true,
				                           .Focused = index == 0 };
		}

		m_Schedule = Schedule::Build({ demands.data(), count }, WakeupMargin);

		for (std::size_t index = 0; index < count; ++index)
		{
			if (const Result<void> added = AddOutput(index, requested[index], m_Schedule.Plans()[index]); !added)
			{
				return added;
			}
		}

		return {};
	}

	[[nodiscard]] Result<void> AddOutput(std::size_t index, const OutputRequest& request, const OutputPlan& plan)
	{
		const OutputConfiguration wanted{
			.Generation = 1,
			.Resolution = PixelSize<DeviceSpace>{ static_cast<std::int32_t>(request.Width),
			                                      static_cast<std::int32_t>(request.Height) },
			.Period = plan.Period,
			.Format = PixelFormat{ FormatXrgb8888, 0, ModifierLinear },
		};

		HeadlessOutput* const presenter = m_Device.Add(wanted);

		if (presenter == nullptr)
		{
			return Failure(ENOSPC, "more outputs than the headless device holds");
		}

		BoundOutput& bound = m_Bound[index];

		bound.Presenter = presenter;

		// The renderer is told exactly what admission allowed, which is the point of the bridge in
		// Compositor/Schedule.h: a tier step that reduced the allowance without making the work cheaper is
		// a plan the frame would then miss against, so one pair of figures both seeds the budget and
		// charges the renderer.
		bound.Renderer.emplace(
			SimulatedRendererPolicy{ .PlannedCpu = plan.Planned.Cpu,
		                             .FloorCpu = plan.Floor.Cpu,
		                             .PlannedGpu = plan.Planned.Gpu,
		                             .FloorGpu = plan.Floor.Gpu }
		);

		bound.Rebind();
		bound.OnTargetsInvalidated.ConnectTo<&BoundOutput::Rebind>(presenter->TargetsInvalidated, bound);

		m_Outputs[index].Bind(*presenter, *bound.Renderer, 0, presenter->Configuration(), {}, plan.Budget());

		// The first frame has nothing behind it. Every subsequent one is damage relative to what reached
		// the glass last time and there is no last time — so the output owes its whole extent, which is
		// the case `FrameOutput::AddDamage` exists to accept from outside the scene.
		m_Outputs[index].DamageWholeOutput();

		m_Count = index + 1;

		return {};
	}

	void Report() const
	{
		spdlog::info(
			"admitted {} output(s), U {} per mille, deepest rung {}",
			m_Schedule.Count(),
			m_Schedule.Admitted().UtilizationPerMille(),
			Name(m_Schedule.Admitted().Deepest())
		);

		if (!m_Schedule.Admitted().IsFeasible())
		{
			spdlog::warn("this configuration does not fit: every output is at its floor and frames will drop");
		}

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			const OutputPlan& plan = m_Schedule.Plans()[index];

			spdlog::info(
				"  output {}: period {:.3f}ms, cpu {:.3f}ms, gpu {:.3f}ms{}{}",
				index,
				ToMilliseconds(plan.Period),
				ToMilliseconds(plan.Planned.Cpu),
				ToMilliseconds(plan.Planned.Gpu),
				plan.Reduced ? ", reduced" : "",
				plan.Lengthened ? ", lengthened" : ""
			);

			// What the last iteration decided for this output. `FrameOutput::Last` is kept for exactly
			// this — a log line or a sweep reading the verdict and the slack without the loop having to
			// report through a channel that does not otherwise exist.
			spdlog::info(
				"             last {}, flip pending {}", m_Outputs[index].Last(), m_Outputs[index].IsFlipPending()
			);
		}

		if (m_PriorityRefused)
		{
			spdlog::warn("running at normal priority: {}", *m_PriorityRefused);
		}

		spdlog::info(
			"{} iteration(s), {} spurious wake(s){}", m_Iterations, m_Ring.Spurious(), m_Idled ? ", idled" : ""
		);

		if (m_Ring.Broken() != 0)
		{
			spdlog::warn("{} source(s) stopped waking the ring and are drained on the timer alone", m_Ring.Broken());
		}
	}

	Options m_Options{};

	MonotonicClock m_Clock;

	// The publication boundary, with nothing on the far side of it yet. The loop acquires from an empty
	// ring and posts a watermark nobody reads, which is correct behaviour rather than a stub: what
	// dispatch adds is a producer, and decision 83 adds its descriptor to the source set above.
	SnapshotRing m_Snapshots;
	ReturnChannel m_Returns;

	// The real walk, bound from the day it exists. The ring above is empty until dispatch is written,
	// so what it evaluates every iteration is an empty scene — which is the floor case rather than a
	// stub, and the one that has to cost nothing.
	SceneEvaluator m_Evaluator{ m_Clock };

	HeadlessDevice m_Device{ m_Clock };
	std::array<BoundOutput, MaxOutputs> m_Bound{};
	std::array<FrameOutput, MaxOutputs> m_Outputs{};
	std::size_t m_Count = 0;

	Schedule m_Schedule{};

	Interrupt m_Interrupt;
	FrameRing m_Ring;
	std::array<IEventSource*, 2> m_Sources{};

	std::unique_ptr<FrameLoop> m_Loop;

	bool m_RealTime = false;

	Result<void> m_Result{};
	std::optional<Error> m_PriorityRefused{};
	std::uint64_t m_Iterations = 0;
	bool m_Idled = false;
};

} // namespace

Result<void> Run(const Options& options)
{
	Options resolved = options;

	// Docs/Architecture.md#backends, enforced by the backend rather than by convention: headless and
	// nested force `SCHED_FIFO` and `mlockall` off unless explicitly overridden, because a real-time
	// thread inside a normal-priority host is an effective way to hard-lock the desktop somebody is
	// developing on. `--realtime` is the override that rule names, and it is the only thing that beats
	// this.
	if (resolved.Backend != BackendKind::Drm && !resolved.RealTimeForced)
	{
		resolved.RealTime = false;
	}

	if (resolved.Backend == BackendKind::Auto)
	{
		// Auto picks nested where there is a host and DRM otherwise, and neither is built. Headless is
		// what there is, so that is what auto resolves to — said out loud rather than silently, because
		// somebody who typed nothing and got headless should be able to find out why.
		resolved.Backend = BackendKind::Headless;

		spdlog::info("no backend selected and only headless is built; running headless");
	}

	if (resolved.Backend != BackendKind::Headless)
	{
		return Failure(ENOSYS, "only the headless backend is built");
	}

	if (resolved.RealTime)
	{
		// Process-wide and before the thread exists, which is the ordering that matters for both: the
		// limit is enforced per thread but set on the process, and `MCL_FUTURE` wants to be in force
		// before the allocations it is meant to cover.
		if (const Result<void> reserved = ReserveRealTime({ .Priority = resolved.Priority }); !reserved)
		{
			spdlog::warn("no RLIMIT_RTTIME: {}", reserved.error());
		}

		if (const Result<void> locked = LockMemory(); !locked)
		{
			spdlog::warn("pages are not locked: {}", locked.error());
		}
	}

	const auto compositor = std::make_unique<Compositor>();

	if (const Result<void> opened = compositor->Open(resolved); !opened)
	{
		return opened;
	}

	return compositor->Run();
}
