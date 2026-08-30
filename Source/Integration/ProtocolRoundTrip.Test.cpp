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

	// **Not fired, and it is the surface having no window rather than the callback being unanswered.**
	// A frame callback is answered when the content this surface committed reaches the glass, and a
	// surface with no role and no buffer is in nobody's scene — there is no entity for the return leg to
	// report against, so there is nothing to be waiting on. The mapped case is
	// `AMappedWindowIsToldWhenItsFrameReachedTheGlass` below.
	GYRO_CHECK_EQ(callback.Fired, std::uint32_t{ 0 });

	session.Present(1, session.Clock.Now());
	session.Turn();

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

[[nodiscard]] bool Listen(Session& session, BoundCompositor& bound, Keyboard& keyboard)
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

	session.Turn();

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
	GYRO_CHECK(toplevel.WindowEvents.States.empty());

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

GYRO_TEST(ProtocolRoundTrip, AMappedWindowIsToldWhenItsFrameReachedTheGlass)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-presented" };
	GYRO_REQUIRE(session.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	session.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x44 }));

	toplevel.Drawn.Surface.Commit();
	session.Turn();

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

	session.Turn();

	// Nothing yet: the commit has reached the world, and the world has not reached a panel.
	GYRO_CHECK_EQ(first.Fired, std::uint32_t{ 0 });

	const Instant shown = Advanced(session.Clock.Now(), std::chrono::milliseconds{ 8 });

	session.Present(1, shown);
	session.Turn();

	GYRO_CHECK(!session.Client.Fault().has_value());

	// **This is the frame the window was waiting for, and it is what makes an application redraw.** The
	// timestamp is the instant the frame reached the glass rather than the moment gyro got round to
	// saying so, because a toolkit paces itself by differencing two of these.
	GYRO_REQUIRE_EQ(first.Fired, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(first.Stamp, static_cast<std::uint32_t>(Monotonic::ToNanoseconds(shown) / 1'000'000));

	// And the next frame is not answered on the strength of the last one. A callback that fired again
	// unasked would be a client drawing faster than it committed.
	session.Present(2, Advanced(shown, std::chrono::milliseconds{ 16 }));
	session.Turn();

	GYRO_CHECK_EQ(first.Fired, std::uint32_t{ 1 });

	// A second frame, asked for the same way, is answered the same way — which is the loop an animating
	// application actually runs in.
	Callback second;

	(void)toplevel.Drawn.Surface.Frame(second);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	session.Turn();
	session.Present(3, Advanced(shown, std::chrono::milliseconds{ 32 }));
	session.Turn();

	GYRO_CHECK_EQ(second.Fired, std::uint32_t{ 1 });
}

GYRO_TEST(ProtocolRoundTrip, AWindowThatUnmappedIsNotToldAboutFramesItIsNotIn)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-unmapped" };
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

	GYRO_REQUIRE(toplevel.SurfaceEvents.Serial != 0);

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	session.Turn();

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

	session.Turn();
	session.Present(1, session.Clock.Now());
	session.Turn();

	GYRO_CHECK(!session.Client.Fault().has_value());
	GYRO_CHECK_EQ(callback.Fired, std::uint32_t{ 0 });
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

