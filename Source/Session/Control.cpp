#include "Session/Control.h"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <utility>

#include "Session/Transport.h"

namespace Session
{
namespace
{
// How many agents may be waiting to be accepted. Small on purpose: an agent connects once per session
// and retries, so a backlog is a machine under attack rather than a machine under load.
constexpr int Backlog = 8;

// How many ready descriptors one `epoll_wait` reports. The loop runs again while it fills, so this is
// a batch size and not a limit on connections.
constexpr int ReadyBatch = 16;

// Send a refusal and say so. The last thing gyro says on a connection: the caller drops it.
void Refuse(RawFd socket, int code, const char* reason)
{
	std::array<std::byte, MaxMessageBytes> bytes{};
	const Refused message{ .Code = static_cast<std::uint32_t>(code), .Text = Reason{ reason } };
	const std::size_t written = message.Encode(bytes);

	// Nothing to do about a refusal that could not be sent, and nothing worth logging either: the
	// connection is being closed, which is the part that matters.
	const Result<void> sent = Send(socket, std::span<const std::byte>{ bytes }.first(written), RawFd{});

	spdlog::warn("session handover refused: {} ({}){}", reason, code, sent ? "" : ", and the refusal did not send");
}

// The uid, gid and pid the kernel attributes to the far end. Not what the peer said — it says nothing.
[[nodiscard]] Result<::ucred> PeerOf(RawFd socket) noexcept
{
	::ucred credentials{};
	::socklen_t length = sizeof credentials;

	if (::getsockopt(socket.Value, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0)
	{
		return FailFromErrno("reading the credentials of a handover peer");
	}

	return credentials;
}

[[nodiscard]] Result<int> SocketOption(RawFd socket, int option) noexcept
{
	int value = 0;
	::socklen_t length = sizeof value;

	if (::getsockopt(socket.Value, SOL_SOCKET, option, &value, &length) != 0)
	{
		return FailFromErrno("reading an option of an offered descriptor");
	}

	return value;
}

// The directory part of a path, for the one `mkdir` `Open` performs.
[[nodiscard]] std::string_view DirectoryOf(std::string_view path) noexcept
{
	const std::size_t slash = path.rfind('/');

	return slash == std::string_view::npos || slash == 0 ? std::string_view{} : path.substr(0, slash);
}
} // namespace

std::string RuntimeDirectoryFor(std::uint32_t uid, std::string_view root)
{
	return std::format("{}/{}", root, uid);
}

Result<void> InspectOffer(RawFd offered, std::string_view directory) noexcept
{
	if (!offered.IsValid())
	{
		return Failure(EBADF, "the offer carried no descriptor");
	}

	const Result<int> domain = SocketOption(offered, SO_DOMAIN);

	if (!domain)
	{
		return std::unexpected{ domain.error() };
	}

	if (*domain != AF_UNIX)
	{
		return Failure(EBADF, "the offered descriptor is not an AF_UNIX socket");
	}

	const Result<int> type = SocketOption(offered, SO_TYPE);

	if (!type)
	{
		return std::unexpected{ type.error() };
	}

	// Stream, because that is what a Wayland client connects with. A sequenced-packet or datagram
	// socket here would be accepted by `wl_display_add_socket_fd` and then never produce a client.
	if (*type != SOCK_STREAM)
	{
		return Failure(EBADF, "the offered descriptor is not a stream socket");
	}

	const Result<int> listening = SocketOption(offered, SO_ACCEPTCONN);

	if (!listening)
	{
		return std::unexpected{ listening.error() };
	}

	if (*listening == 0)
	{
		return Failure(EBADF, "the offered descriptor is not a listening socket");
	}

	::sockaddr_un address{};
	::socklen_t length = sizeof address;

	if (::getsockname(offered.Value, reinterpret_cast<::sockaddr*>(&address), &length) != 0)
	{
		return FailFromErrno("reading the address of an offered listener");
	}

	// An unnamed socket reports only the family; an abstract one reports a leading NUL. Neither is a
	// path a client could be told to connect to, and neither is inside anybody's runtime directory.
	if (length <= offsetof(::sockaddr_un, sun_path) || address.sun_path[0] == '\0')
	{
		return Failure(EACCES, "the offered listener is not bound to a path");
	}

	const std::size_t bytes = static_cast<std::size_t>(length) - offsetof(::sockaddr_un, sun_path);
	const std::string_view whole{ address.sun_path, bytes };
	const std::size_t end = whole.find('\0');
	const std::string_view path = end == std::string_view::npos ? whole : whole.substr(0, end);

	if (!path.starts_with(directory) || path.size() <= directory.size() || path[directory.size()] != '/')
	{
		return Failure(EACCES, "the offered listener is bound outside the offering user's runtime directory", path);
	}

	// **A textual prefix is only a prefix, so the traversal is refused rather than resolved.** The
	// path is what the offering process passed to `bind`, so `/run/user/1000/../../tmp/x` starts with
	// the right directory and is not in it. Resolving would mean a `realpath` on a path a peer chose,
	// on the thread that must not block, against a directory that peer can rearrange underneath it.
	if (path.find("/../") != std::string_view::npos || path.ends_with("/.."))
	{
		return Failure(EACCES, "the offered listener's path climbs out of the runtime directory", path);
	}

	return {};
}

Result<std::unique_ptr<SessionControl>> SessionControl::Open(std::string_view path, std::string_view runtimeRoot)
{
	::sockaddr_un address{};
	address.sun_family = AF_UNIX;

	// Checked rather than truncated: a truncated path names a *different* socket, which is either
	// nothing or something else entirely.
	if (path.empty() || path.size() >= sizeof address.sun_path)
	{
		return Failure(ENAMETOOLONG, "the control socket path does not fit a sockaddr_un");
	}

	std::memcpy(address.sun_path, path.data(), path.size());

	if (const std::string_view directory = DirectoryOf(path); !directory.empty())
	{
		const std::string owned{ directory };

		if (::mkdir(owned.c_str(), 0755) != 0 && errno != EEXIST)
		{
			return FailFromErrno("creating the control socket's directory", directory);
		}
	}

	// A stale socket is gyro's own from before a restart, and unlinking it is the whole of the restart
	// path. Anything else at the path is refused: this runs as a boot service and removing a file
	// somebody else put there is not a thing to do unasked.
	const std::string owned{ path };
	struct ::stat existing{};

	if (::stat(owned.c_str(), &existing) == 0)
	{
		if (!S_ISSOCK(existing.st_mode))
		{
			return Failure(EEXIST, "the control socket path holds something that is not a socket", path);
		}

		if (::unlink(owned.c_str()) != 0)
		{
			return FailFromErrno("removing a stale control socket", path);
		}
	}

	std::unique_ptr<SessionControl> control{ new SessionControl };

	control->m_Listener = Fd{ ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0) };

