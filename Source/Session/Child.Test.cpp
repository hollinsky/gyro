#include "Session/Child.h"

#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <vector>

#include "Core/Fd.h"
#include "Testing/Test.h"

// The fork, against real programs.
//
// **`/bin/sh` rather than a helper this build produces**, because what is being tested is `execvpe`
// and the environment reaching the other side of it — and a helper binary would have to be found,
// which is the same `PATH` question one layer down.

namespace
{
using namespace Session;

// Wait for the child to finish, since nothing here has a loop to be woken by `SIGCHLD`.
[[nodiscard]] int Await(Child& child)
{
	for (int attempt = 0; attempt < 1000; ++attempt)
	{
		if (const std::optional<int> ended = child.Reap(); ended)
		{
			return *ended;
		}

		// The parent has nothing else to do, so a blocking wait on nothing in particular is the
		// cheapest way to give the child the processor.
		::usleep(1000);
	}

	return -1;
}

[[nodiscard]] std::vector<std::string> Shell(std::string_view script)
{
	return { "/bin/sh", "-c", std::string{ script } };
}
} // namespace

GYRO_TEST(Child, StartsAProgramAndCollectsIt)
{
	Child child;
	GYRO_CHECK(!child.IsRunning());

	GYRO_REQUIRE(child.Start(Shell("exit 0"), "wayland-0").has_value());
	GYRO_CHECK(child.IsRunning());

	const int status = Await(child);
	GYRO_REQUIRE(WIFEXITED(status));
	GYRO_CHECK(WEXITSTATUS(status) == 0);
	GYRO_CHECK(!child.IsRunning());

	// Reaped once. A second call answers nothing rather than waiting on a pid that is no longer this
	// process's, which is what would happen if the field were not cleared.
	GYRO_CHECK(!child.Reap().has_value());
}

// The shell's connection, and the whole reason it is a descriptor rather than a path: the variable
// names a file the child inherited, so nothing had to be told where the socket is.
GYRO_TEST(Child, PassesAConnectionAsAnInheritedDescriptor)
{
	int pair[2] = { -1, -1 };
	GYRO_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);

	const Fd mine{ pair[0] };
	const Fd theirs{ pair[1] };

	Child child;

	// `test -e /proc/self/fd/N` is the child asking whether the descriptor the variable names actually
	// survived the exec, which is the part that would silently not — a `WAYLAND_SOCKET` naming a closed
	// descriptor is a client that fails to connect for a reason nothing in its log explains.
	GYRO_REQUIRE(child
	                 .Start(
						 Shell("test -n \"$WAYLAND_SOCKET\" && test -e \"/proc/self/fd/$WAYLAND_SOCKET\""),
						 "wayland-0",
						 theirs.Borrow()
					 )
	                 .has_value());

	const int status = Await(child);
	GYRO_REQUIRE(WIFEXITED(status));
	GYRO_CHECK(WEXITSTATUS(status) == 0);
}

// A client that is not the shell is started with no connection at all, so nothing it inherits could
// be mistaken for one — including a `WAYLAND_SOCKET` left in the agent's own environment by whatever
// started it, which would otherwise name a descriptor in the wrong process.
GYRO_TEST(Child, StartsAnOrdinaryClientWithNoSocketVariable)
{
	GYRO_REQUIRE(::setenv("WAYLAND_SOCKET", "7", 1) == 0);

	Child child;

	GYRO_REQUIRE(child.Start(Shell("test -z \"$WAYLAND_SOCKET\""), "wayland-0").has_value());

	const int status = Await(child);

	GYRO_REQUIRE(::unsetenv("WAYLAND_SOCKET") == 0);
	GYRO_REQUIRE(WIFEXITED(status));
	GYRO_CHECK(WEXITSTATUS(status) == 0);
}

GYRO_TEST(Child, PassesTheDisplayInTheEnvironment)
{
	Child child;

	// The whole of what the agent exists to guarantee: the variable is there before the program is,
	// so there is no window in which a client could look for it and not find it.
	GYRO_REQUIRE(child.Start(Shell("test \"$WAYLAND_DISPLAY\" = wayland-9"), "wayland-9").has_value());

	const int status = Await(child);
	GYRO_REQUIRE(WIFEXITED(status));
	GYRO_CHECK_EQ(WEXITSTATUS(status), 0);
}

GYRO_TEST(Child, ReplacesADisplayTheEnvironmentAlreadyHad)
{
	// An agent started from inside another compositor's session inherits that one's display, and a
	// client handed it would connect to the wrong compositor — which looks like gyro drawing nothing.
	GYRO_REQUIRE(::setenv("WAYLAND_DISPLAY", "wayland-inherited", 1) == 0);

	Child child;
	GYRO_REQUIRE(child.Start(Shell("test \"$WAYLAND_DISPLAY\" = wayland-2"), "wayland-2").has_value());

	const int status = Await(child);
	GYRO_REQUIRE(WIFEXITED(status));
	GYRO_CHECK_EQ(WEXITSTATUS(status), 0);

	GYRO_CHECK(::unsetenv("WAYLAND_DISPLAY") == 0);
}

GYRO_TEST(Child, ReportsAProgramThatIsNotThere)
{
	Child child;

	// `execvpe` failing is the child's problem rather than the parent's — the fork has already
	// succeeded — so it arrives as an exit status and not as an error from `Start`. 127 is what a shell
	// reports for the same thing.
	GYRO_REQUIRE(child.Start({ std::vector<std::string>{ "gyro-no-such-program" } }, "wayland-0").has_value());

	const int status = Await(child);
	GYRO_REQUIRE(WIFEXITED(status));
	GYRO_CHECK_EQ(WEXITSTATUS(status), 127);
}

GYRO_TEST(Child, RefusesASecondProgramAndAnEmptyOne)
{
	Child child;

	GYRO_REQUIRE(!child.Start({}, "wayland-0").has_value());

	GYRO_REQUIRE(child.Start(Shell("sleep 30"), "wayland-0").has_value());

	const Result<void> second = child.Start(Shell("exit 0"), "wayland-0");
	GYRO_REQUIRE(!second.has_value());
	GYRO_CHECK(second.error().Code() == EBUSY);

	child.Stop();

	const int status = Await(child);
	GYRO_CHECK(WIFSIGNALED(status));
	GYRO_CHECK(DescribeExit(status) == "killed by signal 15");
}
