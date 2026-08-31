// `setenv` and `mkdtemp` are POSIX rather than ISO C, and glibc gates both behind a feature-test
// macro. Named here for the reason Compositor/Wait.cpp names its own: what is wanted from the platform
// is stated rather than inherited.
#define _POSIX_C_SOURCE 200809L

#include "Protocol/Server.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "Testing/Test.h"

// What is worth testing here is the bring-up rather than the protocol: that a socket is actually bound
// and not merely a name returned, that the loop hands out one real descriptor, and that the failures a
// boot service meets on a busy machine — a name already taken, a double open — are refusals rather
// than a second display nobody tracks. The requests a client sends arrive with the globals that answer
// them, which is the next step; here there is nothing yet to answer.

namespace
{
// A private XDG_RUNTIME_DIR, so the test binds real sockets under a directory it owns rather than the
// developer's live session — where `wayland-0` is a running compositor and colliding with it is the
// test's fault, not a finding. `mkdtemp` makes it `0700`, which is the mode libwayland requires of a
// runtime directory and refuses without.
struct PrivateRuntimeDir
{
	PrivateRuntimeDir()
	{
		char pattern[] = "/tmp/gyro-protocol-XXXXXX";
		const char* const made = ::mkdtemp(pattern);

		if (made != nullptr)
		{
			Path = made;
			::setenv("XDG_RUNTIME_DIR", Path.c_str(), 1);
		}
	}

	std::string Path;
};

// Constructed at namespace scope so the override is in place before *any* test's `Open` runs, rather
// than on the first test that happens to name it — a test that binds against the real runtime
// directory is one that collides with whatever is already there.
const PrivateRuntimeDir g_RuntimeDir;

// Read back from the environment rather than from `g_RuntimeDir`, because Host.Test.cpp installs one
// of these too and which of the two wins is static initialisation order across translation units. Both
// are private directories and either is fine to bind under; asking one where the other's socket is, is
// not.
[[nodiscard]] bool SocketFileExists(std::string_view name)
{
	const char* const directory = ::getenv("XDG_RUNTIME_DIR");

	if (directory == nullptr)
	{
		return false;
	}

	const std::string path = std::string{ directory } + "/" + std::string{ name };
	struct stat info = {};

	return ::stat(path.c_str(), &info) == 0;
}
} // namespace

GYRO_TEST(Server, OpensAndBindsARealSocket)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Server server;

	GYRO_REQUIRE(server.Open().has_value());
	GYRO_REQUIRE(server.Bind().has_value());
	GYRO_CHECK(server.IsOpen());

	// A name that names a file, not merely a string that came back. The socket is what a client
	// connects to, so its absence is the failure that looks like success everywhere else.
	GYRO_CHECK(!server.SocketName().empty());
	GYRO_CHECK(SocketFileExists(server.SocketName()));

	// One real descriptor for the root to add to its wait.
	GYRO_CHECK(server.PollFd() >= 0);
}

GYRO_TEST(Server, PollWithNoClientsIsClean)
{
	Server server;

	GYRO_REQUIRE(server.Open().has_value());

	// Nothing is connected, so this reads nothing and returns at once. What it proves is that the loop
	// the root will drive every step is drivable with an empty set.
	GYRO_CHECK(server.Poll().has_value());

	// And pushing events out to no one is equally a no-op rather than a fault.
	server.Flush();
}

GYRO_TEST(Server, AnExplicitNameIsBoundVerbatim)
{
	Server server;

	GYRO_REQUIRE(server.Open().has_value());
	GYRO_REQUIRE(server.Bind("gyro-explicit-0").has_value());
	GYRO_CHECK(server.SocketName() == "gyro-explicit-0");
	GYRO_CHECK(SocketFileExists("gyro-explicit-0"));
}

GYRO_TEST(Server, ASecondOpenIsRefused)
{
	Server server;

	GYRO_REQUIRE(server.Open().has_value());

	// A second bring-up on an already-open server is the caller's mistake, and the display it already
	// holds is left untouched rather than leaked behind a second one.
	const Result<void> again = server.Open();

	GYRO_CHECK(!again.has_value());
	GYRO_CHECK(server.IsOpen());
}

