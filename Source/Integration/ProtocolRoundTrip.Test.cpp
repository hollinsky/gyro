// `setenv` and `mkdtemp` are POSIX rather than ISO C, and glibc gates both behind a feature-test
// macro. Named here for the reason Protocol/Server.Test.cpp names its own: what is wanted from the
// platform is stated rather than inherited.
#define _POSIX_C_SOURCE 200809L

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Core/Clock.h"
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Core/Time.h"
#include "Geometry/Space.h"
#include "Protocol/Host.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"
#include "Testing/Test.h"
#include "Wayland/Wayland.h"
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

// The texture space as the nothing a host touches yet, per Protocol/Host.Test.cpp's own fake. It
// counts so that "never reached" is asserted rather than assumed.
class CountingTextures final : public ITextures
{
public:
	[[nodiscard]] Result<TextureId> Adopt(PixelSize<BufferSpace>, std::uint32_t, std::span<const std::byte>) override
	{
		++Adopted;

		return Failure(ENOSYS, "no texture space in this test");
	}

	void Retire(TextureId) noexcept override { ++Retired; }

	std::uint32_t Adopted = 0;
	std::uint32_t Retired = 0;
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

// Everything a client needs before it can make a surface: the display, the registry, and the one
// global gyro advertises.
struct BoundCompositor
{
	Registry Listener;
	Wayland::WlCompositor Compositor;
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

	return bound.Compositor.IsValid();
}
} // namespace

GYRO_TEST(ProtocolRoundTrip, TheRegistryCarriesTheCompositorAndNothingElseYet)
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

	// The list is exactly one long, which is the honest state of this layer: a client can build the
	// objects it draws with and has nowhere to show them. When `wl_shm` and `xdg_wm_base` land, this
	// number goes up in the same commit as the thing it counts.
	GYRO_CHECK_EQ(bound.Listener.Globals.size(), std::size_t{ 1 });
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

	// The world is untouched, which is what a surface with no buffer means and what makes this an
	// honest waypoint rather than a half-built window.
	GYRO_CHECK_EQ(session.Store.Count(), std::uint32_t{ 0 });
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
