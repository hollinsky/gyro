#include "Session/Listener.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>

#include "Core/Fd.h"
#include "Session/Control.h"
#include "Testing/Test.h"

// The listener an agent binds, against real sockets in a temporary directory.
//
// **What every test here is really asking is whether Session/Control.h would take it**, which is why
// `InspectOffer` appears below rather than a hand-written set of `getsockopt` calls: the two files are
// the two halves of one agreement, and a test that checked the half it can see would keep passing
// while the halves drifted.

namespace
{
using namespace Session;

class TemporaryDirectory
{
public:
	TemporaryDirectory()
	{
		std::string pattern = (std::filesystem::temp_directory_path() / "gyro-listener-XXXXXX").string();

		if (const char* made = ::mkdtemp(pattern.data()); made != nullptr)
		{
			m_Path = made;
		}
	}

	~TemporaryDirectory()
	{
		if (!m_Path.empty())
		{
			std::error_code ignored;
			std::filesystem::remove_all(m_Path, ignored);
		}
	}

	TemporaryDirectory(const TemporaryDirectory&) = delete;
	TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

	[[nodiscard]] const std::string& Path() const noexcept { return m_Path; }

private:
	std::string m_Path;
};

// Whether a client could connect to it, which is the only thing a display number means to a person.
[[nodiscard]] bool Reachable(const std::string& path)
{
	const Fd socket{ ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0) };

	if (!socket.IsValid() || path.size() >= sizeof(::sockaddr_un::sun_path))
	{
		return false;
	}

	::sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, path.data(), path.size());

	return ::connect(socket.Get(), reinterpret_cast<const ::sockaddr*>(&address), sizeof address) == 0;
}
} // namespace

GYRO_TEST(WaylandListener, BindsTheFirstFreeDisplay)
{
	const TemporaryDirectory directory;
	GYRO_REQUIRE(!directory.Path().empty());

	const Result<WaylandListener> first = BindWaylandListener(directory.Path());
	GYRO_REQUIRE(first.has_value());

	GYRO_CHECK(first->Name == "wayland-0");
	GYRO_CHECK(first->Path == directory.Path() + "/wayland-0");
	GYRO_CHECK(Reachable(first->Path));

	// The second agent on the same directory takes the next number rather than the same one, which is
	// what the lock is for: without it both would bind `wayland-0` and the second would unlink the
	// first's socket out from under a live session.
	const Result<WaylandListener> second = BindWaylandListener(directory.Path());
	GYRO_REQUIRE(second.has_value());

	GYRO_CHECK(second->Name == "wayland-1");
	GYRO_CHECK(Reachable(second->Path));
	GYRO_CHECK(Reachable(first->Path));
}

GYRO_TEST(WaylandListener, RefusesADisplayThatIsTaken)
{
	const TemporaryDirectory directory;
	GYRO_REQUIRE(!directory.Path().empty());

	const Result<WaylandListener> held = BindWaylandListener(directory.Path(), "wayland-7");
	GYRO_REQUIRE(held.has_value());

	const Result<WaylandListener> again = BindWaylandListener(directory.Path(), "wayland-7");
	GYRO_REQUIRE(!again.has_value());
	GYRO_CHECK(again.error().Code() == EADDRINUSE);
}

GYRO_TEST(WaylandListener, TakesADisplayWhoseHolderIsGone)
{
	const TemporaryDirectory directory;
	GYRO_REQUIRE(!directory.Path().empty());

	std::string path;

	{
		const Result<WaylandListener> first = BindWaylandListener(directory.Path(), "wayland-3");
		GYRO_REQUIRE(first.has_value());

		path = first->Path;
	}

	// The socket file outlives the process that bound it — a machine that lost power leaves one — and
	// the lock is what says nobody owns it any more.
	GYRO_CHECK(std::filesystem::exists(path));

	const Result<WaylandListener> second = BindWaylandListener(directory.Path(), "wayland-3");
	GYRO_REQUIRE(second.has_value());
	GYRO_CHECK(Reachable(second->Path));
}

GYRO_TEST(WaylandListener, BindsSomethingGyroWouldAccept)
{
	const TemporaryDirectory directory;
	GYRO_REQUIRE(!directory.Path().empty());

	const Result<WaylandListener> listener = BindWaylandListener(directory.Path());
	GYRO_REQUIRE(listener.has_value());

	GYRO_CHECK(InspectOffer(listener->Socket.Borrow(), directory.Path()).has_value());

	// And would not, where the offer names a directory that is not the offering user's.
	const Result<void> elsewhere = InspectOffer(listener->Socket.Borrow(), directory.Path() + "/somebody-else");
	GYRO_REQUIRE(!elsewhere.has_value());
	GYRO_CHECK(elsewhere.error().Code() == EACCES);
}

