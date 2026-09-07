#include "Session/Machine.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/Fd.h"
#include "Core/Signal.h"
#include "Session/Handover.h"
#include "Session/Transport.h"
#include "Testing/Test.h"

// The machine peer's end of the control connection, against a compositor this file is writing.
//
// **Scripted rather than real, and the uid is why.** `Session/Control.h` grants the machine to root and
// to nothing else, and a test binary is not root — so a `SessionControl` at the far end would refuse
// every claim here and the only thing exercisable would be the refusal, which
// `Session/Control.Test.cpp` already covers from the other side. What this file tests is the half that
// does not depend on who is running it: the order the peer says things in, what it does with each
// answer, and what it refuses to send before it has been granted anything.
//
// The cost is stated rather than hidden: nothing here proves gyro answers the way this file pretends
// it does. `Session/Handover.h` is what keeps the two honest, being the one place either end reads the
// format from, and the layout is asserted against hand-written bytes in `Handover.Test.cpp`.

namespace
{
using namespace Session;

class TemporaryDirectory
{
public:
	TemporaryDirectory()
	{
		std::string pattern = (std::filesystem::temp_directory_path() / "gyro-machine-XXXXXX").string();

		if (const char* made = ::mkdtemp(pattern.data()); made != nullptr)
		{
			m_Path = made;
		}
	}

	~TemporaryDirectory()
	{
		if (!m_Path.empty())
		{
			std::error_code ignored;
			std::filesystem::remove_all(m_Path, ignored);
		}
	}

	TemporaryDirectory(const TemporaryDirectory&) = delete;
	TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
	TemporaryDirectory(TemporaryDirectory&&) = delete;
	TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

	[[nodiscard]] std::string In(std::string_view name) const { return std::format("{}/{}", m_Path, name); }

private:
	std::string m_Path;
};

// A compositor that answers whatever the test tells it to: it binds the control socket, accepts one
// connection, and reads and writes messages by hand.
class ScriptedGyro
{
public:
	[[nodiscard]] bool Open(const std::string& path)
	{
		::sockaddr_un address{};
		address.sun_family = AF_UNIX;

		if (path.size() >= sizeof address.sun_path)
		{
			return false;
		}

		std::memcpy(address.sun_path, path.data(), path.size());

		m_Listener = Fd{ ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0) };

		return m_Listener.IsValid() &&
		       ::bind(m_Listener.Get(), reinterpret_cast<const ::sockaddr*>(&address), sizeof address) == 0 &&
		       ::listen(m_Listener.Get(), 4) == 0;
	}

	[[nodiscard]] bool Accept()
	{
		m_Peer = Fd{ ::accept4(m_Listener.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC) };

		return m_Peer.IsValid();
	}

	// The opcode of the next message the peer sent, or nothing where it sent none.
	[[nodiscard]] std::optional<Opcode> Heard()
	{
		Result<Received> received = Receive(m_Peer.Borrow(), m_Bytes);

		if (!received || received->State != ReceiveState::Message)
		{
			return std::nullopt;
		}

		m_Length = received->Bytes;

		const std::optional<MessageHeader> header = ReadHeader(std::span<const std::byte>{ m_Bytes }.first(m_Length));

		return header ? std::optional<Opcode>{ header->Op } : std::nullopt;
	}

	[[nodiscard]] std::optional<Assign> Assignment() const
	{
		return Assign::Decode(std::span<const std::byte>{ m_Bytes }.first(m_Length));
	}

	template<typename Message>
	[[nodiscard]] bool Say(const Message& message)
	{
		std::array<std::byte, MaxMessageBytes> bytes{};
		const std::size_t written = message.Encode(bytes);

		return Send(m_Peer.Borrow(), std::span<const std::byte>{ bytes }.first(written)).has_value();
	}

	void Close() { m_Peer.Reset(); }

private:
	Fd m_Listener;
	Fd m_Peer;
	std::array<std::byte, MaxMessageBytes> m_Bytes{};
	std::size_t m_Length = 0;
};

// The requests gyro said had landed.
struct LandingWatcher
{
	std::vector<MachineRequest> Landed;

	void Observe(MachineRequest request) { Landed.push_back(request); }
};