	if (!control->m_Listener.IsValid())
	{
		return FailFromErrno("creating the control socket");
	}

	if (::bind(control->m_Listener.Get(), reinterpret_cast<const ::sockaddr*>(&address), sizeof address) != 0)
	{
		return FailFromErrno("binding the control socket", path);
	}

	control->m_Path = owned;
	control->m_RuntimeRoot = std::string{ runtimeRoot };

	// **World-writable, and `SO_PEERCRED` is what makes that safe.** Every user's agent has to reach
	// this, and gyro cannot know their uids in advance; the alternative is a group somebody has to be
	// added to, which is a deployment step that fails as "applications cannot find the display".
	if (::chmod(owned.c_str(), 0666) != 0)
	{
		return FailFromErrno("opening the control socket to every user", path);
	}

	if (::listen(control->m_Listener.Get(), Backlog) != 0)
	{
		return FailFromErrno("listening on the control socket", path);
	}

	control->m_Epoll = Fd{ ::epoll_create1(EPOLL_CLOEXEC) };

	if (!control->m_Epoll.IsValid())
	{
		return FailFromErrno("creating the control socket's poll set");
	}

	::epoll_event listener{};
	listener.events = EPOLLIN;
	listener.data.fd = control->m_Listener.Get();

	if (::epoll_ctl(control->m_Epoll.Get(), EPOLL_CTL_ADD, control->m_Listener.Get(), &listener) != 0)
	{
		return FailFromErrno("watching the control socket");
	}

	spdlog::info("session control listening on {}", path);

