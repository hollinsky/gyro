// `setenv` and `mkdtemp` are POSIX rather than ISO C, and glibc gates both behind a feature-test
// macro. Named here for the reason Protocol/Server.Test.cpp names its own: what is wanted from the
// platform is stated rather than inherited.
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cerrno>
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
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Core/Time.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Protocol/Host.h"
#include "Scene/Entity.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"
#include "Testing/Test.h"
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

	void Retire(TextureId id) noexcept override { Retired += id.IsNull() ? 0U : 1U; }

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
struct Session
{
	explicit Session(std::string_view socket)
	{
		Host = MakeClientHost(socket);

		if (!Host || *Host == nullptr)
		{
			return;
		}

		// `Wire::Connection::Open` reads the environment, which is how every client finds a
		// compositor. Setting it here rather than passing a path is what keeps the test on the same
		// path a real client takes.
		::setenv("WAYLAND_DISPLAY", std::string{ socket }.c_str(), 1);

		Opened = (*Host)->Open(Store, Textures).has_value() && Client.Open().has_value();
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

[[nodiscard]] bool Bind(Session& session, BoundCompositor& bound)
{
	const Wayland::WlDisplay display{ session.Client, Wire::ObjectId::Display, Wayland::WlDisplay::WireVersion };

	(void)display.GetRegistry(bound.Listener);

	// **Twice, and the reason is libwayland's event loop rather than this test's impatience.** The
	// first dispatch accepts the connection and adds the client's descriptor as a source of its own;
	// a source added during a dispatch is not itself dispatched in that round, so the request already
	// sitting in the socket is read on the next one. The composition root never notices because its
	// wait goes readable again immediately, which is one more turn of a loop that is running anyway.
	session.Turn();
	session.Turn();

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

	Session session{ "gyro-roundtrip-registry" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

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

	// Exactly four: three a window is built out of, and one a toolkit demands before it will look for
	// them. When `wl_seat` lands this number goes up in the same commit as the thing it counts.
	GYRO_CHECK_EQ(bound.Listener.Globals.size(), std::size_t{ 4 });
}

// What a GTK client actually does with the clipboard global before it has a seat, which is bind it,
// find it answers, and — for a client that owns something copyable — make a source nobody will ever
// ask for. Both have to work for an application to start; neither transfers anything.
//
// The seat is the reason `get_data_device` is not exercised here: it takes one as an argument, gyro
// advertises none, and a client cannot name an object it was never offered. That request has no
// caller until there is input, and this test says so rather than reaching around the protocol to
// pretend otherwise.
GYRO_TEST(ProtocolRoundTrip, ADataSourceIsCreatedAndOffersMimeTypesNobodyWillAskFor)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-data" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

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

	session.Turn();

	// Still connected is the whole of it: a client that offered a MIME type and was ended for it is
	// one that never gets as far as drawing.
	GYRO_CHECK(!session.Client.Fault().has_value());
	GYRO_CHECK_EQ(events.Events, std::size_t{ 0 });

	source.Destroy();

	session.Turn();

	GYRO_CHECK(!session.Client.Fault().has_value());
}

GYRO_TEST(ProtocolRoundTrip, BindingWlShmAnnouncesTheFormatsBeforeAnythingIsAsked)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-formats" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	session.Turn();

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

	Session session{ "gyro-roundtrip-commit" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

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

	session.Turn();

	// Every one of those requests carries arguments that could be transposed by the emitter without
	// any structural check noticing. What says they were not is that libwayland demarshalled all of
	// them against the interface description and gyro's handlers accepted every one — a wrong
	// argument count or a wrong type is a protocol error and the connection would be gone.
	GYRO_CHECK(!session.Client.Fault().has_value());

	// One node, and it is gyro's own floor rather than anything the client authored — a surface with no
	// role is not a window, and nothing about the requests above says it is one.
	GYRO_CHECK_EQ(session.Store.Count(), std::uint32_t{ 1 });
	GYRO_CHECK_EQ(session.Textures.Adopted, std::uint32_t{ 0 });
}

GYRO_TEST(ProtocolRoundTrip, AFrameCallbackIsAcceptedAndNotYetAnswered)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-frame" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

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

	session.Turn();
	session.Turn();

	GYRO_CHECK(!session.Client.Fault().has_value());

	// **Not fired, and that is the state of this step rather than a bug.** A frame callback is
	// answered when the surface's content is about to reach the glass, which is a fact the return leg
	// carries and which nothing here has, because a surface with no buffer never reaches it. A client
	// that drives its animation off callbacks will stop after one frame — and it has nothing to draw
	// anyway. The assertion is here so that the day it starts firing, the commit that made it fire is
	// the one that turns this line around.
	GYRO_CHECK_EQ(callback.Fired, std::uint32_t{ 0 });
}

GYRO_TEST(ProtocolRoundTrip, AZeroBufferScaleEndsTheClient)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-scale" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Wayland::WlSurfaceIgnoring events;
	const Wayland::WlSurface surface = bound.Compositor.CreateSurface(events);
	GYRO_REQUIRE(surface.IsValid());

