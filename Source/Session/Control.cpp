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
#include <optional>
#include <string>
#include <utility>

#include "Core/Trace.h"
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

// **The session row carries a uid and a session id and nothing else about the person.** Never a
// username, never a runtime directory, never the path a listener is bound to. A `.pftrace` is a file
// somebody mails to a person who was not at the machine, and every other way of saying whose session
// this is says who the user is: a name is a person, and a runtime directory or a socket path is a name
// with a prefix on it. A uid is a number that means nothing off the machine that allocated it, and it
// is enough to tell three simultaneous sessions apart, which is the whole job.
//
// **Before a session exists the tag is the uid and afterwards it is the session id**, because a mark is
// tagged with the identity it is about and there is no session id until an offer has been taken. The
// names carry which of the two it is: a mark beginning `agent` or `machine` is tagged with a uid and
// one beginning `session` with a session id. An attribute would be the tidier home for the second number and is not
// available — Core/Trace.h binds one to the slice open on the row, and this row is marks all the way
// down.

// The uid on a mark for a peer the kernel would not name, which is Core/Trace.h's *nothing to say* and
// so prints no uid at all. The cost is that a refusal of an agent genuinely running as root prints the
// same way, which is a machine with a larger problem than its trace.
constexpr std::uint32_t UnknownUid = 0;