	return control;
}

SessionControl::~SessionControl()
{
	// The path is gyro's for as long as the socket is bound, and a file left behind is the next run's
	// stale socket. Unlinked here rather than left to `Open`'s cleanup so that the ordinary exit does
	// not depend on the next start.
	if (!m_Path.empty())
	{
		::unlink(m_Path.c_str());
	}
}

Result<void> SessionControl::Drain()
{
	while (true)
	{
		std::array<::epoll_event, ReadyBatch> ready{};

		const int count = ::epoll_wait(m_Epoll.Get(), ready.data(), ReadyBatch, 0);

		if (count < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}

			return FailFromErrno("polling the control socket");
		}

		if (count == 0)
		{
			return {};
		}

		for (int index = 0; index < count; ++index)
		{
			const int descriptor = ready[static_cast<std::size_t>(index)].data.fd;

			if (descriptor == m_Listener.Get())
			{
				if (const Result<void> accepted = AcceptAll(); !accepted)
				{
					return accepted;
				}

				continue;
			}

			// Looked up rather than held across the batch: an earlier event may have dropped this
			// connection, and the descriptor would then name whatever the kernel reused it for.
			const auto found = m_Connections.find(descriptor);

			if (found == m_Connections.end())
			{
				continue;
			}

			if (!Pump(found->second))
			{
				Forget(descriptor);
			}
		}
	}
}

Result<void> SessionControl::AcceptAll()
{
	while (true)
	{
		Fd accepted{ ::accept4(m_Listener.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC) };

		if (!accepted.IsValid())
		{
			if (errno == EINTR)
			{
				continue;
			}

			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				return {};
			}

			// **Not a failure of the compositor.** A peer that vanished between the poll and the accept
			// is `ECONNABORTED`, and a process out of descriptors is `EMFILE` — neither is a reason to
			// stop serving the sessions already established, so the loop ends and the wait comes back.
			spdlog::warn("accepting a handover connection failed: {}", Error::FromErrno("accept"));

			return {};
		}

		const Result<::ucred> peer = PeerOf(accepted.Borrow());

		if (!peer)
		{
			Refuse(accepted.Borrow(), EACCES, "the kernel would not name the peer");

			continue;
		}

		const std::uint32_t uid = peer->uid;

		if (m_Connections.size() >= MaxConnections)
		{
			Refuse(accepted.Borrow(), EMFILE, "too many handover connections on this machine");

			continue;
		}

		std::size_t mine = 0;

		for (const auto& [descriptor, connection] : m_Connections)
		{
			mine += connection.Uid == uid ? 1U : 0U;
		}

		if (mine >= MaxConnectionsPerUid)
		{
			Refuse(accepted.Borrow(), EMFILE, "too many handover connections for this user");

			continue;
		}

		::epoll_event watch{};
		watch.events = EPOLLIN;
		watch.data.fd = accepted.Get();

		if (::epoll_ctl(m_Epoll.Get(), EPOLL_CTL_ADD, accepted.Get(), &watch) != 0)
		{
			return FailFromErrno("watching a handover connection");
		}

		const int descriptor = accepted.Get();

		m_Connections.emplace(descriptor, Connection{ .Socket = std::move(accepted), .Uid = uid });
	}
}

bool SessionControl::Pump(Connection& connection)
{
	while (true)
	{
		std::array<std::byte, MaxMessageBytes> bytes{};

		Result<Received> received = Receive(connection.Socket.Borrow(), bytes);

		if (!received)
		{
			spdlog::warn("a handover connection failed: {}", received.error());

			return false;
		}

		if (received->State == ReceiveState::Empty)
		{
			return true;
		}

		if (received->State == ReceiveState::Ended)
		{
			return false;
		}

		if (received->Truncated)
		{
			Refuse(connection.Socket.Borrow(), EMSGSIZE, "the message did not fit");

			return false;
		}

		if (!Handle(
				connection,
				std::span<const std::byte>{ bytes }.first(received->Bytes),
				std::span<Fd>{ received->Attached }.first(received->AttachedCount)
			))
		{
			return false;
		}
	}
}

