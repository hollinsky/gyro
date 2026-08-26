// sigaction, pthread scheduling, and the rest of what this file asks of the platform are POSIX rather
// than ISO C, and glibc hides them under -std=c++NN. Named here for the reason Core/Clock.cpp names
// its own: what is wanted from the platform is stated rather than inherited from a build flag.
#define _POSIX_C_SOURCE 200809L

#include "Compositor/Compositor.h"

#include <signal.h>
#include <spdlog/spdlog.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
#include "Compositor/Wait.h"
#include "Core/Clock.h"
#include "Core/ColorState.h"
#include "Core/Handle.h"
#include "Core/Signal.h"
#include "Core/SlotAllocator.h"
#include "Core/Time.h"
#include "Core/Trace.h"
#include "Core/Wake.h"
#include "Dispatch/Loop.h"
#include "Drm/Device.h"
#include "Drm/Output.h"
#include "Frame/Evaluator.h"
#include "Frame/Loop.h"
#include "Geometry/AxisTransform.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Gym/Gym.h"
#include "Headless/Device.h"
#include "Headless/Output.h"
#include "Headless/Renderer.h"
#include "Input/Chord.h"
#include "Input/Devices.h"
#include "Nested/Host.h"
#include "Nested/Output.h"
#include "Protocol/Host.h"
#include "Publication/Return.h"
#include "Publication/Ring.h"
#include "Render/Allocator.h"
#include "Render/Device.h"
#include "Render/Governor.h"
#include "Render/Renderer.h"
#include "Render/Textures.h"
#include "Scene/Output.h"
#include "Seam/EventSource.h"
#include "Seam/Importer.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Trace/Recorder.h"
#include "Virtual/Device.h"
#include "Virtual/Dump.h"
#include "Virtual/Heap.h"
#include "Virtual/Output.h"

