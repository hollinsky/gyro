// `setenv` and `mkdtemp` are POSIX rather than ISO C, and glibc gates both behind a feature-test
// macro. Named here for the reason Protocol/Server.Test.cpp names its own: what is wanted from the
// platform is stated rather than inherited.
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Core/Clock.h"
#include "Core/Fd.h"
#include "Core/Input.h"
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Core/Time.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Protocol/Host.h"
#include "Protocol/Tier.h"
#include "Publication/Return.h"
#include "Scene/Entity.h"
#include "Scene/Output.h"
#include "Scene/Return.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"
#include "Testing/Test.h"
#include "Wayland/ExtForeignToplevelListV1.h"
#include "Wayland/GyroBindingsV1.h"
#include "Wayland/GyroChromeV1.h"
#include "Wayland/LinuxDmabufV1.h"
#include "Wayland/PresentationTime.h"
#include "Wayland/Viewporter.h"
#include "Wayland/Wayland.h"
#include "Wayland/XdgShell.h"
#include "Wire/Connection.h"

// gyro's client talking to gyro's server, over a real socket.
//
// **This is the only place the two halves of decision 2's split meet**, and that is why it is here
// rather than beside either of them: the server codec is libwayland's with gyro's generated wrappers
// over it, the client codec is gyro's own `Wire`, and no module may name both. `Protocol` must never
// reach the client bindings — gyro's clients are not gyro — and `Nested` must never reach the server.
// Only something above both can put one in front of the other.
//
// **What it proves that neither side's own tests can** is that the bytes agree. Protocol/Surface.Test
// asserts what a handler does with a request; Integration/WaylandBindings.Test asserts what the
// generated client makes of bytes a test wrote. Neither notices if the two disagree about an argument
// order, because each is checked against the same emitter that produced the other. A real request
// crossing a real socket is checked against libwayland's own demarshaller in the middle, which is a
// third party to both.
//
// The negative cases matter as much as the positive one. A compositor that quietly accepts a buffer
// scale of zero divides by it later, on the frame thread, at whatever moment the client chose.

namespace
{
// A private XDG_RUNTIME_DIR, so the test binds a real socket under a directory it owns rather than
// the developer's live session — where `wayland-0` is a running compositor and colliding with it is
// the test's fault rather than a finding. `mkdtemp` makes it `0700`, which is the mode libwayland
// requires of a runtime directory and refuses without.
struct PrivateRuntimeDir
{
	PrivateRuntimeDir()
	{
		char pattern[] = "/tmp/gyro-roundtrip-XXXXXX";
		const char* const made = ::mkdtemp(pattern);

		if (made != nullptr)
		{
			Path = made;
			::setenv("XDG_RUNTIME_DIR", Path.c_str(), 1);
		}
	}

	std::string Path;
};

const PrivateRuntimeDir g_RuntimeDir;

// The texture space as a fake that mints real ids and remembers what it was handed. Real ids matter:
// a surface retires the one it replaces, and an id that was never valid would make the replacement
// indistinguishable from nothing having happened.
class CountingTextures final : public ITextures
{
public:
	using ITextures::Adopt;

	[[nodiscard]] Result<TextureId> Adopt(
		PixelSize<BufferSpace> size,
		std::uint32_t stride,
		std::span<const std::byte> pixels,
		TextureAlpha alpha
	) override
	{
		++Adopted;

		Size = size;
		Stride = stride;
		Alpha = alpha;
		First = pixels.empty() ? std::byte{} : pixels.front();
		Bytes = pixels.size();

		return TextureId{ Adopted, 1 };
	}

	[[nodiscard]] Result<TextureId> Adopt(
		PixelSize<BufferSpace> size,
		TextureFormat format,
		std::span<const TexturePlane> planes,
		ITextureRelease* release
	) override
	{
		++Adopted;

		Size = size;
		Described = format;
		Planes = planes.size();

		if (release != nullptr)
		{
			Owed.push_back(release);
		}

		return TextureId{ Adopted, 1 };
	}

	void Abandon(const ITextureRelease& release) noexcept override { std::erase(Owed, &release); }

	[[nodiscard]] std::span<const TextureFormat> Formats() const noexcept override { return Advertised; }

	// The watermark passed everything, which is what `TextureRegistry::Reclaim` does for real.
	void ReleaseAll() noexcept
	{
		const std::vector<ITextureRelease*> owed = std::move(Owed);

		Owed.clear();

		for (ITextureRelease* const release : owed)
		{
			release->OnTextureReleased();
		}
	}

	void Retire(TextureId id) noexcept override { Retired += id.IsNull() ? 0U : 1U; }

	std::vector<TextureFormat> Advertised;
	std::vector<ITextureRelease*> Owed;
	TextureFormat Described{};
	std::size_t Planes = 0;
	std::uint32_t Adopted = 0;
	std::uint32_t Retired = 0;
	PixelSize<BufferSpace> Size{};
	std::uint32_t Stride = 0;
	TextureAlpha Alpha = TextureAlpha::Premultiplied;
	std::byte First{};
	std::size_t Bytes = 0;
};

// A pool the way a toolkit makes one, filled with a byte a test can recognise on the far side.
//
// **`sealing` is which toolkit.** Mesa and libwayland ask for `MFD_ALLOW_SEALING` and GTK does not,
// and the difference is invisible to the client and decides which of gyro's two paths its pixels take
// — so it is a parameter here rather than two structs.
struct ClientPool
{
	explicit ClientPool(std::size_t size, bool sealing = true) : Size{ size }
	{
		const int descriptor =
			::memfd_create("gyro-roundtrip", sealing ? MFD_CLOEXEC | MFD_ALLOW_SEALING : MFD_CLOEXEC);

		if (descriptor < 0 || ::ftruncate(descriptor, static_cast<off_t>(size)) != 0)
		{
			return;
		}

		Descriptor = Fd{ descriptor };
	}

	void Fill(std::byte value) const
	{
		void* const writable = ::mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_SHARED, Descriptor.Borrow().Value, 0);

		if (writable != MAP_FAILED)
		{
			std::memset(writable, static_cast<int>(value), Size);
			::munmap(writable, Size);
		}
	}

	[[nodiscard]] Fd Take() { return std::move(Descriptor); }

	std::size_t Size = 0;
	Fd Descriptor;
};

// What the client hears about a buffer, which for `wl_buffer` is one event and it is the one that
// matters: whether it may draw into that memory again.
// The format list `wl_shm` owes a client the moment it binds.
class ShmFormats final : public Wayland::WlShmListener
{
public:
	void OnFormat(Wayland::WlShmFormat format) override { Seen.push_back(format); }

	std::vector<Wayland::WlShmFormat> Seen;
};

class BufferEvents final : public Wayland::WlBufferListener
{
public:
	void OnRelease() override { ++Released; }

	std::uint32_t Released = 0;
};

// What the registry announced, in the order it announced it.
class Registry final : public Wayland::WlRegistryListener
{
public:
	struct Global
	{
		std::uint32_t Name = 0;
		std::string Interface;
		std::uint32_t Version = 0;
	};

	void OnGlobal(std::uint32_t name, std::string_view interface, std::uint32_t version) override
	{
		Globals.push_back(Global{ .Name = name, .Interface = std::string{ interface }, .Version = version });
	}

	void OnGlobalRemove(std::uint32_t name) override { Removed.push_back(name); }

	[[nodiscard]] const Global* Find(std::string_view interface) const noexcept
	{
		for (const Global& global : Globals)
		{
			if (global.Interface == interface)
			{
				return &global;
			}
		}

		return nullptr;
	}

	std::vector<Global> Globals;
	std::vector<std::uint32_t> Removed;
};

// The server, the client, and the one verb that moves bytes between them.
//
// **Both directions in one call, and both non-blocking.** The composition root's real loop parks on a
// descriptor between these steps; a test has both ends in one thread, so the only ordering that has
// to be right is the one the root also honours — the host reads inside `Advance`, and the flush is
// the last thing before the thread would have slept.
struct Pair
{
	// `trust` is what the client on the far end is granted, and **which of the run's two sockets this
	// connects to is the whole of how it is granted**. Trust belongs to the listener (Protocol/Tier.h),
	// so there is no request a client sends to acquire it and no argument this test could pass: a
	// `User` client reaches the socket applications reach, and a `System` one reaches the socket
	// `Server::BindSystem` binds for a shell. Both are bound by the same `HostListener::Own` a bare
	// `gyro` runs under, so what these tests exercise is the configuration a shell will actually meet
	// rather than a listener the test built to stand in for one.
	explicit Pair(std::string_view socket, Trust trust = Trust::User)
	{
		Host = MakeClientHost(HostListener::Own, socket);

		if (!Host || *Host == nullptr)
		{
			return;
		}

		const std::string display =
			trust == Trust::System ? std::string{ (*Host)->SystemSocketName() } : std::string{ socket };

		// `Wire::Connection::Open` reads the environment, which is how every client finds a
		// compositor. Setting it here rather than passing a path is what keeps the test on the same
		// path a real client takes.
		::setenv("WAYLAND_DISPLAY", display.c_str(), 1);

		Opened = !display.empty() && (*Host)->Open(Store, Textures).has_value();

		Opened = Opened && Client.Open().has_value();

		// The return leg, wired the way the composition root wires it: before anything can have
		// committed, because a first frame owed against an unobserved drain is a callback nothing would
		// ever send.
		Returns.SetOutputs(1);
		(*Host)->Observe(Returns);
	}

	// The frame thread's half, with no frame thread: a report saying this sequence reached the glass on
	// the one output, delivered the way `Dispatch/Loop.h` delivers one. `Seal` first, because the loop
	// seals what the step's commits declared against the sequence about to carry them.
	// The panel's own account of the flip is defaulted away, because most of these cases are about
	// whether a client heard anything at all and a backend that measured nothing is a state gyro has to
	// work in. `wp_presentation` is where the three are given values.
	void
	Present(std::uint64_t sequence, Instant at, std::uint64_t vblank = 0, Duration period = {}, bool hardware = false)
	{
		Returns.Seal(sequence, Store);

		FrameReport report{};
		report.Watermark = sequence;
		report.OutputCount = 1;
		report.Presentations[0] = { .Sequence = sequence,
			                        .At = at,
			                        .Vblank = vblank,
			                        .Period = period,
			                        .Vsync = hardware,
			                        .HardwareClock = hardware };

		Returns.Drain(report);
	}

	// One turn of the loop: what the client has written is read and answered, and what the server owes
	// is written back and demarshalled.
	void Turn()
	{
		GYRO_CHECK(Client.Flush().has_value());

		(void)(*Host)->Advance(Store, Textures, Clock.Now());

		(*Host)->Flush();

		// `EPIPE` is the server having ended this client, which several cases below are about — it is
		// asserted on through `Fault()` rather than here, because the fault carries which object and
		// which code and this only carries that something went wrong.
		(void)Client.Drain();
	}

	ManualClock Clock{ Monotonic::FromNanoseconds(1'000'000'000) };
	SceneStore Store{ Clock };
	CountingTextures Textures;
	Result<std::unique_ptr<ClientHost>> Host;
	SceneReturn Returns;
	Wire::Connection Client;
	bool Opened = false;
};

// Everything a client needs before it can draw: the display, the registry, and both globals.
struct BoundCompositor
{
	Registry Listener;
	Wayland::WlCompositor Compositor;
	ShmFormats ShmEvents;
	Wayland::WlShm Shm;
	Wayland::XdgWmBaseIgnoring ShellEvents;
	Wayland::XdgWmBase Shell;
};

[[nodiscard]] bool Bind(Pair& pair, BoundCompositor& bound)
{
	const Wayland::WlDisplay display{ pair.Client, Wire::ObjectId::Display, Wayland::WlDisplay::WireVersion };

	(void)display.GetRegistry(bound.Listener);

	// **Twice, and the reason is libwayland's event loop rather than this test's impatience.** The
	// first dispatch accepts the connection and adds the client's descriptor as a source of its own;
	// a source added during a dispatch is not itself dispatched in that round, so the request already
	// sitting in the socket is read on the next one. The composition root never notices because its
	// wait goes readable again immediately, which is one more turn of a loop that is running anyway.
	pair.Turn();
	pair.Turn();

	const Registry::Global* const compositor = bound.Listener.Find(Wayland::WlCompositor::WireName);

	if (compositor == nullptr)
	{
		return false;
	}

	bound.Compositor = bound.Listener.Object().Bind<Wayland::WlCompositor>(compositor->Name, compositor->Version);

	const Registry::Global* const shm = bound.Listener.Find(Wayland::WlShm::WireName);

	if (shm == nullptr)
	{
		return false;
	}

	bound.Shm = bound.Listener.Object().Bind<Wayland::WlShm>(shm->Name, shm->Version, bound.ShmEvents);

	const Registry::Global* const shell = bound.Listener.Find(Wayland::XdgWmBase::WireName);

	if (shell == nullptr)
	{
		return false;
	}

	bound.Shell = bound.Listener.Object().Bind<Wayland::XdgWmBase>(shell->Name, shell->Version, bound.ShellEvents);

	return bound.Compositor.IsValid() && bound.Shm.IsValid() && bound.Shell.IsValid();
}
} // namespace

GYRO_TEST(ProtocolRoundTrip, TheRegistryCarriesTheGlobalsAToolkitLooksFor)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-registry" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	const Registry::Global* const compositor = bound.Listener.Find(Wayland::WlCompositor::WireName);
	GYRO_REQUIRE(compositor != nullptr);

	// The version is a promise about the events gyro sends, not a ceiling on what it can parse.
	// Version 6 obliges a `preferred_buffer_scale` per surface and there is no output model reaching
	// this module yet, so a client told 6 would draw at the wrong scale on a HiDPI panel and never be
	// corrected. See Protocol/Compositor.h.
	GYRO_CHECK_EQ(compositor->Version, std::uint32_t{ 5 });

	const Registry::Global* const shm = bound.Listener.Find(Wayland::WlShm::WireName);
	GYRO_REQUIRE(shm != nullptr);

	// Version 2 is `wl_shm.release`, which is a request rather than an event — so naming it owes the
	// client nothing that is not already implemented.
	GYRO_CHECK_EQ(shm->Version, std::uint32_t{ 2 });

	const Registry::Global* const shell = bound.Listener.Find(Wayland::XdgWmBase::WireName);
	GYRO_REQUIRE(shell != nullptr);

	// Version 4 for `wl_compositor`'s reason: it owes a `configure_bounds`, and that event is what stops
	// a toolkit sizing every window against `wl_output.scale` — which on a fractionally scaled panel is
	// the ceiling of the real scale and so a screen a third narrower than the one the world is on. 5 is
	// not taken: a client told 5 and sent no `wm_capabilities` is entitled to assume it has maximise,
	// minimise, fullscreen and the window menu, which is a titlebar of buttons that do nothing.
	GYRO_CHECK_EQ(shell->Version, std::uint32_t{ 4 });

	const Registry::Global* const data = bound.Listener.Find(Wayland::WlDataDeviceManager::WireName);
	GYRO_REQUIRE(data != nullptr);

	// The one global here that is not part of building a window: GTK will not open a display without
	// it and gives up before it looks for the other three. Version 3 is where every toolkit stops
	// asking, and no version of it promises more than another while there is no seat to reach a
	// selection or a drag through. See Protocol/Data.h.
	GYRO_CHECK_EQ(data->Version, std::uint32_t{ 3 });

	const Registry::Global* const seat = bound.Listener.Find(Wayland::WlSeat::WireName);
	GYRO_REQUIRE(seat != nullptr);

	// Version 5 is `wl_pointer.frame` and the three axis events beside it, which is the whole of what
	// gyro sends about a scroll. Not 8, which is the high-resolution wheel and would be a promise about
	// events this binary does not send. See Protocol/Seat.h.
	GYRO_CHECK_EQ(seat->Version, std::uint32_t{ 5 });

	const Registry::Global* const dmabuf = bound.Listener.Find(Wayland::ZwpLinuxDmabufV1::WireName);
	GYRO_REQUIRE(dmabuf != nullptr);

	// Version 3 is the last one whose contract is a list of format-modifier pairs. Four obliges
	// `get_default_feedback` to answer with a main device and tranches, and a client that asks and is
	// never answered waits on a roundtrip forever. See Protocol/Dmabuf.h.
	GYRO_CHECK_EQ(dmabuf->Version, std::uint32_t{ 3 });

	const Registry::Global* const subcompositor = bound.Listener.Find(Wayland::WlSubcompositor::WireName);
	GYRO_REQUIRE(subcompositor != nullptr);

	// One, because `wl_subcompositor` has never been revised.
	GYRO_CHECK_EQ(subcompositor->Version, std::uint32_t{ 1 });

	const Registry::Global* const viewporter = bound.Listener.Find(Wayland::WpViewporter::WireName);
	GYRO_REQUIRE(viewporter != nullptr);

	// One, for `wl_subcompositor`'s reason. Its absence is not a degradation: a toolkit that states its
	// surface size with a viewport destination and finds no viewporter states no size at all, and the
	// window arrives at its buffer's pixel count. See Protocol/Viewporter.h.
	GYRO_CHECK_EQ(viewporter->Version, std::uint32_t{ 1 });

	const Registry::Global* const presentation = bound.Listener.Find(Wayland::WpPresentation::WireName);
	GYRO_REQUIRE(presentation != nullptr);

	// One rather than two: they differ only in what `refresh` may say on an output with no constant
	// refresh rate, and gyro's world model carries the mode's nominal period and nothing else. See
	// Protocol/Presentation.h.
	GYRO_CHECK_EQ(presentation->Version, std::uint32_t{ 1 });

	// Exactly nine: three a window is built out of, one a toolkit demands before it will look for
	// them, the seat that makes the window typeable, the one that lets a client hand over a buffer a
	// panel can scan out instead of pixels gyro has to copy, the one that lets it say how the parts of
	// its own window are stacked, the one that lets it say how big any of them is, and the one that
	// tells it when what it drew was actually seen — which is also the one whose absence meant gyro
	// could not be run nested inside gyro.
	GYRO_CHECK_EQ(bound.Listener.Globals.size(), std::size_t{ 9 });
}

// What a GTK client actually does with the clipboard global before it has a seat, which is bind it,
// find it answers, and — for a client that owns something copyable — make a source nobody will ever
// ask for. Both have to work for an application to start; neither transfers anything.
//
// The transfer itself is the two tests below this one, which is where the selection landed.
GYRO_TEST(ProtocolRoundTrip, ADataSourceIsCreatedAndOffersMimeTypesNobodyWillAskFor)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-data" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	const Registry::Global* const data = bound.Listener.Find(Wayland::WlDataDeviceManager::WireName);
	GYRO_REQUIRE(data != nullptr);

	const Wayland::WlDataDeviceManager manager =
		bound.Listener.Object().Bind<Wayland::WlDataDeviceManager>(data->Name, data->Version);
	GYRO_REQUIRE(manager.IsValid());

	// A source is never selected, so none of these ever arrive. Counted anyway, because "nothing came
	// back" is the claim being made.
	class SourceEvents final : public Wayland::WlDataSourceIgnoring
	{
	public:
		void OnTarget(std::string_view) override { ++Events; }

		void OnSend(std::string_view, Fd) override { ++Events; }

		void OnCancelled() override { ++Events; }

		std::size_t Events = 0;
	};

	SourceEvents events;

	Wayland::WlDataSource source = manager.CreateDataSource(events);
	GYRO_REQUIRE(source.IsValid());

	source.Offer("text/plain;charset=utf-8");
	source.SetActions(Wayland::WlDataDeviceManagerDndAction::Copy);

	pair.Turn();

	// Still connected is the whole of it: a client that offered a MIME type and was ended for it is
	// one that never gets as far as drawing.
	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(events.Events, std::size_t{ 0 });

	source.Destroy();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());
}

GYRO_TEST(ProtocolRoundTrip, BindingWlShmAnnouncesTheFormatsBeforeAnythingIsAsked)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-formats" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	pair.Turn();

	// **The client asked nothing and is owed events anyway**, which is the whole of what `OnBound`
	// exists for: `wl_shm`'s contract begins with the compositor telling a client what it may draw in.
	// A toolkit that finds no `argb8888` in this list will not draw at all.
	GYRO_REQUIRE(bound.ShmEvents.Seen.size() == 2);
	GYRO_CHECK(bound.ShmEvents.Seen[0] == Wayland::WlShmFormat::Argb8888);
	GYRO_CHECK(bound.ShmEvents.Seen[1] == Wayland::WlShmFormat::Xrgb8888);
}

GYRO_TEST(ProtocolRoundTrip, ASurfaceAndARegionSurviveAWholeCommit)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-commit" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Wayland::WlSurfaceIgnoring events;
	const Wayland::WlSurface surface = bound.Compositor.CreateSurface(events);
	GYRO_REQUIRE(surface.IsValid());

	const Wayland::WlRegion region = bound.Compositor.CreateRegion();
	GYRO_REQUIRE(region.IsValid());

	region.Add(0, 0, 100, 100);
	region.Subtract(40, 40, 20, 20);

	surface.SetInputRegion(region);
	surface.SetOpaqueRegion(region);
	surface.SetBufferScale(2);
	surface.SetBufferTransform(Wayland::WlOutputTransform::_90);
	surface.Damage(0, 0, 50, 50);
	surface.DamageBuffer(0, 0, 100, 100);
	surface.Offset(3, 4);
	surface.Commit();

	pair.Turn();

	// Every one of those requests carries arguments that could be transposed by the emitter without
	// any structural check noticing. What says they were not is that libwayland demarshalled all of
	// them against the interface description and gyro's handlers accepted every one — a wrong
	// argument count or a wrong type is a protocol error and the connection would be gone.
	GYRO_CHECK(!pair.Client.Fault().has_value());

	// Two nodes, and both are gyro's own — the session's floor and the chrome root above it (187) —
	// rather than anything the client authored: a surface with no role is not a window, and nothing
	// about the requests above says it is one.
	GYRO_CHECK_EQ(pair.Store.Count(), std::uint32_t{ 2 });
	GYRO_CHECK_EQ(pair.Textures.Adopted, std::uint32_t{ 0 });
}

GYRO_TEST(ProtocolRoundTrip, AFrameCallbackIsAcceptedAndNotYetAnswered)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-frame" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Wayland::WlSurfaceIgnoring events;
	const Wayland::WlSurface surface = bound.Compositor.CreateSurface(events);
	GYRO_REQUIRE(surface.IsValid());

	class Callback final : public Wayland::WlCallbackListener
	{
	public:
		void OnDone(std::uint32_t) override { ++Fired; }

		std::uint32_t Fired = 0;
	} callback;

	(void)surface.Frame(callback);
	surface.Commit();

	pair.Turn();
	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// **Not fired, and it is the surface having no window rather than the callback being unanswered.**
	// A frame callback is answered when the content this surface committed reaches the glass, and a
	// surface with no role and no buffer is in nobody's scene — there is no entity for the return leg to
	// report against, so there is nothing to be waiting on. The mapped case is
	// `AMappedWindowIsToldWhenItsFrameReachedTheGlass` below.
	GYRO_CHECK_EQ(callback.Fired, std::uint32_t{ 0 });

	pair.Present(1, pair.Clock.Now());
	pair.Turn();

	GYRO_CHECK_EQ(callback.Fired, std::uint32_t{ 0 });
}

GYRO_TEST(ProtocolRoundTrip, AZeroBufferScaleEndsTheClient)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-scale" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Wayland::WlSurfaceIgnoring events;
	const Wayland::WlSurface surface = bound.Compositor.CreateSurface(events);
	GYRO_REQUIRE(surface.IsValid());

	surface.SetBufferScale(0);
	surface.Commit();

	pair.Turn();

	// A scale of zero is a division on the frame thread later, so it is refused where it arrives. The
	// client is told which object and which code, because a toolkit that gets `invalid_scale` can fix
	// itself and one that gets a dropped connection cannot.
	GYRO_REQUIRE(pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(pair.Client.Fault()->Code, static_cast<std::uint32_t>(Wayland::WlSurfaceError::InvalidScale));
}

GYRO_TEST(ProtocolRoundTrip, AnOffsetOnAttachEndsAModernClient)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-offset" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Wayland::WlSurfaceIgnoring events;
	const Wayland::WlSurface surface = bound.Compositor.CreateSurface(events);
	GYRO_REQUIRE(surface.IsValid());

	// Version 5 moved the offset off `attach` and onto its own request, and made sending it the old
	// way an error rather than a compatibility path — because a client that believes it moved its
	// buffer and a compositor that ignored it disagree about where the window is.
	surface.Attach(Wayland::WlBuffer{}, 3, 4);
	surface.Commit();

	pair.Turn();

	GYRO_REQUIRE(pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(pair.Client.Fault()->Code, static_cast<std::uint32_t>(Wayland::WlSurfaceError::InvalidOffset));
}

namespace
{
// A surface with one committed frame behind it, which is as far as a client can get before there is a
// role to say the surface is a window at all.
// A client's `wl_surface`, recording which outputs it has been told it is on. That pair is the whole
// of what a `wl_surface` receives below version 6, and it is how a toolkit learns what scale to draw
// at — so a surface that hears nothing lays out at 1x whatever the outputs said when it bound them.
class SurfaceEvents final : public Wayland::WlSurfaceIgnoring
{
public:
	void OnEnter(Wayland::WlOutput output) override { Entered.push_back(output); }

	void OnLeave(Wayland::WlOutput output) override { Left.push_back(output); }

	std::vector<Wayland::WlOutput> Entered;
	std::vector<Wayland::WlOutput> Left;
};

struct DrawnSurface
{
	SurfaceEvents Events;
	Wayland::WlSurface Surface;
	BufferEvents Released;
	Wayland::WlShmPool Pool;
	Wayland::WlBuffer Buffer;
};

// 16x8 at four bytes a pixel, which is small enough that a whole-buffer assertion is a number a
// reader can check by hand.
constexpr std::int32_t Width = 16;
constexpr std::int32_t Height = 8;
constexpr std::int32_t Stride = Width * 4;
constexpr std::size_t PoolBytes = static_cast<std::size_t>(Stride) * static_cast<std::size_t>(Height) * 2;

[[nodiscard]] bool Draw(BoundCompositor& bound, DrawnSurface& drawn, std::byte fill, bool sealing = true)
{
	ClientPool pool{ PoolBytes, sealing };

	if (!pool.Descriptor.IsValid())
	{
		return false;
	}

	pool.Fill(fill);

	drawn.Surface = bound.Compositor.CreateSurface(drawn.Events);
	drawn.Pool = bound.Shm.CreatePool(pool.Take(), static_cast<std::int32_t>(PoolBytes));

	if (!drawn.Surface.IsValid() || !drawn.Pool.IsValid())
	{
		return false;
	}

	drawn.Buffer = drawn.Pool.CreateBuffer(0, Width, Height, Stride, Wayland::WlShmFormat::Argb8888, drawn.Released);

	return drawn.Buffer.IsValid();
}
} // namespace

