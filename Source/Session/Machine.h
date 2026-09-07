#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Signal.h"
#include "Session/Handover.h"

// The machine peer's end of the control connection: who is on which screen.
//
// **It is Session/Agent.h's shape for the other half of the socket, and the asymmetry between them is
// the point.** An agent offers *its own* session and can say nothing about anybody else's; this says
// nothing about a session at all and decides which of them a person is looking at. Two roles on one
// socket rather than two sockets, because what separates them is the uid the kernel reports and gyro
// already reads it — a second socket would be a second path with the same check on it.
//
// **The party at the far end is the login agent** (Docs/Architecture.md#the-login-agent), which is root
// because PAM, `setuid` and creating a runtime directory all require it. gyro refuses the claim from
// any other uid: assignment is the verb decision 43 builds locking out of, so a uid that could reach it
// could put its own session onto a locked panel and read somebody's desktop without authenticating.
// root could already do that by other means, which is exactly why it is the only uid that may.
//
// **Nothing here waits**, which is Session/Agent.h's rule one role over: `Connect` makes one attempt and
// `Drain` reads what has arrived, so the caller owns the wait and how patient to be with a compositor
// that has not started is a policy rather than a property of the handshake.

namespace Session
{
// Where the claim has got to.
//
// **A state rather than a flag, for the reason `AgentState` is one**: the two intermediate values are
// what tell a person whose screen stayed on the greeter which end stopped answering. Stuck in
// `Greeting` is a gyro that took the connection and not the message; stuck in `Claiming` is one that
// would not grant the machine and did not say so.
enum class MachineState : std::uint8_t
{
	// No connection. The ordinary state before gyro has started, and the one every failure returns to.
	Apart,

	// Connected, `Hello` sent, waiting for `Welcome`.
	Greeting,

	// `Manage` sent, waiting for `Managing`.
	Claiming,

	// gyro grants the machine role to this connection. Assignments may be made.
	Running,
};

[[nodiscard]] constexpr std::string_view Name(MachineState state) noexcept
{
	switch (state)
	{
		case MachineState::Apart:
			return "apart";
		case MachineState::Greeting:
			return "greeting";
		case MachineState::Claiming:
			return "claiming";
		case MachineState::Running:
			return "running";
	}

	return "?";
}

class MachinePeer
{
public:
	MachinePeer() = default;

	~MachinePeer() = default;

	// Neither copied nor moved: the caller polls a descriptor this owns and connects to its signal.
	MachinePeer(const MachinePeer&) = delete;
	MachinePeer& operator=(const MachinePeer&) = delete;
	MachinePeer(MachinePeer&&) = delete;
	MachinePeer& operator=(MachinePeer&&) = delete;

	// One attempt at the control socket, then the greeting and the claim together.
	//
	// **Both are sent without waiting for the first to be answered**, which is a deliberate difference
	// from the agent's sequence and rests on the socket rather than on optimism: a sequenced-packet
	// socket delivers in order, so gyro reads `Hello` before `Manage` and answers them in the order it
	// read them. What that buys is that the claim is on the wire before any agent could have offered a
	// session, which is the race the claim exists to close — gyro must know somebody is entitled to
	// place a session before the first one arrives, or it places one itself and the screen jumps.
	//
	// The failures a caller retries on are the ordinary ones of a compositor that has not started:
	// `ENOENT` where the socket is not there yet, `ECONNREFUSED` where the path is left over from a gyro
	// that died.
	[[nodiscard]] Result<void> Connect(std::string_view controlPath);

	// What the caller waits on, or an invalid descriptor while apart.
	[[nodiscard]] RawFd Descriptor() const noexcept { return m_Socket.Borrow(); }

	// Read what gyro has said and act on it, until nothing is left.
	[[nodiscard]] Result<void> Drain();

	[[nodiscard]] MachineState State() const noexcept { return m_State; }

	// Ask for a user's session on some outputs, or on every output where the name is empty.
	//
	// **`Request` rather than `Assign`, which is what the message is called.** The two would be the same
	// word for the same act, and a member function may not share a name with a type in the namespace it
	// is written in — so the verb is the one gyro's side already answers to, its signal being
	// `Requested`.
	//
	// **It may be asked for a user who has no session yet, and that is the ordinary case rather than an
	// error.** A login agent forks an agent for the person it authenticated and says where they go; the
	// two race by construction, and gyro holds the request until it can be satisfied. `Landed` is what
	// says it was.
	//
	// Refused while not `Running`, because a request gyro would answer `EACCES` to is one this end can
	// decline to send.
	[[nodiscard]] Result<void> Request(const MachineRequest& request);

	// A request gyro has satisfied: that user's session is on those screens now.
	//
	// **A signal rather than a state, because there is no single outstanding request.** A machine with
	// two panels has two, they land in whatever order their sessions become showable, and a peer that
	// wanted to know which is asking about the echo rather than about this object.
	Signal<MachineRequest> Landed;

	// The errno gyro refused with, or zero. **Non-zero is terminal**, for `SessionAgent::Rejection`'s
	// reason: a refusal is the compositor having judged the claim, so reconnecting and claiming again
	// asks the same question at whatever rate the backoff allows.
	[[nodiscard]] int Rejection() const noexcept { return m_Rejection; }

	[[nodiscard]] std::string_view Rejected() const noexcept { return m_Rejected.Text(); }

private:
	// Drop the connection and return to `Apart`.
	void Part();

	// Act on one datagram. Answers false where the connection is finished.
	[[nodiscard]] bool Handle(std::span<const std::byte> message);

	[[nodiscard]] Result<void> Say(std::span<const std::byte> message);

	Fd m_Socket;

	MachineState m_State = MachineState::Apart;

	// The version gyro said is in force. Nothing branches on it yet, and it is kept for the reason the
	// agent keeps its copy: the first message this build sends that a version 1 gyro would not
	// understand is the one that has to ask.
	std::uint32_t m_Version = 0;

	int m_Rejection = 0;

	Reason m_Rejected;
};
} // namespace Session
