#include "Session/Transport.h"

#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <cstring>

namespace Session
{
namespace
{
// What the control message can hold, on both directions of travel. `SCM_RIGHTS` puts the descriptors
// in one array, so this is the send limit and the receive limit at once.
constexpr std::size_t ControlBytes = CMSG_SPACE(MaxAttached * sizeof(int));

// A control buffer, aligned for the header the kernel writes into it. A `std::byte` array is
// otherwise aligned for a byte, and `CMSG_FIRSTHDR` would hand back a misaligned `cmsghdr*`.
struct alignas(::cmsghdr) ControlBuffer
{
	std::array<std::byte, ControlBytes> Bytes{};
};
} // namespace

Result<void> Send(RawFd socket, std::span<const std::byte> message, std::span<const RawFd> attached) noexcept
{
	if (attached.size() > MaxAttached)
	{
		return Failure(EINVAL, "sending more descriptors on one handover message than the protocol has roles");
	}

	ControlBuffer control{};

	std::array<int, MaxAttached> descriptors{};
	std::size_t count = 0;

	// **Invalid descriptors are dropped rather than sent as `-1`**, so a caller that has nothing to
	// attach can pass a span of one and get a message with no control data — which is what makes the
	// `Refuse` path below able to answer any message with the same call.
	for (const RawFd one : attached)
	{
		if (one.IsValid())
		{
			descriptors[count] = one.Value;
			++count;
		}
	}

	// `msg_iov` is not const in the struct and the bytes are, so the cast is the API's rather than a
	// choice: `sendmsg` does not write through it.
	::iovec segment{
		.iov_base = const_cast<std::byte*>(message.data()),
		.iov_len = message.size(),
	};

	::msghdr header{};
	header.msg_iov = &segment;
	header.msg_iovlen = 1;

	if (count != 0)
	{
		const std::size_t bytes = sizeof(int) * count;

		header.msg_control = control.Bytes.data();
		header.msg_controllen = CMSG_SPACE(bytes);

		::cmsghdr* rights = CMSG_FIRSTHDR(&header);
		rights->cmsg_level = SOL_SOCKET;
		rights->cmsg_type = SCM_RIGHTS;
		rights->cmsg_len = CMSG_LEN(bytes);

		// One `SCM_RIGHTS` carrying every descriptor rather than one per descriptor: the kernel
		// installs them as a unit, so a receiver cannot be handed a message with half a session on it.
		std::memcpy(CMSG_DATA(rights), descriptors.data(), bytes);
	}

	while (true)
	{
		// `MSG_NOSIGNAL` because a peer that exited between the poll and this call would otherwise take
		// the whole compositor down with `SIGPIPE`, and a peer exiting is the ordinary end of a session
		// rather than an error. `MSG_DONTWAIT` because this runs on the dispatch thread and a socket
		// whose peer has stopped reading must not be able to stop the world.
		const ::ssize_t sent = ::sendmsg(socket.Value, &header, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (sent >= 0)
		{
			return {};
		}

		if (errno == EINTR)
		{
			continue;
		}

		// EAGAIN included, and it is not retried. The messages here are tens of bytes against a socket
		// buffer measured in kilobytes, so a full one is a peer that has stopped reading — which is the
		// same thing as a peer that is gone, and is answered the same way by the caller.
		return FailFromErrno("sending a handover message");
	}
}

Result<Received> Receive(RawFd socket, std::span<std::byte> into) noexcept
{
	Received received;
	ControlBuffer control{};

	::iovec segment{
		.iov_base = into.data(),
		.iov_len = into.size(),
	};

	::msghdr header{};
	header.msg_iov = &segment;
	header.msg_iovlen = 1;
	header.msg_control = control.Bytes.data();
	header.msg_controllen = ControlBytes;

	::ssize_t taken = 0;

	while (true)
	{
		// `MSG_TRUNC` makes the return value the datagram's real length rather than what fit, which is
		// the only way to tell a message that was too long from one that happened to fill the buffer.
		// `MSG_CMSG_CLOEXEC` because a descriptor that arrives without it is one an `exec` would carry
		// into a child — and the party at the other end of this socket exists to spawn children.
		taken = ::recvmsg(socket.Value, &header, MSG_DONTWAIT | MSG_CMSG_CLOEXEC | MSG_TRUNC);

		if (taken >= 0)
		{
			break;
		}

		if (errno == EINTR)
		{
			continue;
		}

		if (errno == EAGAIN || errno == EWOULDBLOCK)
		{
			return received;
		}

		return FailFromErrno("receiving a handover message");
	}

	// Descriptors first and unconditionally, so that they are owned before any judgement is passed on
	// the bytes they came with. Everything below may decide the message is nonsense; none of it has to
	// remember to close anything.
	for (::cmsghdr* rights = CMSG_FIRSTHDR(&header); rights != nullptr; rights = CMSG_NXTHDR(&header, rights))
	{
		if (rights->cmsg_level != SOL_SOCKET || rights->cmsg_type != SCM_RIGHTS)
		{
			continue;
		}

		const std::size_t count = (rights->cmsg_len - CMSG_LEN(0)) / sizeof(int);

		for (std::size_t index = 0; index < count; ++index)
		{
			int descriptor = InvalidFd;
			std::memcpy(&descriptor, CMSG_DATA(rights) + index * sizeof(int), sizeof descriptor);

			if (received.AttachedCount < received.Attached.size())
			{
				received.Attached[received.AttachedCount] = Fd{ descriptor };
				++received.AttachedCount;

				continue;
			}

			// Room ran out inside one control message, which `MaxAttached` says cannot happen while the
			// kernel is the one sizing it — but the loop is the wrong place to prove that, so the
			// descriptor is closed rather than dropped on the floor.
			const Fd unwanted{ descriptor };

			received.Truncated = true;
		}
	}

	if ((header.msg_flags & MSG_CTRUNC) != 0)
	{
		received.Truncated = true;
	}

	// **Zero is the end of the connection, and an empty datagram is deliberately not told apart from
	// it.** The two are distinguishable — Linux sets `MSG_EOR` on a datagram and not on a shutdown —
	// and the distinction buys nothing: Session/Handover.h has no message shorter than a header, so an
	// empty one is a peer this end is about to stop talking to either way.
	if (taken == 0)
	{
		received.State = ReceiveState::Ended;

		return received;
	}

	received.State = ReceiveState::Message;

	const std::size_t length = static_cast<std::size_t>(taken);

	if (length > into.size())
	{
		received.Bytes = into.size();
		received.Truncated = true;

		return received;
	}

	received.Bytes = length;

	return received;
}
} // namespace Session