GYRO_TEST(ProtocolRoundTrip, AnAttachedBufferReachesTheTextureSpaceAndComesStraightBack)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-shm" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	DrawnSurface drawn;
	GYRO_REQUIRE(Draw(bound, drawn, std::byte{ 0xC3 }));

	drawn.Surface.Attach(drawn.Buffer, 0, 0);
	drawn.Surface.DamageBuffer(0, 0, Width, Height);
	drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// The pixels made it across a real socket, through a descriptor libwayland passed with
	// `SCM_RIGHTS`, into a mapping gyro sealed, and out the far side as an id. The byte is what says
	// the offset and stride arithmetic landed on the rows the client meant rather than on a page of
	// zeroes next to them.
	GYRO_CHECK_EQ(pair.Textures.Adopted, std::uint32_t{ 1 });
	GYRO_CHECK(pair.Textures.Size == (PixelSize<BufferSpace>{ Width, Height }));
	GYRO_CHECK_EQ(pair.Textures.Stride, static_cast<std::uint32_t>(Stride));
	GYRO_CHECK(pair.Textures.Alpha == TextureAlpha::Premultiplied);
	GYRO_CHECK_EQ(pair.Textures.Bytes, static_cast<std::size_t>(Stride) * static_cast<std::size_t>(Height));
	GYRO_CHECK(pair.Textures.First == std::byte{ 0xC3 });

	// **Released in the same step it was committed in**, which is the point of copying rather than
	// sampling the client's memory: a toolkit with one buffer can draw its next frame immediately,
	// instead of allocating a second one to have somewhere to draw while gyro finishes with the first.
	GYRO_CHECK_EQ(drawn.Released.Released, std::uint32_t{ 1 });
}

GYRO_TEST(ProtocolRoundTrip, APoolNoDescriptorCanSealStillReachesTheTextureSpace)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-unsealable" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	DrawnSurface drawn;
	GYRO_REQUIRE(Draw(bound, drawn, std::byte{ 0x9E }, false));

	drawn.Surface.Attach(drawn.Buffer, 0, 0);
	drawn.Surface.DamageBuffer(0, 0, Width, Height);
	drawn.Surface.Commit();

	pair.Turn();

	// **This is GTK, and gyro used to end the connection here.** A `memfd` made without
	// `MFD_ALLOW_SEALING` can never take `F_SEAL_SHRINK`, and refusing it meant a person launching
	// Firefox got a crash reporter rather than a window. The pixels come out of the descriptor with
	// `pread` instead of a mapping, and nothing above `Protocol/Shm.h` can tell which path they took —
	// which is what this test is for, since the unit tests can see the path and a client cannot.
	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(pair.Textures.Adopted, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(pair.Textures.Bytes, static_cast<std::size_t>(Stride) * static_cast<std::size_t>(Height));
	GYRO_CHECK(pair.Textures.First == std::byte{ 0x9E });
	GYRO_CHECK_EQ(drawn.Released.Released, std::uint32_t{ 1 });
}

GYRO_TEST(ProtocolRoundTrip, ACommitThatDidNotAttachKeepsTheContentItHad)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-keep" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	DrawnSurface drawn;
	GYRO_REQUIRE(Draw(bound, drawn, std::byte{ 0x40 }));

	drawn.Surface.Attach(drawn.Buffer, 0, 0);
	drawn.Surface.Commit();

	pair.Turn();

	GYRO_REQUIRE(pair.Textures.Adopted == 1);

	// A client that commits a new input region and nothing else must not lose its window, which is
	// what makes this gated on the attach rather than on there being a buffer.
	drawn.Surface.SetBufferScale(2);
	drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(pair.Textures.Adopted, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(pair.Textures.Retired, std::uint32_t{ 0 });
}

GYRO_TEST(ProtocolRoundTrip, EveryAttachedFrameIsANewIdAndRetiresTheOneItReplaces)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-redraw" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	DrawnSurface drawn;
	GYRO_REQUIRE(Draw(bound, drawn, std::byte{ 0x08 }));

	drawn.Surface.Attach(drawn.Buffer, 0, 0);
	drawn.Surface.Commit();

	pair.Turn();

	// The same `wl_buffer` again, which is what a toolkit that reuses one buffer does on every frame.
	drawn.Surface.Attach(drawn.Buffer, 0, 0);
	drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// **A new id rather than an overwrite of the old one.** The frame thread may still be recording
	// from a snapshot that names the previous id, so writing over its pixels would tear a window that
	// is on screen right now; the old id stays drawable and retires when the watermark says nothing
	// can still be reading it. Seam/Importer.h carries the argument.
	GYRO_CHECK_EQ(pair.Textures.Adopted, std::uint32_t{ 2 });
	GYRO_CHECK_EQ(pair.Textures.Retired, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(drawn.Released.Released, std::uint32_t{ 2 });
}

GYRO_TEST(ProtocolRoundTrip, ABufferDestroyedWhileStagedIsNotReleasedAfterwards)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-stale-attach" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	DrawnSurface drawn;
	GYRO_REQUIRE(Draw(bound, drawn, std::byte{ 0x41 }));

	BufferEvents secondReleased;
	const Wayland::WlBuffer second =
		drawn.Pool.CreateBuffer(Stride * Height, Width, Height, Stride, Wayland::WlShmFormat::Argb8888, secondReleased);

	GYRO_REQUIRE(second.IsValid());

	// **Staged and then destroyed, with no commit in between**, which is the whole of the case. The
	// attach is the only resource a surface keeps across a return to the event loop, and the protocol
	// lets a client destroy a buffer it has attached and not committed — a toolkit that reallocates
	// mid-drag does it on every size it passes through.
	drawn.Surface.Attach(drawn.Buffer, 0, 0);
	drawn.Buffer.Destroy();

	// **The region is the point of the test and not scenery.** libwayland frees the `wl_resource` on
	// destroy and the allocator hands the same block to the next object this client asks for, so the
	// stale pointer stops being null and starts naming a live object of another interface. A
	// `wl_buffer.release` sent through it reached a `wl_region`, whose interface has no events, and
	// took the compositor down — every client of every user with it.
	const Wayland::WlRegion recycled = bound.Compositor.CreateRegion();
	GYRO_REQUIRE(recycled.IsValid());

	// The superseding attach, which is what calls the release the buffer no longer exists to hear.
	drawn.Surface.Attach(second, 0, 0);
	drawn.Surface.DamageBuffer(0, 0, Width, Height);
	drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// The surviving buffer went through, which says the surface recovered rather than merely failing
	// to crash: one adoption, and the release for it back on the buffer that is still there.
	GYRO_CHECK_EQ(pair.Textures.Adopted, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(secondReleased.Released, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(drawn.Released.Released, std::uint32_t{ 0 });
}

GYRO_TEST(ProtocolRoundTrip, AttachingNothingTakesTheContentAway)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-detach" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	DrawnSurface drawn;
	GYRO_REQUIRE(Draw(bound, drawn, std::byte{ 0x99 }));

	drawn.Surface.Attach(drawn.Buffer, 0, 0);
	drawn.Surface.Commit();

	pair.Turn();

	GYRO_REQUIRE(pair.Textures.Adopted == 1);

	// Attaching a null buffer is how a client takes its window off the screen without destroying
	// anything, and it has to be told apart from a commit that simply did not attach.
	drawn.Surface.Attach(Wayland::WlBuffer{}, 0, 0);
	drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(pair.Textures.Adopted, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(pair.Textures.Retired, std::uint32_t{ 1 });
}

namespace
{
// What the compositor tells a client about its window. Both configures, kept in the order they
// arrived — `xdg_surface.configure` is the one that closes the sequence, and a client that acted on
// the toplevel's numbers before it would be acting on a proposal that was still being assembled.
class ShellEvents final : public Wayland::XdgSurfaceListener
{
public:
	void OnConfigure(std::uint32_t serial) override
	{
		Serial = serial;
		++Configured;
	}

	std::uint32_t Serial = 0;
	std::uint32_t Configured = 0;
};

class ToplevelEvents final : public Wayland::XdgToplevelIgnoring
{
public:
	void OnConfigure(std::int32_t width, std::int32_t height, std::span<const std::byte> states) override
	{
		Width = width;
		Height = height;
		++Configured;

		// Decoded rather than counted, because a test that knew only how many states arrived could not
		// tell `activated` from `maximized` — and the failure that matters is a window told it is one
		// thing when gyro meant another.
		States.clear();

		for (std::size_t at = 0; at + sizeof(std::uint32_t) <= states.size(); at += sizeof(std::uint32_t))
		{
			std::uint32_t value = 0;

			std::memcpy(&value, states.data() + at, sizeof(value));

			States.push_back(static_cast<Wayland::XdgToplevelState>(value));
		}
	}

	void OnClose() override { ++Closed; }

	[[nodiscard]] bool Has(Wayland::XdgToplevelState state) const noexcept
	{
		return std::find(States.begin(), States.end(), state) != States.end();
	}

	std::int32_t Width = -1;
	std::int32_t Height = -1;
	std::vector<Wayland::XdgToplevelState> States;
	std::uint32_t Configured = 0;
	std::uint32_t Closed = 0;
};

// A client taking a surface all the way to a window, one step at a time so a test can stop anywhere
// along the sequence.
struct Toplevel
{
	DrawnSurface Drawn;
	ShellEvents SurfaceEvents;
	ToplevelEvents WindowEvents;
	Wayland::XdgSurface XdgSurface;
	Wayland::XdgToplevel Window;
};

[[nodiscard]] bool Role(BoundCompositor& bound, Toplevel& toplevel, std::byte fill)
{
	if (!Draw(bound, toplevel.Drawn, fill))
	{
		return false;
	}

	toplevel.XdgSurface = bound.Shell.GetXdgSurface(toplevel.Drawn.Surface, toplevel.SurfaceEvents);

	if (!toplevel.XdgSurface.IsValid())
	{
		return false;
	}

	toplevel.Window = toplevel.XdgSurface.GetToplevel(toplevel.WindowEvents);

	return toplevel.Window.IsValid();
}

// The window gyro authored, found the way anything without an id would find it: the floor is the
// *first* root and a window is a child of it. The chrome root (187) is the last, and `ChromeNode`
// below is the same walk down the other one.
[[nodiscard]] const Entity* WindowNode(const SceneStore& scene)
{
	const Entity* const floor = scene.Find(scene.FirstRoot());

	return floor == nullptr ? nullptr : scene.Find(floor->FirstChild);
}

// A client's keyboard, recording what the seat told it. Everything here is what a toolkit would act
// on: the layout it maps, the focus it draws a caret for, and the keys it turns into characters.
class KeyboardEvents final : public Wayland::WlKeyboardListener
{
public:
	void OnKeymap(Wayland::WlKeyboardKeymapFormat format, Fd fd, std::uint32_t size) override
	{
		++Keymaps;

		Format = format;
		KeymapSize = size;
		KeymapFd = std::move(fd);
	}

	void OnEnter(std::uint32_t serial, Wayland::WlSurface surface, std::span<const std::byte> keys) override
	{
		++Entered;

		EnterSerial = serial;
		Focused = surface;
		HeldOnEnter = keys.size() / sizeof(std::uint32_t);
	}

	void OnLeave(std::uint32_t serial, Wayland::WlSurface surface) override
	{
		(void)serial;
		(void)surface;

		++Left;
	}

	void OnKey(std::uint32_t serial, std::uint32_t time, std::uint32_t key, Wayland::WlKeyboardKeyState state) override
	{
		(void)serial;

		++Keys;

		LastKey = key;
		LastState = state;
		LastTime = time;
	}

	void OnModifiers(
		std::uint32_t serial,
		std::uint32_t depressed,
		std::uint32_t latched,
		std::uint32_t locked,
		std::uint32_t group
	) override
	{
		(void)serial;
		(void)latched;
		(void)locked;
		(void)group;

		++Modifiers;

		Depressed = depressed;
	}

	void OnRepeatInfo(std::int32_t rate, std::int32_t delay) override
	{
		++Repeats;

		Rate = rate;
		Delay = delay;
	}

	std::uint32_t Keymaps = 0;
	std::uint32_t Entered = 0;
	std::uint32_t Left = 0;
	std::uint32_t Keys = 0;
	std::uint32_t Modifiers = 0;
	std::uint32_t Repeats = 0;

	Wayland::WlKeyboardKeymapFormat Format = Wayland::WlKeyboardKeymapFormat::NoKeymap;
	std::uint32_t KeymapSize = 0;
	Fd KeymapFd;

	std::uint32_t EnterSerial = 0;
	Wayland::WlSurface Focused;
	std::size_t HeldOnEnter = 0;

	std::uint32_t LastKey = 0;
	Wayland::WlKeyboardKeyState LastState = Wayland::WlKeyboardKeyState::Released;
	std::uint32_t LastTime = 0;
	std::uint32_t Depressed = 0;

	std::int32_t Rate = -1;
	std::int32_t Delay = -1;
};

class SeatEvents final : public Wayland::WlSeatListener
{
public:
	void OnCapabilities(Wayland::WlSeatCapability capabilities) override { Capabilities = capabilities; }

	void OnName(std::string_view name) override { Name = name; }

	Wayland::WlSeatCapability Capabilities{};
	std::string Name;
};

// A client with a keyboard, which is `wl_seat` and then `get_keyboard` — the two steps every toolkit
// takes and the only way to reach the interface at all.
struct Keyboard
{
	SeatEvents SeatListener;
	KeyboardEvents Listener;
	Wayland::WlSeat Seat;
	Wayland::WlKeyboard Device;
};

[[nodiscard]] bool Listen(Pair& pair, BoundCompositor& bound, Keyboard& keyboard)
{
	const Registry::Global* const seat = bound.Listener.Find(Wayland::WlSeat::WireName);

	if (seat == nullptr)
	{
		return false;
	}

	keyboard.Seat = bound.Listener.Object().Bind<Wayland::WlSeat>(seat->Name, seat->Version, keyboard.SeatListener);

	if (!keyboard.Seat.IsValid())
	{
		return false;
	}

	keyboard.Device = keyboard.Seat.GetKeyboard(keyboard.Listener);

	pair.Turn();

	return keyboard.Device.IsValid();
}

// One key, as the composition root hands it over: past the escape chord, with the device's own
// instant on it.
[[nodiscard]] KeyEvent Press(std::uint32_t code, bool pressed, Instant when)
{
	return KeyEvent{ .Code = code, .Pressed = pressed, .When = when };
}
} // namespace

GYRO_TEST(ProtocolRoundTrip, AToplevelIsConfiguredBeforeItIsAskedToDrawAnything)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-configure" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x20 }));

	// The empty commit is the client asking *what size should I be*, and it is the protocol's own
	// handshake rather than a courtesy: a toolkit will not draw a pixel until it has been answered.
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(toplevel.WindowEvents.Configured, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(toplevel.SurfaceEvents.Configured, std::uint32_t{ 1 });
	GYRO_CHECK(toplevel.SurfaceEvents.Serial != 0);

	// **Zero by zero and no states, which is gyro having no opinion.** A number here is one the client
	// must obey, so inventing one would be the compositor doing layout; an empty state list is a window
	// that is not maximised, not fullscreen, not being resized and not activated — all four of which
	// are true.
	GYRO_CHECK_EQ(toplevel.WindowEvents.Width, 0);
	GYRO_CHECK_EQ(toplevel.WindowEvents.Height, 0);
	GYRO_CHECK(toplevel.WindowEvents.States.empty());

	// Nothing is on screen: the client has been told it may draw and has not.
	GYRO_CHECK_EQ(pair.Store.Count(), std::uint32_t{ 2 });
}

// **A refusal is still an answer, and this is the test that says so.** gyro declines fullscreen and
// maximise because they are window management a shell owns (51) — but xdg-shell's own wording is that
// *the compositor will respond by emitting a configure event*, and only then that *whether the client
// is actually put into a fullscreen state is subject to compositor policies*. The policy is optional;
// the configure is not.
//
// What the silence cost was a browser whose fullscreen button did nothing: Firefox asks, waits for the
// configure that tells it what it got, and never completes the transition — so the page's own
// `fullscreenchange` never fires and a person clicking the control on a video sees nothing move.
GYRO_TEST(ProtocolRoundTrip, ADeclinedFullscreenIsStillAnsweredWithAConfigure)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-fullscreen" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x33 }));

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_REQUIRE_EQ(toplevel.WindowEvents.Configured, std::uint32_t{ 1 });

	const std::uint32_t opening = toplevel.SurfaceEvents.Serial;

	GYRO_REQUIRE(opening != 0);

	toplevel.Window.SetFullscreen(Wayland::WlOutput{});

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// The answer arrived, under its own serial: a client acknowledges what it was told rather than what
	// it asked, so a second question sharing the first one's serial is a client that cannot tell them
	// apart.
	GYRO_CHECK_EQ(toplevel.WindowEvents.Configured, std::uint32_t{ 2 });
	GYRO_CHECK_EQ(toplevel.SurfaceEvents.Configured, std::uint32_t{ 2 });
	GYRO_CHECK(toplevel.SurfaceEvents.Serial != opening);

	// And it says no. The state list is what the client reads to find out, and `fullscreen` is not in
	// it — which is the compositor declining out loud rather than by silence.
	GYRO_CHECK(!toplevel.WindowEvents.Has(Wayland::XdgToplevelState::Fullscreen));

	// Unsetting is answered too, which the protocol asks for in the same words and which a toolkit
	// leaving fullscreen waits on exactly as hard.
	toplevel.Window.UnsetFullscreen();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(toplevel.WindowEvents.Configured, std::uint32_t{ 3 });
}

// The same contract on the other pair of requests, which the protocol words identically.
GYRO_TEST(ProtocolRoundTrip, ADeclinedMaximiseIsStillAnsweredWithAConfigure)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-maximise" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x34 }));

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_REQUIRE_EQ(toplevel.WindowEvents.Configured, std::uint32_t{ 1 });

	toplevel.Window.SetMaximized();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(toplevel.WindowEvents.Configured, std::uint32_t{ 2 });
	GYRO_CHECK(!toplevel.WindowEvents.Has(Wayland::XdgToplevelState::Maximized));

	toplevel.Window.UnsetMaximized();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(toplevel.WindowEvents.Configured, std::uint32_t{ 3 });
}

// **The order Firefox actually opens a window in**, which is not the order the two tests above use: it
// asks to be maximised on the `xdg_toplevel` it has just made and only then commits the `wl_surface`
// for the first time. Both halves of what that used to hit are asserted here.
//
// What a person saw was Firefox failing to start at all — a window that never appeared and a browser
// that dumped a minidump and quit, saying gyro had sent it a serial gyro never sent. It had: the
// maximise was answered with the opening configure before the client had committed, which marked the
// surface configured, so the client's own first commit — bufferless, as the protocol requires — was
// read as a window being unmapped and threw the serial away underneath the acknowledgement already on
// the wire.
GYRO_TEST(ProtocolRoundTrip, AWindowThatAsksToBeMaximisedBeforeItsFirstCommitStillOpens)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-maximise-first" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x35 }));

	// Before the commit, which is the whole point: the protocol has the opening configure be the answer
	// to that commit, so there is nothing for gyro to say yet.
	toplevel.Window.SetMaximized();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(toplevel.WindowEvents.Configured, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(toplevel.SurfaceEvents.Configured, std::uint32_t{ 0 });

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	// One configure, and it is the opening one. The maximise is not lost — it is declined (51), and the
	// state list says so in the same event that answers the commit.
	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_REQUIRE_EQ(toplevel.WindowEvents.Configured, std::uint32_t{ 1 });
	GYRO_CHECK(!toplevel.WindowEvents.Has(Wayland::XdgToplevelState::Maximized));

	const std::uint32_t opening = toplevel.SurfaceEvents.Serial;

	GYRO_REQUIRE(opening != 0);

	// A second bufferless commit, which is what a toolkit does between the configure and its first frame
	// to land a geometry or an opaque region. It is not an unmap: there is no window to unmap.
	toplevel.XdgSurface.SetWindowGeometry(0, 0, 64, 64);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// And the acknowledgement of a configure gyro did send is accepted, rather than ending the client
	// for quoting a serial it was given.
	toplevel.XdgSurface.AckConfigure(opening);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// The window is on screen, which is the thing the person was waiting for.
	GYRO_CHECK(pair.Store.Count() > std::uint32_t{ 2 });
}

GYRO_TEST(ProtocolRoundTrip, AnAcknowledgedFrameBecomesAWindowCentredOnTheOutput)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-map" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x71 }));

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_REQUIRE(toplevel.SurfaceEvents.Serial != 0);

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.DamageBuffer(0, 0, Width, Height);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// The floor, the empty chrome root above it, the window, and the pixels under it. Two nodes per
	// window rather than one, which is a toplevel as decision 111 describes it: a container holding its
	// own surface and, one day, its subsurfaces.
	GYRO_CHECK_EQ(pair.Store.Count(), std::uint32_t{ 4 });
	GYRO_CHECK_EQ(pair.Textures.Adopted, std::uint32_t{ 1 });

	const Entity* const window = WindowNode(pair.Store);
	GYRO_REQUIRE(window != nullptr);

	// Centred, which with no shell and no pointer is the whole of gyro's placement policy — and it is
	// the one placement that takes no parameter, so there is no number here that somebody chose.
	GYRO_CHECK_EQ(window->Translation.Model().X, (1920.0 - static_cast<double>(Width)) / 2.0);
	GYRO_CHECK_EQ(window->Translation.Model().Y, (1080.0 - static_cast<double>(Height)) / 2.0);

	const Entity* const content = pair.Store.Find(window->FirstChild);
	GYRO_REQUIRE(content != nullptr);
	GYRO_CHECK(content->Kind == NodeKind::Image);
	GYRO_CHECK_EQ(content->Extent.Width, static_cast<float>(Width));
}

// **The bug this whole global exists for, end to end.** A client renders at twice the size and says so
// with a viewport destination rather than a buffer scale — which is what Firefox does for the
// subsurface its page is in, and what every toolkit does under a fractional scale, because
// `wl_surface.set_buffer_scale` cannot say 1.7. A compositor with no viewporter reads no size at all
// and the window arrives at its buffer's own pixel count: twice as wide and twice as tall as the
// person asked for, running off the edge of the screen.
//
// The buffer is not scaled here and that is deliberate: the destination has to be read on its own,
// because the failing client never sends a scale.
GYRO_TEST(ProtocolRoundTrip, AViewportDestinationIsTheSizeTheWindowArrivesAt)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-viewport" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	const Registry::Global* const advertised = bound.Listener.Find(Wayland::WpViewporter::WireName);
	GYRO_REQUIRE(advertised != nullptr);

	const Wayland::WpViewporter viewporter =
		bound.Listener.Object().Bind<Wayland::WpViewporter>(advertised->Name, advertised->Version);
	GYRO_REQUIRE(viewporter.IsValid());

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x71 }));

	const Wayland::WpViewport viewport = viewporter.GetViewport(toplevel.Drawn.Surface);
	GYRO_REQUIRE(viewport.IsValid());

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_REQUIRE(toplevel.SurfaceEvents.Serial != 0);

	viewport.SetDestination(Width / 2, Height / 2);

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.DamageBuffer(0, 0, Width, Height);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	const Entity* const window = WindowNode(pair.Store);
	GYRO_REQUIRE(window != nullptr);

	const Entity* const content = pair.Store.Find(window->FirstChild);
	GYRO_REQUIRE(content != nullptr);

	// Half the buffer in each axis, and the window around it the same: the destination is a resize of
	// the node rather than a factor carried beside its extent, so there is one answer to how big this
	// window is and every party downstream reads the same one.
	GYRO_CHECK_EQ(content->Extent.Width, static_cast<float>(Width) / 2.0F);
	GYRO_CHECK_EQ(content->Extent.Height, static_cast<float>(Height) / 2.0F);
	GYRO_CHECK_EQ(window->Extent.Width, static_cast<float>(Width) / 2.0F);

	// And it is placed against that size, which is the visible half of the bug: a window twice as big
	// as the compositor thinks is one whose centring is wrong by a quarter of a screen.
	GYRO_CHECK_EQ(window->Translation.Model().X, (1920.0 - static_cast<double>(Width) / 2.0) / 2.0);

	// The whole buffer is still what is sampled. A destination scales rather than crops, and a node
	// that reported fewer texels than it has would be one `Frame/Assign.h` believes is being resized.
	GYRO_REQUIRE(content->Content != NoContent);
	GYRO_CHECK_EQ(pair.Store.Images()[content->Content].Source.Extent.Width, static_cast<float>(Width));
}

GYRO_TEST(ProtocolRoundTrip, ASourceReachingPastTheBufferEndsTheClientAtTheCommit)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-viewsrc" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	const Registry::Global* const advertised = bound.Listener.Find(Wayland::WpViewporter::WireName);
	GYRO_REQUIRE(advertised != nullptr);

	const Wayland::WpViewporter viewporter =
		bound.Listener.Object().Bind<Wayland::WpViewporter>(advertised->Name, advertised->Version);
	GYRO_REQUIRE(viewporter.IsValid());

	DrawnSurface drawn;
	GYRO_REQUIRE(Draw(bound, drawn, std::byte{ 0x33 }));

	const Wayland::WpViewport viewport = viewporter.GetViewport(drawn.Surface);
	GYRO_REQUIRE(viewport.IsValid());

	// **Legal at the request and wrong only once there is a buffer to measure it against**, which is
	// why the protocol defers it and why gyro does: a client is entitled to describe the rectangle
	// before it attaches the buffer that holds it, and refusing at `set_source` would end a client for
	// a legal ordering.
	viewport.SetSource(
		Wire::Fixed::FromInt(0), Wire::Fixed::FromInt(0), Wire::Fixed::FromInt(Width * 2), Wire::Fixed::FromInt(Height)
	);

	pair.Turn();

	GYRO_REQUIRE(!pair.Client.Fault().has_value());

	drawn.Surface.Attach(drawn.Buffer, 0, 0);
	drawn.Surface.Commit();

	pair.Turn();

	GYRO_REQUIRE(pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(pair.Client.Fault()->Code, static_cast<std::uint32_t>(Wayland::WpViewportError::OutOfBuffer));
}

