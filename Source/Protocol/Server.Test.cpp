// `setenv` and `mkdtemp` are POSIX rather than ISO C, and glibc gates both behind a feature-test
// macro. Named here for the reason Compositor/Wait.cpp names its own: what is wanted from the platform
// is stated rather than inherited.
#define _POSIX_C_SOURCE 200809L

#include "Protocol/Server.h"

#include <sys/stat.h>

#include <cstdlib>
#include <string>

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

[[nodiscard]] bool SocketFileExists(std::string_view name)
{
	const std::string path = g_RuntimeDir.Path + "/" + std::string{ name };
	struct stat info = {};

	return ::stat(path.c_str(), &info) == 0;
}
} // namespace

GYRO_TEST(Server, OpensAndBindsARealSocket)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	Server server;

	GYRO_REQUIRE(server.Open().has_value());
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

	GYRO_REQUIRE(server.Open("gyro-explicit-0").has_value());
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

	GYRO_REQUIRE(first.Open("gyro-contended").has_value());

	// The name is already bound by `first`, so the second server cannot have it — and comes back not
	// open rather than half-constructed.
	Server second;
	const Result<void> taken = second.Open("gyro-contended");

	GYRO_CHECK(!taken.has_value());
	GYRO_CHECK(!second.IsOpen());
}
