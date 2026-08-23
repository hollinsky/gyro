// sigaction, pthread scheduling, and the rest of what this file asks of the platform are POSIX rather
// than ISO C, and glibc hides them under -std=c++NN. Named here for the reason Core/Clock.cpp names
// its own: what is wanted from the platform is stated rather than inherited from a build flag.
#define _POSIX_C_SOURCE 200809L

#include "Compositor/Compositor.h"

#include <signal.h>
#include <spdlog/spdlog.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include "Blit/Blit.h"
#include "Compositor/RealTime.h"
#include "Compositor/Schedule.h"
#include "Compositor/Uring.h"
#include "Core/Clock.h"
#include "Core/ColorState.h"
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
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Virtual/Device.h"
#include "Virtual/Dump.h"
#include "Virtual/Heap.h"
#include "Virtual/Output.h"

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
// Both members are held through the seam rather than by their concrete type, because there is now
// more than one backend and the loop was never the thing that knew which. The renderer is a
// `unique_ptr` because `IRenderer` is neither copyable nor movable, by the contract
// Docs/Architecture.md#device-migration puts on it: a renderer is replaced by being destroyed and
// constructed.
//
// The colour state is copied here rather than read back off the presenter, because `IPresenter` does
// not carry a configuration — a presenter answers about targets and frames, and what the output was
// *asked* for is the root's own record.
struct BoundOutput
{
	IPresenter* Presenter = nullptr;
	std::unique_ptr<IRenderer> Renderer;
	ColorState Color{};
	Connection<> OnTargetsInvalidated;

	// The root's half of decision 41: the images went away, so whatever holds them has to be told before
	// the next record names one. `FrameOutput` has already damaged the whole output by the time this
	// runs; what is left is the binding, which is the root's because the root is what paired this
	// renderer with this presenter.
	void Rebind() noexcept
	{
		if (Presenter != nullptr && Renderer)
		{
			(void)Renderer->BindTargets(Presenter->Targets(), Color);
		}
	}
};

// A backend, as the composition root needs one: a source to pump, a presenter and a renderer per
// output, and whatever has to be stopped afterwards.
//
// **This is not a seam type and must not become one.** Seam/EventSource.h is deliberately a
// descriptor and a drain; everything below that this adds is the root doing what only the root can.
// `NextEvent` is the case Docs/Open.md's *whether a source can answer when it will next have
// something* entry describes — a clock-driven presenter has an answer and a DRM file never will, so
// putting the verb on `IEventSource` would be adding an interface for the fakes alone. Two fakes is
// not two implementations; that entry settles when nested lands, and a host's frame callback is the
// first real source with a genuine answer. Until then the knowledge lives here, where it is one
// virtual call wide instead of a promise every backend has to keep.
//
// `Build` rather than a constructor per output because a backend allocates — target rings, a writer
// thread — and a failure has to come back as a sentence naming what could not be had.
class IBackend
{
public:
	IBackend() = default;

	virtual ~IBackend() = default;

	IBackend(const IBackend&) = delete;
	IBackend& operator=(const IBackend&) = delete;
	IBackend(IBackend&&) = delete;
	IBackend& operator=(IBackend&&) = delete;

	[[nodiscard]] virtual IEventSource& Source() noexcept = 0;

	// When the simulated hardware next moves, or `Duration::max()` for never.
	[[nodiscard]] virtual Instant NextEvent() const noexcept = 0;

	// One output, its presenter, and the renderer that draws into it.
	[[nodiscard]] virtual Result<void>
	Build(std::size_t index, const OutputConfiguration& wanted, const OutputPlan& plan, BoundOutput& into) = 0;

	// Everything that has to stop before the process exits, called once the frame thread has been
	// joined. A backend with no thread and no file does nothing here.
	virtual void Close() noexcept {}

	// What this backend did, into the log, after `Close`.
	virtual void Report() const {}
};

// The sweep's instrument: simulated vblanks and a renderer that charges a cost and draws nothing.
class HeadlessBackend final : public IBackend
{
public:
	explicit HeadlessBackend(const IClock& clock) : m_Device{ clock } {}

	[[nodiscard]] IEventSource& Source() noexcept override { return m_Device; }

	[[nodiscard]] Instant NextEvent() const noexcept override { return m_Device.NextEvent(); }