GYRO_TEST(ProtocolRoundTrip, AFractionalCropWithNothingToScaleItToEndsTheClient)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-viewcrop" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	const Registry::Global* const advertised = bound.Listener.Find(Wayland::WpViewporter::WireName);
	GYRO_REQUIRE(advertised != nullptr);

	const Wayland::WpViewporter viewporter =
		bound.Listener.Object().Bind<Wayland::WpViewporter>(advertised->Name, advertised->Version);
	GYRO_REQUIRE(viewporter.IsValid());

	DrawnSurface drawn;
	GYRO_REQUIRE(Draw(bound, drawn, std::byte{ 0x44 }));

	const Wayland::WpViewport viewport = viewporter.GetViewport(drawn.Surface);
	GYRO_REQUIRE(viewport.IsValid());

	// A crop with no destination makes the surface's size the source's, and a surface cannot be 8.5
	// pixels wide. The same rectangle with a destination beside it is legal, which is what makes this
	// the *pair* being wrong rather than the number.
	viewport.SetSource(
		Wire::Fixed::FromInt(0), Wire::Fixed::FromInt(0), Wire::Fixed::FromDouble(8.5), Wire::Fixed::FromInt(Height)
	);

	drawn.Surface.Attach(drawn.Buffer, 0, 0);
	drawn.Surface.Commit();

	pair.Turn();

	GYRO_REQUIRE(pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(pair.Client.Fault()->Code, static_cast<std::uint32_t>(Wayland::WpViewportError::BadSize));
}

namespace
{
// A client's `wl_output`, recording everything the bind burst carried.
class OutputEvents final : public Wayland::WlOutputListener
{
public:
	void OnGeometry(
		std::int32_t x,
		std::int32_t y,
		std::int32_t physicalWidth,
		std::int32_t physicalHeight,
		Wayland::WlOutputSubpixel subpixel,
		std::string_view make,
		std::string_view model,
		Wayland::WlOutputTransform transform
	) override
	{
		(void)subpixel;
		(void)make;
		(void)model;

		X = x;
		Y = y;
		PhysicalWidth = physicalWidth;
		PhysicalHeight = physicalHeight;
		Transform = transform;
	}

	void OnMode(Wayland::WlOutputMode flags, std::int32_t width, std::int32_t height, std::int32_t refresh) override
	{
		Flags = flags;
		Width = width;
		Height = height;
		Refresh = refresh;
	}

	void OnDone() override { ++Done; }

	void OnScale(std::int32_t factor) override { Factor = factor; }

	void OnName(std::string_view name) override { (void)name; }

	void OnDescription(std::string_view description) override { (void)description; }

	std::int32_t X = -1;
	std::int32_t Y = -1;
	std::int32_t PhysicalWidth = -1;
	std::int32_t PhysicalHeight = -1;
	std::int32_t Width = -1;
	std::int32_t Height = -1;
	std::int32_t Refresh = -1;
	std::int32_t Factor = -1;
	std::uint32_t Done = 0;
	Wayland::WlOutputMode Flags{};
	Wayland::WlOutputTransform Transform = Wayland::WlOutputTransform::Normal;
};
} // namespace

GYRO_TEST(ProtocolRoundTrip, AnOutputCarriesTheCeilingOfADerivedScale)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-output" };
	GYRO_REQUIRE(pair.Opened);

	// A 27-inch 4K panel at arm's length, which is what decision 164 derives 1.7x for and is the panel
	// fractional scaling exists for. There is no integer within the snapping band of it.
	const std::array outputs{ SceneOutput{ .Bounds = { { 0.0, 0.0 }, { 2259.0, 1271.0 } },
		                                   .Density = Scale::FromNumerator(204),
		                                   .Period = std::chrono::nanoseconds{ 6'944'444 },
		                                   .Grid = { 3840, 2160 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	const Registry::Global* const advertised = bound.Listener.Find(Wayland::WlOutput::WireName);
	GYRO_REQUIRE(advertised != nullptr);

	// Version 3 is `geometry`, `mode`, `done`, `scale` and `release`. Four would oblige a `name` and a
	// `description`, which are supposed to be the connector's and stop at Drm/Device.h — and an
	// invented name is worse than none, because it is what a client keys per-monitor state on.
	GYRO_CHECK_EQ(advertised->Version, std::uint32_t{ 3 });

	OutputEvents events;
	const Wayland::WlOutput output =
		bound.Listener.Object().Bind<Wayland::WlOutput>(advertised->Name, advertised->Version, events);
	GYRO_REQUIRE(output.IsValid());

	pair.Turn();

	// **Decision 56.** `wl_output.scale` is an integer and 1.7 is not, so the client is told 2, renders
	// at 2, and gyro downscales — the direction that degrades gracefully. A client told 1 would be
	// magnified onto the panel, which is the artefact this whole global exists to prevent.
	GYRO_CHECK_EQ(events.Factor, 2);

	// The mode is the panel's own grid rather than its logical size, which is what the protocol asks
	// for and the one number here the scale does not touch.
	GYRO_CHECK_EQ(events.Width, 3840);
	GYRO_CHECK_EQ(events.Height, 2160);
	GYRO_CHECK(Any(events.Flags & Wayland::WlOutputMode::Current));

	// 144 Hz, in the thousandths the protocol states a rate in, and rounded rather than truncated: the
	// period is 6'944'444 ns, which divides to 144'000.01 and reports a panel as 143.999 Hz to every
	// client that prints one if the remainder is dropped. It is the only cadence figure a client has
	// before it has drawn anything — `wp_presentation` answers after the first frame lands — and a zero
	// here is a media player falling back to 60.
	GYRO_CHECK_EQ(events.Refresh, 144'000);

	// The physical size is zero by zero, which is a statement rather than a gap: decision 164's whole
	// argument is that millimetres mean nothing until a viewing distance is applied, and that has
	// already happened by the time a scale reaches a client. Zero is what the protocol says to send for
	// a physical size that does not make sense.
	GYRO_CHECK_EQ(events.PhysicalWidth, 0);
	GYRO_CHECK_EQ(events.PhysicalHeight, 0);

	// One `done`, which is what makes the group atomic: a client applies nothing it has read until that
	// arrives, so a scale and the position it belongs with are never read half apart.
	GYRO_CHECK_EQ(events.Done, std::uint32_t{ 1 });
}

GYRO_TEST(ProtocolRoundTrip, AMappedWindowIsToldWhichOutputItIsOn)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-enter" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(2), .Grid = { 3840, 2160 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	const Registry::Global* const advertised = bound.Listener.Find(Wayland::WlOutput::WireName);
	GYRO_REQUIRE(advertised != nullptr);

	OutputEvents events;
	const Wayland::WlOutput output =
		bound.Listener.Object().Bind<Wayland::WlOutput>(advertised->Name, advertised->Version, events);
	GYRO_REQUIRE(output.IsValid());

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x52 }));

	toplevel.Drawn.Surface.Commit();
	pair.Turn();

	GYRO_REQUIRE(toplevel.SurfaceEvents.Serial != 0);

	// Nothing yet: a surface with no window in the world is on no output, and it is the map rather than
	// the bind that puts it on one.
	GYRO_CHECK(toplevel.Drawn.Events.Entered.empty());

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.DamageBuffer(0, 0, Width, Height);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// **This is the event that carries the scale to the client.** A toolkit takes the largest scale
	// among the outputs it has entered and redraws at it; one that is never entered stays at 1x however
	// many outputs it bound, which on a 2x panel is every window on the machine drawn at a quarter of
	// the area and magnified.
	GYRO_REQUIRE_EQ(toplevel.Drawn.Events.Entered.size(), std::size_t{ 1 });
	GYRO_CHECK(toplevel.Drawn.Events.Entered.front().Id() == output.Id());
	GYRO_CHECK(toplevel.Drawn.Events.Left.empty());

	// And once, rather than once per iteration: the comparison is against what the client has been told
	// rather than against nothing.
	pair.Turn();

	GYRO_CHECK_EQ(toplevel.Drawn.Events.Entered.size(), std::size_t{ 1 });
}

GYRO_TEST(ProtocolRoundTrip, AMappedWindowIsToldWhenItsFrameReachedTheGlass)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-presented" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x44 }));

	toplevel.Drawn.Surface.Commit();
	pair.Turn();

	GYRO_REQUIRE(toplevel.SurfaceEvents.Serial != 0);

	class Callback final : public Wayland::WlCallbackListener
	{
	public:
		void OnDone(std::uint32_t stamp) override
		{
			++Fired;
			Stamp = stamp;
		}

		std::uint32_t Fired = 0;
		std::uint32_t Stamp = 0;
	} first;

	// The whole of what a toolkit does per frame: ask to be told when this one lands, then hand over the
	// pixels for it.
	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	(void)toplevel.Drawn.Surface.Frame(first);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.DamageBuffer(0, 0, Width, Height);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	// Nothing yet: the commit has reached the world, and the world has not reached a panel.
	GYRO_CHECK_EQ(first.Fired, std::uint32_t{ 0 });

	const Instant shown = Advanced(pair.Clock.Now(), std::chrono::milliseconds{ 8 });

	pair.Present(1, shown);
	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// **This is the frame the window was waiting for, and it is what makes an application redraw.** The
	// timestamp is the instant the frame reached the glass rather than the moment gyro got round to
	// saying so, because a toolkit paces itself by differencing two of these.
	GYRO_REQUIRE_EQ(first.Fired, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(first.Stamp, static_cast<std::uint32_t>(Monotonic::ToNanoseconds(shown) / 1'000'000));

	// And the next frame is not answered on the strength of the last one. A callback that fired again
	// unasked would be a client drawing faster than it committed.
	pair.Present(2, Advanced(shown, std::chrono::milliseconds{ 16 }));
	pair.Turn();

	GYRO_CHECK_EQ(first.Fired, std::uint32_t{ 1 });

	// A second frame, asked for the same way, is answered the same way — which is the loop an animating
	// application actually runs in.
	Callback second;

	(void)toplevel.Drawn.Surface.Frame(second);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();
	pair.Present(3, Advanced(shown, std::chrono::milliseconds{ 32 }));
	pair.Turn();

	GYRO_CHECK_EQ(second.Fired, std::uint32_t{ 1 });
}

namespace
{
// A client's whole view of one content update.
class Feedback final : public Wayland::WpPresentationFeedbackListener
{
public:
	void OnSyncOutput(Wayland::WlOutput output) override { SyncedTo = output.Id(); }

	void OnPresented(
		std::uint32_t tvSecHi,
		std::uint32_t tvSecLo,
		std::uint32_t tvNsec,
		std::uint32_t refresh,
		std::uint32_t seqHi,
		std::uint32_t seqLo,
		Wayland::WpPresentationFeedbackKind flags
	) override
	{
		++Presented;
		Nanoseconds =
			static_cast<std::int64_t>(((static_cast<std::uint64_t>(tvSecHi) << 32U) | tvSecLo) * 1'000'000'000U) +
			tvNsec;
		Refresh = refresh;
		Vblank = (static_cast<std::uint64_t>(seqHi) << 32U) | seqLo;
		Flags = flags;
	}

	void OnDiscarded() override { ++Discarded; }

	std::uint32_t Presented = 0;
	std::uint32_t Discarded = 0;
	std::int64_t Nanoseconds = 0;
	std::uint32_t Refresh = 0;
	std::uint64_t Vblank = 0;
	Wayland::WpPresentationFeedbackKind Flags{};
	Wire::ObjectId SyncedTo{};
};

// `wp_presentation` itself, which says one thing and says it at bind.
class PresentationEvents final : public Wayland::WpPresentationListener
{
public:
	void OnClockId(std::uint32_t clkId) override { Clock = clkId; }

	// A sentinel no `clockid_t` uses, so *not said yet* is distinguishable from `CLOCK_REALTIME`.
	std::uint32_t Clock = 0xffffffffU;
};

[[nodiscard]] bool Has(Wayland::WpPresentationFeedbackKind flags, Wayland::WpPresentationFeedbackKind wanted) noexcept
{
	return (static_cast<std::uint32_t>(flags) & static_cast<std::uint32_t>(wanted)) != 0;
}
} // namespace

// **What a frame callback cannot say.** `wl_callback.done` carries truncated milliseconds and answers
// *you may draw again*; this answers *what you drew was seen, at this nanosecond, on that panel, and
// here is how much the number is worth*. The difference is what a media player matches audio against
// and what gyro's own nested backend builds its frame clock out of — `Nested/Host.h` requires this
// global of whatever it is a client of, which is why gyro could not be run inside gyro without it.
GYRO_TEST(ProtocolRoundTrip, AWindowLearnsWhenItsPixelsBecameLightAndOnWhichPanel)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-presentation" };
	GYRO_REQUIRE(pair.Opened);

	// 144 Hz, so the refresh figure that comes back is one no fallback would have produced.
	const std::array outputs{ SceneOutput{ .Bounds = { {}, { 1920.0, 1080.0 } },
		                                   .Density = Scale::FromInteger(1),
		                                   .Period = std::chrono::nanoseconds{ 6'944'444 },
		                                   .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	const Registry::Global* const advertised = bound.Listener.Find(Wayland::WpPresentation::WireName);
	GYRO_REQUIRE(advertised != nullptr);

	PresentationEvents events;
	const Wayland::WpPresentation presentation =
		bound.Listener.Object().Bind<Wayland::WpPresentation>(advertised->Name, advertised->Version, events);
	GYRO_REQUIRE(presentation.IsValid());

	// The client also binds the output, because `sync_output` names a resource rather than a panel and
	// is simply not sent to a client that never bound the global it would have named.
	const Registry::Global* const panel = bound.Listener.Find(Wayland::WlOutput::WireName);
	GYRO_REQUIRE(panel != nullptr);

	OutputEvents ignored;
	const Wayland::WlOutput output =
		bound.Listener.Object().Bind<Wayland::WlOutput>(panel->Name, panel->Version, ignored);
	GYRO_REQUIRE(output.IsValid());

	pair.Turn();

	// **The clock is named before anything is asked**, and it is `CLOCK_MONOTONIC` because that is
	// gyro's timebase throughout. A client must be able to read the same clock itself, which is what
	// rules out a compositor-private domain.
	GYRO_CHECK_EQ(events.Clock, static_cast<std::uint32_t>(CLOCK_MONOTONIC));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x76 }));

	toplevel.Drawn.Surface.Commit();
	pair.Turn();

	GYRO_REQUIRE(toplevel.SurfaceEvents.Serial != 0);

	Feedback feedback;

	// Asked for before the pixels it is about, exactly as a frame callback is.
	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	(void)presentation.Feedback(toplevel.Drawn.Surface, feedback);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.DamageBuffer(0, 0, Width, Height);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	// The commit has reached the world and the world has not reached a panel.
	GYRO_CHECK_EQ(feedback.Presented, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(feedback.Discarded, std::uint32_t{ 0 });

	const Instant shown = Advanced(pair.Clock.Now(), std::chrono::milliseconds{ 8 });

	// A flip the backend measured properly: a retrace counter, a period, and a timestamp it says came
	// from the display hardware.
	pair.Present(1, shown, 4210, std::chrono::nanoseconds{ 6'944'444 }, true);
	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	GYRO_REQUIRE_EQ(feedback.Presented, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(feedback.Discarded, std::uint32_t{ 0 });

	// **Nanoseconds, unrounded**, which is the whole reason a client asks for one of these: the frame
	// callback's millisecond cannot express a refresh boundary on a panel whose refresh is seven of
	// them.
	GYRO_CHECK_EQ(feedback.Nanoseconds, Monotonic::ToNanoseconds(shown));

	// The panel's own counter, not gyro's published sequence — which was 1 for this same frame. The two
	// count different things, and a client differencing them to find a dropped refresh would be reading
	// how often gyro published rather than how often the screen refreshed.
	GYRO_CHECK_EQ(feedback.Vblank, std::uint64_t{ 4210 });

	// What the backend measured, in nanoseconds until the next refresh.
	GYRO_CHECK_EQ(feedback.Refresh, std::uint32_t{ 6'944'444 });

	// The honesty half, forwarded from the backend rather than assumed. `hw_completion` is deliberately
	// absent: nothing gyro reads answers that question separately from `hw_clock`, and claiming it on
	// the strength of another flag is the fabrication `Seam/PresentationInfo.h` exists to prevent.
	GYRO_CHECK(Has(feedback.Flags, Wayland::WpPresentationFeedbackKind::Vsync));
	GYRO_CHECK(Has(feedback.Flags, Wayland::WpPresentationFeedbackKind::HwClock));
	GYRO_CHECK(!Has(feedback.Flags, Wayland::WpPresentationFeedbackKind::HwCompletion));

	// And which panel it was, as the object this client bound.
	GYRO_CHECK(feedback.SyncedTo == output.Id());
}

// **A backend that measured no period still owes the client a prediction**, and gyro has one: the mode
// it programmed. Zero is the protocol's *no useful prediction*, and sending it where the mode is known
// would have a client fall back to guessing at a number gyro is holding.
GYRO_TEST(ProtocolRoundTrip, AnUnmeasuredFlipFallsBackToTheModeTheOutputIsRunning)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-refresh" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{ .Bounds = { {}, { 1920.0, 1080.0 } },
		                                   .Density = Scale::FromInteger(1),
		                                   .Period = std::chrono::nanoseconds{ 16'666'666 },
		                                   .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	const Registry::Global* const advertised = bound.Listener.Find(Wayland::WpPresentation::WireName);
	GYRO_REQUIRE(advertised != nullptr);

	PresentationEvents events;
	const Wayland::WpPresentation presentation =
		bound.Listener.Object().Bind<Wayland::WpPresentation>(advertised->Name, advertised->Version, events);
	GYRO_REQUIRE(presentation.IsValid());

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x77 }));

	toplevel.Drawn.Surface.Commit();
	pair.Turn();

	GYRO_REQUIRE(toplevel.SurfaceEvents.Serial != 0);

	Feedback feedback;

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	(void)presentation.Feedback(toplevel.Drawn.Surface, feedback);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	// A flip with nothing measured about it, which is the headless presenter and every backend that
	// cannot get a hardware timestamp.
	pair.Present(1, pair.Clock.Now());
	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_REQUIRE_EQ(feedback.Presented, std::uint32_t{ 1 });

	GYRO_CHECK_EQ(feedback.Refresh, std::uint32_t{ 16'666'666 });

	// **And the flags say the number is not a measurement.** A client told the timestamp came from
	// display hardware would treat it as exact, which on this path it is not.
	GYRO_CHECK(!Has(feedback.Flags, Wayland::WpPresentationFeedbackKind::HwClock));

	// The retrace counter is genuinely absent, and zero is what the protocol says to send for an output
	// with no way to report one. Substituting the published sequence would be a plausible-looking number
	// counting something else.
	GYRO_CHECK_EQ(feedback.Vblank, std::uint64_t{ 0 });
}

// **The one thing this protocol must never do is timestamp a frame nobody saw.** A client that commits
// twice before the panel scans has pixels from the first update that were superseded in the world and
// never turned into light — and telling it otherwise is a media player measuring the latency of a frame
// it did not display. `wl_surface.frame` behaves the opposite way in the same situation, because *you
// may draw again* survives being asked twice, and that difference is deliberate on both sides.
GYRO_TEST(ProtocolRoundTrip, AContentUpdateNothingShowedIsDiscardedRatherThanTimestamped)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-discarded" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	const Registry::Global* const advertised = bound.Listener.Find(Wayland::WpPresentation::WireName);
	GYRO_REQUIRE(advertised != nullptr);

	PresentationEvents events;
	const Wayland::WpPresentation presentation =
		bound.Listener.Object().Bind<Wayland::WpPresentation>(advertised->Name, advertised->Version, events);
	GYRO_REQUIRE(presentation.IsValid());

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x78 }));

	toplevel.Drawn.Surface.Commit();
	pair.Turn();

	GYRO_REQUIRE(toplevel.SurfaceEvents.Serial != 0);

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);

	class Callback final : public Wayland::WlCallbackListener
	{
	public:
		void OnDone(std::uint32_t) override { ++Fired; }

		std::uint32_t Fired = 0;
	} first;
	Callback second;

	Feedback superseded;
	Feedback shown;

	// Two content updates inside one refresh, each asking for both events.
	(void)presentation.Feedback(toplevel.Drawn.Surface, superseded);
	(void)toplevel.Drawn.Surface.Frame(first);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	(void)presentation.Feedback(toplevel.Drawn.Surface, shown);
	(void)toplevel.Drawn.Surface.Frame(second);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();
	pair.Present(1, pair.Clock.Now(), 9, std::chrono::nanoseconds{ 16'666'666 }, true);
	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// The frame the panel actually scanned is the second, and it is the only one timestamped.
	GYRO_CHECK_EQ(shown.Presented, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(shown.Discarded, std::uint32_t{ 0 });

	GYRO_CHECK_EQ(superseded.Presented, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(superseded.Discarded, std::uint32_t{ 1 });

	// **Both callbacks fire, and that is not an inconsistency.** They answer a different question, and a
	// client owed two *draw again*s that received one would be a window drawing at half the rate it
	// asked for.
	GYRO_CHECK_EQ(first.Fired, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(second.Fired, std::uint32_t{ 1 });
}

GYRO_TEST(ProtocolRoundTrip, AWindowThatUnmappedIsNotToldAboutFramesItIsNotIn)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-unmapped" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x55 }));

	toplevel.Drawn.Surface.Commit();
	pair.Turn();

	GYRO_REQUIRE(toplevel.SurfaceEvents.Serial != 0);

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	class Callback final : public Wayland::WlCallbackListener
	{
	public:
		void OnDone(std::uint32_t) override { ++Fired; }

		std::uint32_t Fired = 0;
	} callback;

	// Attaching nothing takes the window down, and a frame callback asked for in the same commit is one
	// nothing will ever show. The window's route back to this client is gone with it, which is what
	// keeps a report against a retiring node — still drawn, because its exit is running (114) — from
	// reaching a client that has taken its window away.
	(void)toplevel.Drawn.Surface.Frame(callback);
	toplevel.Drawn.Surface.Attach(Wayland::WlBuffer{}, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();
	pair.Present(1, pair.Clock.Now());
	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(callback.Fired, std::uint32_t{ 0 });
}

GYRO_TEST(ProtocolRoundTrip, ABufferCommittedBeforeTheConfigureIsAcknowledgedEndsTheClient)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-unconfigured" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x33 }));

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	// The ack is deliberately skipped. A compositor that accepted this would be showing a frame drawn
	// for a size nobody agreed on, which is a window that appears at the wrong shape and then jumps.
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_REQUIRE(pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(pair.Client.Fault()->Code, static_cast<std::uint32_t>(Wayland::XdgSurfaceError::UnconfiguredBuffer));

	GYRO_CHECK_EQ(pair.Store.Count(), std::uint32_t{ 2 });
}

GYRO_TEST(ProtocolRoundTrip, DestroyingTheToplevelRetiresTheWindowRatherThanRemovingIt)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-unmap" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x55 }));

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_REQUIRE(pair.Store.Count() == 4);

	toplevel.Window.Destroy();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// **Still four, and still where it was.** A window that is closing is a window a person is still
	// looking at, so the subtree keeps its links and its position and goes on being drawn until every
	// channel on it has settled; the store frees it on the serialisation pass that finds it at rest.
	// Freeing here instead is a window that vanishes rather than one that leaves.
	GYRO_CHECK_EQ(pair.Store.Count(), std::uint32_t{ 4 });

	const Entity* const window = WindowNode(pair.Store);
	GYRO_REQUIRE(window != nullptr);
	GYRO_CHECK(window->Retiring);
}

// The whole of what a person does with a keyboard, in the order it happens: bind a seat, be told
// there is one keyboard on it, receive the layout, open a window, and type into it.
GYRO_TEST(ProtocolRoundTrip, ASeatOffersOneKeyboardAndHandsOverALayoutBeforeAnythingIsTyped)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-seat" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	// All three, and nothing beyond them. A client told there is a capability gyro has not built would
	// wait for an event that cannot come, so the set is exactly what is routed.
	GYRO_CHECK(
		keyboard.SeatListener.Capabilities ==
		(Wayland::WlSeatCapability::Keyboard | Wayland::WlSeatCapability::Pointer | Wayland::WlSeatCapability::Touch)
	);
	GYRO_CHECK(keyboard.SeatListener.Name == "seat0");

	// **The layout arrives without being asked for and before any key does**, which is the contract:
	// a client that has not mapped the keymap cannot turn a keycode into a character, so a `key` ahead
	// of it would be one it has to drop.
	GYRO_CHECK_EQ(keyboard.Listener.Keymaps, std::uint32_t{ 1 });
	GYRO_CHECK(keyboard.Listener.Format == Wayland::WlKeyboardKeymapFormat::XkbV1);
	GYRO_CHECK(keyboard.Listener.KeymapSize > 0);
	GYRO_CHECK(keyboard.Listener.KeymapFd.IsValid());
	GYRO_CHECK_EQ(keyboard.Listener.Keys, std::uint32_t{ 0 });

	// Repeat is the client's, and these two numbers are the whole of gyro's part in it.
	GYRO_CHECK_EQ(keyboard.Listener.Repeats, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(keyboard.Listener.Rate, 25);
	GYRO_CHECK_EQ(keyboard.Listener.Delay, 400);
}

// The descriptor is the layout, and it is one nothing can edit. A client maps it read-only and hands
// the bytes to xkbcommon; a client that maps it writable and scribbles would otherwise be rewriting
// the keyboard of every other application on the machine.
GYRO_TEST(ProtocolRoundTrip, TheKeymapDescriptorIsATextKeymapAndCannotBeWritten)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-keymap" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));
	GYRO_REQUIRE(keyboard.Listener.KeymapFd.IsValid());

	void* const mapped = ::mmap(
		nullptr, keyboard.Listener.KeymapSize, PROT_READ, MAP_PRIVATE, keyboard.Listener.KeymapFd.Borrow().Value, 0
	);
	GYRO_REQUIRE(mapped != MAP_FAILED);

	// What xkbcommon parses, recognisable from its first line without linking it.
	const std::string_view text{ static_cast<const char*>(mapped), keyboard.Listener.KeymapSize };
	GYRO_CHECK(text.find("xkb_keymap") != std::string_view::npos);

	// Null-terminated, and the size includes the terminator: the client is handed a C string rather
	// than a length to trust.
	GYRO_CHECK_EQ(text.back(), '\0');

	static_cast<void>(::munmap(mapped, keyboard.Listener.KeymapSize));

	// Sealed, so the descriptor that left this process cannot grow, shrink or be written through.
	GYRO_CHECK(::ftruncate(keyboard.Listener.KeymapFd.Borrow().Value, 0) != 0);
}