// The whole of what a person does with a keyboard, in the order it happens: bind a seat, be told
// there is one keyboard on it, receive the layout, open a window, and type into it.
GYRO_TEST(ProtocolRoundTrip, ASeatOffersOneKeyboardAndHandsOverALayoutBeforeAnythingIsTyped)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-seat" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(session, bound, keyboard));

	// A keyboard and a pointer, and no touch. A client told there is a capability gyro has not built
	// would wait for an `enter` that cannot come, so the set is exactly what is routed.
	GYRO_CHECK(
		keyboard.SeatListener.Capabilities == (Wayland::WlSeatCapability::Keyboard | Wayland::WlSeatCapability::Pointer)
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

	Session session{ "gyro-roundtrip-keymap" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(session, bound, keyboard));
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

	Session session{ "gyro-roundtrip-typing" };
	GYRO_REQUIRE(session.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	session.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(session, bound, keyboard));

	// Nothing is focused before there is a window, and a key typed at that moment reaches nobody
	// rather than the last thing that happened to be there.
	(*session.Host)->OnKey(Press(30, true, session.Clock.Now()), false);
	(*session.Host)->OnKey(Press(30, false, session.Clock.Now()), false);

	session.Turn();

	GYRO_CHECK_EQ(keyboard.Listener.Entered, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(keyboard.Listener.Keys, std::uint32_t{ 0 });

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x40 }));

	toplevel.Drawn.Surface.Commit();

	session.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	session.Turn();

	// **Focus in the wakeup the window opened in**, which is what the seat comparing after the
	// dispatch buys: the commit that mapped it and the event that says so are one turn, so a person
	// who starts typing the instant a window appears is typing into it.
	GYRO_CHECK_EQ(keyboard.Listener.Entered, std::uint32_t{ 1 });
	GYRO_CHECK(keyboard.Listener.Focused.Id() == toplevel.Drawn.Surface.Id());
	GYRO_CHECK_EQ(keyboard.Listener.HeldOnEnter, std::size_t{ 0 });

	(*session.Host)->OnKey(Press(30, true, session.Clock.Now()), false);

	session.Turn();

	GYRO_CHECK_EQ(keyboard.Listener.Keys, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(keyboard.Listener.LastKey, std::uint32_t{ 30 });
	GYRO_CHECK(keyboard.Listener.LastState == Wayland::WlKeyboardKeyState::Pressed);

	// The kernel's numbering all the way to the client, which is what makes the eight XKB adds the
	// client's own business.
	(*session.Host)->OnKey(Press(30, false, session.Clock.Now()), false);

	session.Turn();

	GYRO_CHECK_EQ(keyboard.Listener.Keys, std::uint32_t{ 2 });
	GYRO_CHECK(keyboard.Listener.LastState == Wayland::WlKeyboardKeyState::Released);
	GYRO_CHECK(!session.Client.Fault().has_value());
}

// A key the compositor took for itself, which is the escape chord: the client hears nothing, and the
// modifier state is still the truth about what a person is holding.
GYRO_TEST(ProtocolRoundTrip, AKeyTheCompositorTookNeverReachesTheWindow)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-chord" };
	GYRO_REQUIRE(session.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	session.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(session, bound, keyboard));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x41 }));

	toplevel.Drawn.Surface.Commit();

	session.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	session.Turn();

	GYRO_REQUIRE(keyboard.Listener.Entered == 1);

	// Left Ctrl held: never consumed, because a person is holding it before anything knows a chord is
	// coming, and a client told the Escape vanished but not the Ctrl can reconcile that state.
	(*session.Host)->OnKey(Press(29, true, session.Clock.Now()), false);

	// And the verb, which gyro takes.
	(*session.Host)->OnKey(Press(1, true, session.Clock.Now()), true);
	(*session.Host)->OnKey(Press(1, false, session.Clock.Now()), true);

	session.Turn();

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

	Session session{ "gyro-roundtrip-held" };
	GYRO_REQUIRE(session.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	session.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(session, bound, keyboard));

	(*session.Host)->OnKey(Press(42, true, session.Clock.Now()), false);

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x42 }));

	toplevel.Drawn.Surface.Commit();

	session.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	session.Turn();

	GYRO_CHECK_EQ(keyboard.Listener.Entered, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(keyboard.Listener.HeldOnEnter, std::size_t{ 1 });
}