	surface.SetBufferScale(0);
	surface.Commit();

	session.Turn();

	// A scale of zero is a division on the frame thread later, so it is refused where it arrives. The
	// client is told which object and which code, because a toolkit that gets `invalid_scale` can fix
	// itself and one that gets a dropped connection cannot.
	GYRO_REQUIRE(session.Client.Fault().has_value());
	GYRO_CHECK_EQ(session.Client.Fault()->Code, static_cast<std::uint32_t>(Wayland::WlSurfaceError::InvalidScale));
}

GYRO_TEST(ProtocolRoundTrip, AnOffsetOnAttachEndsAModernClient)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-offset" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Wayland::WlSurfaceIgnoring events;
	const Wayland::WlSurface surface = bound.Compositor.CreateSurface(events);
	GYRO_REQUIRE(surface.IsValid());

	// Version 5 moved the offset off `attach` and onto its own request, and made sending it the old
	// way an error rather than a compatibility path — because a client that believes it moved its
	// buffer and a compositor that ignored it disagree about where the window is.
	surface.Attach(Wayland::WlBuffer{}, 3, 4);
	surface.Commit();

	session.Turn();

	GYRO_REQUIRE(session.Client.Fault().has_value());
	GYRO_CHECK_EQ(session.Client.Fault()->Code, static_cast<std::uint32_t>(Wayland::WlSurfaceError::InvalidOffset));
}

namespace
{
// A surface with one committed frame behind it, which is as far as a client can get before there is a
// role to say the surface is a window at all.
struct DrawnSurface
{
	Wayland::WlSurfaceIgnoring Events;
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

	Session session{ "gyro-roundtrip-shm" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	DrawnSurface drawn;
	GYRO_REQUIRE(Draw(bound, drawn, std::byte{ 0xC3 }));

	drawn.Surface.Attach(drawn.Buffer, 0, 0);
	drawn.Surface.DamageBuffer(0, 0, Width, Height);
	drawn.Surface.Commit();

	session.Turn();

	GYRO_CHECK(!session.Client.Fault().has_value());

	// The pixels made it across a real socket, through a descriptor libwayland passed with
	// `SCM_RIGHTS`, into a mapping gyro sealed, and out the far side as an id. The byte is what says
	// the offset and stride arithmetic landed on the rows the client meant rather than on a page of
	// zeroes next to them.
	GYRO_CHECK_EQ(session.Textures.Adopted, std::uint32_t{ 1 });
	GYRO_CHECK(session.Textures.Size == (PixelSize<BufferSpace>{ Width, Height }));
	GYRO_CHECK_EQ(session.Textures.Stride, static_cast<std::uint32_t>(Stride));
	GYRO_CHECK(session.Textures.Alpha == TextureAlpha::Premultiplied);
	GYRO_CHECK_EQ(session.Textures.Bytes, static_cast<std::size_t>(Stride) * static_cast<std::size_t>(Height));
	GYRO_CHECK(session.Textures.First == std::byte{ 0xC3 });

	// **Released in the same step it was committed in**, which is the point of copying rather than
	// sampling the client's memory: a toolkit with one buffer can draw its next frame immediately,
	// instead of allocating a second one to have somewhere to draw while gyro finishes with the first.
	GYRO_CHECK_EQ(drawn.Released.Released, std::uint32_t{ 1 });
}

GYRO_TEST(ProtocolRoundTrip, ACommitThatDidNotAttachKeepsTheContentItHad)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-keep" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	DrawnSurface drawn;
	GYRO_REQUIRE(Draw(bound, drawn, std::byte{ 0x40 }));