// The clipboard, end to end, over a real socket.
//
// **This is the one part of the protocol whose whole content is a file descriptor crossing twice**, so
// nothing either side asserts on its own can say it works: the source's pipe, the receiver's pipe and
// gyro's own are three descriptors that have to line up, and a compositor that got the direction wrong
// would look correct in every handler test and paste nothing.

namespace
{
// A client offering something, which is asked for the bytes twice — once by gyro, keeping its own
// copy, and once by whoever pastes.
class CopyingSource final : public Wayland::WlDataSourceIgnoring
{
public:
	explicit CopyingSource(std::string text) noexcept : m_Text{ std::move(text) } {}

	void OnSend(std::string_view mimeType, Fd fd) override
	{
		Asked.emplace_back(mimeType);

		// What every toolkit does with this event, and the descriptor is the client's to close.
		const ssize_t written = ::write(fd.Get(), m_Text.data(), m_Text.size());

		static_cast<void>(written);
	}

	void OnCancelled() override { ++Cancelled; }

	std::vector<std::string> Asked;
	std::uint32_t Cancelled = 0;

private:
	std::string m_Text;
};

// What a client is told is on the clipboard.
class Clipboard final : public Wayland::WlDataDeviceListener
{
public:
	Wayland::WlDataOfferListener* OnDataOffer(Wayland::WlDataOffer id) override
	{
		Offers.push_back(std::make_unique<OfferEvents>());
		Made = id;

		return Offers.back().get();
	}

	void OnEnter(std::uint32_t, Wayland::WlSurface, Wire::Fixed, Wire::Fixed, Wayland::WlDataOffer) override {}

	void OnLeave() override {}

	void OnMotion(std::uint32_t, Wire::Fixed, Wire::Fixed) override {}

	void OnDrop() override {}

	void OnSelection(Wayland::WlDataOffer id) override
	{
		++Selections;
		Selected = id;
	}

	// The types the last offer carried.
	class OfferEvents final : public Wayland::WlDataOfferIgnoring
	{
	public:
		void OnOffer(std::string_view mimeType) override { Mimes.emplace_back(mimeType); }

		std::vector<std::string> Mimes;
	};

	[[nodiscard]] bool Lists(std::string_view mime) const
	{
		return !Offers.empty() &&
		       std::find(Offers.back()->Mimes.begin(), Offers.back()->Mimes.end(), mime) != Offers.back()->Mimes.end();
	}

	std::vector<std::unique_ptr<OfferEvents>> Offers;
	Wayland::WlDataOffer Made;
	Wayland::WlDataOffer Selected;
	std::uint32_t Selections = 0;
};

// Ask an offer for its bytes and read what comes back, turning the loop as many times as it takes for
// the descriptor to have been written into.
[[nodiscard]] std::string Paste(Pair& pair, const Wayland::WlDataOffer& offer, std::string_view mime)
{
	std::array<int, 2> ends{ -1, -1 };

	if (::pipe(ends.data()) < 0)
	{
		return {};
	}

	const Fd read{ ends[0] };

	offer.Receive(mime, Fd{ ends[1] });

	// Two turns rather than one: the request has to reach gyro, and where the selection is still a live
	// client's the `send` then has to reach *it* before anything is written.
	pair.Turn();
	pair.Turn();

	std::string pasted;
	std::array<char, 256> chunk{};

	for (;;)
	{
		const ssize_t got = ::read(read.Get(), chunk.data(), chunk.size());

		if (got <= 0)
		{
			break;
		}

		pasted.append(chunk.data(), static_cast<std::size_t>(got));
	}

	return pasted;
}

// A client with a window, the keyboard, and a data device: everything a person needs to copy.
struct Copier
{
	Toplevel Window;
	Clipboard Listener;
	Wayland::WlDataDeviceManager Manager;
	Wayland::WlDataDevice Device;
};

[[nodiscard]] bool Open(Pair& pair, BoundCompositor& bound, const Keyboard& keyboard, Copier& copier)
{
	const Registry::Global* const data = bound.Listener.Find(Wayland::WlDataDeviceManager::WireName);

	if (data == nullptr)
	{
		return false;
	}

	copier.Manager = bound.Listener.Object().Bind<Wayland::WlDataDeviceManager>(data->Name, data->Version);

	if (!copier.Manager.IsValid())
	{
		return false;
	}

	copier.Device = copier.Manager.GetDataDevice(keyboard.Seat, copier.Listener);

	if (!copier.Device.IsValid() || !Role(bound, copier.Window, std::byte{ 0x40 }))
	{
		return false;
	}

	copier.Window.Drawn.Surface.Commit();

	pair.Turn();

	copier.Window.XdgSurface.AckConfigure(copier.Window.SurfaceEvents.Serial);
	copier.Window.Drawn.Surface.Attach(copier.Window.Drawn.Buffer, 0, 0);
	copier.Window.Drawn.Surface.Commit();

	pair.Turn();

	return true;
}
} // namespace

GYRO_TEST(ProtocolRoundTrip, ACopyReachesTheWindowThatHasTheKeyboard)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-copy" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Copier copier;
	GYRO_REQUIRE(Open(pair, bound, keyboard, copier));

	// A window with no selection anywhere is told so, which is what greys out Paste rather than leaving
	// a menu entry that does nothing.
	GYRO_CHECK_EQ(copier.Listener.Selections, std::uint32_t{ 1 });
	GYRO_CHECK(!copier.Listener.Selected.IsValid());

	CopyingSource source{ "the line a person copied" };
	Wayland::WlDataSource offering = copier.Manager.CreateDataSource(source);
	GYRO_REQUIRE(offering.IsValid());

	offering.Offer("text/plain;charset=utf-8");
	offering.Offer("text/html");

	copier.Device.SetSelection(offering, keyboard.Listener.EnterSerial);

	pair.Turn();

	// **The copy arrives in the wakeup it was made in**, rather than waiting for focus to move: a
	// person who copies and pastes inside one window is the ordinary case, and the alternative is a
	// paste that offers what was on the clipboard before.
	GYRO_CHECK_EQ(copier.Listener.Selections, std::uint32_t{ 2 });
	GYRO_REQUIRE(copier.Listener.Selected.IsValid());

	// The source's own list, verbatim, because the application offering it is still running.
	GYRO_CHECK(copier.Listener.Lists("text/plain;charset=utf-8"));
	GYRO_CHECK(copier.Listener.Lists("text/html"));

	// **The paste goes through the application rather than through gyro**, which is what `send`
	// arriving proves: the descriptor the receiver made is the one the source writes into.
	GYRO_CHECK(Paste(pair, copier.Listener.Selected, "text/plain;charset=utf-8") == "the line a person copied");

	GYRO_CHECK(!pair.Client.Fault().has_value());
}

GYRO_TEST(ProtocolRoundTrip, TheClipboardOutlivesTheApplicationThatFilledIt)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-clipboard" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Copier copier;
	GYRO_REQUIRE(Open(pair, bound, keyboard, copier));

	CopyingSource source{ "the line a person copied" };
	Wayland::WlDataSource offering = copier.Manager.CreateDataSource(source);
	GYRO_REQUIRE(offering.IsValid());

	offering.Offer("text/plain;charset=utf-8");
	offering.Offer("image/png");

	copier.Device.SetSelection(offering, keyboard.Listener.EnterSerial);

	// **gyro asks for the text itself, once, at the moment of the copy** — which is the whole
	// departure, and it is visible from the client's side as a `send` nobody asked for.
	pair.Turn();
	pair.Turn();

	GYRO_REQUIRE(source.Asked.size() == 1);
	GYRO_CHECK(source.Asked[0] == "text/plain;charset=utf-8");

	// And only the text: an image is left where it is, because generating one at every Ctrl+C is work
	// the application does for a paste that mostly never comes.
	GYRO_CHECK(std::find(source.Asked.begin(), source.Asked.end(), "image/png") == source.Asked.end());

	const Wayland::WlDataOffer live = copier.Listener.Selected;
	GYRO_REQUIRE(live.IsValid());

	// The application closes, which on every other compositor is the moment the clipboard empties.
	offering.Destroy();

	pair.Turn();

	// **It still pastes.** Nobody was asked this time — there is nobody left to ask — and the bytes are
	// the ones gyro kept.
	const std::size_t asked = source.Asked.size();

	GYRO_CHECK(Paste(pair, live, "text/plain;charset=utf-8") == "the line a person copied");
	GYRO_CHECK_EQ(source.Asked.size(), asked);

	// Under a name the application never offered, because what was kept is UTF-8 and every other text
	// name is that same byte sequence under a different label.
	GYRO_CHECK(Paste(pair, live, "text/plain") == "the line a person copied");

	// And the image is honestly gone rather than offered and empty.
	GYRO_CHECK(Paste(pair, live, "image/png").empty());

	GYRO_CHECK(!pair.Client.Fault().has_value());
}

GYRO_TEST(ProtocolRoundTrip, AWindowThatOpensTakesFocusAndTheKeysFollowIt)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-typing" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	// Nothing is focused before there is a window, and a key typed at that moment reaches nobody
	// rather than the last thing that happened to be there.
	(*pair.Host)->OnKey(Press(30, true, pair.Clock.Now()), false);
	(*pair.Host)->OnKey(Press(30, false, pair.Clock.Now()), false);

	pair.Turn();

	GYRO_CHECK_EQ(keyboard.Listener.Entered, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(keyboard.Listener.Keys, std::uint32_t{ 0 });

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x40 }));

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	// **Focus in the wakeup the window opened in**, which is what the seat comparing after the
	// dispatch buys: the commit that mapped it and the event that says so are one turn, so a person
	// who starts typing the instant a window appears is typing into it.
	GYRO_CHECK_EQ(keyboard.Listener.Entered, std::uint32_t{ 1 });
	GYRO_CHECK(keyboard.Listener.Focused.Id() == toplevel.Drawn.Surface.Id());
	GYRO_CHECK_EQ(keyboard.Listener.HeldOnEnter, std::size_t{ 0 });

	(*pair.Host)->OnKey(Press(30, true, pair.Clock.Now()), false);

	pair.Turn();

	GYRO_CHECK_EQ(keyboard.Listener.Keys, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(keyboard.Listener.LastKey, std::uint32_t{ 30 });
	GYRO_CHECK(keyboard.Listener.LastState == Wayland::WlKeyboardKeyState::Pressed);

	// The kernel's numbering all the way to the client, which is what makes the eight XKB adds the
	// client's own business.
	(*pair.Host)->OnKey(Press(30, false, pair.Clock.Now()), false);

	pair.Turn();

	GYRO_CHECK_EQ(keyboard.Listener.Keys, std::uint32_t{ 2 });
	GYRO_CHECK(keyboard.Listener.LastState == Wayland::WlKeyboardKeyState::Released);
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// A key the compositor took for itself, which is the escape chord: the client hears nothing, and the
// modifier state is still the truth about what a person is holding.
GYRO_TEST(ProtocolRoundTrip, AKeyTheCompositorTookNeverReachesTheWindow)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-chord" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x41 }));

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_REQUIRE(keyboard.Listener.Entered == 1);

	// Left Ctrl held: never consumed, because a person is holding it before anything knows a chord is
	// coming, and a client told the Escape vanished but not the Ctrl can reconcile that state.
	(*pair.Host)->OnKey(Press(29, true, pair.Clock.Now()), false);

	// And the verb, which gyro takes.
	(*pair.Host)->OnKey(Press(1, true, pair.Clock.Now()), true);
	(*pair.Host)->OnKey(Press(1, false, pair.Clock.Now()), true);

	pair.Turn();

	// The modifier and nothing else.
	GYRO_CHECK_EQ(keyboard.Listener.Keys, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(keyboard.Listener.LastKey, std::uint32_t{ 29 });

	// And the state says the modifier is down, which is what the next window to take focus is told.
	GYRO_CHECK(keyboard.Listener.Depressed != 0);
}

// A person holding a key while a window opens under it. The keys travel with the `enter`, because the
// release is the client's to see and it has no way to know about a press it was never told about.
GYRO_TEST(ProtocolRoundTrip, AWindowThatTakesFocusIsToldWhatIsAlreadyHeldDown)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-held" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	(*pair.Host)->OnKey(Press(42, true, pair.Clock.Now()), false);

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x42 }));

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK_EQ(keyboard.Listener.Entered, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(keyboard.Listener.HeldOnEnter, std::size_t{ 1 });
}

// Closing a window takes focus off it. The entity is still in the tree — decision 114 keeps it there
// while its exit runs — so this is the one assertion that says the keystrokes stop before the pixels
// do.
GYRO_TEST(ProtocolRoundTrip, AWindowThatClosedStopsReceivingKeys)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-closed" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x43 }));

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_REQUIRE(keyboard.Listener.Entered == 1);

	toplevel.Window.Destroy();

	pair.Turn();

	const Entity* const window = WindowNode(pair.Store);
	GYRO_REQUIRE(window != nullptr);
	GYRO_CHECK(window->Retiring);

	(*pair.Host)->OnKey(Press(30, true, pair.Clock.Now()), false);

	pair.Turn();

	GYRO_CHECK_EQ(keyboard.Listener.Keys, std::uint32_t{ 0 });
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// Asking a keyboard-only seat for a pointer. The protocol has a name for it, and a client that is
// told which one can fix itself where one handed an inert object waits for an `enter` forever.
namespace
{
// A client's pointer, recording what the seat told it. Everything here is what a toolkit acts on: what
// it is over, where on it, and the frame that says a group of them was one physical event.
class PointerEvents final : public Wayland::WlPointerIgnoring
{
public:
	void OnEnter(std::uint32_t serial, Wayland::WlSurface surface, Wire::Fixed x, Wire::Fixed y) override
	{
		++Entered;
		EnterSerial = serial;
		On = surface;
		X = x;
		Y = y;
	}

	void OnLeave(std::uint32_t, Wayland::WlSurface surface) override
	{
		++Left;
		LeftSurface = surface;
		On = {};
	}

	void OnMotion(std::uint32_t time, Wire::Fixed x, Wire::Fixed y) override
	{
		++Motions;
		At = time;
		X = x;
		Y = y;
	}

	void
	OnButton(std::uint32_t serial, std::uint32_t, std::uint32_t button, Wayland::WlPointerButtonState state) override
	{
		++Buttons;
		ButtonSerial = serial;
		LastButton = button;
		LastState = state;
	}

	void OnAxis(std::uint32_t, Wayland::WlPointerAxis axis, Wire::Fixed value) override
	{
		++Axes;
		LastAxis = axis;
		LastValue = value;
	}

	void OnFrame() override { ++Frames; }

	void OnAxisSource(Wayland::WlPointerAxisSource source) override
	{
		++Sources;
		LastSource = source;
	}

	void OnAxisStop(std::uint32_t, Wayland::WlPointerAxis) override { ++Stops; }

	void OnAxisDiscrete(Wayland::WlPointerAxis, std::int32_t discrete) override
	{
		++Discretes;
		LastDiscrete = discrete;
	}

	std::uint32_t Entered = 0;
	std::uint32_t Left = 0;
	std::uint32_t Motions = 0;
	std::uint32_t Buttons = 0;
	std::uint32_t Axes = 0;
	std::uint32_t Frames = 0;
	std::uint32_t Sources = 0;
	std::uint32_t Stops = 0;
	std::uint32_t Discretes = 0;
	std::uint32_t EnterSerial = 0;

	// The serial the press arrived with, which is the whole of what a client has to quote back to ask
	// for a move or a resize. A toolkit keeps exactly this.
	std::uint32_t ButtonSerial = 0;
	std::uint32_t At = 0;
	std::int32_t LastDiscrete = 0;
	std::uint32_t LastButton = 0;
	Wayland::WlPointerButtonState LastState{};
	Wayland::WlPointerAxis LastAxis{};
	Wayland::WlPointerAxisSource LastSource{};
	Wire::Fixed LastValue;
	Wire::Fixed X;
	Wire::Fixed Y;
	Wayland::WlSurface On;
	Wayland::WlSurface LeftSurface;
};

struct Pointer
{
	PointerEvents Listener;
	Wayland::WlPointer Device;
};

class TouchEvents final : public Wayland::WlTouchIgnoring
{
public:
	void OnDown(
		std::uint32_t serial,
		std::uint32_t,
		Wayland::WlSurface surface,
		std::int32_t id,
		Wire::Fixed x,
		Wire::Fixed y
	) override
	{
		++Downs;
		DownSerial = serial;
		On = surface;
		Id = id;
		X = x;
		Y = y;
	}

	void OnUp(std::uint32_t, std::uint32_t, std::int32_t id) override
	{
		++Ups;
		Id = id;
	}

	void OnMotion(std::uint32_t, std::int32_t id, Wire::Fixed x, Wire::Fixed y) override
	{
		++Motions;
		Id = id;
		X = x;
		Y = y;
	}

	void OnFrame() override { ++Frames; }

	void OnCancel() override { ++Cancels; }

	std::uint32_t Downs = 0;
	std::uint32_t Ups = 0;
	std::uint32_t Motions = 0;
	std::uint32_t Frames = 0;
	std::uint32_t Cancels = 0;
	std::uint32_t DownSerial = 0;
	std::int32_t Id = -1;
	Wire::Fixed X;
	Wire::Fixed Y;
	Wayland::WlSurface On;
};

struct Touch
{
	TouchEvents Listener;
	Wayland::WlTouch Device;
};

[[nodiscard]] bool Feel(Pair& pair, Keyboard& keyboard, Touch& touch)
{
	touch.Device = keyboard.Seat.GetTouch(touch.Listener);

	pair.Turn();

	return touch.Device.IsValid();
}

// One touchscreen, which is every machine this is about. The id is generational for the reason two of
// them would make matter — `Core/Input.h` — and nothing here needs a second one.
constexpr InputDeviceId Digitizer{ 0, 1 };

// A contact, at a place on the screen. The two arguments are decision 167's split: the phase and the
// slot are the device's, and the point is what the composition root resolved the device's own fraction
// to. A test stands where the root stands and hands over both.
void Finger(Pair& pair, TouchPhase phase, std::int32_t slot, Point<GlobalSpace> at)
{
	(*pair.Host)
		->OnTouch(TouchEvent{ .Point = slot, .Phase = phase, .When = pair.Clock.Now(), .Device = Digitizer }, at);
}

[[nodiscard]] bool Grip(Pair& pair, Keyboard& keyboard, Pointer& pointer)
{
	pointer.Device = keyboard.Seat.GetPointer(pointer.Listener);

	pair.Turn();

	return pointer.Device.IsValid();
}

// A window on screen with the pointer somewhere definite, which is the state every case below starts
// from: one panel, one mapped toplevel, and the pointer parked away from it.
[[nodiscard]] bool Show(Pair& pair, BoundCompositor& bound, Toplevel& toplevel, std::byte fill)
{
	if (!Role(bound, toplevel, fill))
	{
		return false;
	}

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	return true;
}

// The `activated` state, from the far end of the socket: a window that has the keyboard is told so,
// and the one it took the keyboard from is told it lost it.
//
// **This is what a person reads as a window being live**, and it is the whole of what a toolkit needs
// to draw a titlebar in colour rather than grey. Without it every window on the machine looks
// unfocused while a person types into one of them, which is not a subtle artefact — GTK draws the
// entire header bar in the backdrop style.
//
// The count is asserted alongside, because the failure that would hide here is a configure per
// dispatch iteration: the state would be right, every window on the machine would be asked to redraw
// on every wakeup, and nothing on screen would say so.
GYRO_TEST(ProtocolRoundTrip, AWindowIsToldItHasTheKeyboardAndTheOneItTookItFromIsToldItHasNot)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-activated" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel first;
	GYRO_REQUIRE(Show(pair, bound, first, std::byte{ 0x40 }));

	// Two configures: the `0x0` that answered *what size should I be*, carrying no states because the
	// window did not exist yet, and the one the mapping produced. They are one turn apart rather than
	// one frame — the window maps and is told it is focused inside the same `Advance`.
	GYRO_CHECK_EQ(first.WindowEvents.Configured, std::uint32_t{ 2 });
	GYRO_CHECK(first.WindowEvents.Has(Wayland::XdgToplevelState::Activated));

	// Nothing changed, so nothing is sent. A window that is being redrawn is not a window that is being
	// reconfigured.
	pair.Turn();

	GYRO_CHECK_EQ(first.WindowEvents.Configured, std::uint32_t{ 2 });

	Toplevel second;
	GYRO_REQUIRE(Show(pair, bound, second, std::byte{ 0x55 }));

	// Newest on top is `Scene/Focus.h`'s rule and this is a client reading it: the window that just
	// opened has the keyboard, and the one behind it is told in the same turn.
	GYRO_CHECK(second.WindowEvents.Has(Wayland::XdgToplevelState::Activated));
	GYRO_CHECK(!first.WindowEvents.Has(Wayland::XdgToplevelState::Activated));
	GYRO_CHECK_EQ(first.WindowEvents.Configured, std::uint32_t{ 3 });

	// And back, by the route that has nothing to do with the pointer: focus put where a shell or an
	// alt-tab would put it.
	const Entity* const floor = pair.Store.Find(pair.Store.FirstRoot());
	GYRO_REQUIRE(floor != nullptr);

	// The floor's first child is the window that opened first, since the store appends and the sibling
	// list is the z order (55).
	GYRO_REQUIRE(pair.Store.Focus().Focus(floor->FirstChild));

	pair.Turn();

	GYRO_CHECK(first.WindowEvents.Has(Wayland::XdgToplevelState::Activated));
	GYRO_CHECK(!second.WindowEvents.Has(Wayland::XdgToplevelState::Activated));
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// `Alt+Tab` end to end: the keys the root read, through the walk, out as the events a toolkit lights a
// titlebar from. The chord itself is `Input/Chord.Test.cpp`'s and the walk is `Scene/Focus.Test.cpp`'s;
// what is only visible here is that a client hears about the window a person landed on and not about
// every window they passed through on the way.
GYRO_TEST(ProtocolRoundTrip, CyclingFocusLightsTheWindowItLandsOnAndRaisesIt)
{
	Pair pair{ "gyro-roundtrip-cycle" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel first;
	GYRO_REQUIRE(Show(pair, bound, first, std::byte{ 0x40 }));

	Toplevel second;
	GYRO_REQUIRE(Show(pair, bound, second, std::byte{ 0x55 }));

	GYRO_REQUIRE(second.WindowEvents.Has(Wayland::XdgToplevelState::Activated));

	const std::uint32_t configured = first.WindowEvents.Configured;

	// One step and the hand coming off `Alt`, both inside one turn — which is what a person's hand
	// produces on a compositor that wakes for every key.
	(*pair.Host)->OnFocusCycle(FocusCycle::Next);
	(*pair.Host)->OnFocusCycle(FocusCycle::End);

	pair.Turn();

	GYRO_CHECK(first.WindowEvents.Has(Wayland::XdgToplevelState::Activated));
	GYRO_CHECK(!second.WindowEvents.Has(Wayland::XdgToplevelState::Activated));

	// One configure for the whole gesture: the walk is applied before the comparison the events come
	// out of, so a client hears where a person stopped rather than every window they went past.
	GYRO_CHECK_EQ(first.WindowEvents.Configured, configured + 1);

	// And it is in front, which is the half a person sees rather than reads: the floor's last child is
	// the topmost window (55), and with every window centred on the same point a step that only moved
	// focus would be a gesture with nothing on screen behind it.
	const Entity* const floor = pair.Store.Find(pair.Store.FirstRoot());
	GYRO_REQUIRE(floor != nullptr);

	GYRO_CHECK(pair.Store.Focus().Focused() == floor->LastChild);

	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// The pointer, moved the way `Dispatch/Loop.h` moves it: a displacement against the outputs, with no
// rounding anywhere on the way.
void Push(Pair& pair, double x, double y)
{
	static_cast<void>(pair.Store.Pointer().Move({ x, y }, pair.Store.Outputs()));
}

// Where the window's pixels ended up, which is the Floorplanner's answer and not a number this file
// should be repeating. The pointer is pushed to the middle of it.
[[nodiscard]] Point<GlobalSpace> MiddleOfTheWindow(const SceneStore& scene)
{
	const Entity* const window = WindowNode(scene);

	if (window == nullptr)
	{
		return {};
	}

	const Vector3<double> at = window->Translation.Model();

	return { at.X + (static_cast<double>(window->Extent.Width) / 2.0),
		     at.Y + (static_cast<double>(window->Extent.Height) / 2.0) };
}

[[nodiscard]] PointerButton Click(std::uint32_t code, bool pressed, Instant when)
{
	return PointerButton{ .Code = code, .Pressed = pressed, .When = when };
}
} // namespace

// The seat says what it has before a client asks, and there are three of them now. The capabilities
// event is the whole of how a toolkit decides whether to build a touch path at all, so a client that
// binds every object the bitmask claims must get three working objects and no error.
GYRO_TEST(ProtocolRoundTrip, TheSeatOffersAKeyboardAPointerAndATouchscreen)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-pointer" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	GYRO_CHECK(
		(keyboard.SeatListener.Capabilities & Wayland::WlSeatCapability::Pointer) == Wayland::WlSeatCapability::Pointer
	);
	GYRO_CHECK(
		(keyboard.SeatListener.Capabilities & Wayland::WlSeatCapability::Touch) == Wayland::WlSeatCapability::Touch
	);

	Pointer pointer;
	GYRO_REQUIRE(Grip(pair, keyboard, pointer));

	// **Asking for the touchscreen used to end the client**, which was the honest answer for as long as
	// nothing could route a finger: the protocol names `missing_capability` for a capability a seat has
	// never had. What it must not do now is either half of that — no error, and an object that works.
	Touch touch;
	GYRO_REQUIRE(Feel(pair, keyboard, touch));

	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// The pointer arriving on a window and moving across it. The coordinates are the surface's own, which
// is the whole of what the hit test is for: a toolkit decides which of its buttons is under the cursor
// from these two numbers and nothing else.
GYRO_TEST(ProtocolRoundTrip, APointerOverAWindowEntersItAndFollowsTheHand)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-pointing" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(pair, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(pair, bound, toplevel, std::byte{ 0x40 }));

