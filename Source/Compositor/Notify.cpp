#include "Compositor/Notify.h"

#include <sys/socket.h>
#include <sys/un.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include "Core/Fd.h"

namespace
{
constexpr const char* AddressVariable = "NOTIFY_SOCKET";

// A status line is a value in a `KEY=VALUE` protocol, so a newline in it would be a second key. The
// sentences that reach here are composed from a backend name, a count and an error's own context, so
// this has nothing to do today — and it is here because the day one of those becomes a client's
// string is the day nobody remembers to check.
[[nodiscard]] std::string OneLine(std::string_view status)
{
	std::string sanitised{ status };

	std::replace_if(
		sanitised.begin(), sanitised.end(), [](char character) { return character == '\n' || character == '\r'; }, ' '
	);

	return sanitised;
}

// The address, filled in for whichever of the two namespaces it names.
//
// **A leading `@` is the abstract namespace and is what systemd usually hands out**, spelled as a
// leading NUL byte in `sun_path` — and the length is then the name's own, with no terminator, since
// an abstract name is bytes rather than a string and a trailing NUL would be part of it. A leading
// `/` is an ordinary file and takes the terminator. Anything else is refused rather than guessed at:
// `vsock:` is a third form this does not implement, and treating it as a path would produce a
// compositor that silently never becomes ready.
[[nodiscard]] Result<std::pair<sockaddr_un, socklen_t>> AddressFor(std::string_view path)
{
	if (path.empty())
	{
		return Failure(EINVAL, "the service manager's address is empty");
	}

	if (path.front() != '/' && path.front() != '@')
	{
		return Failure(EAFNOSUPPORT, "the service manager's address names no unix socket");
	}

	sockaddr_un address{};
	address.sun_family = AF_UNIX;

	// One byte for the terminator on a filesystem path, and none on an abstract name — which still
	// has to fit, because the leading NUL occupies the first byte either way.
	const std::size_t room = sizeof address.sun_path - (path.front() == '/' ? 1U : 0U);

	if (path.size() > room)
	{
		return Failure(ENAMETOOLONG, "the service manager's address is longer than a unix socket path");
	}

	std::memcpy(address.sun_path, path.data(), path.size());

	socklen_t length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size());

	if (path.front() == '@')
	{
		address.sun_path[0] = '\0';
	}
	else
	{
		length += 1;
	}

	return std::pair{ address, length };
}
} // namespace

std::string ServiceManagerAddress()
{
	const char* const named = std::getenv(AddressVariable);

	return named != nullptr ? std::string{ named } : std::string{};
}

Result<void> NotifyReady(std::string_view address, std::string_view status)
{
	const Result<std::pair<sockaddr_un, socklen_t>> destination = AddressFor(address);

	if (!destination)
	{
		return std::unexpected{ destination.error() };
	}

	const Fd socket{ ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0) };

	if (!socket.IsValid())
	{
		return FailFromErrno("opening a socket to the service manager");
	}

	const std::string message = std::format("READY=1\nSTATUS={}", OneLine(status));

	// **Blocking, which is `sd_notify`'s own behaviour and is the right half of the trade here.** A
	// send that could not be delivered is a unit that never becomes active and is killed at
	// `TimeoutStartSec=`, on a machine with no VT to read the reason from; waiting for a receive buffer
	// the service manager sizes generously is the failure that resolves itself.
	//
	// `MSG_NOSIGNAL` because a datagram to an address nothing is bound to should be an `errno` rather
	// than a signal ending a compositor over a status line.
	while (::sendto(
			   socket.Get(),
			   message.data(),
			   message.size(),
			   MSG_NOSIGNAL,
			   reinterpret_cast<const sockaddr*>(&destination->first),
			   destination->second
		   ) < 0)
	{
		if (errno != EINTR)
		{
			return FailFromErrno("telling the service manager gyro is ready", Subject{ address });
		}
	}

	return {};
}
