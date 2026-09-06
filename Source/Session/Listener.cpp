#include "Session/Listener.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <format>
#include <utility>

namespace Session
{
namespace
{
// How many clients may be waiting to be accepted. libwayland's number, and it is the one that
// matters here rather than a number of this file's own: the queue is what holds the connections that
// arrive while gyro is restarting, so it is sized for a session's worth of applications starting at
// once rather than for a steady state.
constexpr int Backlog = 128;

// Whether a path fits the address structure, which is a fixed array rather than a pointer. Checked
// before the bind because `bind` would otherwise take the truncation and listen on a path that is not
// the one the caller named — and the caller would then export a `WAYLAND_DISPLAY` nothing resolves.
[[nodiscard]] bool Fits(std::string_view path) noexcept
{
	return !path.empty() && path.size() < sizeof(::sockaddr_un::sun_path);
}

// Take the name's lock, or say why not. `EADDRINUSE` is *somebody has this display*, which the
// automatic search below treats as a reason to move on rather than as a failure.
[[nodiscard]] Result<Fd> LockDisplay(const std::string& path)
{
	const std::string lockPath = path + ".lock";

	if (!Fits(lockPath))
	{
		return Failure(ENAMETOOLONG, "the display's lock path is longer than a socket address", Subject{ lockPath });
	}

	// 0660 rather than 0600, matching libwayland: the file is the user's own and the mode is what a
	// reader comparing the two directories would expect to find.
	Fd lock{ ::open(lockPath.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0660) };

	if (!lock.IsValid())
	{
		return FailFromErrno("opening a display's lock file", Subject{ lockPath });
	}

	if (::flock(lock.Get(), LOCK_EX | LOCK_NB) != 0)
	{
		if (errno == EWOULDBLOCK || errno == EINTR)
		{
			return Failure(EADDRINUSE, "the display number is already taken");
		}

		return FailFromErrno("locking a display", Subject{ lockPath });
	}

	return lock;
}

// Create, bind and listen. The tail both listeners share, which is every part of making one that is
// not about the name.
[[nodiscard]] Result<Fd> BindAt(const std::string& path)
{
	Fd socket{ ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0) };

	if (!socket.IsValid())
	{
		return FailFromErrno("creating a listening socket");
	}

	::sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, path.data(), path.size());

	if (::bind(socket.Get(), reinterpret_cast<const ::sockaddr*>(&address), sizeof address) != 0)
	{
		return FailFromErrno("binding a listening socket", Subject{ path });
	}

	if (::listen(socket.Get(), Backlog) != 0)
	{
		return FailFromErrno("listening on a socket", Subject{ path });
	}

	return socket;
}
} // namespace

Result<WaylandListener> BindWaylandListener(std::string_view directory, std::string_view name)
{
	if (directory.empty())
	{
		return Failure(ENOENT, "no runtime directory to bind a listener in");
	}

	if (name.empty())
	{
		for (unsigned number = 0; number < MaxDisplayNumber; ++number)
		{
			Result<WaylandListener> bound = BindWaylandListener(directory, std::format("{}{}", DisplayPrefix, number));

			// Taken is *try the next one*; anything else is a machine that is not going to work and is
			// reported as it happened rather than swallowed by the search.
			if (bound || bound.error().Code() != EADDRINUSE)
			{
				return bound;
			}
		}

		return Failure(EADDRINUSE, "every display number is taken");
	}

	std::string path = std::format("{}/{}", directory, name);

	if (!Fits(path))
	{
		return Failure(ENAMETOOLONG, "the display's path is longer than a socket address", Subject{ path });
	}

	Result<Fd> lock = LockDisplay(path);

	if (!lock)
	{
		return std::unexpected{ lock.error() };
	}

	// **Holding the lock is what makes the unlink safe**, and it is the whole reason the lock is taken
	// before the socket rather than after: a path left behind by a process that died is indistinguishable
	// from a live one by looking at it, and the lock is the thing that tells them apart. `ENOENT` is the
	// ordinary case of a display nobody has used yet.
	if (::unlink(path.c_str()) != 0 && errno != ENOENT)
	{
		return FailFromErrno("removing a stale display socket", Subject{ path });
	}

	Result<Fd> socket = BindAt(path);

	if (!socket)
	{
		return std::unexpected{ socket.error() };
	}

	return WaylandListener{
		.Socket = std::move(*socket),
		.Lock = std::move(*lock),
		.Name = std::string{ name },
		.Path = std::move(path),
	};
}

Result<ShellListener> BindShellListener(std::string_view directory, std::string_view display)
{
	if (directory.empty())
	{
		return Failure(ENOENT, "no runtime directory to bind a shell listener in");
	}

	if (display.empty())
	{
		return Failure(EINVAL, "a shell listener is named after a display and there is none");
	}

	std::string path = std::format("{}/{}{}", directory, display, ShellSuffix);

	if (!Fits(path))
	{
		return Failure(ENAMETOOLONG, "the shell listener's path is longer than a socket address", Subject{ path });
	}

	// **No lock of its own, and the display's is what makes that safe.** This name exists only because
	// that display name was locked, so anybody who could collide here has already been refused one
	// step earlier — and a lock file is a thing the rest of the world reads to find a free display
	// number, which this is not.
	//
	// A stale socket is unlinked for `BindWaylandListener`'s reason, and under the same protection: the
	// display's lock is held by the time this runs.
	if (::unlink(path.c_str()) != 0 && errno != ENOENT)
	{
		return FailFromErrno("removing a stale shell socket", Subject{ path });
	}

	Result<Fd> socket = BindAt(path);

	if (!socket)
	{
		return std::unexpected{ socket.error() };
	}

	return ShellListener{ .Socket = std::move(*socket), .Path = std::move(path) };
}

Result<Fd> ConnectTo(std::string_view path)
{
	if (!Fits(path))
	{
		return Failure(ENAMETOOLONG, "the listener's path is not one an address can hold", Subject{ path });
	}

	// **Not `CLOEXEC`, and that is the point of the call.** This descriptor is going through a `fork`
	// and an `exec` into the shell, so closing it there would leave a `WAYLAND_SOCKET` naming nothing.
	// It costs the agent one inheritable descriptor, which every other program it starts would also
	// inherit — so the agent starts exactly one program and this is it.
	Fd socket{ ::socket(AF_UNIX, SOCK_STREAM, 0) };

	if (!socket.IsValid())
	{
		return FailFromErrno("creating a socket to the shell's listener");
	}

	::sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, path.data(), path.size());

	if (::connect(socket.Get(), reinterpret_cast<const ::sockaddr*>(&address), sizeof address) != 0)
	{
		return FailFromErrno("connecting to the shell's listener", Subject{ path });
	}

	return socket;
}
} // namespace Session