	// **Nothing yet, because nothing has moved a mouse.** A compositor that had sent an `enter` here
	// would be one that draws a cursor on a machine driven by a touchscreen.
	GYRO_CHECK_EQ(pointer.Listener.Entered, std::uint32_t{ 0 });

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);
	GYRO_REQUIRE(middle.X > 0.0);

	Push(pair, middle.X, middle.Y);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Entered, std::uint32_t{ 1 });
	GYRO_CHECK(pointer.Listener.On.Id() == toplevel.Drawn.Surface.Id());

	// The middle of the window is the middle of the surface, because there is no shadow margin on a
	// client that declared no geometry — and the buffer is the harness's 16x8.
	GYRO_CHECK_EQ(pointer.Listener.X.ToInt(), std::int32_t{ 8 });
	GYRO_CHECK_EQ(pointer.Listener.Y.ToInt(), std::int32_t{ 4 });
	GYRO_CHECK_EQ(pointer.Listener.Motions, std::uint32_t{ 0 });

	// A second wakeup with the hand still on the desk sends nothing at all, which is what keeps a
	// window animating under a resting pointer from telling its toolkit the hand is moving.
	pair.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Motions, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(pointer.Listener.Entered, std::uint32_t{ 1 });

	Push(pair, 2.0, 1.0);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Motions, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(pointer.Listener.X.ToInt(), std::int32_t{ 10 });
	GYRO_CHECK_EQ(pointer.Listener.Y.ToInt(), std::int32_t{ 5 });
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// Sliding off the window, which is the event a toolkit un-highlights everything on.
GYRO_TEST(ProtocolRoundTrip, ThePointerLeavesAWindowItSlidOff)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-leaving" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(pair, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(pair, bound, toplevel, std::byte{ 0x40 }));

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);

	Push(pair, middle.X, middle.Y);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Entered, std::uint32_t{ 1 });

	// Off the far side of it, onto gyro's own floor — which has no client behind it, so what the client
	// hears is the leave and nothing after it.
	Push(pair, 600.0, 0.0);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Left, std::uint32_t{ 1 });
	GYRO_CHECK(pointer.Listener.LeftSurface.Id() == toplevel.Drawn.Surface.Id());
	GYRO_CHECK_EQ(pointer.Listener.Entered, std::uint32_t{ 1 });
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// The implicit grab: a button held is a surface that keeps the pointer wherever the hand goes.
//
// **This is what every drag in every toolkit rests on.** A person who presses on a scrollbar and slides
// off the window is still scrolling; a compositor that sent a `leave` at the frame edge would leave the
// thumb stuck where the pointer crossed it, which is the bug this test exists to keep out.
GYRO_TEST(ProtocolRoundTrip, AButtonHeldKeepsThePointerOnTheWindowItWasPressedOn)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-grab" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(pair, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(pair, bound, toplevel, std::byte{ 0x40 }));

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);

	Push(pair, middle.X, middle.Y);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Entered, std::uint32_t{ 1 });

	// `BTN_LEFT`, in the kernel's numbering all the way to the client — the same rule the keyboard is
	// under, and the reason nothing in the middle has a button map.
	(*pair.Host)->OnPointerButton(Click(0x110, true, pair.Clock.Now()));

	pair.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Buttons, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(pointer.Listener.LastButton, std::uint32_t{ 0x110 });
	GYRO_CHECK(pointer.Listener.LastState == Wayland::WlPointerButtonState::Pressed);

	// Off the window entirely, with the button still down. No leave, and the coordinates keep going —
	// negative, because the hand is to the left of where the surface starts.
	Push(pair, -600.0, 0.0);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Left, std::uint32_t{ 0 });
	GYRO_CHECK(pointer.Listener.Motions > 0);
	GYRO_CHECK(pointer.Listener.X.Raw() < 0);

	// The release goes to the window that took the press, and the leave follows it in the same wakeup:
	// the hand is over gyro's floor by now, and the client would otherwise sit believing it still had
	// the pointer until something else moved.
	(*pair.Host)->OnPointerButton(Click(0x110, false, pair.Clock.Now()));

	pair.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Buttons, std::uint32_t{ 2 });
	GYRO_CHECK(pointer.Listener.LastState == Wayland::WlPointerButtonState::Released);
	GYRO_CHECK_EQ(pointer.Listener.Left, std::uint32_t{ 1 });
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// Decision 162's click-to-focus, from the far end of the socket: a press on a window that does not
// have the keyboard takes it, and the `enter` arrives in the wakeup the button did.
//
// **The ordering is the whole of what this checks and it is not cosmetic.** Keys are delivered as the
// devices are drained, ahead of the step that routes the button — so a focus change noticed one
// iteration later is a first keystroke landing in the window a person has just clicked away from,
// which is the bug a user reports as *it typed into the wrong window* and nobody can reproduce on
// demand.
GYRO_TEST(ProtocolRoundTrip, AClickTakesTheKeyboardBackInTheWakeupTheButtonArrivedIn)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-click" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(pair, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(pair, bound, toplevel, std::byte{ 0x40 }));

	GYRO_REQUIRE_EQ(keyboard.Listener.Entered, std::uint32_t{ 1 });

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);

	// Focus taken away by something with no client behind it, which is what the recovery console and
	// the greeter are — and the cheapest way to say *this window is not the focused one* without a
	// second connection.
	const EntityId elsewhere = pair.Store.CreateContainer({}, {}).value();

	pair.Store.Focus().Offer(elsewhere);

	pair.Turn();

	GYRO_REQUIRE_EQ(keyboard.Listener.Left, std::uint32_t{ 1 });

	Push(pair, middle.X, middle.Y);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	// The pointer being on the window is not focus: hovering changes nothing, because follows-mouse is
	// a preference nobody has and this is not it.
	GYRO_REQUIRE_EQ(pointer.Listener.Entered, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(keyboard.Listener.Entered, std::uint32_t{ 1 });

	(*pair.Host)->OnPointerButton(Click(0x110, true, pair.Clock.Now()));

	pair.Turn();

	// One turn, and the client has both: the button it was clicked with and the keyboard it was
	// clicked for.
	GYRO_CHECK_EQ(pointer.Listener.Buttons, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(keyboard.Listener.Entered, std::uint32_t{ 2 });
	GYRO_CHECK(keyboard.Listener.Focused.Id() == toplevel.Drawn.Surface.Id());
	GYRO_CHECK(pair.Store.Focus().Focused() != elsewhere);

	// Clicking about inside the window it is already in sends nothing further: focus that re-entered on
	// every press would be a `leave`/`enter` pair per click, and a toolkit redraws on those.
	(*pair.Host)->OnPointerButton(Click(0x110, false, pair.Clock.Now()));
	(*pair.Host)->OnPointerButton(Click(0x110, true, pair.Clock.Now()));

	pair.Turn();

	GYRO_CHECK_EQ(keyboard.Listener.Entered, std::uint32_t{ 2 });
	GYRO_CHECK_EQ(keyboard.Listener.Left, std::uint32_t{ 1 });
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// A scroll, with the three events that say what did it. The source is the one that decides whether a
// toolkit runs kinetic scrolling at all, so it has to arrive and it has to arrive first.
GYRO_TEST(ProtocolRoundTrip, AScrollCarriesWhatDidItBeforeItCarriesHowFar)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-scroll" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(pair, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(pair, bound, toplevel, std::byte{ 0x40 }));

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);

	Push(pair, middle.X, middle.Y);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	GYRO_REQUIRE(pointer.Listener.Entered == std::uint32_t{ 1 });

	(*pair.Host)
		->OnPointerScroll(
			PointerScroll{ .Axis = ScrollAxis::Vertical,
	                       .Source = ScrollSource::Wheel,
	                       .Distance = 15.0,
	                       .Clicks120 = 120.0,
	                       .When = pair.Clock.Now() }
		);

	pair.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Sources, std::uint32_t{ 1 });
	GYRO_CHECK(pointer.Listener.LastSource == Wayland::WlPointerAxisSource::Wheel);
	GYRO_CHECK_EQ(pointer.Listener.Discretes, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(pointer.Listener.LastDiscrete, std::int32_t{ 1 });
	GYRO_CHECK_EQ(pointer.Listener.Axes, std::uint32_t{ 1 });
	GYRO_CHECK(pointer.Listener.LastAxis == Wayland::WlPointerAxis::VerticalScroll);
	GYRO_CHECK_EQ(pointer.Listener.LastValue.ToInt(), std::int32_t{ 15 });

	// A touchpad that stopped, which is the event a flick decays from and the only one that carries no
	// distance. A wheel never produces one, so nothing here would have made it up.
	(*pair.Host)
		->OnPointerScroll(
			PointerScroll{
				.Axis = ScrollAxis::Vertical, .Source = ScrollSource::Finger, .Stop = true, .When = pair.Clock.Now() }
		);

	pair.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Stops, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(pointer.Listener.Axes, std::uint32_t{ 1 });
	GYRO_CHECK(pointer.Listener.LastSource == Wayland::WlPointerAxisSource::Finger);
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// The dmabuf half: what a client is offered, what it does with a pair it was never offered, and the
// one thing a descriptor buffer owes that a `wl_shm` buffer does not.
//
// **The whole point of the last of these is a release that does *not* arrive.** gyro copies `wl_shm`
// pixels and hands the buffer straight back; descriptors are borrowed, and a client told it may redraw
// while a panel is still scanning the buffer out is a window tearing into itself. There is no way to
// observe that except by committing and then asserting silence, which is what this does.

namespace
{
// What the compositor said a client may allocate in, collected the way a toolkit collects it.
class DmabufFormats final : public Wayland::ZwpLinuxDmabufV1Listener
{
public:
	void OnFormat(std::uint32_t format) override { Codes.push_back(format); }

	void OnModifier(std::uint32_t format, std::uint32_t high, std::uint32_t low) override
	{
		Pairs.emplace_back(format, (static_cast<std::uint64_t>(high) << 32U) | low);
	}

	std::vector<std::uint32_t> Codes;
	std::vector<std::pair<std::uint32_t, std::uint64_t>> Pairs;
};

// `DRM_FORMAT_ARGB8888`, spelled here rather than reached for: this file stands where a client stands,
// and a client has a fourcc because `zwp_linux_buffer_params_v1.create` carries one.
constexpr std::uint32_t Argb8888 = 0x34325241;

// The answer to `create`, which is exactly one of two events.
class ParamsEvents final : public Wayland::ZwpLinuxBufferParamsV1Listener
{
public:
	explicit ParamsEvents(Wayland::WlBufferListener& released) noexcept : m_Released{ &released } {}

	Wayland::WlBufferListener* OnCreated(Wayland::WlBuffer buffer) override
	{
		Created = buffer;

		return m_Released;
	}

	void OnFailed() override { ++Failed; }

	Wayland::WlBuffer Created;
	std::uint32_t Failed = 0;

private:
	Wayland::WlBufferListener* m_Released = nullptr;
};

// A descriptor of the right size for the extent below. A `memfd` rather than a real dmabuf, which is
// all this test needs: nothing here imports it, and what is under test is the protocol and the
// lifetime rather than the driver.
[[nodiscard]] Fd MakeDescriptor(std::size_t size)
{
	const int descriptor = ::memfd_create("gyro-roundtrip-dmabuf", MFD_CLOEXEC);

	if (descriptor < 0 || ::ftruncate(descriptor, static_cast<off_t>(size)) != 0)
	{
		return {};
	}

	return Fd{ descriptor };
}

[[nodiscard]] Wayland::ZwpLinuxDmabufV1 BindDmabuf(BoundCompositor& bound, DmabufFormats& formats)
{
	const Registry::Global* const global = bound.Listener.Find(Wayland::ZwpLinuxDmabufV1::WireName);

	return global != nullptr ?
	           bound.Listener.Object().Bind<Wayland::ZwpLinuxDmabufV1>(global->Name, global->Version, formats) :
	           Wayland::ZwpLinuxDmabufV1{};
}
} // namespace

GYRO_TEST(ProtocolRoundTrip, BindingTheDmabufGlobalAnnouncesEveryPairTheDeviceWillSample)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-dmabuf-formats" };
	GYRO_REQUIRE(pair.Opened);

	// Two tilings of one format, which is the shape a real driver's answer has and the shape a
	// linear-only list would hide: a client picks the tiled one and never allocates an untiled buffer.
	pair.Textures.Advertised = { TextureFormat{ .Code = Argb8888, .Modifier = 0 },
		                         TextureFormat{ .Code = Argb8888, .Modifier = 0x0100000000000001ULL } };

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	DmabufFormats formats;
	GYRO_REQUIRE(BindDmabuf(bound, formats).IsValid());

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// One `format` per distinct fourcc and one `modifier` per pair. A client bound below version 3 has
	// only the first list and reads it as *this format under whatever we work out*, which is linear.
	GYRO_CHECK_EQ(formats.Codes.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(formats.Pairs.size(), std::size_t{ 2 });
	GYRO_CHECK(formats.Pairs[1].second == 0x0100000000000001ULL);
}

GYRO_TEST(ProtocolRoundTrip, APairTheCompositorNeverOfferedComesBackAsFailedRatherThanADeadConnection)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-dmabuf-refused" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	DmabufFormats formats;
	const Wayland::ZwpLinuxDmabufV1 dmabuf = BindDmabuf(bound, formats);
	GYRO_REQUIRE(dmabuf.IsValid());

	BufferEvents released;
	ParamsEvents events{ released };
	const Wayland::ZwpLinuxBufferParamsV1 params = dmabuf.CreateParams(events);
	GYRO_REQUIRE(params.IsValid());

	Fd descriptor = MakeDescriptor(static_cast<std::size_t>(Stride) * static_cast<std::size_t>(Height));
	GYRO_REQUIRE(descriptor.IsValid());

	params.Add(std::move(descriptor), 0, 0, static_cast<std::uint32_t>(Stride), 0, 0);
	params.Create(Width, Height, Argb8888, Wayland::ZwpLinuxBufferParamsV1Flags{});

	pair.Turn();

	// **`failed` rather than a protocol error**, because a client cannot predict what a compositor's
	// device will take and the protocol gives it a fallback path for exactly this. Ending the
	// connection would turn a driver limitation into an application that will not start.
	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(events.Failed, std::uint32_t{ 1 });
	GYRO_CHECK(!events.Created.IsValid());
}

GYRO_TEST(ProtocolRoundTrip, ADescriptorBufferIsNotReleasedUntilNobodyIsReadingIt)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-dmabuf-release" };
	GYRO_REQUIRE(pair.Opened);

	pair.Textures.Advertised = { TextureFormat{ .Code = Argb8888, .Modifier = 0 } };

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	DmabufFormats formats;
	const Wayland::ZwpLinuxDmabufV1 dmabuf = BindDmabuf(bound, formats);
	GYRO_REQUIRE(dmabuf.IsValid());

	BufferEvents released;
	ParamsEvents events{ released };
	const Wayland::ZwpLinuxBufferParamsV1 params = dmabuf.CreateParams(events);
	GYRO_REQUIRE(params.IsValid());

	Fd descriptor = MakeDescriptor(static_cast<std::size_t>(Stride) * static_cast<std::size_t>(Height));
	GYRO_REQUIRE(descriptor.IsValid());

	params.Add(std::move(descriptor), 0, 0, static_cast<std::uint32_t>(Stride), 0, 0);
	params.Create(Width, Height, Argb8888, Wayland::ZwpLinuxBufferParamsV1Flags{});

	pair.Turn();
	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_REQUIRE(events.Created.IsValid());

	Wayland::WlSurfaceIgnoring surfaceEvents;
	const Wayland::WlSurface surface = bound.Compositor.CreateSurface(surfaceEvents);
	GYRO_REQUIRE(surface.IsValid());

	surface.Attach(events.Created, 0, 0);
	surface.DamageBuffer(0, 0, Width, Height);
	surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// The descriptors reached the texture space as one plane in the layout the client stated.
	GYRO_CHECK_EQ(pair.Textures.Adopted, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(pair.Textures.Planes, std::size_t{ 1 });
	GYRO_CHECK(pair.Textures.Described == (TextureFormat{ .Code = Argb8888, .Modifier = 0 }));

	// **And no release, which is the assertion.** A frame drawn from this buffer may still be on
	// screen; telling the client otherwise is what tears a window into itself.
	pair.Turn();

	GYRO_CHECK_EQ(released.Released, std::uint32_t{ 0 });

	// The watermark moves past every snapshot that named the id, which is the only thing that can say
	// nobody is reading.
	pair.Textures.ReleaseAll();

	pair.Turn();

	GYRO_CHECK_EQ(released.Released, std::uint32_t{ 1 });
}