	[[nodiscard]] Result<void>
	Build(std::size_t, const OutputConfiguration& wanted, const OutputPlan& plan, BoundOutput& into) override
	{
		HeadlessOutput* const presenter = m_Device.Add(wanted);

		if (presenter == nullptr)
		{
			return Failure(ENOSPC, "more outputs than the headless device holds");
		}

		into.Presenter = presenter;
		into.Color = presenter->Configuration().Color;

		// The renderer is told exactly what admission allowed, which is the point of the bridge in
		// Compositor/Schedule.h: a tier step that reduced the allowance without making the work cheaper is
		// a plan the frame would then miss against, so one pair of figures both seeds the budget and
		// charges the renderer.
		into.Renderer = std::make_unique<SimulatedRenderer>(SimulatedRendererPolicy{ .PlannedCpu = plan.Planned.Cpu,
		                                                                             .FloorCpu = plan.Floor.Cpu,
		                                                                             .PlannedGpu = plan.Planned.Gpu,
		                                                                             .FloorGpu = plan.Floor.Gpu });

		return {};
	}

private:
	HeadlessDevice m_Device;
};

// The backend whose consumer is a file. A real presenter that allocates, the CPU renderer, and one
// PAM per presented frame.
//
// **It paces exactly as a panel does, and that is most of why it exists.** A virtual output has a
// period and a phase and hands frames back on release, so the frame loop runs against real
// backpressure rather than against a simulation of it — which is what makes the pictures worth
// looking at: they are what a monitor would have shown at that frame boundary.
//
// **Nothing on the path needs a GPU.** `HeapAllocator` rather than udmabuf, `Blit` rather than
// Vulkan, and `TargetFace::Mapped` because a CPU blitter handed a dmabuf is a miswiring
// Seam/Renderer.h makes `EINVAL`. So `--backend=dump` runs in a container, over SSH, and on the
// machine with no seat — which are the places somebody most wants a picture and least has a screen.
class DumpBackend final : public IBackend
{
public:
	DumpBackend(const IClock& clock, std::string directory, std::size_t outputs)
		: m_Clock{ &clock }, m_Device{ clock }, m_Directory{ std::move(directory) }, m_Outputs{ outputs }
	{}

	~DumpBackend() override { Close(); }

	[[nodiscard]] IEventSource& Source() noexcept override { return m_Device; }

	[[nodiscard]] Instant NextEvent() const noexcept override { return m_Device.NextEvent(); }

	[[nodiscard]] Result<void>
	Build(std::size_t index, const OutputConfiguration& wanted, const OutputPlan&, BoundOutput& into) override
	{
		auto renderer = std::make_unique<Blit>(*m_Clock);
		auto dump = std::make_unique<FrameDump>(Destination(index), wanted.Resolution, wanted.Format);

		if (const Result<void> writing = dump->Open(); !writing)
		{
			return writing;
		}

		// The renderer is handed over as the completion gate even though a CPU composite finishes
		// inside `Record` and every point it returns is immediate. Virtual/Device.h accepts null for
		// exactly this case; passing the real renderer instead costs one `IsComplete` per delivery and
		// means the wiring does not have to be revisited if this path ever gains a device that can
		// export a timeline.
		VirtualOutput* const presenter =
			m_Device.Add(wanted, m_Allocator, *dump, renderer.get(), VirtualOutputPolicy{ .Face = TargetFace::Mapped });

		if (presenter == nullptr)
		{
			return Failure(ENOSPC, "more outputs than the virtual device holds");
		}

		if (const Result<void>& allocated = presenter->Status(); !allocated)
		{
			return allocated;
		}

		into.Presenter = presenter;
		into.Color = presenter->Configuration().Color;
		into.Renderer = std::move(renderer);

		m_Dumps[index] = std::move(dump);

		return {};
	}

	void Close() noexcept override
	{
		for (std::unique_ptr<FrameDump>& dump : m_Dumps)
		{
			if (dump)
			{
				dump->Close();
			}
		}
	}

	void Report() const override
	{
		for (std::size_t index = 0; index < m_Dumps.size(); ++index)
		{
			const std::unique_ptr<FrameDump>& dump = m_Dumps[index];

			if (!dump)
			{
				continue;
			}

			spdlog::info("  output {}: {} frame(s) written to {}", index, dump->Written(), dump->Directory());

			// Said as a warning rather than a statistic, because a reader who does not know frames went
			// missing will read a jump in the sequence numbers as something the compositor did. The
			// remedy is a slower output — the cadence is what `--output` sets — rather than a deeper
			// queue, since a queue absorbs a burst and this is a rate.
			if (dump->Dropped() != 0)
			{
				spdlog::warn(
					"             {} frame(s) dropped: the disk did not keep up, so the dump is a sample of the "
					"run. Try a lower rate, as --output={}x{}@10",
					dump->Dropped(),
					m_Resolutions[index].Width,
					m_Resolutions[index].Height
				);
			}

			if (dump->Skipped() != 0)
			{
				spdlog::warn("             {} frame(s) could not be read at all", dump->Skipped());
			}

			if (dump->Failed() != 0)
			{
				spdlog::warn(
					"             {} frame(s) could not be written: {}",
					dump->Failed(),
					dump->FirstFailure() ? dump->FirstFailure()->Context() : std::string_view{ "unknown" }
				);
			}
		}
	}

