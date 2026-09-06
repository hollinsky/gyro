#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Signal.h"
#include "Session/Handover.h"

// The socket a session agent offers its Wayland listener on, and what gyro will accept over it.
//
// **Socket creation is inverted, and this is the far side of the inversion.**
// Docs/Architecture.md#listener-handover: gyro cannot `chown` a socket into a user's runtime
// directory without `CAP_CHOWN`, which
// [decision
// 22](../../Docs/Decisions.md#22-gyro-runs-as-a-dedicated-unprivileged-uid-with-cap_sys_nice-and-nothing-else) refuses
// to hold, so the user's own agent creates the listener and passes the descriptor in. The capability saving is the
// secondary reason. What earns the shape is that **the handover is the session-start event** — gyro learns a session
// exists because a listener arrived rather than because it subscribed to a signal — and the connection stays open for
// the session's lifetime, so its EOF is the session-end event by construction rather than by hope.
//
// **This is an unauthenticated entry point into the one process whose death takes every session on
// the machine**, and the whole of what bounds it is in this file: `SO_PEERCRED` on the connection,
// the four questions `InspectOffer` asks of the descriptor, one session per uid, and the two caps
// below. None of it is defence in depth. Because the *user* creates the socket, a user can create a
// permissive one, so a check that is skipped here is not a redundant check — it is a way for one
// person's session to be handed somebody else's windows.
//
// **What is not here yet, and is the load-bearing one: the per-connection check on the Wayland
// socket.** gyro rejects any *client* connection whose peer uid does not match the session's user,
// and that check lives where the listener is adopted rather than where it is offered. Adoption is not
// built (`Server::Adopt` and the composition root's wiring), so this module hands out a descriptor
// nobody has bound to a session yet — which is why nothing calls `Open` outside its own test.
//
// **One epoll descriptor, so the dispatch thread's wait does not become a registry.**
// Compositor/Wait.h names "a control socket per connected agent" as exactly the thing that would turn
// its array of watched files into something that has to grow, and reopen
// [decision
// 126](../../Docs/Decisions.md#126-the-dispatch-threads-wait-is-a-ppoll-on-one-descriptor-and-the-root-converts-the-wake).
// It does not have to: a listener and one connection per session are files this module can multiplex
// itself, which is what `Descriptor` hands over — one more `pollfd` for the root, whatever is behind
// it. Seam/EventSource.h's *one file, N producers* shape, arrived at from the other direction.

namespace Session
{
// Where the per-user runtime directories live, which is what `pam_systemd` populates.
inline constexpr std::string_view DefaultRuntimeRoot = "/run/user";

// The directory a uid's sockets must be bound inside.
//
// **Built from the uid the kernel reported and never from anything the peer said.** A path in a
// message would be a claim; `SO_PEERCRED` is the kernel's answer to the same question, and the only
// one worth checking a descriptor against.
//
// The root is a parameter because a test cannot bind sockets under `/run/user` on a machine where
// nobody has logged in — and because it is the one part of this that is a deployment fact rather than
// a rule.
[[nodiscard]] std::string RuntimeDirectoryFor(std::uint32_t uid, std::string_view root = DefaultRuntimeRoot);

// Whether an offered descriptor is a Wayland listener the offering user could legitimately have made:
// an `AF_UNIX` stream socket, listening, bound to a path inside `directory`.
//
// **The path is evidence rather than something gyro uses.** Nothing here opens it, and clients find
// the socket through `WAYLAND_DISPLAY` rather than through anything gyro says. What the check buys is
// that a session's listener is somewhere only that user could have put it — a descriptor to a socket
// in a shared directory is one other people can connect to, and the connection-time uid check would
// then be the *only* thing standing between two users' windows.
//
// Answers the errno the refusal should carry: `EBADF` where the descriptor is not the kind of socket
// claimed, `EACCES` where it is bound somewhere it should not be.
[[nodiscard]] Result<void> InspectOffer(RawFd offered, std::string_view directory) noexcept;

// A session gyro has accepted, on its way to the party that will adopt it.
//
// **It is taken rather than broadcast, and Core/Fd.h says why**: a signal carries a fact and never a
// resource, because with N observers at most one could take it and nothing in the signature says
// which. A listener is the resource — exactly one party may serve clients on it — so it leaves
// through a verb that can only be called once. `Ended` beside it *is* a fact, and is a signal.
//
// **A session rather than a listener, which is Session/Handover.h's `Offer` arriving intact.** The
// two sockets are established together or not at all, so the party that adopts them cannot be handed
// a session that is missing half of itself.
struct AcceptedOffer
{
	SessionId Id = SessionId::None;

	std::uint32_t Uid = 0;