// Decision 51's continuous manipulation over a real socket, and decision 166's split down the middle
// of it: gyro turns the pointer into a size and the *client* turns the size into a window.
//
// **What is being checked is that the loop closes with nobody else in it.** The client asks once, and
// from then to the button coming up it is told a new size whenever the hand moves and told nothing
// else — no reply to its request, no second protocol, and no shell.
GYRO_TEST(ProtocolRoundTrip, ADraggedEdgeConfiguresTheWindowUntilTheButtonComesUp)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-resize" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(pair, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(pair, bound, toplevel, std::byte{ 0x40 }));

	// The window took focus as it mapped, so there is a configure outstanding. A real toolkit answers
	// every one of them and this is where the harness does: a resize is throttled to the client's own
	// rate, and a client that has not caught up is not asked for anything new.
	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);

	pair.Turn();

	GYRO_REQUIRE(toplevel.WindowEvents.Width == 0);
	GYRO_REQUIRE(toplevel.WindowEvents.Height == 0);

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);

	Push(pair, middle.X, middle.Y);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	(*pair.Host)->OnPointerButton(Click(0x110, true, pair.Clock.Now()));

	pair.Turn();

	GYRO_REQUIRE(pointer.Listener.ButtonSerial != 0);

	// The corner of the window, quoting the serial of the press a person is holding.
	toplevel.Window.Resize(keyboard.Seat, pointer.Listener.ButtonSerial, Wayland::XdgToplevelResizeEdge::BottomRight);

	// **The gesture is anchored where the pointer is when the request arrives**, which is the only
	// instant gyro has: the press that authorised it happened some iterations ago and the hand has been
	// free to move since. So the request is delivered on its own turn here, exactly as a toolkit's is.
	pair.Turn();

	// **The first configure of a gesture restates the size and announces the state**, which is not a
	// wasted round trip: `resizing` is what tells a toolkit to take its cheap redraw path, and it has to
	// arrive before the sizes that will exercise it. The window has not moved yet, so the size is the one
	// the client already chose.
	GYRO_CHECK_EQ(toplevel.WindowEvents.Width, Width);
	GYRO_CHECK(toplevel.WindowEvents.Has(Wayland::XdgToplevelState::Resizing));

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);

	Push(pair, 60.0, 40.0);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	// The buffer is the harness's 16x8 and the hand has travelled 60 by 40.
	GYRO_CHECK_EQ(toplevel.WindowEvents.Width, Width + 60);
	GYRO_CHECK_EQ(toplevel.WindowEvents.Height, Height + 40);
	GYRO_CHECK(toplevel.WindowEvents.Has(Wayland::XdgToplevelState::Resizing));

	// **And it is still activated**, which is the reason focus and the gesture are answered by one walk:
	// a window told it is being resized and not told it still has the keyboard would go grey under a
	// person's own hand.
	GYRO_CHECK(toplevel.WindowEvents.Has(Wayland::XdgToplevelState::Activated));

	// The compositor grab superseded the client's implicit one, and the leave is how it was told: a
	// toolkit still receiving motion would run its own edge-drag logic underneath gyro's.
	GYRO_CHECK_EQ(pointer.Listener.Left, std::uint32_t{ 1 });

	// **Nothing has changed shape in the world**, which is decision 166: the extent is the client's and
	// it has not drawn one yet.
	GYRO_REQUIRE(WindowNode(pair.Store) != nullptr);
	GYRO_CHECK_EQ(WindowNode(pair.Store)->Extent.Width, static_cast<float>(Width));

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);

	(*pair.Host)->OnPointerButton(Click(0x110, false, pair.Clock.Now()));

	pair.Turn();

	// Letting go clears the state and keeps the size: going back to zero would read as *pick your own
	// size again*, which is a window that springs back to its natural size the moment anything else
	// makes gyro configure it.
	GYRO_CHECK(!toplevel.WindowEvents.Has(Wayland::XdgToplevelState::Resizing));
	GYRO_CHECK_EQ(toplevel.WindowEvents.Width, Width + 60);
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// **The half of a resize that cannot be delegated** (166). The client is asked for one size and comes
// back with another — its own increment, its own minimum, its own opinion — and the edge the person is
// *not* holding must not move by the difference. Anchoring on the request instead is the shimmer
// visible on every compositor that gets this wrong, and it is invisible unless the two sizes differ,
// which is why this test makes them.
GYRO_TEST(ProtocolRoundTrip, TheEdgeAPersonIsNotHoldingStaysWhereItIs)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-anchor" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(pair, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(pair, bound, toplevel, std::byte{ 0x40 }));

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);

	pair.Turn();

	GYRO_REQUIRE(WindowNode(pair.Store) != nullptr);

	// Where the far edge is now, which is the number that must still be true at the end.
	const double right = WindowNode(pair.Store)->Translation.Model().X + static_cast<double>(Width);

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);

	Push(pair, middle.X, middle.Y);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	(*pair.Host)->OnPointerButton(Click(0x110, true, pair.Clock.Now()));

	pair.Turn();

	toplevel.Window.Resize(keyboard.Seat, pointer.Listener.ButtonSerial, Wayland::XdgToplevelResizeEdge::Left);

	pair.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);

	// Pulling the near edge away from the far one, which grows the window.
	Push(pair, -40.0, 0.0);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	GYRO_CHECK_EQ(toplevel.WindowEvents.Width, Width + 40);

	// The client answers with a width of its own — ten short of what it was asked for, which is what a
	// terminal that only grows by whole character cells does on every frame of a drag.
	constexpr std::int32_t Drawn = Width + 30;

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.XdgSurface.SetWindowGeometry(0, 0, Drawn, Height);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_REQUIRE(WindowNode(pair.Store) != nullptr);

	// The window is the size the client drew rather than the size it was asked for...
	GYRO_CHECK_EQ(WindowNode(pair.Store)->Extent.Width, static_cast<float>(Drawn));

	// ...and the edge nobody is holding has not moved by the difference between the two.
	GYRO_CHECK_EQ(WindowNode(pair.Store)->Translation.Model().X + static_cast<double>(Drawn), right);
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// The one bound a resize honours, and decision 166 says why it is not a counterexample to gyro having
// no constraints: a minimum size does not come from a shell, it arrives on the wire from the party
// being resized — the only participant entitled to say that forty pixels is not a window.
GYRO_TEST(ProtocolRoundTrip, AWindowIsNeverAskedToBeSmallerThanItSaidItCouldBe)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-minimum" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(pair, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(pair, bound, toplevel, std::byte{ 0x40 }));

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);

	// Wider than the window already is, which is legal and is what a toolkit that has just laid out its
	// own chrome sends. Staged, and landing with the commit behind it like the window geometry.
	constexpr std::int32_t Smallest = 40;

	toplevel.Window.SetMinSize(Smallest, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);

	Push(pair, middle.X, middle.Y);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	(*pair.Host)->OnPointerButton(Click(0x110, true, pair.Clock.Now()));

	pair.Turn();

	toplevel.Window.Resize(keyboard.Seat, pointer.Listener.ButtonSerial, Wayland::XdgToplevelResizeEdge::Right);

	pair.Turn();

	// The opening configure of the gesture is already clamped, which is worth checking on its own: it is
	// the one configure gyro sends without a hand having moved, and a compositor that clamped only the
	// moving sizes would tell a window to be sixteen wide and then never tell it again.
	GYRO_CHECK_EQ(toplevel.WindowEvents.Width, Smallest);

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);

	// Dragging the edge clean through the far side of the window, which without the bound would floor at
	// one and without the floor would be zero — *pick your own size*, handing the client back the very
	// freedom the gesture is taking away.
	Push(pair, -400.0, 0.0);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	GYRO_CHECK_EQ(toplevel.WindowEvents.Width, Smallest);
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// **A drag that keeps going while the client answers nothing.** This is the shape a compositor must
// never depend on cooperation for, and the version of this file that gated a size on the last
// acknowledgement got it wrong: the opening configure of a gesture asks a window to be the size it
// already is, and a toolkit that treats that as nothing to redraw for is behaving reasonably. Under
// the gate the drag was dead from that moment — the pointer moved, the window did not, and nothing
// anywhere reported an error. See decision 166.
//
// The client here acknowledges nothing at all, which is stronger than any real toolkit and is the
// point: xdg-shell lets a client ignore every configure but the newest, so the coalescing belongs on
// its side and gyro's job is to keep telling it the truth.
GYRO_TEST(ProtocolRoundTrip, ASilentClientIsStillToldEverySizeTheHandAsksFor)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-silent" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(pair, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(pair, bound, toplevel, std::byte{ 0x40 }));

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);

	Push(pair, middle.X, middle.Y);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	(*pair.Host)->OnPointerButton(Click(0x110, true, pair.Clock.Now()));

	pair.Turn();

	toplevel.Window.Resize(keyboard.Seat, pointer.Listener.ButtonSerial, Wayland::XdgToplevelResizeEdge::BottomRight);

	pair.Turn();

	// From here the client says nothing back — no acknowledgement, no buffer, no window geometry.
	for (std::int32_t step = 1; step <= 4; ++step)
	{
		Push(pair, 10.0, 10.0);
		(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

		pair.Turn();

		GYRO_CHECK_EQ(toplevel.WindowEvents.Width, Width + (step * 10));
		GYRO_CHECK_EQ(toplevel.WindowEvents.Height, Height + (step * 10));
	}

	GYRO_CHECK(toplevel.WindowEvents.Has(Wayland::XdgToplevelState::Resizing));
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// A finger landing on a window, which is the whole of what a `down` has to get right: the surface it
// names, the coordinates on that surface, and the id every later event about that finger quotes.
GYRO_TEST(ProtocolRoundTrip, AFingerLandsOnTheWindowUnderItAndTakesTheKeyboardWithIt)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-touching" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Touch touch;
	GYRO_REQUIRE(Feel(pair, keyboard, touch));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(pair, bound, toplevel, std::byte{ 0x40 }));

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);
	GYRO_REQUIRE(middle.X > 0.0);

	Finger(pair, TouchPhase::Down, 0, middle);

	pair.Turn();

	GYRO_CHECK_EQ(touch.Listener.Downs, std::uint32_t{ 1 });
	GYRO_CHECK(touch.Listener.On.Id() == toplevel.Drawn.Surface.Id());

	// The middle of the window is the middle of the harness's 16x8 buffer, exactly as it is for the
	// pointer — the hit test is the same walk and the coordinates are the same space.
	GYRO_CHECK_EQ(touch.Listener.X.ToInt(), std::int32_t{ 8 });
	GYRO_CHECK_EQ(touch.Listener.Y.ToInt(), std::int32_t{ 4 });

	// **One group, closed once.** A `down` and nothing else is still a frame: the protocol says a frame
	// terminates at least one event, and a toolkit that accumulates until one arrives would otherwise
	// hold the contact forever.
	GYRO_CHECK_EQ(touch.Listener.Frames, std::uint32_t{ 1 });

	// **No cursor was drawn to get here**, which is the difference between a touchscreen and a mouse
	// that this compositor has to keep: a machine driven by a finger has no pointer, and the seat's own
	// pointer path refuses to send anything while the cursor is invisible.
	GYRO_CHECK(!pair.Store.Pointer().IsVisible());

	// And the window it landed on has the keyboard, by the same policy a click focuses under (162).
	GYRO_CHECK(toplevel.WindowEvents.Has(Wayland::XdgToplevelState::Activated));
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// The sequence staying on the window it started on, which is what a touch has instead of the pointer's
// implicit grab — and it is not optional: a person dragging a slider whose finger strays past the edge
// of the window is still dragging it, and a compositor that re-resolved every motion would hand the
// rest of that gesture to whatever is behind.
GYRO_TEST(ProtocolRoundTrip, AFingerKeepsTheWindowItWentDownOnAfterItSlidesOff)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-touchgrab" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Touch touch;
	GYRO_REQUIRE(Feel(pair, keyboard, touch));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(pair, bound, toplevel, std::byte{ 0x40 }));

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);

	Finger(pair, TouchPhase::Down, 0, middle);

	pair.Turn();

	GYRO_CHECK_EQ(touch.Listener.Downs, std::uint32_t{ 1 });

	// Off the far side of the window, onto gyro's own floor. The motion still goes to the same client,
	// and the coordinate it carries is outside the surface — which is what the wire's signed fixed point
	// is for, and what a toolkit clamps for itself.
	Finger(pair, TouchPhase::Motion, 0, { middle.X + 400.0, middle.Y });

	pair.Turn();

	GYRO_CHECK_EQ(touch.Listener.Motions, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(touch.Listener.Id, std::int32_t{ 0 });
	GYRO_CHECK(touch.Listener.X.ToInt() > 16);
	GYRO_CHECK_EQ(touch.Listener.Frames, std::uint32_t{ 2 });

	Finger(pair, TouchPhase::Up, 0, {});

	pair.Turn();

	GYRO_CHECK_EQ(touch.Listener.Ups, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(touch.Listener.Frames, std::uint32_t{ 3 });

	// **A motion after the up reaches nobody**, because the sequence is over and the slot names nothing.
	// The failure this keeps out is a finger the compositor never let go of, which delivers a contact to
	// a window a person stopped touching.
	Finger(pair, TouchPhase::Motion, 0, middle);

	pair.Turn();

	GYRO_CHECK_EQ(touch.Listener.Motions, std::uint32_t{ 1 });
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// Two fingers in one wakeup: two ids, and one group. The ids are the seat's own rather than libinput's
// slots, and the frame is per client rather than per contact — a toolkit reading a pinch wants both
// positions before it computes a distance from them.
GYRO_TEST(ProtocolRoundTrip, TwoFingersAreTwoIdentitiesInsideOneGroup)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-pinch" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Touch touch;
	GYRO_REQUIRE(Feel(pair, keyboard, touch));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(pair, bound, toplevel, std::byte{ 0x40 }));

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);

	Finger(pair, TouchPhase::Down, 0, { middle.X - 2.0, middle.Y });
	Finger(pair, TouchPhase::Down, 1, { middle.X + 2.0, middle.Y });

	pair.Turn();

	GYRO_CHECK_EQ(touch.Listener.Downs, std::uint32_t{ 2 });
	GYRO_CHECK_EQ(touch.Listener.Id, std::int32_t{ 1 });
	GYRO_CHECK_EQ(touch.Listener.Frames, std::uint32_t{ 1 });

	// The first finger lifts and the second stays down, so the id it frees is the one the next contact
	// takes — which the protocol permits explicitly and a client has to be able to survive.
	Finger(pair, TouchPhase::Up, 0, {});

	pair.Turn();

	GYRO_CHECK_EQ(touch.Listener.Ups, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(touch.Listener.Id, std::int32_t{ 0 });

	Finger(pair, TouchPhase::Down, 2, middle);

	pair.Turn();

	GYRO_CHECK_EQ(touch.Listener.Downs, std::uint32_t{ 3 });
	GYRO_CHECK_EQ(touch.Listener.Id, std::int32_t{ 0 });
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// The two ways a sequence ends without a finger being lifted, and they are the reason `wl_touch.cancel`
// exists: what a client must never be left holding is a contact that will never end.
GYRO_TEST(ProtocolRoundTrip, ASequenceThatEndsWithoutALiftIsCancelledRatherThanLeftDown)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-cancel" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Touch touch;
	GYRO_REQUIRE(Feel(pair, keyboard, touch));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(pair, bound, toplevel, std::byte{ 0x40 }));

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);

	Finger(pair, TouchPhase::Down, 0, middle);

	pair.Turn();

	GYRO_CHECK_EQ(touch.Listener.Downs, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(touch.Listener.Frames, std::uint32_t{ 1 });

	// The device cancelling — a palm the panel decided was not a finger, or a gesture libinput took for
	// itself. It is the opposite of an up to whatever was tracking the contact: the gesture unwinds
	// rather than committing, which is why `TouchPhase::Cancel` is a phase rather than a variety of up.
	Finger(pair, TouchPhase::Cancel, 0, {});

	pair.Turn();

	GYRO_CHECK_EQ(touch.Listener.Cancels, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(touch.Listener.Ups, std::uint32_t{ 0 });

	// **And no frame behind it**, which the protocol states: the cancel ends the group it was in.
	GYRO_CHECK_EQ(touch.Listener.Frames, std::uint32_t{ 1 });

	// The second way, and the one nothing downstream could ever discover for itself: the touchscreen is
	// unplugged with a finger on it, so no up and no cancel is ever coming from the device. Without the
	// notice, the client holds that contact for the rest of its life.
	Finger(pair, TouchPhase::Down, 1, middle);

	pair.Turn();

	GYRO_CHECK_EQ(touch.Listener.Downs, std::uint32_t{ 2 });

	(*pair.Host)->OnDeviceGone(Digitizer);

	pair.Turn();

	GYRO_CHECK_EQ(touch.Listener.Cancels, std::uint32_t{ 2 });
	GYRO_CHECK_EQ(touch.Listener.Ups, std::uint32_t{ 0 });

	// The point is gone with the device, so the finger somebody lifts on the way out reaches nobody.
	Finger(pair, TouchPhase::Up, 1, {});

	pair.Turn();

	GYRO_CHECK_EQ(touch.Listener.Ups, std::uint32_t{ 0 });
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// A client cannot take the pointer with a number nobody sent it. This is the check that stands where
// the popup grab's has no ledger to stand on ([Popup.h](../Protocol/Popup.h)): one serial, the press
// gyro is holding, and anything else is an application helping itself to a gesture a person did not
// make.
GYRO_TEST(ProtocolRoundTrip, AResizeQuotingASerialNobodySentIsRefused)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-serial" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(pair, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(pair, bound, toplevel, std::byte{ 0x40 }));

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);

	Push(pair, middle.X, middle.Y);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	(*pair.Host)->OnPointerButton(Click(0x110, true, pair.Clock.Now()));

	pair.Turn();

	GYRO_REQUIRE(pointer.Listener.ButtonSerial != 0);

	// One past the press, which is a serial that exists — the counter is the display's and every event
	// spends one — and is not the press this compositor is holding.
	toplevel.Window.Resize(
		keyboard.Seat, pointer.Listener.ButtonSerial + 1, Wayland::XdgToplevelResizeEdge::BottomRight
	);

	pair.Turn();

	Push(pair, 60.0, 40.0);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	// Nothing started: no size, no state, and the client still has the pointer it would have lost to a
	// compositor grab.
	GYRO_CHECK(!toplevel.WindowEvents.Has(Wayland::XdgToplevelState::Resizing));
	GYRO_CHECK_EQ(toplevel.WindowEvents.Width, 0);
	GYRO_CHECK_EQ(pointer.Listener.Left, std::uint32_t{ 0 });
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

namespace
{
// A window shaped the way GTK4 actually shapes one, taken from a real Ptyxis capture: a shadow margin
// inside the surface, a window geometry inset by it, and an input region that is the *geometry* grown
// by GTK4's twelve-pixel resize handle. That band straddles the visible edge — half of it is drawn
// shadow, which is where a person grabs a corner — and every pixel of it is inside the surface, so
// nothing here asks a compositor to honour input past the surface edge.
constexpr std::int32_t Margin = 25;
constexpr std::int32_t Handle = 12;
constexpr std::int32_t VisibleWidth = 150;
constexpr std::int32_t VisibleHeight = 100;
constexpr std::int32_t ShadowedWidth = VisibleWidth + (2 * Margin);
constexpr std::int32_t ShadowedHeight = VisibleHeight + (2 * Margin);
constexpr std::int32_t ShadowedStride = ShadowedWidth * 4;
constexpr std::size_t ShadowedBytes =
	static_cast<std::size_t>(ShadowedStride) * static_cast<std::size_t>(ShadowedHeight);

struct Shadowed
{
	SurfaceEvents Events;
	BufferEvents Released;
	ShellEvents Shell;
	ToplevelEvents WindowEvents;
	Wayland::WlSurface Surface;
	Wayland::WlShmPool Pool;
	Wayland::WlBuffer Buffer;
	Wayland::XdgSurface XdgSurface;
	Wayland::XdgToplevel Window;
};

[[nodiscard]] bool ShowShadowed(Pair& pair, BoundCompositor& bound, Shadowed& window)
{
	ClientPool pool{ ShadowedBytes };

	if (!pool.Descriptor.IsValid())
	{
		return false;
	}

	pool.Fill(std::byte{ 0x60 });

	window.Surface = bound.Compositor.CreateSurface(window.Events);
	window.Pool = bound.Shm.CreatePool(pool.Take(), static_cast<std::int32_t>(ShadowedBytes));

	if (!window.Surface.IsValid() || !window.Pool.IsValid())
	{
		return false;
	}

	window.Buffer = window.Pool.CreateBuffer(
		0, ShadowedWidth, ShadowedHeight, ShadowedStride, Wayland::WlShmFormat::Argb8888, window.Released
	);

	window.XdgSurface = bound.Shell.GetXdgSurface(window.Surface, window.Shell);

	if (!window.Buffer.IsValid() || !window.XdgSurface.IsValid())
	{
		return false;
	}

	window.Window = window.XdgSurface.GetToplevel(window.WindowEvents);

	if (!window.Window.IsValid())
	{
		return false;
	}

	window.Surface.Commit();

	pair.Turn();

	window.XdgSurface.AckConfigure(window.Shell.Serial);
	window.XdgSurface.SetWindowGeometry(Margin, Margin, VisibleWidth, VisibleHeight);

	Wayland::WlRegion input = bound.Compositor.CreateRegion();

	if (!input.IsValid())
	{
		return false;
	}

	input.Add(Margin - Handle, Margin - Handle, VisibleWidth + (2 * Handle), VisibleHeight + (2 * Handle));
	window.Surface.SetInputRegion(input);
	input.Destroy();

	window.Surface.Attach(window.Buffer, 0, 0);
	window.Surface.Commit();

	pair.Turn();

	return true;
}
} // namespace

// **A press in the drawn shadow, which is where GTK4 puts the corner a person grabs.** The window is
// the one above: the visible edge is twenty-five pixels inside the surface, and the twelve pixels
// outside that edge are still surface — still the client's pixels, still inside the region it asked
// for. A compositor that loses them is a compositor no GTK4 window can be resized under, because that
// band is the only place the toolkit will start a resize from.
//
// The coordinate matters as much as the delivery: what the client computes the edge from is the
// surface-local number in the `enter`, and a compositor that reported it relative to the *window*
// instead would have every toolkit resizing from twenty-five pixels off.
GYRO_TEST(ProtocolRoundTrip, APressInTheShadowMarginReachesTheWindowInSurfaceCoordinates)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-shadow" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(pair, keyboard, pointer));

	Shadowed window;
	GYRO_REQUIRE(ShowShadowed(pair, bound, window));

	const Entity* const node = WindowNode(pair.Store);
	GYRO_REQUIRE(node != nullptr);

	// The container is the *window*, so its origin is the visible edge and the surface starts a margin
	// up and to the left of it.
	GYRO_REQUIRE(node->Extent.Width == static_cast<float>(VisibleWidth));

	const Vector3<double> at = node->Translation.Model();

	// Seven pixels outside the visible left edge: drawn shadow, inside the surface, inside the region.
	Push(pair, at.X - 7.0, at.Y + 50.0);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Entered, std::uint32_t{ 1 });
	GYRO_CHECK(pointer.Listener.On.Id() == window.Surface.Id());

	// Surface-local, so the margin is included: 25 - 7 across and 25 + 50 down.
	GYRO_CHECK_EQ(pointer.Listener.X.ToInt(), Margin - 7);
	GYRO_CHECK_EQ(pointer.Listener.Y.ToInt(), Margin + 50);

	// And the shadow beyond the band is not the client's: twenty pixels out is outside the region it
	// asked for, so the pointer leaves rather than sliding along a strip nobody claimed.
	Push(pair, -13.0, 0.0);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Left, std::uint32_t{ 1 });
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

namespace
{
// A window on screen, which is the four steps every test below starts from and none of them is about.
[[nodiscard]] bool Mapped(Pair& pair, BoundCompositor& bound, Toplevel& toplevel, std::byte fill)
{
	if (!Role(bound, toplevel, fill))
	{
		return false;
	}

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	if (toplevel.SurfaceEvents.Serial == 0)
	{
		return false;
	}

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.DamageBuffer(0, 0, Width, Height);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	return true;
}

[[nodiscard]] Wayland::WlSubcompositor Subcompositor(BoundCompositor& bound)
{
	const Registry::Global* const global = bound.Listener.Find(Wayland::WlSubcompositor::WireName);

	if (global == nullptr)
	{
		return {};
	}

	return bound.Listener.Object().Bind<Wayland::WlSubcompositor>(global->Name, global->Version);
}

// The window's own id, which `WindowNode` has and does not return: the floor is the first root and a
// window is a child of it.
[[nodiscard]] EntityId WindowId(const SceneStore& scene)
{
	const Entity* const floor = scene.Find(scene.FirstRoot());

	return floor == nullptr ? EntityId{} : floor->FirstChild;
}

// The nodes under a window's container, in the order the frame walk visits them — which decision 55
// makes the order they are drawn in, so this list is literally back to front on screen.
[[nodiscard]] std::vector<EntityId> Children(const SceneStore& scene, EntityId parent)
{
	std::vector<EntityId> children;
	const Entity* const node = scene.Find(parent);

	for (EntityId at = node == nullptr ? EntityId{} : node->FirstChild; !at.IsNull();)
	{
		children.push_back(at);

		const Entity* const child = scene.Find(at);

		at = child == nullptr ? EntityId{} : child->NextSibling;
	}

	return children;
}
} // namespace

GYRO_TEST(ProtocolRoundTrip, ASubsurfaceReachesTheWorldWhenItsParentCommits)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-subsurface" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	const Wayland::WlSubcompositor subcompositor = Subcompositor(bound);
	GYRO_REQUIRE(subcompositor.IsValid());

	Toplevel toplevel;
	GYRO_REQUIRE(Mapped(pair, bound, toplevel, std::byte{ 0x33 }));

	GYRO_REQUIRE(!WindowId(pair.Store).IsNull());

	// The floor and the chrome root, the window and its pixels: four, which is where every test that
	// maps a window ends.
	GYRO_CHECK_EQ(pair.Store.Count(), std::uint32_t{ 4 });

	DrawnSurface child;
	GYRO_REQUIRE(Draw(bound, child, std::byte{ 0x77 }));

	const Wayland::WlSubsurface role = subcompositor.GetSubsurface(child.Surface, toplevel.Drawn.Surface);
	GYRO_REQUIRE(role.IsValid());

	role.SetPosition(3, 4);
	child.Surface.Attach(child.Buffer, 0, 0);
	child.Surface.DamageBuffer(0, 0, Width, Height);
	child.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// **The child committed and nothing is on screen**, which is the synchronized default doing its
	// job: a toolkit states every part of its window and the window is what publishes them.
	GYRO_CHECK_EQ(pair.Store.Count(), std::uint32_t{ 4 });

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// Two more nodes, which is decision 111's pair a second time: a container to hang the child's own
	// subsurfaces off and the image that is its pixels.
	GYRO_CHECK_EQ(pair.Store.Count(), std::uint32_t{ 6 });

	const std::vector<EntityId> children = Children(pair.Store, WindowId(pair.Store));
	GYRO_REQUIRE_EQ(children.size(), std::size_t{ 2 });

	// The parent's own pixels first and the child over them, which is where a new subsurface starts.
	GYRO_CHECK(pair.Store.Find(children.front())->Kind == NodeKind::Image);

	const Entity* const node = pair.Store.Find(children.back());
	GYRO_REQUIRE(node != nullptr);

	// In the parent surface's own coordinates, which for a window that declared no geometry is the
	// window's own origin.
	GYRO_CHECK_EQ(node->Translation.Model().X, 3.0);
	GYRO_CHECK_EQ(node->Translation.Model().Y, 4.0);
	GYRO_CHECK_EQ(node->Extent.Width, static_cast<float>(Width));
}

GYRO_TEST(ProtocolRoundTrip, ASubsurfacePlacedBelowItsParentIsDrawnUnderIt)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-substack" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	const Wayland::WlSubcompositor subcompositor = Subcompositor(bound);
	GYRO_REQUIRE(subcompositor.IsValid());

	Toplevel toplevel;
	GYRO_REQUIRE(Mapped(pair, bound, toplevel, std::byte{ 0x33 }));

	DrawnSurface child;
	GYRO_REQUIRE(Draw(bound, child, std::byte{ 0x77 }));

	const Wayland::WlSubsurface role = subcompositor.GetSubsurface(child.Surface, toplevel.Drawn.Surface);
	GYRO_REQUIRE(role.IsValid());

	child.Surface.Attach(child.Buffer, 0, 0);
	child.Surface.Commit();

	// **Naming the parent surface itself**, which is the request the container shape exists for: a
	// child may sit under its parent's pixels, and a preorder run whose sibling order is the z order
	// has no other way to say it.
	role.PlaceBelow(toplevel.Drawn.Surface);

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	const std::vector<EntityId> children = Children(pair.Store, WindowId(pair.Store));
	GYRO_REQUIRE_EQ(children.size(), std::size_t{ 2 });

	// The child first, so it is painted first and the parent's own pixels land on top of it.
	const Entity* const first = pair.Store.Find(children.front());
	GYRO_REQUIRE(first != nullptr);
	GYRO_CHECK(first->Kind == NodeKind::Container);

	const Entity* const second = pair.Store.Find(children.back());
	GYRO_REQUIRE(second != nullptr);
	GYRO_CHECK(second->Kind == NodeKind::Image);

	// And back over the top, which is the same edit in the other direction.
	role.PlaceAbove(toplevel.Drawn.Surface);

	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	const std::vector<EntityId> restacked = Children(pair.Store, WindowId(pair.Store));
	GYRO_REQUIRE_EQ(restacked.size(), std::size_t{ 2 });
	GYRO_CHECK(pair.Store.Find(restacked.front())->Kind == NodeKind::Image);
	GYRO_CHECK(pair.Store.Find(restacked.back())->Kind == NodeKind::Container);
}

GYRO_TEST(ProtocolRoundTrip, AWindowThatUnmapsTakesItsSubsurfacesWithIt)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-subunmap" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	const Wayland::WlSubcompositor subcompositor = Subcompositor(bound);
	GYRO_REQUIRE(subcompositor.IsValid());

	Toplevel toplevel;
	GYRO_REQUIRE(Mapped(pair, bound, toplevel, std::byte{ 0x33 }));

	DrawnSurface child;
	GYRO_REQUIRE(Draw(bound, child, std::byte{ 0x77 }));

	const Wayland::WlSubsurface role = subcompositor.GetSubsurface(child.Surface, toplevel.Drawn.Surface);
	GYRO_REQUIRE(role.IsValid());

	child.Surface.Attach(child.Buffer, 0, 0);
	child.Surface.Commit();
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK_EQ(pair.Store.Count(), std::uint32_t{ 6 });

	// The window takes itself off the screen, and the part of it a subsurface holds cannot stay: a
	// rectangle of an application that is no longer showing anything is worse than nothing at all,
	// because a person can click on it.
	const std::vector<EntityId> children = Children(pair.Store, WindowId(pair.Store));
	GYRO_REQUIRE_EQ(children.size(), std::size_t{ 2 });

	const EntityId subsurface = children.back();

	toplevel.Drawn.Surface.Attach({}, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// **Retiring rather than gone**, per decision 114 and for the window's own reason: the subtree
	// keeps its links and its place while whatever is still moving on it finishes, and the store frees
	// it on the pass its last channel settles. What matters here is that the child is in that state
	// too, rather than being a live node hanging off a dead window.
	const Entity* const under = pair.Store.Find(subsurface);
	GYRO_REQUIRE(under != nullptr);
	GYRO_CHECK(under->Retiring);

	// And the child maps again with the window, rather than being an object the client has to rebuild.
	// The window negotiates from the start — a configure, an ack, then a buffer — which is what
	// unmapping cost it; the subsurface negotiates nothing and is simply there again.
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	// Four more on top of the six that are retiring: a window and its pixels, and the subsurface's own
	// pair under them. The client rebuilt no objects and gyro reused no nodes — the ones that went down
	// are still on their way out, which is decision 114's whole point.
	GYRO_CHECK_EQ(pair.Store.Count(), std::uint32_t{ 10 });
}

namespace
{
// What the compositor tells a client about a menu. **A position as well as a size**, which is the one
// thing gyro answers *where* to: a toplevel is told `0 x 0` for pick your own, and a popup is told
// exactly where it goes, because only the compositor knows where the parent is and where the screen
// ends.
class PopupEvents final : public Wayland::XdgPopupListener
{
public:
	void OnConfigure(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override
	{
		X = x;
		Y = y;
		Width = width;
		Height = height;

		++Configured;
	}

	void OnPopupDone() override { ++Done; }

	void OnRepositioned(std::uint32_t token) override
	{
		Token = token;

		++Repositioned;
	}

	std::int32_t X = -1;
	std::int32_t Y = -1;
	std::int32_t Width = -1;
	std::int32_t Height = -1;
	std::uint32_t Configured = 0;
	std::uint32_t Done = 0;
	std::uint32_t Repositioned = 0;
	std::uint32_t Token = 0;
};

// A client's menu, one step at a time for the same reason `Toplevel` is: the positioner is built
// before the popup exists and a test has to be able to stop between the two.
struct Menu
{
	DrawnSurface Drawn;
	ShellEvents SurfaceEvents;
	PopupEvents Events;
	Wayland::XdgPositioner Rules;
	Wayland::XdgSurface XdgSurface;
	Wayland::XdgPopup Popup;
};

// A window at a size of its own choosing, which every case below needs and `Mapped` cannot give: the
// harness buffer is sixteen by eight, and an anchor rectangle on a window that small says nothing
// about the edge of a screen. **Real pixels rather than a declared geometry over the small buffer**,
// because a person has to be able to press the middle of it — `Scene/Hit.h` asks what is drawn there,
// and a window that claims four hundred and paints sixteen is transparent to the pointer.
[[nodiscard]] bool
Sized(Pair& pair, BoundCompositor& bound, Toplevel& toplevel, std::byte fill, std::int32_t width, std::int32_t height)
{
	const std::int32_t stride = width * 4;
	const auto bytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);

	ClientPool pool{ bytes, true };

	if (!pool.Descriptor.IsValid())
	{
		return false;
	}

	pool.Fill(fill);

	DrawnSurface& drawn = toplevel.Drawn;

	drawn.Surface = bound.Compositor.CreateSurface(drawn.Events);
	drawn.Pool = bound.Shm.CreatePool(pool.Take(), static_cast<std::int32_t>(bytes));

	if (!drawn.Surface.IsValid() || !drawn.Pool.IsValid())
	{
		return false;
	}

	drawn.Buffer = drawn.Pool.CreateBuffer(0, width, height, stride, Wayland::WlShmFormat::Argb8888, drawn.Released);
	toplevel.XdgSurface = bound.Shell.GetXdgSurface(drawn.Surface, toplevel.SurfaceEvents);

	if (!drawn.Buffer.IsValid() || !toplevel.XdgSurface.IsValid())
	{
		return false;
	}

	toplevel.Window = toplevel.XdgSurface.GetToplevel(toplevel.WindowEvents);

	if (!toplevel.Window.IsValid())
	{
		return false;
	}

	drawn.Surface.Commit();

	pair.Turn();

	if (toplevel.SurfaceEvents.Serial == 0)
	{
		return false;
	}

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	drawn.Surface.Attach(drawn.Buffer, 0, 0);
	drawn.Surface.Commit();

	pair.Turn();