GYRO_TEST(Server, ATakenNameIsRefused)
{
	Server first;

	GYRO_REQUIRE(first.Open().has_value());
	GYRO_REQUIRE(first.Bind("gyro-contended").has_value());

	// The name is already bound by `first`, so the second server cannot have it — and comes back with
	// no socket rather than half-bound.
	Server second;

	GYRO_REQUIRE(second.Open().has_value());

	const Result<void> taken = second.Bind("gyro-contended");

	GYRO_CHECK(!taken.has_value());
	GYRO_CHECK(second.SocketName().empty());
}

// A listener of the shape a session agent offers: bound under the private runtime directory, listening,
// and this process's own — which is the only uid a same-uid test can admit against.
namespace
{
struct OfferedListener
{
	explicit OfferedListener(std::string_view name)
	{
		const char* const directory = ::getenv("XDG_RUNTIME_DIR");

		if (directory == nullptr)
		{
			return;
		}

		Path = std::string{ directory } + "/" + std::string{ name };

		Socket = Fd{ ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0) };

		if (!Socket.IsValid())
		{
			return;
		}

		::sockaddr_un address{};
		address.sun_family = AF_UNIX;
		std::memcpy(address.sun_path, Path.c_str(), std::min(Path.size(), sizeof address.sun_path - 1));

		::unlink(Path.c_str());

		if (::bind(Socket.Get(), reinterpret_cast<const ::sockaddr*>(&address), sizeof address) != 0 ||
		    ::listen(Socket.Get(), 4) != 0)
		{
			Socket.Reset();
		}
	}

	~OfferedListener()
	{
		if (!Path.empty())
		{
			::unlink(Path.c_str());
		}
	}

	OfferedListener(const OfferedListener&) = delete;
	OfferedListener& operator=(const OfferedListener&) = delete;
	OfferedListener(OfferedListener&&) = delete;
	OfferedListener& operator=(OfferedListener&&) = delete;

	std::string Path;
	Fd Socket;
};

// Connect to a path the way a client does, and answer the descriptor or nothing. Deliberately not
// `wl_display_connect`: what is being tested is whether gyro accepts at all, and a real client would
// go on to demarshal a registry this test has no globals for.
[[nodiscard]] Fd ConnectTo(const std::string& path)
{
	Fd socket{ ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0) };

	if (!socket.IsValid())
	{
		return {};
	}

	::sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, path.c_str(), std::min(path.size(), sizeof address.sun_path - 1));

	if (::connect(socket.Get(), reinterpret_cast<const ::sockaddr*>(&address), sizeof address) != 0)
	{
		return {};
	}

	return socket;
}

// The one client the display is holding, or null where it has none or more than one. libwayland's own
// list walk, because `Server` deliberately hands out a count rather than the clients themselves — and
// what these tests need is the pointer `TrustOf` is asked about, which no gyro API mints.
[[nodiscard]] wl_client* TheOnlyClient(wl_display* display)
{
	wl_list* const clients = wl_display_get_client_list(display);
	wl_client* found = nullptr;
	wl_client* client = nullptr;

	wl_client_for_each(client, clients)
	{
		if (found != nullptr)
		{
			return nullptr;
		}

		found = client;
	}

	return found;
}
} // namespace

GYRO_TEST(Server, AnAdoptedListenerServesClients)
{
	OfferedListener offered{ "gyro-adopted-0" };

	GYRO_REQUIRE(offered.Socket.IsValid());

	Server server;

	GYRO_REQUIRE(server.Open().has_value());
	GYRO_REQUIRE(server.Adopt(std::move(offered.Socket), ::getuid(), static_cast<SessionId>(1)).has_value());

	// No socket of gyro's own: everything this run serves arrived from an agent, which is what the
	// composition root refuses to mix.
	GYRO_CHECK(server.SocketName().empty());

	const Fd client = ConnectTo(offered.Path);

	GYRO_REQUIRE(client.IsValid());

	// The accept runs inside the display's own loop, which is the loop the root already drives every
	// step — so a connection is served by the same `Poll` a request is.
	GYRO_REQUIRE(server.Poll().has_value());
	GYRO_CHECK(server.Clients() == 1);
}