	// Remember what each output's extent was, so `Report` can suggest a rate in the terms the person
	// typed. Set by `Configure` alongside `Build`, because the plan is what knows it.
	void Remember(std::size_t index, PixelSize<DeviceSpace> resolution) noexcept { m_Resolutions[index] = resolution; }

private:
	// One directory for one output, and a level per output beyond that — because `frame-00000042.pam`
	// carries no output identity, so two panels writing into one directory would overwrite each
	// other's frames at every boundary they share. The parent is created here because Virtual/Pam.h
	// creates one level and this is the second.
	[[nodiscard]] std::string Destination(std::size_t index)
	{
		if (m_Outputs <= 1)
		{
			return m_Directory;
		}

		(void)::mkdir(m_Directory.c_str(), 0755);

		return std::format("{}/output-{}", m_Directory, index);
	}

	const IClock* m_Clock = nullptr;

	HeapAllocator m_Allocator{};
	VirtualDevice m_Device;

	std::string m_Directory;
	std::size_t m_Outputs = 0;

	std::array<std::unique_ptr<FrameDump>, MaxOutputs> m_Dumps{};
	std::array<PixelSize<DeviceSpace>, MaxOutputs> m_Resolutions{};
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

		if (const Result<void> ready = Construct(); !ready)
		{
			return ready;
		}

		if (const Result<void> ready = Configure(); !ready)
		{
			return ready;
		}

		m_Sources[0] = &m_Backend->Source();
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

		// After the join and before the report, which is the ordering the whole two-thread dump rests
		// on: nothing is still presenting, so the drain is a drain, and the counters the report reads
		// are final rather than a snapshot of a writer still working.
		m_Backend->Close();

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
		const Instant next = m_Backend->NextEvent();

		return next == Instant{ Duration::max() } ? Wake::Never() : Wake::At(next);
	}

	// Which backend was asked for, built. The one place in the process that names an implementation,
	// which is the whole job of a composition root — everything below this point works through
	// `IBackend`, and `Frame` never learns there was a choice.
	[[nodiscard]] Result<void> Construct()
	{
		switch (m_Options.Backend)
		{
			case BackendKind::Dump:
			{
				auto dump =
					std::make_unique<DumpBackend>(m_Clock, m_Options.DumpDirectory, m_Options.Requested().size());
				m_Dump = dump.get();
				m_Backend = std::move(dump);

				spdlog::info("writing frames to {}", m_Options.DumpDirectory);

				return {};
			}

			case BackendKind::Headless:
				m_Backend = std::make_unique<HeadlessBackend>(m_Clock);

				return {};

			case BackendKind::Auto:
			case BackendKind::Nested:
			case BackendKind::Drm:
				break;
		}

		return Failure(ENOSYS, "that backend is not built");
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

		BoundOutput& bound = m_Bound[index];

		if (m_Dump != nullptr)
		{
			m_Dump->Remember(index, wanted.Resolution);
		}

		if (const Result<void> built = m_Backend->Build(index, wanted, plan, bound); !built)
		{
			return built;
		}

		bound.Rebind();
		bound.OnTargetsInvalidated.ConnectTo<&BoundOutput::Rebind>(bound.Presenter->TargetsInvalidated, bound);

		m_Outputs[index].Bind(*bound.Presenter, *bound.Renderer, 0, wanted, {}, plan.Budget());

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

		m_Backend->Report();

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

	// The one implementation choice in the process, behind the one interface that hides it. Built in
	// `Construct` and destroyed with the root, after every `FrameOutput` that holds one of its
	// presenters by address has already gone — which is why it is declared before them.
	std::unique_ptr<IBackend> m_Backend;

	// The same object, when it is the dump. A backend answers `IBackend` and nothing more; this is
	// the root keeping hold of the one extra thing only the dump has — an extent per output, so the
	// warning about frames it could not write can name the rate that would fix it.
	DumpBackend* m_Dump = nullptr;

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
		// somebody who typed nothing and got headless should be able to find out why. Dump is built too
		// and auto never picks it: writing files is something a person asks for.
		resolved.Backend = BackendKind::Headless;

		spdlog::info("no backend selected and only headless is built; running headless");
	}

	if (resolved.Backend != BackendKind::Headless && resolved.Backend != BackendKind::Dump)
	{
		return Failure(ENOSYS, "only the headless and dump backends are built");
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