	return true;
}

// The desk these cases are measured on, and it is small on purpose: a menu has to reach the edge of a
// screen for anything about constraining to be visible, and a 1920 panel with a 400 window on it
// leaves so much room that every case below would agree.
constexpr double DeskWidth = 800.0;
constexpr double DeskHeight = 600.0;

// The window a person is holding, and the numbers the arithmetic in each case is written against: it
// is centred, so its origin is (200, 150) and the right-hand edge of the desk is 600 in its own
// coordinates.
constexpr std::int32_t WindowWidth = 400;
constexpr std::int32_t WindowHeight = 300;

constexpr std::int32_t MenuWidth = 200;
constexpr std::int32_t MenuHeight = 100;

// The positioner every case below starts from: a menu two hundred by one hundred hanging off the
// right of a one-pixel control, which is a toolkit opening a submenu. The caller states where that
// control is and what it will accept at the edge of a screen, because that is the whole of what these
// cases differ in.
[[nodiscard]] Wayland::XdgPositioner Rules(BoundCompositor& bound, std::int32_t anchorX)
{
	const Wayland::XdgPositioner rules = bound.Shell.CreatePositioner();

	if (!rules.IsValid())
	{
		return rules;
	}

	rules.SetSize(MenuWidth, MenuHeight);
	rules.SetAnchorRect(anchorX, 100, 1, 1);
	rules.SetAnchor(Wayland::XdgPositionerAnchor::Right);
	rules.SetGravity(Wayland::XdgPositionerGravity::Right);
	rules.SetConstraintAdjustment(Wayland::XdgPositionerConstraintAdjustment::SlideX);

	return rules;
}

// The popup taken as far as a configure, which is where its position arrives. Stops there rather than
// mapping, because two of the cases below are only about the number in that event.
[[nodiscard]] bool Raise(Pair& pair, BoundCompositor& bound, Toplevel& toplevel, Menu& menu, std::byte fill)
{
	if (!menu.Rules.IsValid() || !Draw(bound, menu.Drawn, fill))
	{
		return false;
	}

	menu.XdgSurface = bound.Shell.GetXdgSurface(menu.Drawn.Surface, menu.SurfaceEvents);

	if (!menu.XdgSurface.IsValid())
	{
		return false;
	}

	menu.Popup = menu.XdgSurface.GetPopup(toplevel.XdgSurface, menu.Rules, menu.Events);

	if (!menu.Popup.IsValid())
	{
		return false;
	}

	menu.Drawn.Surface.Commit();

	pair.Turn();

	return menu.SurfaceEvents.Serial != 0;
}

// The rest of the sequence, for the one case that needs the menu on screen: a menu gyro is not
// drawing is one `SyncPopups` walks past.
[[nodiscard]] bool Show(Pair& pair, Menu& menu)
{
	menu.XdgSurface.AckConfigure(menu.SurfaceEvents.Serial);
	menu.Drawn.Surface.Attach(menu.Drawn.Buffer, 0, 0);
	menu.Drawn.Surface.Commit();

	pair.Turn();

	return true;
}

} // namespace

// **Where a menu goes, end to end**, which nothing else asserts: `Protocol/Positioner.Test.cpp` checks
// the arithmetic against rectangles a test wrote, and this checks that the rectangles it is given are
// the ones the client described — the anchor in the parent's own coordinates, and a position that
// comes back in them too. A compositor that carried the anchor into global space and answered in it
// would pass every arithmetic case and open every menu the width of the desk away from its control.
GYRO_TEST(ProtocolRoundTrip, AMenuIsPlacedWhereItsPositionerAskedAndHangsUnderItsWindow)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-menu" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { DeskWidth, DeskHeight } }, .Density = Scale::FromInteger(1), .Grid = { 800, 600 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel toplevel;
	GYRO_REQUIRE(Sized(pair, bound, toplevel, std::byte{ 0x40 }, WindowWidth, WindowHeight));

	Menu menu;
	menu.Rules = Rules(bound, 300);

	GYRO_REQUIRE(Raise(pair, bound, toplevel, menu, std::byte{ 0x80 }));

	// Hanging off the right of a control at 300 that is one pixel wide, so the near edge is at 301 —
	// and vertically centred on it, which is half the menu above the control's own middle.
	GYRO_CHECK_EQ(menu.Events.Configured, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(menu.Events.X, 301);
	GYRO_CHECK_EQ(menu.Events.Y, 100 - (MenuHeight / 2));
	GYRO_CHECK_EQ(menu.Events.Width, MenuWidth);
	GYRO_CHECK_EQ(menu.Events.Height, MenuHeight);

	// **The size is not negotiable and the position is not a suggestion**, so the `xdg_surface.configure`
	// that closes the sequence is the client's cue to draw — and only then is there a menu in the world.
	GYRO_REQUIRE(!WindowId(pair.Store).IsNull());
	GYRO_CHECK_EQ(Children(pair.Store, WindowId(pair.Store)).size(), std::size_t{ 1 });

	GYRO_REQUIRE(Show(pair, menu));

	// Under the window rather than under the floor, which is what makes a menu move with the window it
	// belongs to, draw over it, and not be clipped by it.
	GYRO_CHECK_EQ(Children(pair.Store, WindowId(pair.Store)).size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(menu.Events.Configured, std::uint32_t{ 1 });
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// **A menu opened in the middle of a drag, which is the one case `set_parent_size` exists for.** The
// client is answering a configure it has not drawn yet: it has been asked for a wider window, it says
// so in the positioner, and the anchor rectangle it gives is measured on that wider window rather than
// on the one currently on screen. Decision 166 holds the edge a person is *not* holding, so a window
// pulled by its left edge has an origin that moves with every size its client produces — and the
// screen, in that window's coordinates, moves the opposite way.
//
// What it looks like when this is not done is a menu that flips or slides on the frame it opens and
// jumps back on the frame the client redraws, which is the single most visible artefact a menu can
// have: it lands somewhere other than where the hand is.
GYRO_TEST(ProtocolRoundTrip, AMenuOpenedMidResizeIsPlacedAgainstTheSizeItsClientNamed)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-menu-ahead" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { DeskWidth, DeskHeight } }, .Density = Scale::FromInteger(1), .Grid = { 800, 600 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(pair, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Sized(pair, bound, toplevel, std::byte{ 0x40 }, WindowWidth, WindowHeight));

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);

	pair.Turn();

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);

	Push(pair, middle.X, middle.Y);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	(*pair.Host)->OnPointerButton(Click(0x110, true, pair.Clock.Now()));

	pair.Turn();

	GYRO_REQUIRE(pointer.Listener.ButtonSerial != 0);

	toplevel.Window.Resize(keyboard.Seat, pointer.Listener.ButtonSerial, Wayland::XdgToplevelResizeEdge::Left);

	pair.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);

	// A hundred pixels of the near edge pulled away from the far one, which the client has been asked
	// for and has not drawn.
	constexpr std::int32_t Grown = 100;

	Push(pair, -static_cast<double>(Grown), 0.0);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	GYRO_REQUIRE(toplevel.WindowEvents.Width == WindowWidth + Grown);

	// The control is 450 across a window the client says will be 500 wide, so the menu wants to be at
	// 451 and stretch to 651.
	Menu menu;
	menu.Rules = Rules(bound, 450);

	menu.Rules.SetParentSize(WindowWidth + Grown, WindowHeight);
	menu.Rules.SetParentConfigure(toplevel.SurfaceEvents.Serial);

	GYRO_REQUIRE(Raise(pair, bound, toplevel, menu, std::byte{ 0x80 }));

	// **Which fits, and only against the window the client is about to draw.** Against the one on
	// screen the desk ends at 600 in the window's coordinates and the menu would slide back to 400; the
	// wider window's origin is a hundred further left, so the same desk edge is at 700 and nothing has
	// to move. The three numbers are distinct on purpose — a shift applied the wrong way round lands on
	// neither.
	GYRO_CHECK_EQ(menu.Events.Configured, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(menu.Events.X, 451);
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// **The other half of the same rule, and it is the half that was wrong first.** A positioner carries a
// serial rather than a promise, and a client that has already acknowledged that configure is no longer
// ahead of the world — it is describing a size it has agreed to and may already have drawn. Adjusting
// for it anyway is an offset that keeps growing for as long as a person drags, which on screen is a
// menu walking away from the window it belongs to.
//
// Mutter is the authority here and it is a list rather than a comparison: the configure has to still be
// unacknowledged. Serials only go up, so the newest ack is the whole of the lower bound.
GYRO_TEST(ProtocolRoundTrip, AMenuQuotingAConfigureItsClientAnsweredIsPlacedAgainstTheWindowAsItStands)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-menu-behind" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { DeskWidth, DeskHeight } }, .Density = Scale::FromInteger(1), .Grid = { 800, 600 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(pair, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Sized(pair, bound, toplevel, std::byte{ 0x40 }, WindowWidth, WindowHeight));

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);

	pair.Turn();

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);

	Push(pair, middle.X, middle.Y);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	(*pair.Host)->OnPointerButton(Click(0x110, true, pair.Clock.Now()));

	pair.Turn();

	GYRO_REQUIRE(pointer.Listener.ButtonSerial != 0);

	toplevel.Window.Resize(keyboard.Seat, pointer.Listener.ButtonSerial, Wayland::XdgToplevelResizeEdge::Left);

	pair.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);

	constexpr std::int32_t Grown = 100;

	Push(pair, -static_cast<double>(Grown), 0.0);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	GYRO_REQUIRE(toplevel.WindowEvents.Width == WindowWidth + Grown);

	// The whole of the difference from the case above: the client answers the configure it is about to
	// quote. It has said what it will be and it has not drawn it, which is a client gyro must not run
	// ahead of twice.
	const std::uint32_t answered = toplevel.SurfaceEvents.Serial;

	toplevel.XdgSurface.AckConfigure(answered);

	pair.Turn();

	Menu menu;
	menu.Rules = Rules(bound, 450);

	menu.Rules.SetParentSize(WindowWidth + Grown, WindowHeight);
	menu.Rules.SetParentConfigure(answered);

	GYRO_REQUIRE(Raise(pair, bound, toplevel, menu, std::byte{ 0x80 }));

	// So the desk ends at 600 in the window's own coordinates, the menu at 451 would run 51 past it, and
	// sliding is what the client said it would accept.
	GYRO_CHECK_EQ(menu.Events.Configured, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(menu.Events.X, 600 - MenuWidth);
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// **`set_reactive`, which is a menu that stays put while the window under it does not.** A client that
// asks for it is saying *keep this where I put it relative to my window* — and the case a person sees
// is a window dragged towards the edge of a screen with a menu already open on it: without the
// recompute the menu goes off the side of the desk and the items on it become unclickable.
GYRO_TEST(ProtocolRoundTrip, AReactiveMenuIsPlacedAgainWhenItsWindowMovesUnderIt)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-menu-reactive" };
	GYRO_REQUIRE(pair.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { DeskWidth, DeskHeight } }, .Density = Scale::FromInteger(1), .Grid = { 800, 600 } } };
	pair.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(pair, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Sized(pair, bound, toplevel, std::byte{ 0x40 }, WindowWidth, WindowHeight));

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);

	pair.Turn();

	Menu menu;
	menu.Rules = Rules(bound, 300);

	menu.Rules.SetReactive();

	GYRO_REQUIRE(Raise(pair, bound, toplevel, menu, std::byte{ 0x80 }));
	GYRO_REQUIRE(Show(pair, menu));

	// Room to spare: the menu runs to 501 and the desk ends at 600.
	GYRO_CHECK_EQ(menu.Events.Configured, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(menu.Events.X, 301);

	const Point<GlobalSpace> middle = MiddleOfTheWindow(pair.Store);

	Push(pair, middle.X, middle.Y);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	(*pair.Host)->OnPointerButton(Click(0x110, true, pair.Clock.Now()));

	pair.Turn();

	GYRO_REQUIRE(pointer.Listener.ButtonSerial != 0);

	toplevel.Window.Move(keyboard.Seat, pointer.Listener.ButtonSerial);

	pair.Turn();

	// A hundred and fifty to the right, which takes the window's origin to 350 and leaves 450 of desk in
	// front of it — fifty less than the menu needs.
	Push(pair, 150.0, 0.0);
	(*pair.Host)->OnPointerMotion(PointerMotion{ .When = pair.Clock.Now() });

	pair.Turn();

	GYRO_CHECK_EQ(menu.Events.Configured, std::uint32_t{ 2 });
	GYRO_CHECK_EQ(menu.Events.X, 450 - MenuWidth);

	// **And it stops when nothing more changes**, which is what makes this a recompute rather than a
	// per-iteration event: a menu reconfigured on every wakeup is a toolkit redrawing on every wakeup.
	pair.Turn();
	pair.Turn();

	GYRO_CHECK_EQ(menu.Events.Configured, std::uint32_t{ 2 });
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

// The windows on this machine, described to a client that did not open them.
//
// **This is the first global an application never sees**, so half of what is asserted here is a
// negative: an ordinary connection's registry does not carry it, which is Protocol/Tier.h's filter
// doing the only thing it exists for. The other half is the enumeration itself, and the case worth
// having a wire test for at all is the *ordering* — the protocol tells a client to bind and roundtrip,
// and a compositor that announced its windows a moment later would leave every taskbar on the machine
// empty until somebody opened a window.

namespace
{
// One `ext_foreign_toplevel_handle_v1`, recording what the compositor said about a window. Everything
// here is what a switcher would draw.
class ForeignHandleEvents final : public Wayland::ExtForeignToplevelHandleV1Listener
{
public:
	void OnIdentifier(std::string_view identifier) override { Identifier = std::string{ identifier }; }

	void OnTitle(std::string_view title) override
	{
		Title = std::string{ title };
		++Titles;
	}

	void OnAppId(std::string_view appId) override
	{
		AppId = std::string{ appId };
		++AppIds;
	}

	void OnDone() override { ++Done; }

	void OnClosed() override { ++Closed; }

	std::string Identifier;
	std::string Title;
	std::string AppId;

	// Counted rather than only recorded, because the failure that hides behind a correct title is a
	// `done` per dispatch iteration: every switcher on the machine re-laying out its list on every
	// wakeup, with nothing on screen to say so.
	std::uint32_t Titles = 0;
	std::uint32_t AppIds = 0;
	std::uint32_t Done = 0;
	std::uint32_t Closed = 0;
};

// One `ext_foreign_toplevel_list_v1`. Owns the handles it is given, since the protocol mints them and
// the client's only say in the matter is when to destroy them.
class ForeignListEvents final : public Wayland::ExtForeignToplevelListV1Listener
{
public:
	Wayland::ExtForeignToplevelHandleV1Listener* OnToplevel(Wayland::ExtForeignToplevelHandleV1 toplevel) override
	{
		Handles.push_back(std::make_unique<ForeignHandleEvents>());
		Objects.push_back(toplevel);

		return Handles.back().get();
	}

	void OnFinished() override { ++Finished; }

	std::vector<std::unique_ptr<ForeignHandleEvents>> Handles;
	std::vector<Wayland::ExtForeignToplevelHandleV1> Objects;
	std::uint32_t Finished = 0;
};

[[nodiscard]] Wayland::ExtForeignToplevelListV1 BindForeign(BoundCompositor& bound, ForeignListEvents& events)
{
	const Registry::Global* const global = bound.Listener.Find(Wayland::ExtForeignToplevelListV1::WireName);

	return global != nullptr ?
	           bound.Listener.Object().Bind<Wayland::ExtForeignToplevelListV1>(global->Name, global->Version, events) :
	           Wayland::ExtForeignToplevelListV1{};
}
} // namespace

GYRO_TEST(ProtocolRoundTrip, AnApplicationIsNotOfferedTheWindowList)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-foreign-user" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	// The whole of Protocol/Tier.h from the far side of the socket: the interface is advertised in the
	// display and simply is not in this client's registry. The shell's globals are the ones that must
	// never arrive by accident, and this is the only place that can say they did not.
	GYRO_CHECK(bound.Listener.Find(Wayland::ExtForeignToplevelListV1::WireName) == nullptr);

	// The rest of the registry is untouched, which is the failure a filter is most likely to have: an
	// application that lost its shell or its shared memory alongside the global it was never meant to
	// have would be a compositor no window opens on.
	GYRO_CHECK(bound.Listener.Find(Wayland::WlCompositor::WireName) != nullptr);
	GYRO_CHECK(bound.Listener.Find(Wayland::XdgWmBase::WireName) != nullptr);
	GYRO_CHECK(bound.Listener.Find(Wayland::WlSeat::WireName) != nullptr);
}

GYRO_TEST(ProtocolRoundTrip, AShellIsToldAboutTheWindowsThatAreAlreadyOpen)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-foreign-open", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel window;
	GYRO_REQUIRE(Show(pair, bound, window, std::byte{ 0x40 }));

	window.Window.SetTitle("Editing Decisions.md");
	window.Window.SetAppId("com.example.editor");

	pair.Turn();

	// **Bound and answered in one turn, which is the ordering the protocol asks for.** A client binds
	// this global and roundtrips, and the reply to that roundtrip is queued by libwayland while the bind
	// is still being dispatched — so an announcement deferred to the end of gyro's iteration would
	// arrive after the client had already concluded there were no windows.
	ForeignListEvents events;
	const Wayland::ExtForeignToplevelListV1 list = BindForeign(bound, events);

	GYRO_REQUIRE(list.IsValid());

	pair.Turn();

	GYRO_REQUIRE(events.Handles.size() == 1);

	const ForeignHandleEvents& handle = *events.Handles.front();

	GYRO_CHECK(handle.Title == "Editing Decisions.md");
	GYRO_CHECK(handle.AppId == "com.example.editor");
	GYRO_CHECK(!handle.Identifier.empty());

	// The `done` that applies them, and exactly one: the three properties of a window a client has just
	// been introduced to are one atomic state rather than three.
	GYRO_CHECK(handle.Done == 1);
	GYRO_CHECK(handle.Closed == 0);
}

GYRO_TEST(ProtocolRoundTrip, AWindowThatOpensIsAnnouncedAndOneThatGoesIsClosed)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-foreign-life", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	ForeignListEvents events;
	const Wayland::ExtForeignToplevelListV1 list = BindForeign(bound, events);

	GYRO_REQUIRE(list.IsValid());

	pair.Turn();

	// Nothing yet, which is worth asserting rather than assuming: a list that announced a window before
	// one existed would pass every check below.
	GYRO_CHECK(events.Handles.empty());

	{
		Toplevel window;
		GYRO_REQUIRE(Show(pair, bound, window, std::byte{ 0x40 }));

		pair.Turn();

		GYRO_REQUIRE(events.Handles.size() == 1);
		GYRO_CHECK(events.Handles.front()->Closed == 0);

		// A window a person closes: the role object goes, which unmaps it whatever the surface still has
		// attached.
		window.Window.Destroy();
	}

	pair.Turn();

	GYRO_REQUIRE(events.Handles.size() == 1);
	GYRO_CHECK(events.Handles.front()->Closed == 1);

	// And nothing after it, ever. The protocol says the compositor sends no further events on a handle
	// it has closed, and the walk that decides is a comparison rather than a hook — so the case that
	// would break it is exactly this one, an iteration later.
	pair.Turn();

	GYRO_CHECK(events.Handles.front()->Closed == 1);
	GYRO_CHECK(events.Handles.size() == 1);
}

GYRO_TEST(ProtocolRoundTrip, ATitleIsSentWhenItChangesAndNotOtherwise)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-foreign-title", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel window;
	GYRO_REQUIRE(Show(pair, bound, window, std::byte{ 0x40 }));

	window.Window.SetTitle("one");

	pair.Turn();

	ForeignListEvents events;
	GYRO_REQUIRE(BindForeign(bound, events).IsValid());

	pair.Turn();
	GYRO_REQUIRE(events.Handles.size() == 1);

	ForeignHandleEvents& handle = *events.Handles.front();

	GYRO_CHECK(handle.Title == "one");
	GYRO_CHECK(handle.Titles == 1);
	GYRO_CHECK(handle.Done == 1);

	// **A window redrawing is not a window being renamed**, which is what the comparison is for: a
	// client that commits every frame produces no traffic here at all, and a switcher that re-laid out
	// its list on every wakeup would be the cost of getting this wrong.
	window.Drawn.Surface.Commit();

	pair.Turn();
	pair.Turn();

	GYRO_CHECK(handle.Titles == 1);
	GYRO_CHECK(handle.Done == 1);

	window.Window.SetTitle("two");

	pair.Turn();

	GYRO_CHECK(handle.Title == "two");
	GYRO_CHECK(handle.Titles == 2);

	// One `done` for the change, and the app id untouched — the event applies whatever moved rather
	// than restating the window.
	GYRO_CHECK(handle.Done == 2);
	GYRO_CHECK(handle.AppIds == 1);
}

GYRO_TEST(ProtocolRoundTrip, StoppingIsAnsweredAtOnceAndLeavesTheHandlesCurrent)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-foreign-stop", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel window;
	GYRO_REQUIRE(Show(pair, bound, window, std::byte{ 0x40 }));

	window.Window.SetTitle("before");

	pair.Turn();

	ForeignListEvents events;
	const Wayland::ExtForeignToplevelListV1 list = BindForeign(bound, events);

	GYRO_REQUIRE(list.IsValid());

	pair.Turn();
	GYRO_REQUIRE(events.Handles.size() == 1);

	list.Stop();

	pair.Turn();

	// **`finished` comes back in the turn the request went out in**, because the protocol's own sequence
	// has the client wait for it before destroying anything — a compositor that answered later is one
	// every client of it stalls behind.
	GYRO_CHECK(events.Finished == 1);

	// A second window is not announced, which is the whole of what `stop` asks for.
	Toplevel second;
	GYRO_REQUIRE(Show(pair, bound, second, std::byte{ 0x80 }));

	pair.Turn();

	GYRO_CHECK(events.Handles.size() == 1);

	// And the handle the client is still holding goes on being kept current, because `stop` is about the
	// `toplevel` event and says nothing about the rest: a switcher that stopped wanting new entries and
	// then watched its existing labels freeze would be the other reading, and it is not what the request
	// says.
	window.Window.SetTitle("after");

	pair.Turn();

	GYRO_CHECK(events.Handles.front()->Title == "after");
}

namespace
{
// One `gyro_binding_v1`, recording the presses it was told about and when they happened.
class BindingEvents final : public Wayland::GyroBindingV1Listener
{
public:
	void OnPressed(std::uint32_t tvSecHi, std::uint32_t tvSecLo, std::uint32_t tvNsec) override
	{
		++Presses;

		Nanoseconds =
			static_cast<std::int64_t>(((static_cast<std::uint64_t>(tvSecHi) << 32U) | tvSecLo) * 1'000'000'000U) +
			static_cast<std::int64_t>(tvNsec);
	}

	std::uint32_t Presses = 0;
	std::int64_t Nanoseconds = 0;
};

// A shell, a keyboard and a window on one connection, which is not how a session is arranged and is
// what makes the assertion sharp: one client, one seat, and a key that has to arrive on one object
// and not the other.
struct Shell
{
	Wayland::GyroBindingsV1 Manager;
	BindingEvents ChordEvents;
	Wayland::GyroBindingV1 Chord;
	Keyboard Keys;
	Toplevel Window;
};

[[nodiscard]] bool Claim(Pair& pair, BoundCompositor& bound, Shell& shell, Wayland::GyroBindingsV1Modifier modifiers)
{
	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	if (!Listen(pair, bound, shell.Keys) || !Show(pair, bound, shell.Window, std::byte{ 0x40 }))
	{
		return false;
	}

	const Registry::Global* const global = bound.Listener.Find(Wayland::GyroBindingsV1::WireName);

	if (global == nullptr)
	{
		return false;
	}

	// No listener, because the manager declares no events — the generated `Bind` has an overload for
	// exactly that rather than a stub the call site has to invent.
	shell.Manager = bound.Listener.Object().Bind<Wayland::GyroBindingsV1>(global->Name, global->Version);

	if (!shell.Manager.IsValid())
	{
		return false;
	}

	shell.Chord = shell.Manager.Claim(modifiers, XKB_KEY_space, shell.ChordEvents);

	pair.Turn();

	return shell.Chord.IsValid() && shell.Keys.Listener.Entered == 1;
}
} // namespace

GYRO_TEST(ProtocolRoundTrip, AnApplicationCannotClaimAKey)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-bindings-user" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	// The tier from the far side of the socket, and this is the global whose leak would be the worst of
	// the two gyro serves a shell: an application handed it takes a chord out of every other
	// application's reach across the whole machine, and nothing on screen says where the keystroke went.
	GYRO_CHECK(bound.Listener.Find(Wayland::GyroBindingsV1::WireName) == nullptr);

	// The rest of the registry is untouched, which is the failure a filter is most likely to have.
	GYRO_CHECK(bound.Listener.Find(Wayland::WlSeat::WireName) != nullptr);
	GYRO_CHECK(bound.Listener.Find(Wayland::XdgWmBase::WireName) != nullptr);
}

GYRO_TEST(ProtocolRoundTrip, AClaimedChordReachesTheShellAndNotTheWindow)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-bindings-claim", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Shell shell;
	GYRO_REQUIRE(Claim(pair, bound, shell, Wayland::GyroBindingsV1Modifier::Super));

	const Instant when = pair.Clock.Now();

	// **The modifier is pressed and counted before the chord is**, because `Super` is itself a key the
	// window hears about — a chord swallows the key it names and never the modifiers held over it, which
	// is why the baseline is taken here rather than above.
	(*pair.Host)->OnKey(Press(KEY_LEFTMETA, true, when), false);

	pair.Turn();

	const std::uint32_t before = shell.Keys.Listener.Keys;

	(*pair.Host)->OnKey(Press(KEY_SPACE, true, when), false);
	(*pair.Host)->OnKey(Press(KEY_SPACE, false, when), false);

	pair.Turn();

	GYRO_CHECK_EQ(shell.ChordEvents.Presses, std::uint32_t{ 1 });

	// **And the window received nothing**, which is the half a shell cannot check for itself. A chord
	// that let the key through would open a launcher and type a space into whatever was behind it — and
	// the release is swallowed with it, because a client that received only half a keystroke is a client
	// holding a key down forever.
	GYRO_CHECK_EQ(shell.Keys.Listener.Keys, before);

	// **The modifier still went to the window**, and that is not an oversight. What is held down is a
	// fact about a person's hands: a client told `Super` went down and never told it came up would read
	// every keystroke afterwards as a shortcut.
	GYRO_CHECK(shell.Keys.Listener.Modifiers > 0);

	// The instant is the device's own, at nanosecond resolution — which is the point of the event rather
	// than a detail of it, since it is the origin a shell stamps the animation it starts with.
	GYRO_CHECK(shell.ChordEvents.Nanoseconds == Monotonic::ToNanoseconds(when));
	GYRO_CHECK(!pair.Client.Fault().has_value());

	(*pair.Host)->OnKey(Press(KEY_LEFTMETA, false, pair.Clock.Now()), false);
}