GYRO_TEST(Server, ReleasingASessionClosesItsListenerAndEndsItsClients)
{
	OfferedListener offered{ "gyro-adopted-1" };

	GYRO_REQUIRE(offered.Socket.IsValid());

	Server server;

	GYRO_REQUIRE(server.Open().has_value());
	GYRO_REQUIRE(server.Adopt(std::move(offered.Socket), ::getuid(), static_cast<SessionId>(7)).has_value());

	const Fd client = ConnectTo(offered.Path);

	GYRO_REQUIRE(client.IsValid());
	GYRO_REQUIRE(server.Poll().has_value());
	GYRO_REQUIRE(server.Clients() == 1);

	// The agent's connection closing is the session ending, and a session whose windows stayed on
	// screen with nothing behind them would make that event advisory.
	server.Release(static_cast<SessionId>(7));

	GYRO_CHECK(server.Clients() == 0);

	// And nothing is listening on the path any more, so the next client is refused by the kernel rather
	// than accepted into a session that has gone.
	GYRO_CHECK(!ConnectTo(offered.Path).IsValid());
}

GYRO_TEST(Server, ReleasingAnUnknownSessionDoesNothing)
{
	Server server;

	GYRO_REQUIRE(server.Open().has_value());
	GYRO_REQUIRE(server.Bind("gyro-adopted-2").has_value());

	// Every client under `Bind` belongs to no session, so this is the ordinary case rather than an edge:
	// a run that never took an offer still has `Release` called on it by nothing at all.
	server.Release(static_cast<SessionId>(3));

	GYRO_CHECK(server.SocketName() == "gyro-adopted-2");
}

GYRO_TEST(Server, AnAdoptedListenersTrustIsWhatItsClientsGet)
{
	OfferedListener offered{ "gyro-adopted-3" };

	GYRO_REQUIRE(offered.Socket.IsValid());

	Server server;

	GYRO_REQUIRE(server.Open().has_value());

	// **Nothing in the compositor passes this today**, and the test does rather than waiting for the
	// System listener Docs/Open.md still owes: what is being checked is that trust travels from the
	// socket to the client, which is the whole of Tier.h's rule, and a parameter no caller exercises is
	// one that is wrong the day the first one does.
	GYRO_REQUIRE(
		server.Adopt(std::move(offered.Socket), ::getuid(), static_cast<SessionId>(4), Trust::System).has_value()
	);

	const Fd client = ConnectTo(offered.Path);

	GYRO_REQUIRE(client.IsValid());
	GYRO_REQUIRE(server.Poll().has_value());
	GYRO_REQUIRE(server.Clients() == 1);

	wl_client* const admitted = TheOnlyClient(server.Display());

	GYRO_REQUIRE(admitted != nullptr);
	GYRO_CHECK(server.TrustOf(admitted) == Trust::System);
	GYRO_CHECK(server.SessionOf(admitted) == static_cast<SessionId>(4));
}

GYRO_TEST(Server, AClientOnASocketGyroBoundItselfIsAnApplication)
{
	Server server;

	GYRO_REQUIRE(server.Open().has_value());
	GYRO_REQUIRE(server.Bind("gyro-adopted-4").has_value());

	const char* const directory = ::getenv("XDG_RUNTIME_DIR");

	GYRO_REQUIRE(directory != nullptr);

	const Fd client = ConnectTo(std::string{ directory } + "/gyro-adopted-4");

	GYRO_REQUIRE(client.IsValid());
	GYRO_REQUIRE(server.Poll().has_value());

	// libwayland accepted this one, so gyro never saw the connection and has no record of it — which is
	// why the count is zero while the display is holding a client. The answer has to come from the
	// direction the unknown case falls rather than from a lookup that succeeds.
	GYRO_CHECK(server.Clients() == 0);

	wl_client* const unrecorded = TheOnlyClient(server.Display());

	GYRO_REQUIRE(unrecorded != nullptr);
	GYRO_CHECK(server.TrustOf(unrecorded) == Trust::User);
	GYRO_CHECK(server.SessionOf(unrecorded) == SessionId::None);
}