	drawn.Surface.Attach(drawn.Buffer, 0, 0);
	drawn.Surface.Commit();

	session.Turn();

	GYRO_REQUIRE(session.Textures.Adopted == 1);

	// A client that commits a new input region and nothing else must not lose its window, which is
	// what makes this gated on the attach rather than on there being a buffer.
	drawn.Surface.SetBufferScale(2);
	drawn.Surface.Commit();

	session.Turn();

	GYRO_CHECK(!session.Client.Fault().has_value());
	GYRO_CHECK_EQ(session.Textures.Adopted, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(session.Textures.Retired, std::uint32_t{ 0 });
}

GYRO_TEST(ProtocolRoundTrip, EveryAttachedFrameIsANewIdAndRetiresTheOneItReplaces)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-redraw" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	DrawnSurface drawn;
	GYRO_REQUIRE(Draw(bound, drawn, std::byte{ 0x08 }));

	drawn.Surface.Attach(drawn.Buffer, 0, 0);
	drawn.Surface.Commit();

	session.Turn();

	// The same `wl_buffer` again, which is what a toolkit that reuses one buffer does on every frame.
	drawn.Surface.Attach(drawn.Buffer, 0, 0);
	drawn.Surface.Commit();

	session.Turn();

	GYRO_CHECK(!session.Client.Fault().has_value());

	// **A new id rather than an overwrite of the old one.** The frame thread may still be recording
	// from a snapshot that names the previous id, so writing over its pixels would tear a window that
	// is on screen right now; the old id stays drawable and retires when the watermark says nothing
	// can still be reading it. Seam/Importer.h carries the argument.
	GYRO_CHECK_EQ(session.Textures.Adopted, std::uint32_t{ 2 });
	GYRO_CHECK_EQ(session.Textures.Retired, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(drawn.Released.Released, std::uint32_t{ 2 });
}

GYRO_TEST(ProtocolRoundTrip, AttachingNothingTakesTheContentAway)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-detach" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	DrawnSurface drawn;
	GYRO_REQUIRE(Draw(bound, drawn, std::byte{ 0x99 }));

	drawn.Surface.Attach(drawn.Buffer, 0, 0);
	drawn.Surface.Commit();

	session.Turn();

	GYRO_REQUIRE(session.Textures.Adopted == 1);

	// Attaching a null buffer is how a client takes its window off the screen without destroying
	// anything, and it has to be told apart from a commit that simply did not attach.
	drawn.Surface.Attach(Wayland::WlBuffer{}, 0, 0);
	drawn.Surface.Commit();

	session.Turn();

	GYRO_CHECK(!session.Client.Fault().has_value());
	GYRO_CHECK_EQ(session.Textures.Adopted, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(session.Textures.Retired, std::uint32_t{ 1 });
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
		States = states.size();
		++Configured;
	}

	void OnClose() override { ++Closed; }

	std::int32_t Width = -1;
	std::int32_t Height = -1;
	std::size_t States = 0;
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
} // namespace

GYRO_TEST(ProtocolRoundTrip, AToplevelIsConfiguredBeforeItIsAskedToDrawAnything)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-configure" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x20 }));

	// The empty commit is the client asking *what size should I be*, and it is the protocol's own
	// handshake rather than a courtesy: a toolkit will not draw a pixel until it has been answered.
	toplevel.Drawn.Surface.Commit();

	session.Turn();

