#include "Session/Agent.h"

#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <optional>
#include <utility>

#include "Session/Transport.h"

namespace Session
{
Result<void> SessionAgent::Connect(std::string_view controlPath)
{
	if (m_Socket.IsValid())
	{
		return Failure(EISCONN, "the agent is already connected");
	}

	if (controlPath.empty() || controlPath.size() >= sizeof(::sockaddr_un::sun_path))
	{
		return Failure(ENAMETOOLONG, "the control socket's path is not one an address can hold");
	}

	Fd socket{ ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0) };

	if (!socket.IsValid())
	{
		return FailFromErrno("creating a control socket");
	}

	::sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, controlPath.data(), controlPath.size());

	if (::connect(socket.Get(), reinterpret_cast<const ::sockaddr*>(&address), sizeof address) != 0)
	{
		return FailFromErrno("connecting to gyro's control socket", Subject{ controlPath });
	}

	m_Socket = std::move(socket);
	m_State = AgentState::Greeting;

	std::array<std::byte, MaxMessageBytes> bytes{};
	const std::size_t written = Hello{}.Encode(bytes);

	if (const Result<void> said = Say(std::span<const std::byte>{ bytes }.first(written)); !said)
	{
		Part();

		return said;
	}

	return {};
}

Result<void> SessionAgent::Drain()
{
	while (m_Socket.IsValid())
	{
		std::array<std::byte, MaxMessageBytes> bytes{};
		Result<Received> received = Receive(m_Socket.Borrow(), bytes);

		if (!received)
		{
			const Error failure = received.error();

			Part();

			return std::unexpected{ failure };
		}

		if (received->State == ReceiveState::Empty)
		{
			return {};
		}

		if (received->State == ReceiveState::Ended)
		{
			Part();

			return {};
		}

		// **Descriptors are a refusal on this side, whatever the message.** `CarriesListeners` is only
		// ever true of a message travelling the other way, so anything that arrives with one attached is
		// a peer that is not gyro or a gyro speaking something this build does not — and the `Received`
		// closes them either way.
		if (received->AttachedCount != 0 || received->Truncated)
		{
			spdlog::warn("gyro sent a handover message this agent cannot read");

			Part();

			return {};
		}

		if (!Handle(std::span<const std::byte>{ bytes }.first(received->Bytes)))
		{
			Part();

			return {};
		}
	}

	return {};
}

bool SessionAgent::Handle(std::span<const std::byte> message)
{
	const std::optional<MessageHeader> header = ReadHeader(message);

	if (!header)
	{
		spdlog::warn("gyro sent a message that is not one this agent can read");

		return false;
	}

	// An agent receiving a `Hello` or an `Offer` is a peer that is confused or lying, and the control
	// socket is world-writable — so this is the same check gyro makes, made for the same reason.
	if (FromPeer(header->Op))
	{
		spdlog::warn("a handover message travelling the wrong way arrived on the control connection");

		return false;
	}

	if (header->Op == Opcode::Refused)
	{
		const std::optional<Refused> refused = Refused::Decode(message);

		// A refusal that cannot be decoded is still a refusal, and treating it as anything else would
		// have the agent retry into a compositor that has just said no.
		m_Rejection = refused ? static_cast<int>(refused->Code) : EPROTO;
		m_Rejected = refused ? refused->Text : Reason{ "gyro refused in a way this agent cannot read" };

		spdlog::error("gyro refused the handover: {}", m_Rejected.Text());

		return false;
	}

	if (header->Op == Opcode::Welcome)
	{
		const std::optional<Welcome> welcome = Welcome::Decode(message);

		if (!welcome || m_State != AgentState::Greeting)
		{
			spdlog::warn("gyro answered a greeting this agent had not made");

			return false;
		}

		m_Version = welcome->Version;

		// **The roles are read off which descriptors this agent has**, so there is one statement of
		// whether a session has a shell and it is the socket's own existence. The order is the bitmap's,
		// ascending, which is what `IndexOf` reads it back by.
		std::uint32_t roles = static_cast<std::uint32_t>(ListenerRole::Applications);
		std::array<RawFd, MaxListeners> listeners{ m_Applications };
		std::size_t count = 1;

		if (m_Shell.IsValid())
		{
			roles |= static_cast<std::uint32_t>(ListenerRole::Shell);
			listeners[count] = m_Shell;
			++count;
		}

		std::array<std::byte, MaxMessageBytes> bytes{};
		const std::size_t written = Offer{ .Roles = roles }.Encode(bytes);

		// **The listeners are attached rather than moved**, which is Session/Listener.h's point: the
		// same sockets are offered again on the next connection, so what crosses here are copies of the
		// descriptors the kernel makes and not the agent's own.
		if (const Result<void> said =
		        Say(std::span<const std::byte>{ bytes }.first(written),
		            std::span<const RawFd>{ listeners }.first(count));
		    !said)
		{
			spdlog::warn("offering the session's listeners failed: {}", said.error());

			return false;
		}

		m_State = AgentState::Offering;

		return true;
	}

	const std::optional<Accepted> accepted = Accepted::Decode(message);

	if (!accepted || m_State != AgentState::Offering)
	{
		spdlog::warn("gyro accepted an offer this agent had not made");

		return false;
	}

	m_Session = accepted->Id;
	m_State = AgentState::Established;

	return true;
}

Result<void> SessionAgent::Say(std::span<const std::byte> message, std::span<const RawFd> attached)
{
	return Send(m_Socket.Borrow(), message, attached);
}

void SessionAgent::Part()
{
	m_Socket.Reset();
	m_State = AgentState::Apart;
	m_Session = SessionId::None;
	m_Version = 0;
}
} // namespace Session