bool SessionControl::Handle(Connection& connection, std::span<const std::byte> message, std::span<Fd> attached)
{
	const RawFd socket = connection.Socket.Borrow();
	const std::optional<MessageHeader> header = ReadHeader(message);

	if (!header)
	{
		Refuse(socket, EPROTO, "the message is not one this gyro can read");

		return false;
	}

	// A `Welcome` arriving here is a peer that is confused or lying, and neither is a thing to act on.
	if (!FromAgent(header->Op))
	{
		Refuse(socket, EPROTO, "the message travels the other way");

		return false;
	}

	if (attached.size() != DescriptorsFor(header->Op))
	{
		Refuse(socket, EPROTO, "the message carries the wrong number of descriptors");

		return false;
	}

	if (header->Op == Opcode::Hello)
	{
		const std::optional<Hello> hello = Hello::Decode(message);

		if (!hello)
		{
			Refuse(socket, EPROTO, "the greeting is malformed");

			return false;
		}

		if (connection.Greeted)
		{
			Refuse(socket, EPROTO, "the connection has already been greeted");

			return false;
		}

		if (hello->Version < MinimumHandoverVersion)
		{
			Refuse(socket, EPROTO, "the agent speaks a handover version this gyro has dropped");

			return false;
		}

		connection.Greeted = true;

		// The lower of the two, decided here rather than at both ends: one party deciding is what keeps
		// them from disagreeing about what they agreed.
		const std::uint32_t agreed = hello->Version < HandoverVersion ? hello->Version : HandoverVersion;

		std::array<std::byte, MaxMessageBytes> bytes{};
		const std::size_t written = Welcome{ .Version = agreed }.Encode(bytes);

		if (const Result<void> sent = Send(socket, std::span<const std::byte>{ bytes }.first(written), RawFd{}); !sent)
		{
			spdlog::warn("answering a handover greeting failed: {}", sent.error());

			return false;
		}

		return true;
	}

	if (!connection.Greeted)
	{
		Refuse(socket, EPROTO, "an offer arrived before the greeting");

		return false;
	}

	if (connection.Session != SessionId::None)
	{
		Refuse(socket, EBUSY, "this connection has already offered a listener");

		return false;
	}

	if (m_Sessions.contains(connection.Uid))
	{
		Refuse(socket, EBUSY, "this user already has a session");

		return false;
	}

	if (const Result<void> acceptable =
	        InspectOffer(attached[0].Borrow(), RuntimeDirectoryFor(connection.Uid, m_RuntimeRoot));
	    !acceptable)
	{
		Refuse(socket, acceptable.error().Code(), acceptable.error().Sentence());

		return false;
	}

	const SessionId id{ m_NextSession };
	++m_NextSession;

	connection.Session = id;
	m_Sessions.emplace(connection.Uid, id);
	m_Offered.push_back(AcceptedOffer{ .Id = id, .Uid = connection.Uid, .Listener = std::move(attached[0]) });

	std::array<std::byte, MaxMessageBytes> bytes{};
	const std::size_t written = Accepted{ .Id = id }.Encode(bytes);

	if (const Result<void> sent = Send(socket, std::span<const std::byte>{ bytes }.first(written), RawFd{}); !sent)
	{
		spdlog::warn("answering a handover offer failed: {}", sent.error());

		return false;
	}

	spdlog::info("session {} established for uid {}", static_cast<std::uint32_t>(id), connection.Uid);

	return true;
}

std::optional<AcceptedOffer> SessionControl::TakeOffer() noexcept
{
	if (m_Offered.empty())
	{
		return std::nullopt;
	}

	AcceptedOffer offer = std::move(m_Offered.front());
	m_Offered.erase(m_Offered.begin());

	return offer;
}

void SessionControl::Forget(int descriptor)
{
	const auto found = m_Connections.find(descriptor);

	if (found == m_Connections.end())
	{
		return;
	}

	const std::uint32_t uid = found->second.Uid;
	const SessionId session = found->second.Session;

	// Removed before the close so that the descriptor is out of the set while it is still a descriptor.
	// Closing would do it, and doing it here is what keeps the two orderings from ever differing.
	::epoll_ctl(m_Epoll.Get(), EPOLL_CTL_DEL, descriptor, nullptr);

	m_Connections.erase(found);

	if (session == SessionId::None)
	{
		return;
	}

	m_Sessions.erase(uid);

	spdlog::info("session {} ended", static_cast<std::uint32_t>(session));

	Ended.Emit(session);
}
} // namespace Session