// A control socket, a scripted compositor behind it, and a peer connected to it and greeted.
class Fixture
{
public:
	[[nodiscard]] bool Start()
	{
		m_Link.ConnectTo<&LandingWatcher::Observe>(m_Peer.Landed, m_Watcher);

		return m_Gyro.Open(Path()) && m_Peer.Connect(Path()).has_value() && m_Gyro.Accept();
	}

	// Greet and grant, which is the state every assignment starts from.
	[[nodiscard]] bool Grant()
	{
		return m_Gyro.Heard() == Opcode::Hello && m_Gyro.Heard() == Opcode::Manage && m_Gyro.Say(Welcome{}) &&
		       m_Gyro.Say(Managing{}) && m_Peer.Drain().has_value();
	}

	[[nodiscard]] std::string Path() const { return m_Directory.In("control"); }

	[[nodiscard]] MachinePeer& Peer() noexcept { return m_Peer; }

	[[nodiscard]] ScriptedGyro& Gyro() noexcept { return m_Gyro; }

	[[nodiscard]] LandingWatcher& Watcher() noexcept { return m_Watcher; }

private:
	TemporaryDirectory m_Directory;
	ScriptedGyro m_Gyro;
	MachinePeer m_Peer;
	LandingWatcher m_Watcher;
	Connection<MachineRequest> m_Link;
};
} // namespace

// **The claim goes out behind the greeting without waiting for it to be answered**, which is what puts
// it on the wire before any session agent could have offered. The socket is sequenced, so gyro reads
// them in this order whatever it does with them.
GYRO_TEST(MachinePeer, ClaimsTheMachineBehindTheGreeting)
{
	Fixture fixture;

	GYRO_REQUIRE(fixture.Start());
	GYRO_CHECK(fixture.Gyro().Heard() == Opcode::Hello);
	GYRO_CHECK(fixture.Gyro().Heard() == Opcode::Manage);
	GYRO_CHECK(fixture.Peer().State() == MachineState::Claiming);
}

GYRO_TEST(MachinePeer, RunsTheMachineOnceItIsGranted)
{
	Fixture fixture;

	GYRO_REQUIRE(fixture.Start());
	GYRO_REQUIRE(fixture.Grant());
	GYRO_CHECK(fixture.Peer().State() == MachineState::Running);
}

// Nothing may be assigned before gyro has granted the machine, and refusing here rather than sending
// is what keeps a peer from asking a question it already knows the answer to.
GYRO_TEST(MachinePeer, WillNotAssignBeforeItIsGranted)
{
	Fixture fixture;

	GYRO_REQUIRE(fixture.Start());

	// The greeting and the claim, taken off the queue so that what is asserted below is the absence of a
	// third message rather than the presence of the first.
	GYRO_REQUIRE(fixture.Gyro().Heard() == Opcode::Hello);
	GYRO_REQUIRE(fixture.Gyro().Heard() == Opcode::Manage);

	const Result<void> refused = fixture.Peer().Request(MachineRequest{ .Uid = 1000, .Connector = {} });

	GYRO_REQUIRE(!refused.has_value());
	GYRO_CHECK_EQ(refused.error().Code(), ENOTCONN);
	GYRO_CHECK(!fixture.Gyro().Heard().has_value());
}

GYRO_TEST(MachinePeer, AsksForAUserOnEveryOutput)
{
	Fixture fixture;

	GYRO_REQUIRE(fixture.Start());
	GYRO_REQUIRE(fixture.Grant());
	GYRO_REQUIRE(fixture.Peer().Request(MachineRequest{ .Uid = 1000, .Connector = {} }).has_value());
	GYRO_REQUIRE(fixture.Gyro().Heard() == Opcode::Assign);

	const std::optional<Assign> assign = fixture.Gyro().Assignment();

	GYRO_REQUIRE(assign.has_value());
	GYRO_CHECK_EQ(assign->Uid, std::uint32_t{ 1000 });
	GYRO_CHECK(assign->Connector.IsEveryOutput());
}

GYRO_TEST(MachinePeer, AsksForAUserOnOneNamedOutput)
{
	Fixture fixture;
	const std::optional<ConnectorName> connector = ConnectorName::From("eDP-1");

	GYRO_REQUIRE(connector.has_value());
	GYRO_REQUIRE(fixture.Start());
	GYRO_REQUIRE(fixture.Grant());
	GYRO_REQUIRE(fixture.Peer().Request(MachineRequest{ .Uid = 42, .Connector = *connector }).has_value());
	GYRO_REQUIRE(fixture.Gyro().Heard() == Opcode::Assign);

	const std::optional<Assign> assign = fixture.Gyro().Assignment();

	GYRO_REQUIRE(assign.has_value());
	GYRO_CHECK(assign->Connector.Text() == "eDP-1");
}