GYRO_TEST(WaylandListener, RefusesADirectoryThatIsNotThere)
{
	const Result<WaylandListener> nowhere = BindWaylandListener("");
	GYRO_REQUIRE(!nowhere.has_value());
	GYRO_CHECK(nowhere.error().Code() == ENOENT);
}

// The shell's socket is bound beside the display it is named after, and gyro would take it: the same
// four questions are asked of both listeners, because both are sockets a user bound in their own
// runtime directory and only the agent knows which is which.
GYRO_TEST(ShellListener, BindsBesideTheDisplayAndIsSomethingGyroWouldAccept)
{
	const TemporaryDirectory directory;
	GYRO_REQUIRE(!directory.Path().empty());

	const Result<WaylandListener> display = BindWaylandListener(directory.Path(), "wayland-4");
	GYRO_REQUIRE(display.has_value());

	const Result<ShellListener> shell = BindShellListener(directory.Path(), display->Name);
	GYRO_REQUIRE(shell.has_value());

	GYRO_CHECK(shell->Path == directory.Path() + "/wayland-4-shell");
	GYRO_CHECK(Reachable(shell->Path));
	GYRO_CHECK(InspectOffer(shell->Socket.Borrow(), directory.Path()).has_value());

	// **No lock of its own**, which is what makes the name free: it exists only because the display's
	// number was locked, so there is nobody left to race with.
	GYRO_CHECK(!std::filesystem::exists(shell->Path + ".lock"));
}

// A stale socket from a machine that lost power is taken, exactly as the display's is, and under the
// same protection — the caller holds the display's lock by the time this runs.
GYRO_TEST(ShellListener, TakesAStaleSocketLeftBehind)
{
	const TemporaryDirectory directory;
	GYRO_REQUIRE(!directory.Path().empty());

	{
		const Result<WaylandListener> display = BindWaylandListener(directory.Path(), "wayland-2");
		GYRO_REQUIRE(display.has_value());
		GYRO_REQUIRE(BindShellListener(directory.Path(), display->Name).has_value());
	}

	const Result<WaylandListener> again = BindWaylandListener(directory.Path(), "wayland-2");
	GYRO_REQUIRE(again.has_value());

	const Result<ShellListener> shell = BindShellListener(directory.Path(), again->Name);

	GYRO_REQUIRE(shell.has_value());
	GYRO_CHECK(Reachable(shell->Path));
}

GYRO_TEST(ShellListener, RefusesWithNoDisplayToBeNamedAfter)
{
	const TemporaryDirectory directory;
	GYRO_REQUIRE(!directory.Path().empty());

	const Result<ShellListener> nameless = BindShellListener(directory.Path(), "");

	GYRO_REQUIRE(!nameless.has_value());
	GYRO_CHECK_EQ(nameless.error().Code(), EINVAL);
}

// The connection the agent makes on the shell's behalf, which is the whole of how the socket's path
// stays out of every environment on the machine.
GYRO_TEST(ShellListener, ConnectsForTheShellSoNothingHasToBeToldThePath)
{
	const TemporaryDirectory directory;
	GYRO_REQUIRE(!directory.Path().empty());

	const Result<WaylandListener> display = BindWaylandListener(directory.Path(), "wayland-0");
	GYRO_REQUIRE(display.has_value());

	const Result<ShellListener> shell = BindShellListener(directory.Path(), display->Name);
	GYRO_REQUIRE(shell.has_value());

	const Result<Fd> connection = ConnectTo(shell->Path);

	GYRO_REQUIRE(connection.has_value());
	GYRO_CHECK(connection->IsValid());

	// **Inheritable, which is the point rather than an oversight**: this descriptor goes through a fork
	// and an exec into the shell, and one that closed there would leave a `WAYLAND_SOCKET` naming
	// nothing.
	GYRO_CHECK((::fcntl(connection->Get(), F_GETFD) & FD_CLOEXEC) == 0);
}

GYRO_TEST(ShellListener, RefusesADirectoryThatIsNotThere)
{
	const Result<ShellListener> nowhere = BindShellListener("", "wayland-0");

	GYRO_REQUIRE(!nowhere.has_value());
	GYRO_CHECK_EQ(nowhere.error().Code(), ENOENT);
}
