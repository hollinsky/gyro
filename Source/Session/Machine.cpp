#include "Session/Machine.h"

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
Result<void> MachinePeer::Connect(std::string_view controlPath)
{
	if (m_Socket.IsValid())
	{
		return Failure(EISCONN, "the machine peer is already connected");
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
	m_State = MachineState::Greeting;

	std::array<std::byte, MaxMessageBytes> bytes{};
	const std::size_t greeting = Hello{}.Encode(bytes);

	if (const Result<void> said = Say(std::span<const std::byte>{ bytes }.first(greeting)); !said)
	{
		Part();

		return said;
	}

	// **The claim goes out behind the greeting rather than after its answer**, which the header argues
	// for on the socket's ordering: gyro reads the two in the order they were written and answers them
	// in that order, so nothing is being assumed about timing. What it buys is that the machine is
	// claimed before any session agent could have offered, which is the whole reason the claim exists.
	const std::size_t claim = Manage{}.Encode(bytes);

	if (const Result<void> said = Say(std::span<const std::byte>{ bytes }.first(claim)); !said)
	{
		Part();

		return said;
	}

	m_State = MachineState::Claiming;

	return {};
}

Result<void> MachinePeer::Request(const MachineRequest& request)
{
	if (m_State != MachineState::Running)
	{
		return Failure(ENOTCONN, "the machine has not been granted, so there is nothing to assign on");
	}

	std::array<std::byte, MaxMessageBytes> bytes{};
	const Assign assign{ .Uid = request.Uid, .Connector = request.Connector };
	const std::size_t written = assign.Encode(bytes);

	return Say(std::span<const std::byte>{ bytes }.first(written));
}

Result<void> MachinePeer::Drain()
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

		// Descriptors are a refusal on this side whatever the message, which is `SessionAgent::Drain`'s
		// reason unchanged: nothing gyro sends ever carries one, so anything that does is a peer that is
		// not gyro. The `Received` closes them either way.
		if (received->AttachedCount != 0 || received->Truncated)
		{
			spdlog::warn("gyro sent a handover message this machine peer cannot read");

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

bool MachinePeer::Handle(std::span<const std::byte> message)
{
	const std::optional<MessageHeader> header = ReadHeader(message);

	if (!header)
	{
		spdlog::warn("gyro sent a message that is not one this machine peer can read");

		return false;
	}

	// The control socket is world-writable, so a message travelling the wrong way is a peer that is not
	// gyro. The same check gyro makes, for the same reason.
	if (FromPeer(header->Op))
	{
		spdlog::warn("a handover message travelling the wrong way arrived on the control connection");

		return false;
	}

	if (header->Op == Opcode::Refused)
	{
		const std::optional<Refused> refused = Refused::Decode(message);

		// A refusal that cannot be decoded is still a refusal, and treating it as anything else would
		// have the peer retry into a compositor that has just said no.
		m_Rejection = refused ? static_cast<int>(refused->Code) : EPROTO;
		m_Rejected = refused ? refused->Text : Reason{ "gyro refused in a way this peer cannot read" };

		spdlog::error("gyro refused the machine: {}", m_Rejected.Text());

		return false;
	}

	if (header->Op == Opcode::Welcome)
	{
		const std::optional<Welcome> welcome = Welcome::Decode(message);

		// **`Claiming` as well as `Greeting`, because the claim was sent without waiting.** By the time
		// this arrives `Connect` has already moved on, so insisting on `Greeting` here would refuse the
		// answer to a message this object certainly sent.
		if (!welcome || (m_State != MachineState::Greeting && m_State != MachineState::Claiming))
		{
			spdlog::warn("gyro answered a greeting this machine peer had not made");

			return false;
		}

		m_Version = welcome->Version;

		return true;
	}

	if (header->Op == Opcode::Managing)
	{
		if (!Managing::Decode(message) || m_State != MachineState::Claiming)
		{
			spdlog::warn("gyro granted a machine this peer had not claimed");

			return false;
		}

		m_State = MachineState::Running;

		spdlog::info("gyro granted the machine to this process");

		return true;
	}

	const std::optional<Assigned> assigned = Assigned::Decode(message);

	if (!assigned || m_State != MachineState::Running)
	{
		spdlog::warn("gyro answered an assignment this machine peer had not made");

		return false;
	}

	spdlog::info(
		"uid {} is on {}",
		assigned->Uid,
		assigned->Connector.IsEveryOutput() ? "every output" : assigned->Connector.Text()
	);

	Landed.Emit(MachineRequest{ .Uid = assigned->Uid, .Connector = assigned->Connector });

	return true;
}

Result<void> MachinePeer::Say(std::span<const std::byte> message)
{
	return Send(m_Socket.Borrow(), message);
}

void MachinePeer::Part()
{
	m_Socket.Reset();
	m_State = MachineState::Apart;
	m_Version = 0;
}
} // namespace Session