// The answer is what says the screen actually moved, and it arrives whenever gyro could satisfy the
// request rather than when it received it.
GYRO_TEST(MachinePeer, LearnsWhenARequestLands)
{
	Fixture fixture;
	const std::optional<ConnectorName> connector = ConnectorName::From("DP-7");

	GYRO_REQUIRE(connector.has_value());
	GYRO_REQUIRE(fixture.Start());
	GYRO_REQUIRE(fixture.Grant());
	GYRO_REQUIRE(fixture.Peer().Request(MachineRequest{ .Uid = 7, .Connector = *connector }).has_value());
	GYRO_CHECK(fixture.Watcher().Landed.empty());

	GYRO_REQUIRE(fixture.Gyro().Say(Assigned{ .Uid = 7, .Connector = *connector }));
	GYRO_REQUIRE(fixture.Peer().Drain().has_value());

	GYRO_REQUIRE_EQ(fixture.Watcher().Landed.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(fixture.Watcher().Landed[0].Uid, std::uint32_t{ 7 });
	GYRO_CHECK(fixture.Watcher().Landed[0].Connector.Text() == "DP-7");
}

// A refusal is terminal: gyro has judged the claim, so reconnecting and claiming again would ask the
// same question at whatever rate the backoff allows.
GYRO_TEST(MachinePeer, KeepsWhatGyroRefusedWith)
{
	Fixture fixture;

	GYRO_REQUIRE(fixture.Start());
	GYRO_REQUIRE(fixture.Gyro().Heard() == Opcode::Hello);
	GYRO_REQUIRE(fixture.Gyro().Say(Refused{ .Code = EACCES, .Text = Reason{ "only root may run the machine" } }));
	GYRO_REQUIRE(fixture.Peer().Drain().has_value());

	GYRO_CHECK(fixture.Peer().State() == MachineState::Apart);
	GYRO_CHECK_EQ(fixture.Peer().Rejection(), EACCES);
	GYRO_CHECK(fixture.Peer().Rejected() == "only root may run the machine");
}

// gyro going away leaves the peer apart, which is what its caller retries out of — a login agent whose
// compositor restarted re-claims and re-states what it wanted.
GYRO_TEST(MachinePeer, PartsWhenGyroGoesAway)
{
	Fixture fixture;

	GYRO_REQUIRE(fixture.Start());
	GYRO_REQUIRE(fixture.Grant());

	fixture.Gyro().Close();

	GYRO_REQUIRE(fixture.Peer().Drain().has_value());
	GYRO_CHECK(fixture.Peer().State() == MachineState::Apart);
}

// A message travelling the wrong way is a peer that is not gyro, the control socket being
// world-writable. The same check gyro makes, made for the same reason.
GYRO_TEST(MachinePeer, RefusesAMessageTravellingTheWrongWay)
{
	Fixture fixture;

	GYRO_REQUIRE(fixture.Start());
	GYRO_REQUIRE(fixture.Grant());
	GYRO_REQUIRE(fixture.Gyro().Say(Hello{}));
	GYRO_REQUIRE(fixture.Peer().Drain().has_value());

	GYRO_CHECK(fixture.Peer().State() == MachineState::Apart);
}

// Connecting twice is refused rather than leaking the first socket.
GYRO_TEST(MachinePeer, WillNotConnectTwice)
{
	Fixture fixture;

	GYRO_REQUIRE(fixture.Start());

	const Result<void> again = fixture.Peer().Connect(fixture.Path());

	GYRO_REQUIRE(!again.has_value());
	GYRO_CHECK_EQ(again.error().Code(), EISCONN);
}

GYRO_TEST(MachinePeer, ConnectingBeforeGyroIsUpFails)
{
	TemporaryDirectory directory;
	MachinePeer peer;

	const Result<void> failed = peer.Connect(directory.In("nothing-here"));

	GYRO_REQUIRE(!failed.has_value());
	GYRO_CHECK(peer.State() == MachineState::Apart);
}