// Closing a window takes focus off it. The entity is still in the tree — decision 114 keeps it there
// while its exit runs — so this is the one assertion that says the keystrokes stop before the pixels
// do.
GYRO_TEST(ProtocolRoundTrip, AWindowThatClosedStopsReceivingKeys)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-closed" };
	GYRO_REQUIRE(session.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	session.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(session, bound, keyboard));

	Toplevel toplevel;
	GYRO_REQUIRE(Role(bound, toplevel, std::byte{ 0x43 }));

	toplevel.Drawn.Surface.Commit();

	session.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	session.Turn();

	GYRO_REQUIRE(keyboard.Listener.Entered == 1);

	toplevel.Window.Destroy();

	session.Turn();

	const Entity* const window = WindowNode(session.Store);
	GYRO_REQUIRE(window != nullptr);
	GYRO_CHECK(window->Retiring);

	(*session.Host)->OnKey(Press(30, true, session.Clock.Now()), false);

	session.Turn();

	GYRO_CHECK_EQ(keyboard.Listener.Keys, std::uint32_t{ 0 });
	GYRO_CHECK(!session.Client.Fault().has_value());
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

	void OnButton(std::uint32_t, std::uint32_t, std::uint32_t button, Wayland::WlPointerButtonState state) override
	{
		++Buttons;
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

[[nodiscard]] bool Grip(Session& session, Keyboard& keyboard, Pointer& pointer)
{
	pointer.Device = keyboard.Seat.GetPointer(pointer.Listener);

	session.Turn();

	return pointer.Device.IsValid();
}

// A window on screen with the pointer somewhere definite, which is the state every case below starts
// from: one panel, one mapped toplevel, and the pointer parked away from it.
[[nodiscard]] bool Show(Session& session, BoundCompositor& bound, Toplevel& toplevel, std::byte fill)
{
	if (!Role(bound, toplevel, fill))
	{
		return false;
	}

	toplevel.Drawn.Surface.Commit();

	session.Turn();

	toplevel.XdgSurface.AckConfigure(toplevel.SurfaceEvents.Serial);
	toplevel.Drawn.Surface.Attach(toplevel.Drawn.Buffer, 0, 0);
	toplevel.Drawn.Surface.Commit();

	session.Turn();

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

	Session session{ "gyro-roundtrip-activated" };
	GYRO_REQUIRE(session.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	session.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Toplevel first;
	GYRO_REQUIRE(Show(session, bound, first, std::byte{ 0x40 }));

	// Two configures: the `0x0` that answered *what size should I be*, carrying no states because the
	// window did not exist yet, and the one the mapping produced. They are one turn apart rather than
	// one frame — the window maps and is told it is focused inside the same `Advance`.
	GYRO_CHECK_EQ(first.WindowEvents.Configured, std::uint32_t{ 2 });
	GYRO_CHECK(first.WindowEvents.Has(Wayland::XdgToplevelState::Activated));

	// Nothing changed, so nothing is sent. A window that is being redrawn is not a window that is being
	// reconfigured.
	session.Turn();

	GYRO_CHECK_EQ(first.WindowEvents.Configured, std::uint32_t{ 2 });

	Toplevel second;
	GYRO_REQUIRE(Show(session, bound, second, std::byte{ 0x55 }));

	// Newest on top is `Scene/Focus.h`'s rule and this is a client reading it: the window that just
	// opened has the keyboard, and the one behind it is told in the same turn.
	GYRO_CHECK(second.WindowEvents.Has(Wayland::XdgToplevelState::Activated));
	GYRO_CHECK(!first.WindowEvents.Has(Wayland::XdgToplevelState::Activated));
	GYRO_CHECK_EQ(first.WindowEvents.Configured, std::uint32_t{ 3 });

	// And back, by the route that has nothing to do with the pointer: focus put where a shell or an
	// alt-tab would put it.
	const Entity* const floor = session.Store.Find(session.Store.FirstRoot());
	GYRO_REQUIRE(floor != nullptr);

	// The floor's first child is the window that opened first, since the store appends and the sibling
	// list is the z order (55).
	GYRO_REQUIRE(session.Store.Focus().Focus(floor->FirstChild));

	session.Turn();

	GYRO_CHECK(first.WindowEvents.Has(Wayland::XdgToplevelState::Activated));
	GYRO_CHECK(!second.WindowEvents.Has(Wayland::XdgToplevelState::Activated));
	GYRO_CHECK(!session.Client.Fault().has_value());
}

// The pointer, moved the way `Dispatch/Loop.h` moves it: a displacement against the outputs, with no
// rounding anywhere on the way.
void Push(Session& session, double x, double y)
{
	static_cast<void>(session.Store.Pointer().Move({ x, y }, session.Store.Outputs()));
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

// The seat has a pointer now, and it says so before a client asks. Touch is still refused by name,
// which is the half of `wl_seat` that has not been built — the refusal and the capability are one
// statement and a client is entitled to read them as one.
GYRO_TEST(ProtocolRoundTrip, TheSeatOffersAPointerAndStillRefusesTouchByName)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-pointer" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(session, bound, keyboard));

	GYRO_CHECK(
		(keyboard.SeatListener.Capabilities & Wayland::WlSeatCapability::Pointer) == Wayland::WlSeatCapability::Pointer
	);

	Pointer pointer;
	GYRO_REQUIRE(Grip(session, keyboard, pointer));
	GYRO_CHECK(!session.Client.Fault().has_value());

	Wayland::WlTouchIgnoring touch;
	static_cast<void>(keyboard.Seat.GetTouch(touch));

	session.Turn();

	GYRO_REQUIRE(session.Client.Fault().has_value());
	GYRO_CHECK_EQ(session.Client.Fault()->Code, static_cast<std::uint32_t>(Wayland::WlSeatError::MissingCapability));
}

// The pointer arriving on a window and moving across it. The coordinates are the surface's own, which
// is the whole of what the hit test is for: a toolkit decides which of its buttons is under the cursor
// from these two numbers and nothing else.
GYRO_TEST(ProtocolRoundTrip, APointerOverAWindowEntersItAndFollowsTheHand)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-pointing" };
	GYRO_REQUIRE(session.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	session.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(session, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(session, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(session, bound, toplevel, std::byte{ 0x40 }));

	// **Nothing yet, because nothing has moved a mouse.** A compositor that had sent an `enter` here
	// would be one that draws a cursor on a machine driven by a touchscreen.
	GYRO_CHECK_EQ(pointer.Listener.Entered, std::uint32_t{ 0 });

	const Point<GlobalSpace> middle = MiddleOfTheWindow(session.Store);
	GYRO_REQUIRE(middle.X > 0.0);

	Push(session, middle.X, middle.Y);
	(*session.Host)->OnPointerMotion(PointerMotion{ .When = session.Clock.Now() });

	session.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Entered, std::uint32_t{ 1 });
	GYRO_CHECK(pointer.Listener.On.Id() == toplevel.Drawn.Surface.Id());

	// The middle of the window is the middle of the surface, because there is no shadow margin on a
	// client that declared no geometry — and the buffer is the harness's 16x8.
	GYRO_CHECK_EQ(pointer.Listener.X.ToInt(), std::int32_t{ 8 });
	GYRO_CHECK_EQ(pointer.Listener.Y.ToInt(), std::int32_t{ 4 });
	GYRO_CHECK_EQ(pointer.Listener.Motions, std::uint32_t{ 0 });

	// A second wakeup with the hand still on the desk sends nothing at all, which is what keeps a
	// window animating under a resting pointer from telling its toolkit the hand is moving.
	session.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Motions, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(pointer.Listener.Entered, std::uint32_t{ 1 });

	Push(session, 2.0, 1.0);
	(*session.Host)->OnPointerMotion(PointerMotion{ .When = session.Clock.Now() });

	session.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Motions, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(pointer.Listener.X.ToInt(), std::int32_t{ 10 });
	GYRO_CHECK_EQ(pointer.Listener.Y.ToInt(), std::int32_t{ 5 });
	GYRO_CHECK(!session.Client.Fault().has_value());
}

// Sliding off the window, which is the event a toolkit un-highlights everything on.
GYRO_TEST(ProtocolRoundTrip, ThePointerLeavesAWindowItSlidOff)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-leaving" };
	GYRO_REQUIRE(session.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	session.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(session, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(session, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(session, bound, toplevel, std::byte{ 0x40 }));

	const Point<GlobalSpace> middle = MiddleOfTheWindow(session.Store);

	Push(session, middle.X, middle.Y);
	(*session.Host)->OnPointerMotion(PointerMotion{ .When = session.Clock.Now() });

	session.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Entered, std::uint32_t{ 1 });

	// Off the far side of it, onto gyro's own floor — which has no client behind it, so what the client
	// hears is the leave and nothing after it.
	Push(session, 600.0, 0.0);
	(*session.Host)->OnPointerMotion(PointerMotion{ .When = session.Clock.Now() });

	session.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Left, std::uint32_t{ 1 });
	GYRO_CHECK(pointer.Listener.LeftSurface.Id() == toplevel.Drawn.Surface.Id());
	GYRO_CHECK_EQ(pointer.Listener.Entered, std::uint32_t{ 1 });
	GYRO_CHECK(!session.Client.Fault().has_value());
}

// The implicit grab: a button held is a surface that keeps the pointer wherever the hand goes.
//
// **This is what every drag in every toolkit rests on.** A person who presses on a scrollbar and slides
// off the window is still scrolling; a compositor that sent a `leave` at the frame edge would leave the
// thumb stuck where the pointer crossed it, which is the bug this test exists to keep out.
GYRO_TEST(ProtocolRoundTrip, AButtonHeldKeepsThePointerOnTheWindowItWasPressedOn)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-grab" };
	GYRO_REQUIRE(session.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	session.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(session, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(session, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(session, bound, toplevel, std::byte{ 0x40 }));

	const Point<GlobalSpace> middle = MiddleOfTheWindow(session.Store);

	Push(session, middle.X, middle.Y);
	(*session.Host)->OnPointerMotion(PointerMotion{ .When = session.Clock.Now() });

	session.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Entered, std::uint32_t{ 1 });

	// `BTN_LEFT`, in the kernel's numbering all the way to the client — the same rule the keyboard is
	// under, and the reason nothing in the middle has a button map.
	(*session.Host)->OnPointerButton(Click(0x110, true, session.Clock.Now()));

	session.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Buttons, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(pointer.Listener.LastButton, std::uint32_t{ 0x110 });
	GYRO_CHECK(pointer.Listener.LastState == Wayland::WlPointerButtonState::Pressed);

	// Off the window entirely, with the button still down. No leave, and the coordinates keep going —
	// negative, because the hand is to the left of where the surface starts.
	Push(session, -600.0, 0.0);
	(*session.Host)->OnPointerMotion(PointerMotion{ .When = session.Clock.Now() });

	session.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Left, std::uint32_t{ 0 });
	GYRO_CHECK(pointer.Listener.Motions > 0);
	GYRO_CHECK(pointer.Listener.X.Raw() < 0);

	// The release goes to the window that took the press, and the leave follows it in the same wakeup:
	// the hand is over gyro's floor by now, and the client would otherwise sit believing it still had
	// the pointer until something else moved.
	(*session.Host)->OnPointerButton(Click(0x110, false, session.Clock.Now()));

	session.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Buttons, std::uint32_t{ 2 });
	GYRO_CHECK(pointer.Listener.LastState == Wayland::WlPointerButtonState::Released);
	GYRO_CHECK_EQ(pointer.Listener.Left, std::uint32_t{ 1 });
	GYRO_CHECK(!session.Client.Fault().has_value());
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

	Session session{ "gyro-roundtrip-click" };
	GYRO_REQUIRE(session.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	session.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(session, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(session, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(session, bound, toplevel, std::byte{ 0x40 }));

	GYRO_REQUIRE_EQ(keyboard.Listener.Entered, std::uint32_t{ 1 });

	const Point<GlobalSpace> middle = MiddleOfTheWindow(session.Store);

	// Focus taken away by something with no client behind it, which is what the recovery console and
	// the greeter are — and the cheapest way to say *this window is not the focused one* without a
	// second connection.
	const EntityId elsewhere = session.Store.CreateContainer({}, {}).value();

	session.Store.Focus().Offer(elsewhere);

	session.Turn();

	GYRO_REQUIRE_EQ(keyboard.Listener.Left, std::uint32_t{ 1 });

	Push(session, middle.X, middle.Y);
	(*session.Host)->OnPointerMotion(PointerMotion{ .When = session.Clock.Now() });

	session.Turn();

	// The pointer being on the window is not focus: hovering changes nothing, because follows-mouse is
	// a preference nobody has and this is not it.
	GYRO_REQUIRE_EQ(pointer.Listener.Entered, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(keyboard.Listener.Entered, std::uint32_t{ 1 });

	(*session.Host)->OnPointerButton(Click(0x110, true, session.Clock.Now()));

	session.Turn();

	// One turn, and the client has both: the button it was clicked with and the keyboard it was
	// clicked for.
	GYRO_CHECK_EQ(pointer.Listener.Buttons, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(keyboard.Listener.Entered, std::uint32_t{ 2 });
	GYRO_CHECK(keyboard.Listener.Focused.Id() == toplevel.Drawn.Surface.Id());
	GYRO_CHECK(session.Store.Focus().Focused() != elsewhere);

	// Clicking about inside the window it is already in sends nothing further: focus that re-entered on
	// every press would be a `leave`/`enter` pair per click, and a toolkit redraws on those.
	(*session.Host)->OnPointerButton(Click(0x110, false, session.Clock.Now()));
	(*session.Host)->OnPointerButton(Click(0x110, true, session.Clock.Now()));

	session.Turn();

	GYRO_CHECK_EQ(keyboard.Listener.Entered, std::uint32_t{ 2 });
	GYRO_CHECK_EQ(keyboard.Listener.Left, std::uint32_t{ 1 });
	GYRO_CHECK(!session.Client.Fault().has_value());
}

// A scroll, with the three events that say what did it. The source is the one that decides whether a
// toolkit runs kinetic scrolling at all, so it has to arrive and it has to arrive first.
GYRO_TEST(ProtocolRoundTrip, AScrollCarriesWhatDidItBeforeItCarriesHowFar)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-scroll" };
	GYRO_REQUIRE(session.Opened);

	const std::array outputs{ SceneOutput{
		.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };
	session.Store.SetOutputs(outputs);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	Keyboard keyboard;
	GYRO_REQUIRE(Listen(session, bound, keyboard));

	Pointer pointer;
	GYRO_REQUIRE(Grip(session, keyboard, pointer));

	Toplevel toplevel;
	GYRO_REQUIRE(Show(session, bound, toplevel, std::byte{ 0x40 }));

	const Point<GlobalSpace> middle = MiddleOfTheWindow(session.Store);

	Push(session, middle.X, middle.Y);
	(*session.Host)->OnPointerMotion(PointerMotion{ .When = session.Clock.Now() });

	session.Turn();

	GYRO_REQUIRE(pointer.Listener.Entered == std::uint32_t{ 1 });

	(*session.Host)
		->OnPointerScroll(
			PointerScroll{ .Axis = ScrollAxis::Vertical,
	                       .Source = ScrollSource::Wheel,
	                       .Distance = 15.0,
	                       .Clicks120 = 120.0,
	                       .When = session.Clock.Now() }
		);

	session.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Sources, std::uint32_t{ 1 });
	GYRO_CHECK(pointer.Listener.LastSource == Wayland::WlPointerAxisSource::Wheel);
	GYRO_CHECK_EQ(pointer.Listener.Discretes, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(pointer.Listener.LastDiscrete, std::int32_t{ 1 });
	GYRO_CHECK_EQ(pointer.Listener.Axes, std::uint32_t{ 1 });
	GYRO_CHECK(pointer.Listener.LastAxis == Wayland::WlPointerAxis::VerticalScroll);
	GYRO_CHECK_EQ(pointer.Listener.LastValue.ToInt(), std::int32_t{ 15 });

	// A touchpad that stopped, which is the event a flick decays from and the only one that carries no
	// distance. A wheel never produces one, so nothing here would have made it up.
	(*session.Host)
		->OnPointerScroll(
			PointerScroll{ .Axis = ScrollAxis::Vertical,
	                       .Source = ScrollSource::Finger,
	                       .Stop = true,
	                       .When = session.Clock.Now() }
		);

	session.Turn();

	GYRO_CHECK_EQ(pointer.Listener.Stops, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(pointer.Listener.Axes, std::uint32_t{ 1 });
	GYRO_CHECK(pointer.Listener.LastSource == Wayland::WlPointerAxisSource::Finger);
	GYRO_CHECK(!session.Client.Fault().has_value());
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

	Session session{ "gyro-roundtrip-dmabuf-formats" };
	GYRO_REQUIRE(session.Opened);

	// Two tilings of one format, which is the shape a real driver's answer has and the shape a
	// linear-only list would hide: a client picks the tiled one and never allocates an untiled buffer.
	session.Textures.Advertised = { TextureFormat{ .Code = Argb8888, .Modifier = 0 },
		                            TextureFormat{ .Code = Argb8888, .Modifier = 0x0100000000000001ULL } };

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

	DmabufFormats formats;
	GYRO_REQUIRE(BindDmabuf(bound, formats).IsValid());

	session.Turn();

	GYRO_CHECK(!session.Client.Fault().has_value());

	// One `format` per distinct fourcc and one `modifier` per pair. A client bound below version 3 has
	// only the first list and reads it as *this format under whatever we work out*, which is linear.
	GYRO_CHECK_EQ(formats.Codes.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(formats.Pairs.size(), std::size_t{ 2 });
	GYRO_CHECK(formats.Pairs[1].second == 0x0100000000000001ULL);
}

GYRO_TEST(ProtocolRoundTrip, APairTheCompositorNeverOfferedComesBackAsFailedRatherThanADeadConnection)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-dmabuf-refused" };
	GYRO_REQUIRE(session.Opened);

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

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

	session.Turn();

	// **`failed` rather than a protocol error**, because a client cannot predict what a compositor's
	// device will take and the protocol gives it a fallback path for exactly this. Ending the
	// connection would turn a driver limitation into an application that will not start.
	GYRO_CHECK(!session.Client.Fault().has_value());
	GYRO_CHECK_EQ(events.Failed, std::uint32_t{ 1 });
	GYRO_CHECK(!events.Created.IsValid());
}

GYRO_TEST(ProtocolRoundTrip, ADescriptorBufferIsNotReleasedUntilNobodyIsReadingIt)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Session session{ "gyro-roundtrip-dmabuf-release" };
	GYRO_REQUIRE(session.Opened);

	session.Textures.Advertised = { TextureFormat{ .Code = Argb8888, .Modifier = 0 } };

	BoundCompositor bound;
	GYRO_REQUIRE(Bind(session, bound));

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

	session.Turn();
	session.Turn();

	GYRO_CHECK(!session.Client.Fault().has_value());
	GYRO_REQUIRE(events.Created.IsValid());

	Wayland::WlSurfaceIgnoring surfaceEvents;
	const Wayland::WlSurface surface = bound.Compositor.CreateSurface(surfaceEvents);
	GYRO_REQUIRE(surface.IsValid());

	surface.Attach(events.Created, 0, 0);
	surface.DamageBuffer(0, 0, Width, Height);
	surface.Commit();

	session.Turn();

	GYRO_CHECK(!session.Client.Fault().has_value());

	// The descriptors reached the texture space as one plane in the layout the client stated.
	GYRO_CHECK_EQ(session.Textures.Adopted, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(session.Textures.Planes, std::size_t{ 1 });
	GYRO_CHECK(session.Textures.Described == (TextureFormat{ .Code = Argb8888, .Modifier = 0 }));

	// **And no release, which is the assertion.** A frame drawn from this buffer may still be on
	// screen; telling the client otherwise is what tears a window into itself.
	session.Turn();

	GYRO_CHECK_EQ(released.Released, std::uint32_t{ 0 });

	// The watermark moves past every snapshot that named the id, which is the only thing that can say
	// nobody is reading.
	session.Textures.ReleaseAll();

	session.Turn();

	GYRO_CHECK_EQ(released.Released, std::uint32_t{ 1 });
}