	GYRO_CHECK(!session.Client.Fault().has_value());
	GYRO_CHECK_EQ(toplevel.WindowEvents.Configured, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(toplevel.SurfaceEvents.Configured, std::uint32_t{ 1 });
	GYRO_CHECK(toplevel.SurfaceEvents.Serial != 0);

	// **Zero by zero and no states, which is gyro having no opinion.** A number here is one the client
	// must obey, so inventing one would be the compositor doing layout; an empty state list is a window
	// that is not maximised, not fullscreen, not being resized and not activated — all four of which
	// are true.
	GYRO_CHECK_EQ(toplevel.WindowEvents.Width, 0);
	GYRO_CHECK_EQ(toplevel.WindowEvents.Height, 0);
	GYRO_CHECK_EQ(toplevel.WindowEvents.States, std::size_t{ 0 });

	// Nothing is on screen: the client has been told it may draw and has not.
	GYRO_CHECK_EQ(session.Store.Count(), std::uint32_t{ 1 });
}

GYRO_TEST(ProtocolRoundTrip, AnAcknowledgedFrameBecomesAWindowCentredOnTheOutput)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-map" };
	GYRO_REQUIRE(session.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	session.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x71 }));

	toplevel.Drawn.Surface.Commit();

	session.Turn();

	GYRO_REQUIRE(toplevel.SurfaceEvents.Serial != 0);

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.DamageBuffer(0, 0, Width, Height);
	toplevel.Drawn.Surface.Commit();

	session.Turn();

	GYRO_CHECK(!session.Client.Fault().has_value());

	// The floor, the window, and the pixels under it. Two nodes per window rather than one, which is a
	// toplevel as decision 111 describes it: a container holding its own surface and, one day, its
	// subsurfaces.
	GYRO_CHECK_EQ(session.Store.Count(), std::uint32_t{ 3 });
	GYRO_CHECK_EQ(session.Textures.Adopted, std::uint32_t{ 1 });

	const Entity* const window = WindowNode(session.Store);
	GYRO_REQUIRE(window != nullptr);

	// Centred, which with no shell and no pointer is the whole of gyro's placement policy — and it is
	// the one placement that takes no parameter, so there is no number here that somebody chose.
	GYRO_CHECK_EQ(window->Translation.Model().X, (1920.0 - static_cast<double>(Width)) / 2.0);
	GYRO_CHECK_EQ(window->Translation.Model().Y, (1080.0 - static_cast<double>(Height)) / 2.0);

	const Entity* const content = session.Store.Find(window->FirstChild);
	GYRO_REQUIRE(content != nullptr);
	GYRO_CHECK(content->Kind == NodeKind::Image);
	GYRO_CHECK_EQ(content->Extent.Width, static_cast<float>(Width));
}

GYRO_TEST(ProtocolRoundTrip, ABufferCommittedBeforeTheConfigureIsAcknowledgedEndsTheClient)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-unconfigured" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x33 }));

	toplevel.Drawn.Surface.Commit();

	session.Turn();

	// The ack is deliberately skipped. A compositor that accepted this would be showing a frame drawn
	// for a size nobody agreed on, which is a window that appears at the wrong shape and then jumps.
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	session.Turn();

	GYRO_REQUIRE(session.Client.Fault().has_value());
	GYRO_CHECK_EQ(
		session.Client.Fault()->Code, static_cast<std::uint32_t>(Wayland::XdgSurfaceError::UnconfiguredBuffer)
	);

	GYRO_CHECK_EQ(session.Store.Count(), std::uint32_t{ 1 });
}

GYRO_TEST(ProtocolRoundTrip, DestroyingTheToplevelRetiresTheWindowRatherThanRemovingIt)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-unmap" };
	GYRO_REQUIRE(session.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	session.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x55 }));

	toplevel.Drawn.Surface.Commit();

	session.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	session.Turn();

	GYRO_REQUIRE(session.Store.Count() == 3);

	toplevel.Window.Destroy();

	session.Turn();

	GYRO_CHECK(!session.Client.Fault().has_value());

	// **Still three, and still where it was.** A window that is closing is a window a person is still
	// looking at, so the subtree keeps its links and its position and goes on being drawn until every
	// channel on it has settled; the store frees it on the serialisation pass that finds it at rest.
	// Freeing here instead is a window that vanishes rather than one that leaves.
	GYRO_CHECK_EQ(session.Store.Count(), std::uint32_t{ 3 });

	const Entity* const window = WindowNode(session.Store);
	GYRO_REQUIRE(window != nullptr);
	GYRO_CHECK(window->Retiring);
}