	// Where this session's applications connect. Always present.
	Fd Listener;

	// Where this session's shell connects, granted `Trust::System` (Protocol/Tier.h), or invalid for a
	// session whose agent offered none — which is an agent that starts no shell, and is an ordinary
	// state of a machine rather than a failure.
	Fd Shell;
};

// SPEC: how many agents may be mid-handshake at once, across every user on the machine. Decision 27's
// order of magnitude above anything real — a machine with more than this many *sessions* being
// established simultaneously is one gyro would rather refuse than let fill its file table.
inline constexpr std::size_t MaxConnections = 32;

// SPEC: and how many of those may belong to one user. The cap that matters, because the global one is
// reachable by a single uid without it: an agent that connects and never speaks costs a descriptor,
// and a user who can make one can make thirty-two.
inline constexpr std::size_t MaxConnectionsPerUid = 4;

// The control socket, the agents connected to it, and the sessions they have established.
class SessionControl
{
public:
	~SessionControl();

	// Neither copied nor moved: the composition root holds its address and the dispatch thread polls a
	// descriptor it owns.
	SessionControl(const SessionControl&) = delete;
	SessionControl& operator=(const SessionControl&) = delete;
	SessionControl(SessionControl&&) = delete;
	SessionControl& operator=(SessionControl&&) = delete;

	// Bind the control socket at `path` and start listening on it.
	//
	// The parent directory is created where it is missing, because the alternative is a boot service
	// that fails on a machine whose `/run/gyro` has not been made yet — and a stale socket at the path
	// is unlinked, because gyro restarting is the ordinary reason one is there. A path holding
	// something that is *not* a socket is refused rather than removed.
	[[nodiscard]] static Result<std::unique_ptr<SessionControl>>
	Open(std::string_view path, std::string_view runtimeRoot = DefaultRuntimeRoot);

	// What the dispatch thread waits on. One descriptor, whatever is behind it.
	[[nodiscard]] RawFd Descriptor() const noexcept { return m_Epoll.Borrow(); }

	// Accept what has arrived and act on it, until nothing is left.
	//
	// Drained to empty rather than once, for Seam/EventSource.h's reason: a level-triggered wait wakes
	// again immediately on whatever was left behind, which is a spin on the dispatch thread rather than
	// a lost message.
	[[nodiscard]] Result<void> Drain();

	// A listener gyro has accepted and not yet handed over, if there is one. Called by whoever adopts
	// it, after `Drain`.
	[[nodiscard]] std::optional<AcceptedOffer> TakeOffer() noexcept;

	// A session whose agent went away. The connection's EOF is the session ending, which is the whole
	// reason the agent holds it open rather than offering and exiting.
	Signal<SessionId> Ended;

	// How many agents are connected. For a test; nothing in the loop asks.
	[[nodiscard]] std::size_t Connections() const noexcept { return m_Connections.size(); }

private:
	SessionControl() = default;

	// One connected agent. **The session is on the connection rather than in a table beside it**,
	// because the two have identical lifetimes by design — a session exists from the offer that
	// established it until the connection carrying it closes — and a second structure would be a
	// second place for that to stop being true.
	struct Connection
	{
		Fd Socket;

		std::uint32_t Uid = 0;

		// Whether `Hello` has been answered. Nothing else is accepted before it, so that a version
		// disagreement is settled before a descriptor is spent on it.
		bool Greeted = false;

		// Non-null once this connection's offer was accepted.
		SessionId Session = SessionId::None;
	};

	[[nodiscard]] Result<void> AcceptAll();

	// Read and act on everything one connection has to say. Answers false where the connection is
	// finished and should be dropped.
	[[nodiscard]] bool Pump(Connection& connection);

	// Act on one datagram. Answers false where the connection is finished — which is every refusal, a
	// refusal being the last thing gyro says on a connection.
	[[nodiscard]] bool Handle(Connection& connection, std::span<const std::byte> message, std::span<Fd> attached);

	void Forget(int descriptor);

	Fd m_Listener;
	Fd m_Epoll;

	std::string m_Path;

	std::string m_RuntimeRoot;

	// Keyed by the connection's own descriptor, which is what epoll reports.
	std::unordered_map<int, Connection> m_Connections;

	// Which uid holds which session, so that a second agent for a user who already has one is refused
	// rather than silently taking over.
	std::unordered_map<std::uint32_t, SessionId> m_Sessions;

	std::vector<AcceptedOffer> m_Offered;

	// Counts up and never recycles. A session id outlives the session in a log line and in whatever
	// held one, and reuse would make two of those the same session to anything comparing them.
	std::uint32_t m_NextSession = 1;
};
} // namespace Session
