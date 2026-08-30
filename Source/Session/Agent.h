#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Session/Handover.h"

// The agent's end of the handover: one connection to gyro, and the listener offered over it.
//
// **It is the mirror of Session/Control.h and deliberately not its inverse.** gyro's side is a
// listener with N agents behind it and every message is a thing it may refuse; this side is one
// socket, four states, and a peer whose answers are taken at face value — the process on the other
// end is the one that would be compromised, not the one doing the compromising. What the two share is
// Session/Transport.h, so the `sendmsg` that carries a descriptor is written once.
//
// **The listener is borrowed rather than owned, and the reason is the restart.** gyro restarting
// loses every listener it held; the agents re-offer, and what has to arrive on the second connection
// is the *same* socket — same path, same queue, same clients waiting in it. So the socket outlives
// every connection this class makes, which means it cannot be this class's. Session/Listener.h holds
// it.
//
// **The state is the whole of what the caller observes, and there is deliberately no signal beside
// it.** Session/Control.h emits one because the party that learns a session ended is not the party
// that polls for it; here there is one consumer and it is already asking, so a second way to learn
// the same fact would only be a second thing to keep in step.
//
// **Nothing here waits.** `Connect` performs one attempt and `Drain` reads what has arrived, both for
// Seam/EventSource.h's reason one process over: the caller owns the wait, and a retry schedule is a
// policy about how patient to be with a compositor that has not started yet rather than a property of
// the handshake.

namespace Session
{
// Where the handshake has got to. **A state rather than a pair of booleans**, because the two
// intermediate ones are what tell a person whose session did not start which end stopped answering:
// stuck in `Greeting` is a gyro that took the connection and not the message, and stuck in `Offering`
// is one that would not have the listener and did not say so.
enum class AgentState : std::uint8_t
{
	// No connection. The ordinary state before gyro has started, and the one every failure returns to.
	Apart,

	// Connected, `Hello` sent, waiting for `Welcome`.
	Greeting,

	// `Offer` sent with the listener attached, waiting for `Accepted`.
	Offering,

	// gyro holds the listener and the session exists. Clients may be started.
	Established,
};

[[nodiscard]] constexpr std::string_view Name(AgentState state) noexcept
{
	switch (state)
	{
		case AgentState::Apart:
			return "apart";
		case AgentState::Greeting:
			return "greeting";
		case AgentState::Offering:
			return "offering";
		case AgentState::Established:
			return "established";
	}

	return "?";
}

class SessionAgent
{
public:
	// `listener` is the socket to offer and is borrowed for the whole of this object's life.
	explicit SessionAgent(RawFd listener) noexcept : m_Listener{ listener } {}

	~SessionAgent() = default;

	// Neither copied nor moved: the caller polls a descriptor this owns and connects to its signals.
	SessionAgent(const SessionAgent&) = delete;
	SessionAgent& operator=(const SessionAgent&) = delete;
	SessionAgent(SessionAgent&&) = delete;
	SessionAgent& operator=(SessionAgent&&) = delete;

	// One attempt at the control socket, and the greeting if it succeeds. Refused where a connection
	// is already open.
	//
	// The failures a caller retries on are the ordinary ones of a compositor that has not started:
	// `ENOENT` where the socket is not there yet, `ECONNREFUSED` where the path is left over from a
	// gyro that died. Both come back exactly as `connect` reported them, because the difference is
	// worth a line in a log that somebody is reading precisely because nothing appeared on screen.
	[[nodiscard]] Result<void> Connect(std::string_view controlPath);

	// What the caller waits on, or an invalid descriptor while apart.
	[[nodiscard]] RawFd Descriptor() const noexcept { return m_Socket.Borrow(); }

	// Read what gyro has said and act on it, until nothing is left.
	//
	// A failure here is the connection's rather than the agent's: it is dropped, `Lost` is emitted if
	// the session had been established, and the caller reconnects. The one exception is a refusal,
	// which sets `Rejection` — gyro saying it will not do this is not something to retry into.
	[[nodiscard]] Result<void> Drain();

	[[nodiscard]] AgentState State() const noexcept { return m_State; }

	[[nodiscard]] SessionId Session() const noexcept { return m_Session; }

	// The errno gyro refused with, or zero. **Non-zero is terminal**, and that is a policy stated here
	// rather than in the caller: a `Refused` is the compositor having judged the offer, so an agent
	// that reconnected and offered the same listener again would be asking the same question at
	// whatever rate its backoff allows. The sentence beside it is in `Rejected`.
	[[nodiscard]] int Rejection() const noexcept { return m_Rejection; }

	[[nodiscard]] std::string_view Rejected() const noexcept { return m_Rejected.Text(); }

private:
	// Drop the connection and return to `Apart`.
	void Part();

	// Act on one datagram. Answers false where the connection is finished.
	[[nodiscard]] bool Handle(std::span<const std::byte> message);

	[[nodiscard]] Result<void> Say(std::span<const std::byte> message, RawFd attached);

	RawFd m_Listener;

	Fd m_Socket;

	AgentState m_State = AgentState::Apart;

	SessionId m_Session = SessionId::None;

	// The version gyro said is in force. Nothing branches on it yet — there is one version — and it is
	// kept because the first message this build sends that a version 1 gyro would not understand is
	// the one that has to ask.
	std::uint32_t m_Version = 0;

	int m_Rejection = 0;

	Reason m_Rejected;
};
} // namespace Session
