// `setenv` and `mkdtemp` are POSIX rather than ISO C, and glibc gates both behind a feature-test
// macro. Named here for the reason Protocol/Server.Test.cpp names its own: what is wanted from the
// platform is stated rather than inherited.
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

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
#include "Publication/Return.h"
#include "Scene/Entity.h"
#include "Scene/Output.h"
#include "Scene/Return.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"
#include "Testing/Test.h"
#include "Wayland/LinuxDmabufV1.h"
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
struct ClientPool
{
	explicit ClientPool(std::size_t size) : Size{ size }
	{
		const int descriptor = ::memfd_create("gyro-roundtrip", MFD_CLOEXEC | MFD_ALLOW_SEALING);

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
	explicit Pair(std::string_view socket)
	{
		Host = MakeClientHost(HostListener::Own, socket);

		if (!Host || *Host == nullptr)
		{
			return;
		}

		// `Wire::Connection::Open` reads the environment, which is how every client finds a
		// compositor. Setting it here rather than passing a path is what keeps the test on the same
		// path a real client takes.
		::setenv("WAYLAND_DISPLAY", std::string{ socket }.c_str(), 1);

		Opened = (*Host)->Open(Store, Textures).has_value() && Client.Open().has_value();

		// The return leg, wired the way the composition root wires it: before anything can have
		// committed, because a first frame owed against an unobserved drain is a callback nothing would
		// ever send.
		Returns.SetOutputs(1);
		(*Host)->Observe(Returns);
	}

	// The frame thread's half, with no frame thread: a report saying this sequence reached the glass on
	// the one output, delivered the way `Dispatch/Loop.h` delivers one. `Seal` first, because the loop
	// seals what the step's commits declared against the sequence about to carry them.
	void Present(std::uint64_t sequence, Instant at)
	{
		Returns.Seal(sequence, Store);

		FrameReport report{};
		report.Watermark = sequence;
		report.OutputCount = 1;
		report.Presentations[0] = { .Sequence = sequence, .At = at };

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

	// Version 1 for `wl_compositor`'s reason. 4 owes a `configure_bounds` and 5 a `wm_capabilities`,
	// and gyro has no shell, no seat and no output model reaching this module — a client told 5 and
	// sent no capabilities is entitled to assume it has all four, which is a titlebar of buttons that
	// do nothing.
	GYRO_CHECK_EQ(shell->Version, std::uint32_t{ 1 });

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

	// Exactly six: three a window is built out of, one a toolkit demands before it will look for them,
	// the seat that makes the window typeable, and the one that lets a client hand over a buffer a
	// panel can scan out instead of pixels gyro has to copy.
	GYRO_CHECK_EQ(bound.Listener.Globals.size(), std::size_t{ 6 });
}

// What a GTK client actually does with the clipboard global before it has a seat, which is bind it,
// find it answers, and — for a client that owns something copyable — make a source nobody will ever
// ask for. Both have to work for an application to start; neither transfers anything.
//
// `get_data_device` is not exercised here: it resolves now that there is a seat, and what is behind
// it is inert — a device with no selection to read and no drag to start, because both need a pointer.
// The test that belongs here is the one that transfers something, and it lands with the selection.
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

	// One node, and it is gyro's own floor rather than anything the client authored — a surface with no
	// role is not a window, and nothing about the requests above says it is one.
	GYRO_CHECK_EQ(pair.Store.Count(), std::uint32_t{ 1 });
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

[[nodiscard]] bool Draw(BoundCompositor& bound, DrawnSurface& drawn, std::byte fill)
{
	ClientPool pool{ PoolBytes };

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

// The window gyro authored, found the way anything without an id would find it: the floor is the only
// root, and a window is a child of it.
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
	GYRO_CHECK_EQ(pair.Store.Count(), std::uint32_t{ 1 });
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

	// The floor, the window, and the pixels under it. Two nodes per window rather than one, which is a
	// toplevel as decision 111 describes it: a container holding its own surface and, one day, its
	// subsurfaces.
	GYRO_CHECK_EQ(pair.Store.Count(), std::uint32_t{ 3 });
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
	// client that prints one if the remainder is dropped. gyro serves no `wp_presentation`, so this is
	// the only cadence figure a client can obtain — a zero here is a media player falling back to 60.
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

	GYRO_CHECK_EQ(pair.Store.Count(), std::uint32_t{ 1 });
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

	GYRO_REQUIRE(pair.Store.Count() == 3);

	toplevel.Window.Destroy();

	pair.Turn();

	GYRO_CHECK(!pair.Client.Fault().has_value());

	// **Still three, and still where it was.** A window that is closing is a window a person is still
	// looking at, so the subtree keeps its links and its position and goes on being drawn until every
	// channel on it has settled; the store frees it on the serialisation pass that finds it at rest.
	// Freeing here instead is a window that vanishes rather than one that leaves.
	GYRO_CHECK_EQ(pair.Store.Count(), std::uint32_t{ 3 });

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