// Send a refusal and say so. The last thing gyro says on a connection: the caller drops it.
void Refuse(RawFd socket, std::uint32_t uid, int code, const char* reason)
{
	std::array<std::byte, MaxMessageBytes> bytes{};
	const Refused message{ .Code = static_cast<std::uint32_t>(code), .Text = Reason{ reason } };
	const std::size_t written = message.Encode(bytes);

	// Nothing to do about a refusal that could not be sent, and nothing worth logging either: the
	// connection is being closed, which is the part that matters.
	const Result<void> sent = Send(socket, std::span<const std::byte>{ bytes }.first(written));

	// **The sentence is the mark's name**, which it can be because every refusal in this file names
	// itself with a literal — so the timeline says *why* gyro would not take a session rather than
	// carrying a number a reader has to go and look up in this source file.
	TraceMark(reason, TraceSession(), TraceTag(uid));

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
			// A machine out of descriptors, or a peer that went away between the poll and the accept.
			// Worth a mark because it is a session that never started and there is nothing else on any
			// row to say one was trying to.
			TraceMark("agent accept failed", TraceSession());

			spdlog::warn("accepting a handover connection failed: {}", Error::FromErrno("accept"));

			return {};
		}

		const Result<::ucred> peer = PeerOf(accepted.Borrow());

		if (!peer)
		{
			Refuse(accepted.Borrow(), UnknownUid, EACCES, "the kernel would not name the peer");

			continue;
		}

		const std::uint32_t uid = peer->uid;

		// Before the caps rather than after them, so that a connection gyro immediately refuses still
		// shows as having arrived: *nothing at all* and *refused* are the two answers a person is trying
		// to tell apart when a session did not start.
		TraceMark("agent connected", TraceSession(), TraceTag(uid));

		if (m_Connections.size() >= MaxConnections)
		{
			Refuse(accepted.Borrow(), uid, EMFILE, "too many handover connections on this machine");

			continue;
		}

		std::size_t mine = 0;

		for (const auto& [descriptor, connection] : m_Connections)
		{
			mine += connection.Uid == uid ? 1U : 0U;
		}

		if (mine >= MaxConnectionsPerUid)
		{
			Refuse(accepted.Borrow(), uid, EMFILE, "too many handover connections for this user");

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
			Refuse(connection.Socket.Borrow(), connection.Uid, EMSGSIZE, "the message did not fit");

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
		Refuse(socket, connection.Uid, EPROTO, "the message is not one this gyro can read");

		return false;
	}

	// A `Welcome` arriving here is a peer that is confused or lying, and neither is a thing to act on.
	if (!FromPeer(header->Op))
	{
		Refuse(socket, connection.Uid, EPROTO, "the message travels the other way");

		return false;
	}

	// **Only an offer may carry a descriptor at all, and how many is checked below against what the
	// offer said.** The two halves are separate because the second cannot be asked here: the roles are
	// in the payload, and this runs before anything has decoded one.
	if (!CarriesListeners(header->Op) && !attached.empty())
	{
		Refuse(socket, connection.Uid, EPROTO, "the message carries descriptors its kind never does");

		return false;
	}

	if (header->Op == Opcode::Hello)
	{
		const std::optional<Hello> hello = Hello::Decode(message);

		if (!hello)
		{
			Refuse(socket, connection.Uid, EPROTO, "the greeting is malformed");

			return false;
		}

		if (connection.Greeted)
		{
			Refuse(socket, connection.Uid, EPROTO, "the connection has already been greeted");

			return false;
		}

		if (hello->Version < MinimumHandoverVersion)
		{
			Refuse(socket, connection.Uid, EPROTO, "the agent speaks a handover version this gyro has dropped");

			return false;
		}

		connection.Greeted = true;

		// The lower of the two, decided here rather than at both ends: one party deciding is what keeps
		// them from disagreeing about what they agreed.
		const std::uint32_t agreed = hello->Version < HandoverVersion ? hello->Version : HandoverVersion;

		std::array<std::byte, MaxMessageBytes> bytes{};
		const std::size_t written = Welcome{ .Version = agreed }.Encode(bytes);

		if (const Result<void> sent = Send(socket, std::span<const std::byte>{ bytes }.first(written)); !sent)
		{
			// The agent is stuck in `Greeting` from here on and will not say so, because from its side
			// nothing happened. This mark is the only record that gyro is the end that stopped answering.
			TraceMark("agent greeting unanswered", TraceSession(), TraceTag(connection.Uid));

			spdlog::warn("answering a handover greeting failed: {}", sent.error());

			return false;
		}

		TraceMark("agent greeted", TraceSession(), TraceTag(connection.Uid));

		return true;
	}

	if (!connection.Greeted)
	{
		Refuse(socket, connection.Uid, EPROTO, "a message arrived before the greeting");

		return false;
	}

	if (header->Op == Opcode::Manage)
	{
		if (!Manage::Decode(message))
		{
			Refuse(socket, connection.Uid, EPROTO, "the machine claim is malformed");

			return false;
		}

		// **root and nothing else, and the argument is what the capability would otherwise buy.**
		// Assignment moves a screen from one person's session to another's, which is the verb decision
		// 43 builds locking out of — so a uid that could reach it could put its own session on a locked
		// panel and be looking at somebody's desktop without authenticating. root is the one uid that
		// could already do that by other means, so granting it this adds nothing it did not have; any
		// other uid granted it would be a bypass with a message behind it. Docs/Architecture.md#the-login-agent
		// is why the party at the far end is root anyway: PAM, `setuid` and creating a runtime directory
		// all require it.
		if (connection.Uid != 0)
		{
			Refuse(socket, connection.Uid, EACCES, "only root may run the machine");

			return false;
		}

		if (connection.Session != SessionId::None)
		{
			Refuse(socket, connection.Uid, EBUSY, "this connection has offered a session");

			return false;
		}

		if (connection.Machine)
		{
			Refuse(socket, connection.Uid, EBUSY, "this connection is already the machine");

			return false;
		}

		if (m_Machine >= 0)
		{
			Refuse(socket, connection.Uid, EBUSY, "another connection is already the machine");

			return false;
		}

		std::array<std::byte, MaxMessageBytes> bytes{};
		const std::size_t written = Managing{}.Encode(bytes);

		if (const Result<void> sent = Send(socket, std::span<const std::byte>{ bytes }.first(written)); !sent)
		{
			// The mirror of the greeting's: the peer is waiting to be told it may assign anything, and
			// from its side nothing happened. Not recorded as the machine, because it is about to be
			// dropped and a claim gyro cannot answer is one it has not granted.
			TraceMark("machine claim unanswered", TraceSession(), TraceTag(connection.Uid));

			spdlog::warn("answering a machine claim failed: {}", sent.error());

			return false;
		}

		connection.Machine = true;
		m_Machine = socket.Value;

		TraceMark("machine claimed", TraceSession(), TraceTag(connection.Uid));

		spdlog::info("the machine is being run by uid {}", connection.Uid);

		return true;
	}

	if (header->Op == Opcode::Assign)
	{
		// **The claim is what makes this reachable, and the check is here rather than on the uid.** A
		// root connection that has not claimed the machine is one that might be about to offer a
		// session, and a request arriving on it is a peer that has skipped a step rather than one that
		// is entitled — saying so is what keeps the two roles from blurring for the one uid that could
		// hold either.
		if (!connection.Machine)
		{
			Refuse(socket, connection.Uid, EACCES, "this connection has not claimed the machine");

			return false;
		}

		const std::optional<Assign> assign = Assign::Decode(message);

		if (!assign)
		{
			Refuse(socket, connection.Uid, EPROTO, "the assignment is malformed");

			return false;
		}

		// **Emitted rather than acted on, and nothing is answered here.** Whether a user has a session
		// and whether gyro has a screen by that name are both questions for the composition root, and
		// the answer may be *not yet* for as long as the machine runs — so `Assigned` is sent by
		// whoever satisfies it, through `Satisfied` below.
		TraceMark("machine assigned", TraceSession(), TraceTag(assign->Uid));

		spdlog::info(
			"the machine asks for uid {} on {}",
			assign->Uid,
			assign->Connector.IsEveryOutput() ? "every output" : assign->Connector.Text()
		);

		Requested.Emit(MachineRequest{ .Uid = assign->Uid, .Connector = assign->Connector });

		return true;
	}

	if (connection.Machine)
	{
		Refuse(socket, connection.Uid, EBUSY, "this connection runs the machine");

		return false;
	}

	if (connection.Session != SessionId::None)
	{
		Refuse(socket, connection.Uid, EBUSY, "this connection has already offered a listener");

		return false;
	}

	if (m_Sessions.contains(connection.Uid))
	{
		Refuse(socket, connection.Uid, EBUSY, "this user already has a session");

		return false;
	}

	const std::optional<Offer> offer = Offer::Decode(message);

	if (!offer)
	{
		Refuse(socket, connection.Uid, EPROTO, "the offer is malformed");

		return false;
	}

	// **The bitmap before the descriptors, because the bitmap is what says how many there are.** A set
	// with a bit this build has no name for is refused rather than masked: the bits *are* the count, so
	// ignoring one would leave a socket attached that nobody has a role for.
	if (!RolesAreWellFormed(offer->Roles))
	{
		Refuse(socket, connection.Uid, EPROTO, "the offer names listener roles this gyro does not have");

		return false;
	}

	if (attached.size() != ListenersIn(offer->Roles))
	{
		Refuse(socket, connection.Uid, EPROTO, "the offer carries a different number of listeners than it names");

		return false;
	}

	const std::string directory = RuntimeDirectoryFor(connection.Uid, m_RuntimeRoot);

	// **Every listener is judged before any of them is taken.** A session is offered whole, so it is
	// accepted whole or not at all — half a session established with a shell socket refused is exactly
	// the state offering it in one message exists to make impossible.
	for (const Fd& listener : attached)
	{
		if (const Result<void> acceptable = InspectOffer(listener.Borrow(), directory); !acceptable)
		{
			Refuse(socket, connection.Uid, acceptable.error().Code(), acceptable.error().Sentence());

			return false;
		}
	}

	const SessionId id{ m_NextSession };
	++m_NextSession;

	connection.Session = id;
	m_Sessions.emplace(connection.Uid, id);

	AcceptedOffer accepted;
	accepted.Id = id;
	accepted.Uid = connection.Uid;
	accepted.Listener = std::move(attached[IndexOf(offer->Roles, ListenerRole::Applications)]);

	if (Offers(offer->Roles, ListenerRole::Shell))
	{
		accepted.Shell = std::move(attached[IndexOf(offer->Roles, ListenerRole::Shell)]);
	}

	m_Offered.push_back(std::move(accepted));

	std::array<std::byte, MaxMessageBytes> bytes{};
	const std::size_t written = Accepted{ .Id = id }.Encode(bytes);

	if (const Result<void> sent = Send(socket, std::span<const std::byte>{ bytes }.first(written)); !sent)
	{
		// The mirror of the greeting above, one state along: gyro holds the listeners and the agent is
		// stuck in `Offering` waiting to be told so.
		TraceMark("session acceptance unanswered", TraceSession(), TraceTag(static_cast<std::uint64_t>(id)));

		spdlog::warn("answering a handover offer failed: {}", sent.error());

		return false;
	}

	// Two literals rather than one and a flag, because whether a session came with a shell is the first
	// thing to ask of a session that established and then showed a person nothing.
	TraceMark(
		Offers(offer->Roles, ListenerRole::Shell) ? "session established with a shell" : "session established",
		TraceSession(),
		TraceTag(static_cast<std::uint64_t>(id))
	);

	spdlog::info(
		"session {} established for uid {}{}",
		static_cast<std::uint32_t>(id),
		connection.Uid,
		Offers(offer->Roles, ListenerRole::Shell) ? " with a shell listener beside it" : " with no shell listener"
	);

	return true;
}