GYRO_TEST(ProtocolRoundTrip, AChordIsNotTheSameKeyWithAnotherModifierOnIt)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-bindings-exact", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Shell shell;
	GYRO_REQUIRE(Claim(pair, bound, shell, Wayland::GyroBindingsV1Modifier::Super));

	(*pair.Host)->OnKey(Press(KEY_LEFTMETA, true, pair.Clock.Now()), false);
	(*pair.Host)->OnKey(Press(KEY_LEFTCTRL, true, pair.Clock.Now()), false);

	pair.Turn();

	const std::uint32_t before = shell.Keys.Listener.Keys;

	(*pair.Host)->OnKey(Press(KEY_SPACE, true, pair.Clock.Now()), false);
	(*pair.Host)->OnKey(Press(KEY_SPACE, false, pair.Clock.Now()), false);

	pair.Turn();

	// A subset match would be a shell shadowing every chord that contains one it claimed, and the
	// application that wanted `Ctrl+Super+Space` would never receive a keystroke its author is sure it
	// sent.
	GYRO_CHECK_EQ(shell.ChordEvents.Presses, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(shell.Keys.Listener.Keys, before + 2);

	(*pair.Host)->OnKey(Press(KEY_LEFTCTRL, false, pair.Clock.Now()), false);
	(*pair.Host)->OnKey(Press(KEY_LEFTMETA, false, pair.Clock.Now()), false);
}

GYRO_TEST(ProtocolRoundTrip, AChordsReleaseIsSwallowedEvenWhenTheHandLetGoOfTheModifierFirst)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-bindings-release", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Shell shell;
	GYRO_REQUIRE(Claim(pair, bound, shell, Wayland::GyroBindingsV1Modifier::Super));

	(*pair.Host)->OnKey(Press(KEY_LEFTMETA, true, pair.Clock.Now()), false);

	pair.Turn();

	const std::uint32_t before = shell.Keys.Listener.Keys;

	(*pair.Host)->OnKey(Press(KEY_SPACE, true, pair.Clock.Now()), false);

	// **`Super` comes up before `Space` does**, which is what a hand does about half the time — and it is
	// the case a release matched against the chord again would get wrong, because by then the modifiers
	// no longer say `Super`. The window would receive a release for a press it never saw, which every
	// toolkit turns into a key stuck down.
	(*pair.Host)->OnKey(Press(KEY_LEFTMETA, false, pair.Clock.Now()), false);
	(*pair.Host)->OnKey(Press(KEY_SPACE, false, pair.Clock.Now()), false);

	pair.Turn();

	GYRO_CHECK_EQ(shell.ChordEvents.Presses, std::uint32_t{ 1 });

	// One, and it is the `Super` release rather than the `Space` one: a modifier is never swallowed,
	// because what a person is holding is a fact the window has to be able to reconcile.
	GYRO_CHECK_EQ(shell.Keys.Listener.Keys, before + 1);
	GYRO_CHECK_EQ(shell.Keys.Listener.LastKey, std::uint32_t{ KEY_LEFTMETA });
}

GYRO_TEST(ProtocolRoundTrip, AChordTheShellGivesUpGoesBackToTheWindow)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-bindings-drop", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Shell shell;
	GYRO_REQUIRE(Claim(pair, bound, shell, Wayland::GyroBindingsV1Modifier::Super));

	shell.Chord.Destroy();

	(*pair.Host)->OnKey(Press(KEY_LEFTMETA, true, pair.Clock.Now()), false);

	pair.Turn();

	const std::uint32_t before = shell.Keys.Listener.Keys;

	(*pair.Host)->OnKey(Press(KEY_SPACE, true, pair.Clock.Now()), false);
	(*pair.Host)->OnKey(Press(KEY_SPACE, false, pair.Clock.Now()), false);

	pair.Turn();

	// The key comes back the moment the shell gives it up. A binding whose object is gone and whose chord
	// is still swallowed would be a key that stops working for the rest of the session with nothing to
	// point at.
	GYRO_CHECK_EQ(shell.ChordEvents.Presses, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(shell.Keys.Listener.Keys, before + 2);

	(*pair.Host)->OnKey(Press(KEY_LEFTMETA, false, pair.Clock.Now()), false);
}

GYRO_TEST(ProtocolRoundTrip, DroppingTheManagerLeavesTheChordsItClaimed)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-bindings-manager", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Shell shell;
	GYRO_REQUIRE(Claim(pair, bound, shell, Wayland::GyroBindingsV1Modifier::Super));

	// A shell that has claimed everything it wants is entitled to drop the factory, and a chord that
	// stopped working because of it would be a protocol nobody could use the way it reads.
	shell.Manager.Destroy();

	pair.Turn();

	(*pair.Host)->OnKey(Press(KEY_LEFTMETA, true, pair.Clock.Now()), false);
	(*pair.Host)->OnKey(Press(KEY_SPACE, true, pair.Clock.Now()), false);
	(*pair.Host)->OnKey(Press(KEY_SPACE, false, pair.Clock.Now()), false);

	pair.Turn();

	GYRO_CHECK_EQ(shell.ChordEvents.Presses, std::uint32_t{ 1 });
	GYRO_CHECK(!pair.Client.Fault().has_value());

	(*pair.Host)->OnKey(Press(KEY_LEFTMETA, false, pair.Clock.Now()), false);
}

GYRO_TEST(ProtocolRoundTrip, TheCompositorsOwnKeysAreNotAShellsToClaim)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-bindings-leader", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Shell shell;
	GYRO_REQUIRE(Claim(pair, bound, shell, Wayland::GyroBindingsV1Modifier::Super));

	// A second chord on the leader itself. The composition root feeds every key to `Input::Chord` first
	// and hands this host what is left, already marked — so `Ctrl+Alt+Esc` arrives here consumed, exactly
	// as it does on a real run. gyro is a boot service holding the display with no virtual terminal
	// behind it, and a shell that could take the way out is a shell whose crash takes the machine.
	BindingEvents leader;
	const Wayland::GyroBindingV1 claimed = shell.Manager.Claim(
		Wayland::GyroBindingsV1Modifier::Control | Wayland::GyroBindingsV1Modifier::Alt, XKB_KEY_Escape, leader
	);

	GYRO_REQUIRE(claimed.IsValid());

	pair.Turn();

	(*pair.Host)->OnKey(Press(KEY_LEFTCTRL, true, pair.Clock.Now()), false);
	(*pair.Host)->OnKey(Press(KEY_LEFTALT, true, pair.Clock.Now()), false);
	(*pair.Host)->OnKey(Press(KEY_ESC, true, pair.Clock.Now()), true);

	pair.Turn();

	GYRO_CHECK_EQ(leader.Presses, std::uint32_t{ 0 });

	(*pair.Host)->OnKey(Press(KEY_ESC, false, pair.Clock.Now()), true);
	(*pair.Host)->OnKey(Press(KEY_LEFTALT, false, pair.Clock.Now()), false);
	(*pair.Host)->OnKey(Press(KEY_LEFTCTRL, false, pair.Clock.Now()), false);
}

namespace
{
// The chrome root and what hangs on it (187). Decision 55 makes the *last* root the frontmost, and the
// floor is created first, so this walk and `WindowNode`'s go down the two ends of the top level.
[[nodiscard]] EntityId ChromeRoot(const SceneStore& scene)
{
	EntityId last;

	for (EntityId root = scene.FirstRoot(); !root.IsNull(); root = scene.Find(root)->NextSibling)
	{
		last = root;
	}

	return last;
}

[[nodiscard]] const Entity* ChromeNode(const SceneStore& scene)
{
	const Entity* const root = scene.Find(ChromeRoot(scene));

	return root == nullptr ? nullptr : scene.Find(root->FirstChild);
}

// A shell's surface, declared and then mapped in that order — which is the order the protocol insists
// on, because what being chrome changes is decided when the surface enters the world.
struct Chrome
{
	Wayland::GyroChromeManagerV1 Manager;
	Wayland::GyroChromeV1 Object;
	Toplevel Window;
};

[[nodiscard]] bool BindChrome(BoundCompositor& bound, Chrome& chrome)
{
	const Registry::Global* const global = bound.Listener.Find(Wayland::GyroChromeManagerV1::WireName);

	if (global == nullptr)
	{
		return false;
	}

	// No listener on either interface, because neither declares an event: what a shell learns about its
	// own launcher, it learns through `xdg_toplevel`.
	chrome.Manager = bound.Listener.Object().Bind<Wayland::GyroChromeManagerV1>(global->Name, global->Version);

	return chrome.Manager.IsValid();
}

[[nodiscard]] bool Declare(Pair& pair, BoundCompositor& bound, Chrome& chrome, std::byte fill)
{
	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	pair.Store.SetOutputs(outputs);

	if (!BindChrome(bound, chrome) || !Role(bound, chrome.Window, fill))
	{
		return false;
	}

	chrome.Object = chrome.Manager.GetChrome(chrome.Window.Window);

	if (!chrome.Object.IsValid())
	{
		return false;
	}

	chrome.Window.Drawn.Surface.Commit();

	pair.Turn();

	chrome.Window.XdgSurface.AckConfigure(chrome.Window.SurfaceEvents.Serial);
	chrome.Window.Drawn.Surface.Attach(chrome.Window.Drawn.Buffer, 0, 0);
	chrome.Window.Drawn.Surface.Commit();

	pair.Turn();

	return true;
}
} // namespace

GYRO_TEST(ProtocolRoundTrip, AnApplicationCannotDeclareItselfChrome)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-chrome-user" };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	// The tier from the far side of the socket. An application handed this could draw a window over
	// every other window on the machine, keep it there through every click, and stay out of any list a
	// person could find it in — which is a credential prompt nobody can dismiss or attribute.
	GYRO_CHECK(bound.Listener.Find(Wayland::GyroChromeManagerV1::WireName) == nullptr);

	// The rest of the registry is untouched, which is the failure a filter is most likely to have.
	GYRO_CHECK(bound.Listener.Find(Wayland::XdgWmBase::WireName) != nullptr);
	GYRO_CHECK(bound.Listener.Find(Wayland::WlCompositor::WireName) != nullptr);
}

GYRO_TEST(ProtocolRoundTrip, ALauncherHangsAboveTheWindowsRatherThanAmongThem)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-chrome-above", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel window;
	GYRO_REQUIRE(Show(pair, bound, window, std::byte{ 0x20 }));

	Chrome chrome;
	GYRO_REQUIRE(Declare(pair, bound, chrome, std::byte{ 0x60 }));

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// **The whole of what being chrome does to the scene**: the two surfaces are in different chains.
	// An ordinary window is a child of the floor, which is the first root; the launcher is a child of
	// the chrome root, which is the last — so it is in front of every window of this session, and no
	// reordering inside the floor can reach it.
	const Entity* const ordinary = WindowNode(pair.Store);
	const Entity* const launcher = ChromeNode(pair.Store);

	GYRO_REQUIRE(ordinary != nullptr);
	GYRO_REQUIRE(launcher != nullptr);

	GYRO_CHECK_EQ(ordinary->Parent, pair.Store.FirstRoot());
	GYRO_CHECK_EQ(launcher->Parent, ChromeRoot(pair.Store));
	GYRO_CHECK(ordinary->Parent != launcher->Parent);

	// And it is a window in every other respect — it was configured, it acknowledged, it has pixels
	// under it. Being chrome takes nothing away from xdg-shell, which is the reason this is an object on
	// a toplevel rather than a role of its own.
	GYRO_CHECK(!launcher->FirstChild.IsNull());
	GYRO_CHECK(chrome.Window.SurfaceEvents.Serial != 0);
}

GYRO_TEST(ProtocolRoundTrip, AShellIsNotToldAboutItsOwnLauncher)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-chrome-list", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Toplevel window;
	GYRO_REQUIRE(Show(pair, bound, window, std::byte{ 0x20 }));

	window.Window.SetTitle("Editing Decisions.md");

	Chrome chrome;
	GYRO_REQUIRE(Declare(pair, bound, chrome, std::byte{ 0x60 }));

	chrome.Window.Window.SetTitle("Launcher");

	pair.Turn();

	ForeignListEvents events;
	const Wayland::ExtForeignToplevelListV1 list = BindForeign(bound, events);

	GYRO_REQUIRE(list.IsValid());

	pair.Turn();

	// **One window on this machine, not two.** The client most likely to be holding one of these lists is
	// the same shell that drew the launcher — so without the filter the first thing a switcher shows a
	// person is the switcher, and the close button beside it asks the shell to close itself.
	GYRO_REQUIRE(events.Handles.size() == 1);
	GYRO_CHECK(events.Handles.front()->Title == "Editing Decisions.md");
}

GYRO_TEST(ProtocolRoundTrip, AMaterialArrivesWithThePixelsItWasSentWith)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-chrome-material", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Chrome chrome;
	GYRO_REQUIRE(Declare(pair, bound, chrome, std::byte{ 0x60 }));

	// Nothing asked for, nothing drawn: a surface is `None` until a shell says otherwise, which is what
	// every application's window stays.
	GYRO_REQUIRE(ChromeNode(pair.Store) != nullptr);
	GYRO_CHECK(ChromeNode(pair.Store)->Dress == Material::None);

	chrome.Object.SetMaterial(Wayland::GyroChromeV1Material::Glass);

	pair.Turn();

	// **Nothing yet, and that is the point.** The request landed and was parsed; what has not happened is
	// a `wl_surface.commit`. A material applied at the request would let a shell change what its
	// launcher is made of a frame before it changes what is drawn on it.
	GYRO_CHECK(ChromeNode(pair.Store)->Dress == Material::None);

	chrome.Window.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(ChromeNode(pair.Store)->Dress == Material::Glass);

	// The other direction, because a material that could only be put on would be one a shell could not
	// take off — an overview's dimming has to leave when the overview does.
	chrome.Object.SetMaterial(Wayland::GyroChromeV1Material::None);
	chrome.Window.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(ChromeNode(pair.Store)->Dress == Material::None);
	GYRO_CHECK(!pair.Client.Fault().has_value());
}

GYRO_TEST(ProtocolRoundTrip, AMaterialGyroHasNoNameForEndsTheConnectionRatherThanDrawingNothing)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-chrome-unknown", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Chrome chrome;
	GYRO_REQUIRE(Declare(pair, bound, chrome, std::byte{ 0x60 }));

	// A shell built against a newer version of this protocol than the compositor it is talking to.
	chrome.Object.SetMaterial(static_cast<Wayland::GyroChromeV1Material>(7));

	pair.Turn();

	GYRO_REQUIRE(pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(pair.Client.Fault()->Code, static_cast<std::uint32_t>(Wayland::GyroChromeV1Error::BadMaterial));
}

GYRO_TEST(ProtocolRoundTrip, AWindowAlreadyOnScreenCannotBecomeChrome)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-chrome-late", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Chrome chrome;
	GYRO_REQUIRE(BindChrome(bound, chrome));
	GYRO_REQUIRE(Show(pair, bound, chrome.Window, std::byte{ 0x60 }));

	// **Refused rather than served, because the surface is already in the world.** Honouring it would
	// mean moving a mapped window between two roots and out of two lists while a person was looking at
	// it — the window they clicked a moment ago rising above everything and vanishing from their own
	// switcher, with nothing on screen to say why.
	chrome.Object = chrome.Manager.GetChrome(chrome.Window.Window);

	pair.Turn();

	GYRO_REQUIRE(pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(
		pair.Client.Fault()->Code, static_cast<std::uint32_t>(Wayland::GyroChromeManagerV1Error::AlreadyMapped)
	);
}

GYRO_TEST(ProtocolRoundTrip, ASurfaceIsDeclaredChromeOnceAndOnlyOnce)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-chrome-twice", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Chrome chrome;
	GYRO_REQUIRE(BindChrome(bound, chrome));
	GYRO_REQUIRE(Role(bound, chrome.Window, std::byte{ 0x60 }));

	chrome.Object = chrome.Manager.GetChrome(chrome.Window.Window);

	pair.Turn();

	GYRO_REQUIRE(!pair.Client.Fault().has_value());

	// A second declaration is a shell that has lost track of its own surfaces, and the state it would
	// leave behind is two objects setting the material of one launcher — the last request to arrive
	// winning, which is a flicker nobody can reproduce.
	const Wayland::GyroChromeV1 again = chrome.Manager.GetChrome(chrome.Window.Window);

	pair.Turn();

	GYRO_REQUIRE(pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(
		pair.Client.Fault()->Code, static_cast<std::uint32_t>(Wayland::GyroChromeManagerV1Error::AlreadyChrome)
	);
	GYRO_CHECK(again.IsValid());
}

GYRO_TEST(ProtocolRoundTrip, AChromeObjectCannotBeDroppedWhileItsSurfaceIsOnScreen)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-chrome-drop", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Chrome chrome;
	GYRO_REQUIRE(Declare(pair, bound, chrome, std::byte{ 0x60 }));

	// **The one destroy in gyro's own protocols that can fail.** Chrome is fixed for the life of a
	// surface, so there is nothing to give a mapped one back to: the alternative is a launcher that
	// drops behind the windows and appears in the switcher in the middle of a person typing into it.
	chrome.Object.Destroy();

	pair.Turn();

	GYRO_REQUIRE(pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(pair.Client.Fault()->Code, static_cast<std::uint32_t>(Wayland::GyroChromeV1Error::Mapped));
}

GYRO_TEST(ProtocolRoundTrip, DroppingTheManagerLeavesTheSurfaceItDeclared)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Pair pair{ "gyro-roundtrip-chrome-manager", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Chrome chrome;
	GYRO_REQUIRE(Declare(pair, bound, chrome, std::byte{ 0x60 }));

	chrome.Manager.Destroy();

	pair.Turn();

	// A factory is a factory. The same rule the chord protocol states (186), checked here for the same
	// reason: a shell that tidies up its manager and finds its panel has quietly become an ordinary
	// window would have no way to tell what it did.
	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_REQUIRE(ChromeNode(pair.Store) != nullptr);

	chrome.Object.SetMaterial(Wayland::GyroChromeV1Material::Smoke);
	chrome.Window.Drawn.Surface.Commit();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_CHECK(ChromeNode(pair.Store)->Dress == Material::Smoke);
}

namespace
{
// The newest chrome surface rather than the backmost, which is a distinction only a remap introduces.
// `ChromeNode` above takes the chrome root's *first* child and is right for every test that maps one
// launcher once. A surface taken down and put back is a **new entity appended beside the old one**,
// and the old one goes on being a child of that root for as long as its exit is still running (114) —
// so the launcher a person is actually looking at is the last child, which is also decision 55's
// frontmost.
[[nodiscard]] EntityId NewestChrome(const SceneStore& scene)
{
	const Entity* const root = scene.Find(ChromeRoot(scene));

	return root == nullptr ? EntityId{} : root->LastChild;
}

// The whole of taking a launcher off the screen, and it is the only request a client has for it:
// attach nothing and commit. There is no hide verb in xdg-shell and gyro's chrome protocol does not
// add one, so this is the path a run bar takes on every press of its own chord.
void Hide(Pair& pair, Toplevel& toplevel)
{
	toplevel.Drawn.Surface.Attach(Wayland::WlBuffer{}, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();
}

// And putting it back, which is deliberately **not** a second attach. Unmapping takes the configure
// and the acknowledgement away with the window (Protocol/Shell.cpp), so the client negotiates from
// the start exactly as it did the first time — the extra turn here is that sequence rather than the
// test being impatient, because the commit carrying the buffer maps nothing and asks to be configured.
[[nodiscard]] bool Reveal(Pair& pair, Toplevel& toplevel)
{
	const std::uint32_t before = toplevel.SurfaceEvents.Configured;

	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	// A compositor that mapped on this commit would have skipped the negotiation it just tore down,
	// which is the failure this returns false for rather than asserting on: the caller is what says
	// where it happened.
	if (toplevel.SurfaceEvents.Configured == before)
	{
		return false;
	}

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Commit();

	pair.Turn();

	return true;
}
} // namespace

GYRO_TEST(ProtocolRoundTrip, ALauncherTakenDownAndPutBackIsChromeAgainAndWearsTheMaterialItHad)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	// **The path a run bar is on every single time a person presses its chord**, and the one nothing
	// had walked: chrome is declared once, before the first map and never again, while the surface it
	// was declared on goes up and down all day. Everything that makes a launcher a launcher is decided
	// inside `ClientXdgSurface::Map` — which root it hangs on, which kind of focus it takes, what it is
	// dressed in — so a second map is a second chance to decide all three wrongly, and what a person
	// would see is their launcher coming back as an ordinary window: behind whatever they had open,
	// with its glass gone.
	Pair pair{ "gyro-roundtrip-chrome-again", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Chrome chrome;
	GYRO_REQUIRE(Declare(pair, bound, chrome, std::byte{ 0x60 }));

	chrome.Object.SetMaterial(Wayland::GyroChromeV1Material::Glass);
	chrome.Window.Drawn.Surface.Commit();

	pair.Turn();

	const EntityId first = NewestChrome(pair.Store);
	GYRO_REQUIRE(!first.IsNull());
	GYRO_REQUIRE(pair.Store.Find(first) != nullptr);
	GYRO_CHECK(pair.Store.Find(first)->Dress == Material::Glass);

	Hide(pair, chrome.Window);

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// The window it was is on its way out rather than gone, which is decision 114 and is why the walk
	// above takes the last child: the entity is still hanging on the chrome root, still being drawn,
	// and the next map appends a second one beside it.
	GYRO_REQUIRE(pair.Store.Find(first) != nullptr);
	GYRO_CHECK(pair.Store.Find(first)->Retiring);

	GYRO_REQUIRE(Reveal(pair, chrome.Window));

	GYRO_CHECK(!pair.Client.Fault().has_value());

	const EntityId second = NewestChrome(pair.Store);
	GYRO_REQUIRE(!second.IsNull());
	GYRO_CHECK(second != first);

	// **On the chrome root and not on the floor**, which is the whole of being drawn in front (187).
	// The declaration lives on the `xdg_toplevel` rather than on the entity, so it survives an unmap
	// that threw the entity away — and this is what says so.
	GYRO_CHECK_EQ(pair.Store.Find(second)->Parent, ChromeRoot(pair.Store));
	GYRO_CHECK(!pair.Store.Find(second)->Retiring);

	// And the material with it. `set_material` is double-buffered onto the toplevel's own commit and
	// the shell sent it once, before the surface ever came down; a compositor that kept the material on
	// the entity rather than on the toplevel would bring the launcher back as plain glass-less pixels
	// over whatever a person had open, with no request the shell could have sent to prevent it.
	GYRO_CHECK(pair.Store.Find(second)->Dress == Material::Glass);

	// The floor is untouched throughout: a launcher is never among the windows, on its first map or on
	// its tenth. This is the walk `WindowNode` does, asked for nothing rather than for a window.
	const Entity* const floor = pair.Store.Find(pair.Store.FirstRoot());
	GYRO_REQUIRE(floor != nullptr);
	GYRO_CHECK(floor->FirstChild.IsNull());
}

GYRO_TEST(ProtocolRoundTrip, ALauncherGivesTheKeyboardBackToTheWindowUnderItAndTakesItAgainWhenItReturns)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	// **What a person feels rather than what the scene holds**: they press the chord, type into the run
	// bar, press escape, and go on typing into the document that was in front of them. The middle step
	// is the one that has never run — focus leaves a chrome surface at its retirement (114), and the
	// window it leaves to is whatever was underneath, which is the stack `Scene/Focus.h` keeps rather
	// than anything the shell says.
	Pair pair{ "gyro-roundtrip-chrome-keyboard", Trust::System };
	GYRO_REQUIRE(pair.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(pair, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(pair, bound, keyboard));

	Toplevel document;
	GYRO_REQUIRE(Show(pair, bound, document, std::byte{ 0x20 }));

	pair.Turn();

	GYRO_REQUIRE(keyboard.Listener.Entered == 1);
	GYRO_CHECK(keyboard.Listener.Focused.Id() == document.Drawn.Surface.Id());

	Chrome chrome;
	GYRO_REQUIRE(Declare(pair, bound, chrome, std::byte{ 0x60 }));

	// The launcher takes the keyboard when it maps, because one a person cannot type into is not one.
	// This transition does send a `leave`, and it is the only one of the three that does — see below.
	GYRO_CHECK_EQ(keyboard.Listener.Entered, std::uint32_t{ 2 });
	GYRO_CHECK_EQ(keyboard.Listener.Left, std::uint32_t{ 1 });
	GYRO_CHECK(keyboard.Listener.Focused.Id() == chrome.Window.Drawn.Surface.Id());

	Hide(pair, chrome.Window);

	// **And gives it back to the window that had it**, rather than to nothing. A compositor that let
	// focus fall to null here would leave a person's next keystroke going nowhere after they dismissed
	// a launcher — which reads as the keyboard having died rather than as a bug in the shell.
	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(keyboard.Listener.Entered, std::uint32_t{ 3 });
	GYRO_CHECK(keyboard.Listener.Focused.Id() == document.Drawn.Surface.Id());

	GYRO_REQUIRE(Reveal(pair, chrome.Window));

	GYRO_CHECK(!pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(keyboard.Listener.Entered, std::uint32_t{ 4 });
	GYRO_CHECK(keyboard.Listener.Focused.Id() == chrome.Window.Drawn.Surface.Id());

	// **What is deliberately not asserted here is the `leave` count, and it is a gap rather than a
	// preference.** An unmap sends no `wl_keyboard.leave` at all: `ClientXdgSurface::Unmap` takes the
	// entity out of the window-to-surface table before it retires the entity, and `SeatGlobal::SendLeave`
	// resolves the surface it is leaving *through that table* — so by the time the seat compares the
	// world's focus against what it has told the client, there is nothing left for it to name. Its own
	// comment says a destroyed surface takes its focus with it and owes no leave, which is true, and the
	// path arrives there for a surface that is merely unmapped, which is not.
	//
	// A client is therefore told `enter` on the window under the launcher without ever being told it
	// left the launcher, and what a toolkit does with that is go on drawing a caret in a surface it has
	// taken off the screen. It costs nothing while one process owns both — the shell dismissed its own
	// launcher and knows — and it is wrong the moment the surface that had focus belongs to somebody
	// else. `SeatGlobal::SendPointerLeave` is the same code and the same gap.
	//
	// Asserting the counts that are right today would write the bug into the suite, so what stands here
	// instead is the sentence naming it.
}