namespace
{

// SPEC: what is held between the predicted finish and the deadline, covering the error in the cost
// model. It is `TimingPolicy::Margin`, and it is the figure admission control is given, because a set
// admitted against a smaller margin than the record-time check holds is a set that passes the test and
// misses the frame.
//
// A hundred microseconds because that is what a capture says the model is worth: `Budget`'s marks are
// already a maximum over their window, so this covers only a composite costing more than the most
// expensive one recently seen, and over twelve hundred frames of the materials gym the measured cost
// exceeded its mark twice, by at most thirty-two microseconds. The rest is headroom for a cold window
// and for the marks still climbing after a mode change.
constexpr Duration CompletionMargin = std::chrono::microseconds{ 100 };

// What is held *ahead* of the armed instant, so that this thread is already running when the work has
// to start. It is `TimingPolicy::Lead`, and admission control is deliberately **not** given it: it is a
// fact about when gyro wakes up rather than about whether a set of outputs fits in a period, and an
// allocator holding it would refuse configurations gyro can serve.
//
// **Measured rather than specified, and the measurement is mostly not gyro's.** Two hundred and eighty
// microseconds of it is the kernel getting this thread onto a core after the ring's timeout expires,
// and a hundred and fifty-four is the drain and the snapshot acquisition once it is there — three
// hundred and forty-one together at the worst of twelve hundred iterations. Five hundred is that with
// room, and `Frame/Loop.h`'s `lead` row is where the next capture says whether it is still enough.
//
// **The large half is the processor waking up, not the scheduler.** A `SCHED_FIFO` thread that sleeps a
// whole refresh lets its core fall into a deep idle state, and this machine's costs a hundred and
// fifty-two microseconds to leave — which a bare `clock_nanosleep` reproduces with no compositor
// anywhere near it: three microseconds late after a fifty microsecond sleep, a hundred and forty-six
// after a thirteen millisecond one. So this figure is a *power management* number wearing a schedule's
// clothes, and the way to make it small is to tell the kernel gyro cannot afford the deep state rather
// than to keep enlarging the lead. See Open.md, *the idle state the frame thread wakes from*.
//
// **It buys latency rather than spending it, which is the opposite of what a margin usually does.** A
// lead does not delay the composite — it moves it to the *start* of the refresh window instead of the
// end, and the frame reaches the same vblank either way. What the window's end costs is a frame that
// is finished with no room left, so the next flip is the one that shows it. Measured across twelve
// hundred iterations of the materials gym: sixteen point six milliseconds from the composite to the
// glass against twenty at a lead of zero, no floor composites against fifty, and sixty frames a second
// with nothing repeated in both. The bound from above is a refresh — a lead longer than the period
// arms before the previous frame's window and would render against a scene the loop has not been
// handed yet — and that is far above anything the wakeup costs.
constexpr Duration ArmingLead = std::chrono::microseconds{ 500 };

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

// How the panel is turned, in the world's vocabulary rather than the seam's.
//
// The two enumerations are value-identical today and this could be a cast. It is a switch with no
// default label instead, so that the day one of them grows a case the other does not is a build failure
// here rather than a monitor rendered upside down — which is a bug a reinterpretation cannot express
// and a person would report as the panel being wrong.
[[nodiscard]] constexpr AxisOrientation Orientation(OutputTransform transform) noexcept
{
	switch (transform)
	{
		case OutputTransform::Normal:
			return AxisOrientation::Normal;
		case OutputTransform::Rotate90:
			return AxisOrientation::Rotate90;
		case OutputTransform::Rotate180:
			return AxisOrientation::Rotate180;
		case OutputTransform::Rotate270:
			return AxisOrientation::Rotate270;
		case OutputTransform::Flipped:
			return AxisOrientation::Flipped;
		case OutputTransform::Flipped90:
			return AxisOrientation::Flipped90;
		case OutputTransform::Flipped180:
			return AxisOrientation::Flipped180;
		case OutputTransform::Flipped270:
			return AxisOrientation::Flipped270;
	}

	return AxisOrientation::Normal;
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

// What a person hits when the stutter just happened. A pointer for `g_Stopping`'s reason, and a
// handler for the same one: the alternative is a thread parked in `sigwait`, and there is already a
// thread waiting on this — the writer's — that would then have two ways to be woken.
std::atomic<Recorder*> g_Recording{ nullptr };

extern "C" void OnTraceSignal(int)
{
	// One relaxed store, which is why `Recorder::Request` is the verb rather than `Snapshot`. Interning,
	// encoding and writing a file are none of them things a signal handler may do, and all of them
	// happen on the writer thread a moment later.
	if (Recorder* const recorder = g_Recording.load(std::memory_order_acquire))
	{
		recorder->Request();
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

	// **`SIGUSR1` is installed whether or not anything is recording**, because the run that turns out to
	// need a trace is the one nobody armed. With no ring behind it the handler is a load and a branch;
	// with one it is the difference between having the last thirty seconds and asking the user to
	// reproduce it.
	struct sigaction trace = {};
	trace.sa_handler = &OnTraceSignal;
	trace.sa_flags = SA_RESTART;
	::sigemptyset(&trace.sa_mask);

	if (::sigaction(SIGUSR1, &trace, nullptr) != 0)
	{
		return Failure(errno, "installing gyro's trace handler");
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
// The configuration is copied here rather than read back off the presenter, because `IPresenter` does
// not carry one — a presenter answers about targets and frames. **What is copied is what was
// *achieved* rather than what was asked for**, which used to be the same thing and stopped being one
// when nested landed: a host that will not take `XR24` linear hands back a tiled modifier, and a
// tiling window manager configures the window's size before gyro has drawn a pixel. Binding
// `FrameOutput` to the request would then give it the wrong extent to scissor to and the wrong colour
// state to bind targets under, on the backend where both routinely differ.
struct BoundOutput
{
	IPresenter* Presenter = nullptr;
	std::unique_ptr<IRenderer> Renderer;

	// The dispatch half of the same renderer, or null where it has none.
	//
	// **Two pointers to one object, and the second is not redundant.** Seam/Importer.h is a separate
	// interface because the *thread* is the difference — everything on `IRenderer` runs on the frame
	// thread and everything on `ITextureImporter` runs on dispatch — and the composition root is the
	// only place both halves are visible at once, which decision 131 says is the reason the verb is at
	// the waist rather than private to `Blit`. So the root is what pairs them, exactly as it pairs a
	// renderer with a presenter.
	//
	// **Two outputs may name one importer, which `Render`'s arrival made a real case**
	// *(2026-08-23)*. `Blit` is its own importer and there is one per output; Render/Textures.h belongs
	// to the *device* and every Vulkan output on the machine points at the same one, because a table
	// per renderer would hold a `VkImage` and a descriptor set per monitor for every window. So the
	// gather below is a set rather than a list.
	//
	// Null is still an ordinary answer: the simulated renderer has no pixels to import into. A dispatch
	// loop handed no importers refuses every adopt, which is what makes `--gym=card` fail to open on
	// those backends instead of drawing four empty rectangles.
	ITextureImporter* Importer = nullptr;
	OutputConfiguration Configuration{};
	Connection<> OnTargetsInvalidated;
	Connection<const OutputConfiguration&> OnReconfigured;

	// Why the targets could not be bound, kept rather than printed. `Reconfigured` is emitted from the
	// presenter's settle, which runs inside the frame loop's drain — so this arrives on the frame
	// thread, where this file's own rule is that nothing is logged and a failure is carried out to
	// whoever can. `Report` is that.
	//
	// **Kept even though the loop's refusal counter would eventually speak**, because what that says is
	// `record names an unbound target` — the symptom. A bind that failed on device memory or on a
	// modifier the device vetoed would still print a sentence about a target index, and send the reader
	// to the loop rather than to the import.
	std::optional<Error> BindFailure{};

	// The root's half of decision 41: the images went away, so whatever holds them has to be told before
	// the next record names one. `FrameOutput` has already damaged the whole output by the time this
	// runs; what is left is the binding, which is the root's because the root is what paired this
	// renderer with this presenter.
	//
	// **Two verbs rather than one, because the seam has two signals and they mean different things.**
	// Seam/Presenter.h fixes the order: the imports go on `TargetsInvalidated`, since the presenter is
	// about to close those descriptors, and the new set exists only by `Reconfigured`. `Targets()` is
	// empty in between and says so. Binding that empty span was not *wrong* — Seam/Renderer.h has a bind
	// imply a release and Render/Renderer.cpp does it first — but it said *release* by doing something
	// else that happens to, one line away from the half that was actually missing.
	void Release() noexcept
	{
		if (Renderer)
		{
			Renderer->ReleaseTargets();
		}
	}

	// **The other half, and its absence is what blackened every nested output after its first resize.**
	// A window manager configures gyro's toplevel before it has drawn a pixel, so a resize is not an
	// occasional event on that backend — it is the second thing that happens. `TargetsInvalidated`
	// dropped the imports, `Reconfigured` built a new set, and nothing bound it: every record from then
	// on named a target the renderer did not have, the loop returned in silence, and the window held its
	// first frame for the rest of the run.
	//
	// The configuration is adopted here rather than left at what was asked for, for the reason this
	// struct's own comment gives about construction: a host hands back the size and the modifier it
	// actually gave, and binding a new target set under the old colour state is that same mismatch one
	// signal later.
	void Adopt(const OutputConfiguration& achieved) noexcept
	{
		Configuration = achieved;
		Rebind();
	}

	void Rebind() noexcept
	{
		if (Presenter == nullptr || !Renderer)
		{
			return;
		}

		// The first one is kept, for Frame/Loop.h's reason on the refusal beside it: a renderer that
		// cannot bind is a standing condition, and the reason it first could not is the one that names
		// what the run started doing wrong.
		if (const Result<void> bound = Renderer->BindTargets(Presenter->Targets(), Configuration.Color);
		    !bound && !BindFailure)
		{
			BindFailure = bound.error();
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

	// Whether this backend has run out of reasons to keep going.
	//
	// **Root-local for `NextEvent`'s reason, and it has exactly one answer that is not `false`.** A
	// headless sweep stops on `--frames`, a dump stops when it is interrupted, and a KMS output does
	// not stop at all — but a nested session ends when the person closes the window, which is the
	// ordinary way to quit it, and it ends when the host hangs up, which is not. Neither of those is
	// an `IEventSource` question: `Drain` already answers whether a read failed, and the loop
	// deliberately does not act on it, because a backend that has *finished* and one that failed a
	// read are different things and only the root can sequence a shutdown.
	[[nodiscard]] virtual bool IsFinished() const noexcept { return false; }

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
		into.Configuration = presenter->Configuration();

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
		into.Configuration = presenter->Configuration();
		into.Importer = renderer.get();
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

// The daily driver: gyro as a client of another compositor, one host window per output.
//
// **It is the first backend whose presenter and renderer are the same device's, and that is decision
// 120 arriving.** A nested output has no GBM device handed to it and no swapchain allocating on its
// behalf, so the only thing on the machine that can produce its targets is the Vulkan device that is
// about to draw into them — and `Nested` may not name `Render`, so the root is what pairs them. Here
// that pairing is three lines and one `IDmabufAllocator`, which is exactly what moving the interface
// to the waist bought.
//
// **The device is opened once and the renderers are per output**, for `BoundOutput`'s reason: binding
// targets is not a per-frame call, so a writer belongs to one presenter's target set, while the queue
// the work serialises on is the device's. Every output names device zero because there is one GPU.
//
// **Nothing here forces real time and nothing here should.** Docs/Architecture.md#backends makes
// nested drop `SCHED_FIFO` and `mlockall` unless explicitly overridden, and `Run` below is where that
// happens — a real-time thread inside a normal-priority host is an effective way to hard-lock the
// desktop somebody is developing on.
class NestedBackend final : public IBackend
{
public:
	NestedBackend(const IClock& clock, bool governor) noexcept : m_Clock{ &clock }, m_Governs{ governor } {}

	// Open the connection and the device, in that order.
	//
	// The connection first because it is what says whether there is a session to nest in at all, and a
	// machine with no `WAYLAND_DISPLAY` should answer that rather than spend a Vulkan instance
	// discovering it. Both failures are sentences: the host's is *what it did not offer*, the device's
	// is Render/Device.h's own.
	[[nodiscard]] Result<void> Open()
	{
		if (const Result<void> connected = m_Host.Open(); !connected)
		{
			return connected;
		}

		// Sized before the frame thread exists, per Wire/Connection.h: a nested output asks for a
		// `wp_presentation_feedback` object per commit and `Present` runs inside the frame section,
		// where a vector growing is an abort. Two per window per ring slot plus the fixed objects is
		// generous by an order of magnitude and costs eight bytes an id.
		m_Host.Connection().Reserve(256);

		Result<VulkanDevice> device = VulkanDevice::Open();

		if (!device)
		{
			return std::unexpected{ device.error() };
		}

		m_Device = std::move(*device);
		m_Allocator.emplace(m_Device);

		// One table for every output on this device, which is Render/Textures.h's whole argument: the
		// root is the only party that sees both a device and the renderers over it, and a table per
		// renderer would hold one copy of every window's pixels per monitor.
		m_Textures.emplace(m_Device);

		if (const Result<void>& built = m_Textures->Status(); !built)
		{
			return built;
		}

		if (!m_Device.Description().CopiesFromHost)
		{
			// Said out loud for `ExportsTimeline`'s reason: it decides which clients this session can
			// draw at all. A driver without `VK_EXT_host_image_copy` takes dmabuf buffers and refuses
			// software ones, and a person seeing a blank window from an `wl_shm` toolkit should find
			// the sentence that explains it in the same log as everything else.
			spdlog::warn("this device cannot fill an image from host memory, so software client buffers are refused");
		}

		spdlog::info("rendering on {} ({})", m_Device.Description().DeviceName(), m_Device.Description().DriverName());

		if (m_Governs)
		{
			// Decision 142, and the composition root is where it belongs for the reason every other
			// machine-wide policy in this file is here: the frequency floor is one number for the whole
			// system, and this is the only party that owns something whose lifetime is the session. It
			// runs before any output is built, so the probe's own submissions are the only work on the
			// device and the clock it reads is a measurement rather than a mixture.
			m_Governor = GpuGovernor::Take(*m_Clock, m_Device, *m_Textures);
		}

		if (!m_Device.Description().ExportsTimeline)
		{
			// Decision 108's case, said out loud because it decides which of the two commit paths this
			// session runs. lavapipe advertises the extension and then refuses to create an exportable
			// semaphore, so the floor tier finishes inside `Record` and hands back an immediate point —
			// which needs no acquire point at all and is therefore fine, rather than degraded.
			spdlog::info("this device exports no timeline, so every composite finishes before it is committed");
		}

		return {};
	}

	[[nodiscard]] IEventSource& Source() noexcept override { return m_Host; }

	[[nodiscard]] Instant NextEvent() const noexcept override { return m_Host.NextEvent(); }

	[[nodiscard]] bool IsFinished() const noexcept override
	{
		// The host hanging up is not something to keep drawing through: every window is gone and every
		// `Present` from here on is refused by a connection that has latched its failure.
		if (m_Host.Connection().Failed().has_value())
		{
			return true;
		}

		// **Every window, not any window.** Closing one of three under `--outputs=3` is a person
		// removing an output, and the session carries on with two — which is the same thing hotplug
		// will be. It is over when the last one goes.
		for (std::size_t index = 0; index < m_Count; ++index)
		{
			if (!m_Windows[index]->IsClosed())
			{
				return false;
			}
		}

		return m_Count != 0;
	}

	[[nodiscard]] Result<void>
	Build(std::size_t index, const OutputConfiguration& wanted, const OutputPlan&, BoundOutput& into) override
	{
		if (index >= m_Windows.size())
		{
			return Failure(ENOSPC, "more outputs than one host connection carries");
		}

		auto renderer = std::make_unique<VulkanRenderer>(*m_Clock, m_Device, *m_Textures);

		auto window = std::make_unique<Nested::NestedOutput>(
			m_Host,
			*m_Allocator,
			renderer.get(),
			wanted,
			Nested::NestedOutputPolicy{ .Title = std::format("gyro output {}", index) }
		);

		if (const Result<void> opened = window->Open(); !opened)
		{
			return opened;
		}

		into.Presenter = window.get();
		into.Configuration = window->Configuration();
		into.Renderer = std::move(renderer);

		// The device's table, not this renderer's — Render/Textures.h's whole argument, and the reason
		// the gather above is a set. Every output on this device hands back the same pointer.
		into.Importer = &*m_Textures;

		m_Windows[index] = std::move(window);
		m_Count = index + 1;

		return {};
	}

	void Report() const override
	{
		for (std::size_t index = 0; index < m_Count; ++index)
		{
			const Nested::NestedOutput& window = *m_Windows[index];

			spdlog::info(
				"  output {}: {} commit(s), {} discarded by the host, {} held for a composite",
				index,
				window.Commits,
				window.Discarded,
				window.Held()
			);

			// The two figures a session's pacing is only as good as. A discard is a frame the host threw
			// away — occlusion, a superseded commit, a move to another monitor — and a held commit is one
			// gyro could not hand over until its own GPU had finished, which is the fallback path saying
			// what it cost.
			if (window.Held() != 0)
			{
				spdlog::warn(
					"             this window ran without explicit sync, so its presentation timestamps "
					"include gyro's own polling and are not a measurement of the host's cadence"
				);
			}
		}

		if (const std::optional<Wire::ProtocolFault>& fault = m_Host.Fault(); fault.has_value())
		{
			spdlog::error("the host ended the connection: {} on {}", fault->Message, fault->Object);
		}
	}

private:
	const IClock* m_Clock = nullptr;

	// Declared before the device, so it is destroyed after it — a window holds `wl_buffer`s over
	// descriptors the device exported, and the protocol objects have to go while the connection is
	// still open.
	Nested::NestedHost m_Host;

	VulkanDevice m_Device;
	std::optional<VulkanAllocator> m_Allocator;

	// After the device and before the windows, so it is destroyed after every renderer that
	// registered with it and before the device that owns its handles. Every renderer detaches in its
	// own destructor, which is what makes this ordering a statement rather than a hope.
	std::optional<VulkanTextures> m_Textures;

	// The frequency policy, held for the length of the session because its destructor is what puts the
	// machine's minimum clock back. It owns no Vulkan handle — the renderer its probe drew through was
	// built and destroyed inside `Take` — so it sits here for readability rather than for ordering.
	bool m_Governs = true;
	GpuGovernor m_Governor;

	// `unique_ptr` because a presenter is neither copyable nor movable and the array has to be built
	// one at a time, which is `BoundOutput::Renderer`'s reason exactly.
	std::array<std::unique_ptr<Nested::NestedOutput>, MaxOutputs> m_Windows{};
	std::size_t m_Count = 0;
};

// The panel: a DRM device, one output per connected connector, and a clock that is the hardware's.
//
// **It is the same shape as the nested backend with the host replaced by the card**, which is decision
// 5's claim being paid off rather than a coincidence — the seam was designed from the KMS
// specification, so the backend that arrives last is the one that fits without moving anything.
//
// **Master is first-open and nothing here claims a seat.** Decision 7 deferred choosing a D-Bus
// client until this backend needed one; it does not. The node comes from a udev rule and the process
// that opens it first is master for the life of the file description, which on a machine with no VTs
// nothing can take away.
class DrmBackend final : public IBackend
{
public:
	DrmBackend(const IClock& clock, bool governor) noexcept : m_Clock{ &clock }, m_Governs{ governor } {}

	// The card first, for `NestedBackend::Open`'s reason exactly: it is what says whether there is a
	// panel to drive at all, and a machine with nothing connected should answer that rather than spend
	// a Vulkan instance discovering it.
	[[nodiscard]] Result<void> Open(std::string_view path)
	{
		Result<std::unique_ptr<Drm::DrmDevice>> device = Drm::DrmDevice::Open(path);

		if (!device)
		{
			return std::unexpected{ device.error() };
		}

		m_Card = std::move(*device);

		spdlog::info(
			"driving {} ({}) with {} connected output(s)", m_Card->Path(), m_Card->Driver(), m_Card->Pipelines().size()
		);

		if (!m_Card->HasMonotonicTimestamps())
		{
			// The clock's error bar depends on this and nothing else can tell it. `simpledrm` is the case
			// that produces it — no vblank at all, so a completion arrives when the commit is applied
			// rather than at a boundary — and every deadline this session schedules is then predicted from
			// a cadence that is gyro's own polling.
			spdlog::warn(
				"{} does not report monotonic page-flip timestamps, so this session's pacing figures are "
				"not a measurement of the panel",
				m_Card->Path()
			);
		}

		Result<VulkanDevice> rendering = VulkanDevice::Open();

		if (!rendering)
		{
			return std::unexpected{ rendering.error() };
		}

		m_Device = std::move(*rendering);
		m_Allocator.emplace(m_Device);
		m_Textures.emplace(m_Device);

		if (const Result<void>& built = m_Textures->Status(); !built)
		{
			return built;
		}

		spdlog::info("rendering on {} ({})", m_Device.Description().DeviceName(), m_Device.Description().DriverName());

		if (m_Governs)
		{
			m_Governor = GpuGovernor::Take(*m_Clock, m_Device, *m_Textures);
		}

		return {};
	}

	[[nodiscard]] IEventSource& Source() noexcept override { return *m_Card; }

	// The device's file is what wakes this backend, so the ordinary answer is never. What is not on the
	// file is a composite a commit is waiting for, which is the held path saying when to look again.
	[[nodiscard]] Instant NextEvent() const noexcept override
	{
		Instant next{ Duration::max() };

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			next = std::min(next, m_Panels[index]->NextEvent());
		}

		return next;
	}

	[[nodiscard]] Result<void>
	Build(std::size_t index, const OutputConfiguration& wanted, const OutputPlan&, BoundOutput& into) override
	{
		if (index >= m_Card->Pipelines().size())
		{
			return Failure(ENOSPC, "more outputs than this device has connected");
		}

		auto renderer = std::make_unique<VulkanRenderer>(*m_Clock, m_Device, *m_Textures);

		auto panel =
			std::make_unique<Drm::DrmOutput>(*m_Card, m_Card->Pipelines()[index], *m_Allocator, renderer.get());

		if (const Result<void> opened = panel->Open(wanted); !opened)
		{
			return opened;
		}

		spdlog::info(
			"  {}: {} on {}",
			m_Card->Pipelines()[index].Name,
			panel->Configuration(),
			panel->IsExplicitlySynchronized() ? "an in-fence" : "a held commit"
		);

		into.Presenter = panel.get();
		into.Configuration = panel->Configuration();
		into.Renderer = std::move(renderer);
		into.Importer = &*m_Textures;

		m_Panels[index] = std::move(panel);
		m_Count = index + 1;

		return {};
	}

	void Report() const override
	{
		for (std::size_t index = 0; index < m_Count; ++index)
		{
			const Drm::DrmOutput& panel = *m_Panels[index];

			spdlog::info("  {}: {} commit(s), {} held for a composite", panel.Pipe().Name, panel.Commits, panel.Held());

			if (panel.Held() != 0)
			{
				spdlog::warn(
					"             this output ran without an in-fence, so its presentation timestamps "
					"include gyro's own polling and are not a measurement of the panel's cadence"
				);
			}
		}
	}

private:
	const IClock* m_Clock = nullptr;

	// Declared before the rendering device, so it is destroyed after it: an output holds framebuffers
	// over descriptors the device exported, and `drmModeRmFB` has to run while both still exist.
	std::unique_ptr<Drm::DrmDevice> m_Card;

	VulkanDevice m_Device;
	std::optional<VulkanAllocator> m_Allocator;
	std::optional<VulkanTextures> m_Textures;

	bool m_Governs = true;
	GpuGovernor m_Governor;

	std::array<std::unique_ptr<Drm::DrmOutput>, MaxOutputs> m_Panels{};
	std::size_t m_Count = 0;
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

		// Decision 83's doorbell, opened whether or not there is anything to ring it. An unrung eventfd
		// costs a descriptor and one armed poll; making it conditional would put a `nullptr` in the source
		// array that every loop over it has to answer for, to save that.
		if (const Result<void> ready = m_Publication.Open(); !ready)
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

		// **Armed before the threads exist, because a ring cannot be handed to a thread that is already
		// running without a moment where the thread has none.** The buffers are the root's and outlive
		// both loops; what each thread does at its own start is take the pointer and its own id.
		if (options.TraceBytes != 0)
		{
			m_Recorder.emplace(
				TracePolicy{
					.Bytes = options.TraceBytes, .Path = std::filesystem::path{ options.TracePath }, .Pid = ::getpid() }
			);

			m_FrameTrace = m_Recorder->Arm("frame", m_Clock);

			// Whichever author this run has, and none under `--no-socket`, where there is no second
			// thread to give a row to. `OpenDispatch` runs after this, so the question is the options
			// rather than the loop that does not exist yet.
			if (m_Options.Gym || m_Options.Clients)
			{
				m_DispatchTrace = m_Recorder->Arm("dispatch", m_Clock);
			}
		}

		m_Sources[0] = &m_Backend->Source();
		m_Sources[1] = &m_Interrupt;
		m_Sources[2] = &m_Publication;

		m_Loop = std::make_unique<FrameLoop>(
			m_Clock,
			m_Snapshots,
			m_Returns,
			m_Evaluator,
			Timing{ TimingPolicy{ .Margin = CompletionMargin, .Lead = ArmingLead } }
		);
		m_Loop->Bind({ m_Outputs.data(), m_Count });
		m_Loop->Listen({ m_Sources.data(), m_Sources.size() });

		// Last, because it lays the world out against the modes the backend *achieved* rather than the
		// ones that were asked for, and `Configure` above is where those stop differing.
		return OpenDispatch();
	}

	[[nodiscard]] Result<void> Run()
	{
		g_Stopping.store(&m_Interrupt, std::memory_order_release);

		if (m_Recorder)
		{
			g_Recording.store(&*m_Recorder, std::memory_order_release);
			m_Recorder->Start();
		}

		if (const Result<void> installed = InstallStopHandlers(); !installed)
		{
			g_Stopping.store(nullptr, std::memory_order_release);
			g_Recording.store(nullptr, std::memory_order_release);

			return installed;
		}

		// **Started first, so that the first thing the panel shows is the world rather than a black
		// frame.** It is a head start and not a handshake: the frame thread composites whatever the ring
		// holds, so losing the race costs one frame and the doorbell fetches the next one. What a
		// handshake would buy is not worth a startup path that can deadlock.
		std::thread world;

		if (m_Dispatch)
		{
			world = std::thread{ [this] {
				Record(m_DispatchTrace);

				m_DispatchResult = Pump();

				// A dispatch thread that stopped is a world that stopped changing, and the frame thread has
				// no way to notice: it would go on compositing the last snapshot forever, at rate, with
				// nothing to say why. So the failure ends the process through the path shutdown already
				// takes rather than becoming a hang somebody has to attach a debugger to.
				if (!m_DispatchResult)
				{
					m_Interrupt.Raise();
				}
			} };
		}

		// The thread is the root's, per decision 80, and so is everything platform about it: its
		// priority, its page residency, and the `while` inside it. What it runs is one portable call in a
		// loop.
		std::thread frame{ [this] {
			Record(m_FrameTrace);

			m_Result = Iterate();
		} };

		frame.join();

		// **After the frame thread and not beside it.** The frame thread is what decides a run is over —
		// `--frames`, a signal, a host window closed — and stopping the author first would spend the last
		// frames of the run compositing a world that had already been told to stop moving.
		if (world.joinable())
		{
			m_DispatchWait.Stop();
			world.join();
		}

		g_Stopping.store(nullptr, std::memory_order_release);

		// **The last snapshot is taken after both threads have stopped and before the writer thread is
		// told to.** A signal arriving now would find nothing left to record; what is wanted is the ring
		// as the run ended, which is what is in it at exactly this moment.
		if (m_Recorder && m_Options.TraceAtExit)
		{
			const std::filesystem::path path{ m_Options.TracePath };

			if (const Result<TraceSummary> written = m_Recorder->Snapshot(path); written)
			{
				m_Traced = *written;
			}
			else
			{
				spdlog::error("the trace could not be written to {}: {}", path.string(), written.error());
			}
		}

		if (m_Recorder)
		{
			g_Recording.store(nullptr, std::memory_order_release);
			m_Recorder->Stop();
		}

		// After the join and before the report, which is the ordering the whole two-thread dump rests
		// on: nothing is still presenting, so the drain is a drain, and the counters the report reads
		// are final rather than a snapshot of a writer still working.
		m_Backend->Close();

		Report();

		// The frame thread's answer first, because it is the one that carries what the run was for. A
		// dispatch failure is only the return value when the frame side had nothing of its own to say —
		// and by then it has already stopped the run through the interrupt above, so the two are one
		// event reported once.
		return m_Result ? m_DispatchResult : m_Result;
	}

private:
	// A thread taking its own ring, which is the one part of enrollment that cannot be done for it: the
	// thread-local pointer belongs to this thread and so does the id the kernel knows it by.
	void Record(TraceBuffer* buffer) noexcept
	{
		if (m_Recorder && buffer != nullptr)
		{
			m_Recorder->Join(*buffer, ::gettid());
		}
	}

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

			// **The return leg's doorbell, rung only where somebody is waiting on it.** A report crosses
			// every iteration and carries a presented frame on the ones an output flipped; what dispatch
			// does with that is a `wl_surface.frame` callback, and nothing wakes it to do so. So the count
			// is published for the dispatch thread's own check, and the write to the eventfd happens only
			// where a client is actually owed a frame — a splash, a console, and an idle desktop present
			// with nothing in the ledger and spend no syscall at all.
			if (m_Shown.load() != m_Loop->Shown())
			{
				m_Shown.store(m_Loop->Shown());

				if (m_ClientsOwed.load())
				{
					m_DispatchWait.Nudge();
				}
			}

			// Bounded mode stops at idle rather than at the count, and the count is the guard rather than
			// the goal. A `Settled` wake *is* Docs/Architecture.md#doing-nothing-must-cost-nothing reached
			// — nothing owed, nothing outstanding, nothing to arm — so a run that gets there has shown the
			// thing worth showing, and waiting out the remaining iterations would only show that the
			// binary can block. The count is what catches the case where idle never arrives.
			if (m_Options.Iterations != 0)
			{
				// **With an author, idle is both halves at rest, and this thread can only see one of
				// them.** A frame thread that folds to `Settled` has caught up with the scene it holds; it
				// says nothing about whether the author is between motions. `lanes` is exactly that case —
				// it publishes a still scene, sleeps until the next lane falls due, and retargets — so a
				// bounded run that stopped at the frame side's first idle would end after one frame and
				// report it as *doing nothing costs nothing*, which is the one claim in the design this
				// flag exists to keep honest.
				//
				// Both conditions are needed and they answer different questions. Holding a sequence is the
				// cheapest true statement that a scene ever reached this thread, and without it a `settle`
				// gym would stop the run in the window between its first step and the frame thread's first
				// acquire. `m_WorldSettled` is the author saying it owes nothing further. Without an author
				// there is nothing to wait for and the empty ring folding to idle is the whole answer.
				const bool world =
					m_Dispatch == nullptr || (m_Loop->Held() != 0 && m_WorldSettled.load(std::memory_order_acquire));

				m_Idled = wake.Which == Wake::Kind::Settled && world;

				if (m_Idled || m_Iterations >= m_Options.Iterations)
				{
					return {};
				}
			}

			if (m_Interrupt.IsRaised() || m_Backend->IsFinished())
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

	// The machine's keyboards, and the way back out of a compositor that owns the screen.
	//
	// **Not fatal, and the two failures it distinguishes are worth the sentence each.** A machine with
	// no seat at all — a kiosk, a board with a panel and nothing plugged into it — must still boot to a
	// splash, so an absent device set is a run that shows pictures and takes no input. A machine whose
	// udev rules have not been applied is the same code path and a completely different situation, and
	// the log line is what tells them apart before somebody spends an afternoon on it.
	[[nodiscard]] Result<void> OpenInput()
	{
		if (!m_Panel)
		{
			return {};
		}

		Result<std::unique_ptr<Input::Devices>> devices = Input::Devices::Open();

		if (!devices)
		{
			spdlog::warn(
				"no input: {}. gyro holds the panel and there is no way to reach it from this keyboard; "
				"check the udev rules for /dev/input",
				devices.error()
			);

			return {};
		}

		m_Input = std::move(*devices);
		m_Key.ConnectTo<&Compositor::OnKey>(m_Input->Key, *this);
		m_DispatchWait.Watch(m_Input->Descriptor().Value);

		// Said out loud on every run that has a keyboard, because a chord nobody knows about is a chord
		// nobody uses, and this one is the only exit.
		spdlog::info("ctrl+alt+esc then q quits, t writes a trace");

		return {};
	}

	// One key, on the dispatch thread that drained it.
	//
	// **The compositor looks first and clients get what is left**, which is the ordering the escape
	// hatch depends on: a full-screen client that grabbed the keyboard cannot be what decides whether
	// the chord is seen. What is left goes to the seat, which routes it to whatever has focus — and a
	// key the chord took goes there too, marked as taken, because the modifier state a client is told
	// about is what a person is holding down rather than what gyro passed on.
	void OnKey(const KeyEvent& event)
	{
		const Input::ChordVerdict verdict = m_Chord.Feed(event);

		switch (verdict.Action)
		{
			case Input::ChordAction::Quit:
				spdlog::info("stopping: ctrl+alt+esc q");

				// Both halves, because either one alone leaves a thread parked: the frame thread is
				// waiting on a vblank and this one on its own descriptor.
				m_Interrupt.Raise();
				m_DispatchWait.Stop();

				break;

			case Input::ChordAction::Trace:
				// The same request `SIGUSR1` makes, and it is deliberately the same call rather than a
				// second path: one relaxed store, picked up by the writer thread, with nothing done here
				// that a signal handler could not do.
				if (m_Recorder)
				{
					m_Recorder->Request();
				}
				else
				{
					spdlog::info("nothing is being recorded; start gyro with --trace");
				}

				break;

			case Input::ChordAction::None:
				break;
		}

		// Only where there is one: `--gym` runs the same loop with no clients behind it, and a scene gyro
		// authored for itself has nothing to route a keystroke to.
		if (m_Clients)
		{
			m_Clients->OnKey(event, verdict.Consumed);
		}
	}

	// Everything the dispatch thread does, and it is decision 80's shape again with the threads
	// swapped: step, and wait for the wake the step returned.
	//
	// Normal priority, deliberately and by omission — `PromoteToRealTime` is called on the frame thread
	// and a scheduling policy is one thread's property, so the author is preemptible by the compositor
	// that reads it. That is decision 61's order, and it is what makes a scene walk that overruns cost a
	// stale frame rather than a missed one.
	[[nodiscard]] Result<void> Pump()
	{
		while (!m_DispatchWait.IsStopping())
		{
			// Read before the step, because the step's drain is what would consume it: a frame presented
			// after this load and before the sleep below is one the drain cannot have seen, and comparing
			// against a value taken afterwards would be comparing the count with itself.
			const std::uint64_t shown = m_Shown.load();

			// **Input first, before the step it may cause.** A key that arrives here becomes a retarget
			// inside the author's `Advance` below, in the same iteration, which is what keeps the chain
			// from a keystroke to the frame it changes one causal sequence rather than two — the same
			// ordering Frame/Loop.h drains its sources under, one thread over.
			if (m_Input && !m_InputFailed)
			{
				if (const Result<void> drained = m_Input->Drain(); !drained)
				{
					// Reported once and then left alone. A device set that has stopped working is not a
					// reason to stop compositing — the screen is the thing gyro exists to keep showing — and
					// a line per iteration is how a log stops being readable.
					m_InputFailed = true;

					spdlog::error("input stopped: {}", drained.error());
				}
			}

			const std::uint64_t before = m_Dispatch->Publications();
			const Wake wake = m_Dispatch->Step();

			// Published for the bounded run's benefit and nothing else. `Never()` on this side is *the
			// world has stopped changing* — no author owes a retarget and no channel owes a retirement —
			// which is the half of idle the frame thread cannot see, since what reaches it is a scene and
			// not the intent behind one. Released before the store below so that a frame thread reading
			// it has already seen everything the step published.
			m_WorldSettled.store(wake.Which == Wake::Kind::Settled, std::memory_order_release);

			// **Rung when a snapshot actually crossed, which is narrower than decision 83's unconditional
			// signal** — and narrower for a case that decision did not have in front of it. A step whose
			// publish the ring refused has nothing new for the frame thread to acquire, and it is reached
			// only when the frame thread is already four publishes behind, so signalling there would wake a
			// late thread to hand it what it already holds, at the retry cadence. This is not the
			// conditional form that decision defers and leaves a lost wakeup in: that one reads the frame
			// thread's idleness and races it. This reads dispatch's own counter, which nothing else writes.
			if (m_Dispatch->Publications() != before)
			{
				m_Publication.Raise();
			}

			// Between the step and the wait, because the stop may have arrived during a scene walk and
			// the alternative is a thread parked on a deadline the root is waiting out.
			if (m_DispatchWait.IsStopping())
			{
				return {};
			}

			// **Immediately before the sleep, which is the only place it can be.** Everything gyro owes
			// its clients — a frame callback for a frame that reached the glass, a `wl_buffer.release` for
			// pixels it has finished with — was queued during the step above and is sitting in a libwayland
			// buffer until something pushes it. Flushing at the top of the *next* iteration would be a
			// deadlock rather than a delay: a settled world arms no deadline, so the only thing that would
			// wake this thread is the client acting on the callback it has not been sent. The host is an
			// author and reads its clients inside `Advance`; the writing back is the root's, because the
			// root is what knows the thread is about to stop running.
			if (m_Clients != nullptr)
			{
				m_Clients->Flush();
			}

			// **Published before the sleep and paired with the check below**, which is the half of the
			// handshake that closes the window the doorbell alone leaves: the frame thread may have
			// presented between this step's drain and this store, in which case it read `false` and rang
			// nothing, and what catches that is the count having moved since the step began.
			m_ClientsOwed.store(m_Dispatch->Owing());

			if (m_Dispatch->Owing() && m_Shown.load() != shown)
			{
				continue;
			}

			const Instant now = m_Clock.Now();

			if (const Result<void> waited = m_DispatchWait.WaitUntil(DispatchDeadline(wake, now), now); !waited)
			{
				return waited;
			}
		}

		return {};
	}

	// The wake dispatch answered, as an instant to sleep until.
	//
	// **`Continuous` is the one that needs an answer here rather than in `Wake`.** It means *now, and
	// again every interval after* — and on the frame side a zero interval is "every frame the output
	// offers", which is a rate a vblank supplies. Dispatch has no vblank, so the same value would be a
	// spin at whatever rate a scene walk happens to take. The fastest panel in the set is what the author
	// must have meant, since publishing faster than the quickest thing that can read it is work nobody
	// sees, and the root is the only place holding that number — which is why the conversion is here and
	// not in Dispatch/Loop.h.
	//
	// Nothing produces one today: a gym answers `Never()` or `At()`, and the serializer's republication is
	// `Timed` by construction. This is what it will mean when something does.
	[[nodiscard]] std::optional<Instant> DispatchDeadline(Wake wake, Instant now) const noexcept
	{
		switch (wake.Which)
		{
			case Wake::Kind::Settled:
				return std::nullopt;

			case Wake::Kind::Timed:
				return wake.When;

			case Wake::Kind::Continuous:
			{
				const Duration interval = wake.Interval > Duration::zero() ? wake.Interval : ShortestPeriod();

				return std::max(wake.When, Advanced(now, interval));
			}
		}

		return std::nullopt;
	}

	// The period of the quickest thing that can read a publication.
	[[nodiscard]] Duration ShortestPeriod() const noexcept
	{
		Duration shortest = Duration::max();

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			shortest = std::min(shortest, m_Schedule.Plans()[index].Period);
		}

		// No outputs is not a rate, and the retry cadence is the only other honest interval in the
		// process. Unreachable while `Configure` admits at least one; here so the fold above cannot
		// return `Duration::max()` as a sleep.
		return shortest == Duration::max() ? PublishRetryInterval : shortest;
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

			case BackendKind::Nested:
			{
				auto nested = std::make_unique<NestedBackend>(m_Clock, m_Options.Governor);

				if (const Result<void> opened = nested->Open(); !opened)
				{
					return opened;
				}

				m_Backend = std::move(nested);

				return {};
			}

			case BackendKind::Drm:
			{
				auto drm = std::make_unique<DrmBackend>(m_Clock, m_Options.Governor);

				if (const Result<void> opened = drm->Open(m_Options.Device); !opened)
				{
					return opened;
				}

				m_Backend = std::move(drm);

				// **Only a run that owns a panel reads the machine's keyboards.** libinput's udev backend
				// takes every device on the seat, which under the nested backend would be gyro reading the
				// host compositor's keyboard behind its back — every keystroke in the session, whatever has
				// focus. So the flag is the panel rather than a switch somebody has to remember.
				m_Panel = true;

				return {};
			}

			case BackendKind::Auto:
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

		m_Schedule = Schedule::Build({ demands.data(), count }, CompletionMargin);

		for (std::size_t index = 0; index < count; ++index)
		{
			if (const Result<void> added = AddOutput(index, requested[index], m_Schedule.Plans()[index]); !added)
			{
				return added;
			}
		}

		return {};
	}

	// The world's author, and the outputs it authors against.
	//
	// **There are two authors and the loop steps one**, which is what `--gym` selects between: a gym is
	// gyro authoring for itself with nothing on the far end, and the client host is a person's windows
	// arriving over a socket. Under `--no-socket` there is neither — no dispatch thread is started, the
	// ring stays empty, and the frame loop evaluates an empty scene every iteration, which is the floor
	// case Docs/Architecture.md#doing-nothing-must-cost-nothing is about rather than a stub, and has to
	// stay reachable in one command.
	//
	// **The host's descriptor is wired into the wait here and nowhere else.** `ISceneAuthor` has no verb
	// for one — a gym has no socket to answer for — so the concrete type is the thing that carries it,
	// and this is the only party holding it before the author is handed over. `m_Clients` keeps that
	// pointer afterwards for exactly two jobs: the flush before each sleep, and the log line.
	[[nodiscard]] Result<void> OpenDispatch()
	{
		std::unique_ptr<ISceneAuthor> author;

		if (m_Options.Gym)
		{
			const GymKind gym = *m_Options.Gym;

			spdlog::info("authoring the {} gym: {}", Name(gym), Describe(gym));

			// The sentence Gym/Gym.h says a caller owes the person. `Blit` fails a whole record on one item
			// it cannot express, so a gym authoring a material or a rotated quad under the CPU renderer
			// writes no frames *at all* rather than frames missing a node — and the symptom is a directory
			// that stays empty, which is what somebody would otherwise file as a bug against the dump
			// backend. The test is the backend rather than a question put to `IRenderer`, because the seam
			// has no verb for it and adding one for a warning would be an interface written for the fakes.
			if (m_Options.Backend == BackendKind::Dump && !DrawsOnCpu(gym))
			{
				spdlog::warn("no CPU composite can draw it, so this run will write no frames at all");
			}

			Result<std::unique_ptr<ISceneAuthor>> made = MakeGym(Name(gym));

			if (!made)
			{
				return std::unexpected{ made.error() };
			}

			author = std::move(*made);
		}
		else if (m_Options.Clients)
		{
			Result<std::unique_ptr<ClientHost>> made = MakeClientHost(m_Options.Socket);

			if (!made)
			{
				return std::unexpected{ made.error() };
			}

			m_Clients = made->get();

			spdlog::info("hosting clients on {}", m_Clients->SocketName());

			// Four globals, which is a window and nothing a person can do to it. Said out loud because
			// the alternative is somebody filing the silence as a bug: an application will open, appear
			// centred, and then ignore every click and keystroke and never repaint. The clipboard is
			// named the same way, because a global that is advertised and does nothing is the other kind
			// of silence somebody would spend an afternoon on.
			spdlog::info(
				"wl_compositor, wl_shm, xdg_wm_base and wl_data_device_manager are the globals; a window will open, be "
				"placed and redraw against the frames that reach the glass, and there is no seat to route input and "
				"nothing behind the clipboard, so it will not respond to a click or a keystroke and cannot copy or "
				"paste"
			);

			author = std::move(*made);
		}
		else
		{
			return {};
		}

		if (const Result<void> ready = m_DispatchWait.Open(); !ready)
		{
			return ready;
		}

		// Beside the stop, so the thread wakes for a client with the same `ppoll` it wakes for a retry
		// deadline with. Borrowed: the descriptor is the event loop's and dies with the host.
		if (m_Clients != nullptr)
		{
			m_DispatchWait.Watch(m_Clients->PollFd());
		}

		if (const Result<void> opened = OpenInput(); !opened)
		{
			return opened;
		}

		std::array<SceneOutput, MaxOutputs> outputs{};

		if (const Result<void> laid = Layout(outputs); !laid)
		{
			return laid;
		}

		// Every importer that can be drawn through, gathered for the texture registry. An image a scene
		// can draw anywhere has to be adopted everywhere it might be drawn, and the root is the only
		// party holding the whole set.
		//
		// **Distinct importers, not one per output.** Where the two coincide — `Blit`, which is its own
		// importer — the loop is the same loop. Where they do not, adopting an id twice into one table
		// would make the second call a *replacing* adopt of the first, which retires the image the first
		// one just created and leaves the id naming a copy nobody needed.
		std::array<ITextureImporter*, MaxOutputs> importers{};
		std::size_t importing = 0;

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			ITextureImporter* const importer = m_Bound[index].Importer;

			if (importer == nullptr)
			{
				continue;
			}

			if (std::ranges::contains(std::span{ importers.data(), importing }, importer))
			{
				continue;
			}

			importers[importing] = importer;
			++importing;
		}

		m_Dispatch =
			std::make_unique<DispatchLoop>(m_Clock, m_Snapshots, m_Returns, std::span{ importers.data(), importing });

		// **Before `Open` and therefore before any client can have committed**, which is the only ordering
		// that has no window in it: the host reads its clients inside `Advance`, so a connection made
		// afterwards would be one a first frame could already have been owed against. Decision 115 puts
		// the drain in `Scene` and has `Protocol` observe what it derives, and this is the one line where
		// the two meet — the root is the only party that holds both.
		if (m_Clients != nullptr)
		{
			m_Clients->Observe(m_Dispatch->Return());
		}

		if (const Result<void> opened = m_Dispatch->Open(std::move(author), { outputs.data(), m_Count }); !opened)
		{
			return opened;
		}

		return {};
	}

	// Where the outputs sit in the space the world is laid out in.
	//
	// **Left to right in the order they were configured, edge to edge.** That is a placeholder with a
	// person behind it rather than an arbitrary choice: it is what somebody with two monitors on a desk
	// sees before they have said anything, and it is the arrangement a window dragged off the right edge
	// of one screen has to arrive on the next under. It becomes a real layout — read from configuration,
	// moved by a person dragging a monitor in a settings panel — when there is anything to read it from.
	// Decision 87 already puts the answer here, in the one place holding both the mode the backend agreed
	// to and the arrangement the world wants.
	//
	// **The order is load-bearing**, and `DispatchLoop::Open` says so from the other side: the snapshot's
	// per-output wake and placement runs are positional, so index `i` here is the frame loop's output `i`.
	[[nodiscard]] Result<void> Layout(std::span<SceneOutput> outputs)
	{
		double left = 0.0;

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			const OutputConfiguration& achieved = m_Bound[index].Configuration;

			// **One logical pixel per device pixel until a scale is negotiated.** No backend reports a
			// density and `--output` does not carry one, so a fraction invented here would be a layout
			// nobody asked for — and decision 54's settled snap is measured against it, which would put the
			// error on every animation rather than only on the arrangement.
			const Scale density = Scale::FromInteger(1);
			const double width =
				static_cast<double>(density.LogicalFromDevice(achieved.Resolution.Width, Rounding::Nearest));
			const double height =
				static_cast<double>(density.LogicalFromDevice(achieved.Resolution.Height, Rounding::Nearest));

			// Generational from the day there is one, rather than an index that a hotplug would hand to a
			// different monitor. Nothing reads it yet; what makes it worth minting now is that the
			// allocator is the thing hotplug releases into, and a second minting scheme written later is
			// one to reconcile.
			const std::optional<OutputId> id = m_OutputIds.Allocate();

			if (!id)
			{
				return Failure(ENOSPC, "more outputs than the world has identities for");
			}

			outputs[index] = SceneOutput{
				.Id = *id,
				.Generation = achieved.Generation,
				.Bounds = { { left, 0.0 }, { width, height } },
				.Density = density,
				.Grid = achieved.Resolution,
				.Orientation = Orientation(achieved.Transform),
			};

			left += width;
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
		bound.OnTargetsInvalidated.ConnectTo<&BoundOutput::Release>(bound.Presenter->TargetsInvalidated, bound);
		bound.OnReconfigured.ConnectTo<&BoundOutput::Adopt>(bound.Presenter->Reconfigured, bound);

		m_Outputs[index].Bind(*bound.Presenter, *bound.Renderer, 0, bound.Configuration, {}, plan.Budget());

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

			// **The frame thread's own refusals, said here because it may not say them itself.** Every
			// line above this one reports success for a run that drew nothing: the schedule admitted the
			// output, the loop assessed it, the walk cost what it cost, and then a renderer that cannot
			// express one item in the list refused the whole frame and the loop returned in silence. A
			// black window with a clean log is the worst shape a bug can take, because it sends the
			// reader to the wiring — the presenter, the ring, the gym — before the one place that knew.
			if (const std::optional<Error>& refusal = m_Outputs[index].FirstRefusal(); refusal)
			{
				spdlog::warn("             {} frame(s) refused, the first: {}", m_Outputs[index].Refused(), *refusal);
			}

			// After the refusals rather than before, because it is the cause and they are what a reader
			// actually saw. A renderer with nothing bound refuses every record it is handed, so this line
			// is what collapses the hundreds above it into one sentence.
			if (const std::optional<Error>& failure = m_Bound[index].BindFailure; failure)
			{
				spdlog::error("             the targets were never bound: {}", *failure);
			}
		}

		m_Backend->Report();

		if (m_Dispatch)
		{
			spdlog::info(
				"authored {} publication(s), {} deferred, {} frame report(s) collected",
				m_Dispatch->Publications(),
				m_Dispatch->Deferrals(),
				m_Dispatch->Reports()
			);

			// Deferrals are retained rather than lost, so this is not a correctness report — it is the
			// frame thread having been four publishes behind that many times, which is what
			// Docs/Open.md's publication-pacing question wants counted rather than argued.
			if (m_Dispatch->Deferrals() != 0)
			{
				spdlog::warn(
					"the ring was full on {} publish(es); the frame thread is behind", m_Dispatch->Deferrals()
				);
			}

			if (!m_DispatchResult)
			{
				spdlog::error("the world stopped being authored: {}", m_DispatchResult.error());
			}
		}

		if (m_Recorder && m_Recorder->IsRecording())
		{
			// **What the ring held rather than what it was sized for**, because the second is a constant
			// and the first is the answer to the only question a person asks of it: is this long enough
			// to still contain the thing I noticed. A run that overwrote records says so, since a ring
			// that lapped is one where `--trace-buffer` is the knob that was wanted.
			const std::uint64_t written = m_Recorder->Written();
			const std::uint64_t capacity = m_Recorder->Capacity();

			if (m_Traced)
			{
				spdlog::info(
					"traced {} event(s) covering {:.1f}s to {} ({} bytes)",
					m_Traced->Events,
					ToSeconds(m_Traced->Covered),
					m_Options.TracePath,
					m_Traced->Bytes
				);
			}
			else
			{
				spdlog::info(
					"recorded {} trace event(s) in {} slot(s); SIGUSR1 writes them to {}",
					written,
					capacity,
					m_Options.TracePath
				);
			}

			if (written > capacity)
			{
				spdlog::warn(
					"the trace ring lapped {} time(s); --trace-buffer is what buys more of the run", written / capacity
				);
			}

			if (const std::uint64_t snapshots = m_Recorder->Snapshots(); snapshots != 0 && !m_Traced)
			{
				spdlog::info("{} snapshot(s) were written on request", snapshots);
			}

			if (!m_Recorder->Outcome())
			{
				spdlog::error("a requested trace snapshot failed: {}", m_Recorder->Outcome().error());
			}
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

	// Decision 83's source: dispatch writes, the frame thread reads, and nothing is read back across it.
	// The descriptor says *something was published* and the ring says what.
	Interrupt m_Publication;

	FrameRing m_Ring;
	std::array<IEventSource*, 3> m_Sources{};

	std::unique_ptr<FrameLoop> m_Loop;

	// The other thread's half. Declared after everything it points into — the ring, the return channel,
	// the clock — so that it is destroyed before them, and after `m_Loop` so that the reader outlives
	// nothing the writer still owns.
	DispatchWait m_DispatchWait;

	// The client host, when this run has one, borrowed from the author the dispatch loop owns. Non-null
	// exactly when the author is a `ClientHost`, which is the one fact `ISceneAuthor` deliberately does
	// not carry — a gym would have to answer for a socket it does not have. Three jobs: the flush before
	// each sleep, the descriptor the wait was given, and the keys the chord did not take.
	ClientHost* m_Clients = nullptr;

	// The device set, the compositor's own keys, and the connection between them. Declared beside the
	// host rather than with the backend because both are the dispatch thread's, and destroyed before
	// the wait that borrows the descriptor.
	std::unique_ptr<Input::Devices> m_Input;
	Input::Chord m_Chord;
	Connection<const KeyEvent&> m_Key;
	bool m_InputFailed = false;

	// Whether this run drives a panel, which is the one condition under which gyro takes the machine's
	// input devices for itself. Set where the DRM backend is built.
	bool m_Panel = false;

	std::unique_ptr<DispatchLoop> m_Dispatch;

	// Whether the author owes anything further. Written by dispatch, read by the frame thread, and read
	// by nothing in the design — it exists for `--frames`' idle stop, which is the one place a run has to
	// distinguish a world at rest from a world between motions. Not a channel: it carries no state
	// anything renders from, and losing an update costs an iteration rather than a frame.
	std::atomic<bool> m_WorldSettled{ false };

	// The return leg's doorbell, as the two halves of one handshake.
	//
	// **`m_ClientsOwed` is dispatch saying a client is waiting on a frame that has not reached the glass
	// yet**, and `m_Shown` is the frame thread's count of the frames that have. The frame thread rings
	// `DispatchWait::Nudge` when it moves the second and finds the first set; the dispatch thread
	// declines to sleep when the second has moved since it began its step. Either half alone loses a
	// wakeup — the two variables are written on one thread and read on the other in the opposite order,
	// which is the one interleaving release and acquire do not cover — so both are sequentially
	// consistent, at a cost of one fence per frame on a path that is already doing a syscall.
	//
	// **Without them a settled world with a client in it is a deadlock and not a delay.** Dispatch arms
	// no deadline when nothing is owed, the return channel carries no descriptor, and the only thing
	// that would wake the thread is the client acting on the frame callback that is sitting undrained.
	std::atomic<bool> m_ClientsOwed{ false };
	std::atomic<std::uint64_t> m_Shown{ 0 };

	// One identity per output, minted where hotplug will release them.
	SlotAllocator<OutputTag> m_OutputIds{ MaxOutputs };

	bool m_RealTime = false;

	// **Declared after the clock it stamps with, and outliving both threads either way.** A ring is
	// reached through a thread-local pointer that nothing ever clears, so the storage has to outlive
	// every thread that might still write into it — which `Run` already guarantees by joining both
	// before it returns, and which this ordering keeps true if that ever stops being so.
	std::optional<Recorder> m_Recorder{};
	TraceBuffer* m_FrameTrace = nullptr;
	TraceBuffer* m_DispatchTrace = nullptr;
	std::optional<TraceSummary> m_Traced{};

	Result<void> m_Result{};
	Result<void> m_DispatchResult{};
	std::optional<Error> m_PriorityRefused{};
	std::uint64_t m_Iterations = 0;
	bool m_Idled = false;
};

} // namespace

Result<void> Run(const Options& options)
{
	// **The panel first, then the priority.** Both rules live in Options.h with the ordering between
	// them, because reading the backend before `Auto` has been settled makes gyro booting on a panel
	// with no arguments give up `SCHED_FIFO` — see ResolveOptions.
	const bool host = ::getenv("WAYLAND_DISPLAY") != nullptr || ::getenv("WAYLAND_SOCKET") != nullptr;
	const Options resolved = ResolveOptions(options, host);

	// Said out loud rather than silently, because somebody who typed nothing and got one of the two
	// should be able to find out why without reading this file. `WAYLAND_SOCKET` counts as well as
	// `WAYLAND_DISPLAY`, because a client launched by something that already opened the socket inherits
	// the descriptor rather than the path — which is how a sandboxed client reaches a compositor whose
	// socket it cannot open, and Wire's own connect path already prefers it.
	if (options.Backend == BackendKind::Auto)
	{
		spdlog::info(
			host ? "no backend selected and WAYLAND_DISPLAY is set; running nested" :
				   "no backend selected and there is no wayland host; driving the panel"
		);
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