void SessionControl::Satisfied(const MachineRequest& request) noexcept
{
	if (m_Machine < 0)
	{
		return;
	}

	const auto found = m_Connections.find(m_Machine);

	if (found == m_Connections.end())
	{
		return;
	}

	std::array<std::byte, MaxMessageBytes> bytes{};
	const std::size_t written = Assigned{ .Uid = request.Uid, .Connector = request.Connector }.Encode(bytes);

	// **A send that fails is not a request that failed.** The screen has already moved, which is the
	// fact the peer was waiting for; what is lost is its notification, and the connection is about to
	// be dropped by the next `Pump` in any case. Saying so here rather than tearing anything down keeps
	// the world and the wire from disagreeing about what happened.
	if (const Result<void> sent =
	        Send(found->second.Socket.Borrow(), std::span<const std::byte>{ bytes }.first(written));
	    !sent)
	{
		TraceMark("machine assignment unanswered", TraceSession(), TraceTag(request.Uid));

		spdlog::warn("telling the machine an assignment landed failed: {}", sent.error());
	}
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
	const bool machine = found->second.Machine;

	// Removed before the close so that the descriptor is out of the set while it is still a descriptor.
	// Closing would do it, and doing it here is what keeps the two orderings from ever differing.
	::epoll_ctl(m_Epoll.Get(), EPOLL_CTL_DEL, descriptor, nullptr);

	m_Connections.erase(found);

	// **The machine going away puts gyro back in charge of placement rather than freezing the screen
	// where it was.** A login agent that crashed is restarted by the service manager and re-states what
	// it wanted; between the two there is nobody entitled to move a screen, and gyro's own stand-in is
	// what keeps a session arriving in that window from landing nowhere at all.
	//
	// Ahead of the two below and returning, because a machine connection never offered a session:
	// *parted without establishing anything* would be a second and misleading account of the same
	// event.
	if (machine)
	{
		m_Machine = -1;

		TraceMark("machine parted", TraceSession(), TraceTag(uid));

		spdlog::info("the machine peer went away");

		return;
	}

	if (session == SessionId::None)
	{
		// An agent that connected and went away without establishing anything. Nothing else records it —
		// there is no session to end and no refusal to log — and at boot it is precisely the shape of an
		// agent that died mid-handshake.
		TraceMark("agent parted", TraceSession(), TraceTag(uid));

		return;
	}

	m_Sessions.erase(uid);

	// Before the signal, so that what the observers go on to do is on the timeline after the fact that
	// caused it rather than interleaved with it.
	TraceMark("session ended", TraceSession(), TraceTag(static_cast<std::uint64_t>(session)));

	spdlog::info("session {} ended", static_cast<std::uint32_t>(session));

	Ended.Emit(session);
}
} // namespace Session
