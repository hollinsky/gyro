#include "Session/Control.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Core/Fd.h"
#include "Core/Signal.h"
#include "Session/Handover.h"
#include "Session/Transport.h"
#include "Testing/Test.h"

// The control socket, against a real one — every test here binds an `AF_UNIX` socket in a temporary
// directory and speaks the handover over it.
//
// **Same-uid, which leaves the one check this file cannot exercise.** `SO_PEERCRED` is what attributes
// a connection to a user, and a test process is the only user it can be: everything below is the
// offering peer *and* the party being protected from, so the uid comparison never rejects anything
// here. What is tested instead is everything that check does not cover — the four questions asked of
// the descriptor itself, the order the messages have to arrive in, and the caps — and the uid path
// stays unexercised until gyro runs as its own user.

namespace
{
using namespace Session;

// A directory that removes itself, so a failed test leaves no sockets behind in `/tmp`.
class TemporaryDirectory
{
public:
	TemporaryDirectory()
	{
		std::string pattern = (std::filesystem::temp_directory_path() / "gyro-handover-XXXXXX").string();

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

	[[nodiscard]] const std::string& Path() const noexcept { return m_Path; }

	[[nodiscard]] std::string In(std::string_view name) const { return std::format("{}/{}", m_Path, name); }

private:
	std::string m_Path;
};

// A listening `AF_UNIX` stream socket at a path, which is what an agent offers.
[[nodiscard]] Fd MakeListener(const std::string& path)
{
	::sockaddr_un address{};
	address.sun_family = AF_UNIX;

	if (path.size() >= sizeof address.sun_path)
	{
		return Fd{};
	}

	std::memcpy(address.sun_path, path.data(), path.size());

	Fd socket{ ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0) };

	if (!socket.IsValid())
	{
		return Fd{};
	}

	if (::bind(socket.Get(), reinterpret_cast<const ::sockaddr*>(&address), sizeof address) != 0 ||
	    ::listen(socket.Get(), 4) != 0)
	{
		return Fd{};
	}

	return socket;
}

// What an agent does, without being one: connect, send, and read the answer.
class Peer
{
public:
	[[nodiscard]] bool ConnectTo(const std::string& path)
	{
		::sockaddr_un address{};
		address.sun_family = AF_UNIX;

		if (path.size() >= sizeof address.sun_path)
		{
			return false;
		}

		std::memcpy(address.sun_path, path.data(), path.size());

		m_Socket = Fd{ ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0) };

		return m_Socket.IsValid() &&
		       ::connect(m_Socket.Get(), reinterpret_cast<const ::sockaddr*>(&address), sizeof address) == 0;
	}

	template<typename Message>
	[[nodiscard]] bool SendMessage(const Message& message, RawFd attached = RawFd{})
	{
		std::array<std::byte, MaxMessageBytes> bytes{};
		const std::size_t written = message.Encode(bytes);

		return Send(m_Socket.Borrow(), std::span<const std::byte>{ bytes }.first(written), attached).has_value();
	}

	// The opcode of whatever came back, or nothing where nothing did.
	[[nodiscard]] std::optional<Opcode> Answer()
	{
		Result<Received> received = Receive(m_Socket.Borrow(), m_Bytes);

		if (!received || received->State != ReceiveState::Message)
		{
			return std::nullopt;
		}

		m_Length = received->Bytes;

		const std::optional<MessageHeader> header = ReadHeader(std::span<const std::byte>{ m_Bytes }.first(m_Length));

		return header ? std::optional<Opcode>{ header->Op } : std::nullopt;
	}

	// The refusal that came back, for a test that cares which one it was.
	[[nodiscard]] std::optional<Refused> Refusal() const
	{
		return Refused::Decode(std::span<const std::byte>{ m_Bytes }.first(m_Length));
	}

	[[nodiscard]] std::optional<Accepted> Acceptance() const
	{
		return Accepted::Decode(std::span<const std::byte>{ m_Bytes }.first(m_Length));
	}

	void Disconnect() { m_Socket.Reset(); }

private:
	Fd m_Socket;
	std::array<std::byte, MaxMessageBytes> m_Bytes{};
	std::size_t m_Length = 0;
};

// The sessions that have ended, since that half of the boundary is a signal rather than a verb.
struct EndWatcher
{
	std::vector<SessionId> Ended;

	void Observe(SessionId id) { Ended.push_back(id); }
};

// Everything a test needs standing up: a control socket, a runtime root with this user's directory
// under it, and the uid the kernel will report for every connection made here.
class Fixture
{
public:
	Fixture()
	{
		std::error_code ignored;
		std::filesystem::create_directories(RuntimeDirectory(), ignored);

		Result<std::unique_ptr<SessionControl>> opened =
			SessionControl::Open(m_Directory.In("control"), m_Directory.In("run"));

		if (opened)
		{
			m_Control = std::move(*opened);
			m_Link.ConnectTo<&EndWatcher::Observe>(m_Control->Ended, m_Watcher);
		}
	}

	[[nodiscard]] SessionControl& Control() const noexcept { return *m_Control; }

	[[nodiscard]] EndWatcher& Watcher() noexcept { return m_Watcher; }

	[[nodiscard]] std::string ControlPath() const { return m_Directory.In("control"); }

	[[nodiscard]] std::string RuntimeDirectory() const
	{
		return std::format("{}/{}", m_Directory.In("run"), ::geteuid());
	}

	[[nodiscard]] std::string OutsidePath(std::string_view name) const { return m_Directory.In(name); }

	// Connect a peer and get past the greeting, which every test but one wants.
	[[nodiscard]] bool Greet(Peer& peer)
	{
		if (!peer.ConnectTo(ControlPath()) || !peer.SendMessage(Hello{}))
		{
			return false;
		}

		return Control().Drain().has_value() && peer.Answer() == Opcode::Welcome;
	}

private:
	TemporaryDirectory m_Directory;
	std::unique_ptr<SessionControl> m_Control;
	EndWatcher m_Watcher;
	Connection<SessionId> m_Link;
};
} // namespace

GYRO_TEST(SessionControl, GreetingIsAnswered)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(peer.ConnectTo(fixture.ControlPath()));
	GYRO_REQUIRE(peer.SendMessage(Hello{ .Version = HandoverVersion }));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_CHECK(peer.Answer() == Opcode::Welcome);
	GYRO_CHECK_EQ(fixture.Control().Connections(), std::size_t{ 1 });
}

GYRO_TEST(SessionControl, OfferIsAcceptedAndHandedOver)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(fixture.Greet(peer));

	const Fd listener = MakeListener(std::format("{}/wayland-0", fixture.RuntimeDirectory()));

	GYRO_REQUIRE(listener.IsValid());
	GYRO_REQUIRE(peer.SendMessage(Offer{}, listener.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Accepted);

	const std::optional<Accepted> accepted = peer.Acceptance();

	GYRO_REQUIRE(accepted.has_value());
	GYRO_CHECK(accepted->Id != SessionId::None);

	std::optional<AcceptedOffer> offer = fixture.Control().TakeOffer();

	GYRO_REQUIRE(offer.has_value());
	GYRO_CHECK(offer->Id == accepted->Id);
	GYRO_CHECK_EQ(offer->Uid, ::geteuid());
	GYRO_CHECK(offer->Listener.IsValid());

	// Taken once. The listener is a resource and `TakeOffer` is where ownership stops travelling.
	GYRO_CHECK(!fixture.Control().TakeOffer().has_value());
}

// A version disagreement has to be settled before a descriptor is spent, so nothing else is accepted
// until the greeting is answered.
GYRO_TEST(SessionControl, OfferBeforeTheGreetingIsRefused)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(peer.ConnectTo(fixture.ControlPath()));

	const Fd listener = MakeListener(std::format("{}/wayland-0", fixture.RuntimeDirectory()));

	GYRO_REQUIRE(listener.IsValid());
	GYRO_REQUIRE(peer.SendMessage(Offer{}, listener.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Refused);

	const std::optional<Refused> refused = peer.Refusal();

	GYRO_REQUIRE(refused.has_value());
	GYRO_CHECK_EQ(refused->Code, static_cast<std::uint32_t>(EPROTO));
	GYRO_CHECK(!fixture.Control().TakeOffer().has_value());
}

GYRO_TEST(SessionControl, ASecondOfferOnOneConnectionIsRefused)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(fixture.Greet(peer));

	const Fd first = MakeListener(std::format("{}/wayland-0", fixture.RuntimeDirectory()));
	const Fd second = MakeListener(std::format("{}/wayland-1", fixture.RuntimeDirectory()));

	GYRO_REQUIRE(first.IsValid() && second.IsValid());
	GYRO_REQUIRE(peer.SendMessage(Offer{}, first.Borrow()));
	GYRO_REQUIRE(peer.SendMessage(Offer{}, second.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Accepted);
	GYRO_REQUIRE(peer.Answer() == Opcode::Refused);

	const std::optional<Refused> refused = peer.Refusal();

	GYRO_REQUIRE(refused.has_value());
	GYRO_CHECK_EQ(refused->Code, static_cast<std::uint32_t>(EBUSY));
}

// One session per user. A second agent for somebody who already has one is refused rather than
// silently taking over, because taking over would hand a running session's windows to a new listener.
GYRO_TEST(SessionControl, ASecondSessionForOneUserIsRefused)
{
	Fixture fixture;
	Peer first;
	Peer second;

	GYRO_REQUIRE(fixture.Greet(first));
	GYRO_REQUIRE(fixture.Greet(second));

	const Fd mine = MakeListener(std::format("{}/wayland-0", fixture.RuntimeDirectory()));
	const Fd theirs = MakeListener(std::format("{}/wayland-1", fixture.RuntimeDirectory()));

	GYRO_REQUIRE(mine.IsValid() && theirs.IsValid());
	GYRO_REQUIRE(first.SendMessage(Offer{}, mine.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(first.Answer() == Opcode::Accepted);

	GYRO_REQUIRE(second.SendMessage(Offer{}, theirs.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(second.Answer() == Opcode::Refused);

	const std::optional<Refused> refused = second.Refusal();

	GYRO_REQUIRE(refused.has_value());
	GYRO_CHECK_EQ(refused->Code, static_cast<std::uint32_t>(EBUSY));
}

GYRO_TEST(SessionControl, ADescriptorThatIsNotListeningIsRefused)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(fixture.Greet(peer));

	const Fd bound{ ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0) };

	GYRO_REQUIRE(bound.IsValid());
	GYRO_REQUIRE(peer.SendMessage(Offer{}, bound.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Refused);

	const std::optional<Refused> refused = peer.Refusal();

	GYRO_REQUIRE(refused.has_value());
	GYRO_CHECK_EQ(refused->Code, static_cast<std::uint32_t>(EBADF));
}

// A listener in a directory other users can reach is one other users can connect to, which would
// leave the connection-time uid check as the only thing between two people's windows.
GYRO_TEST(SessionControl, AListenerOutsideTheRuntimeDirectoryIsRefused)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(fixture.Greet(peer));

	const Fd listener = MakeListener(fixture.OutsidePath("wayland-0"));

	GYRO_REQUIRE(listener.IsValid());
	GYRO_REQUIRE(peer.SendMessage(Offer{}, listener.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Refused);

	const std::optional<Refused> refused = peer.Refusal();

	GYRO_REQUIRE(refused.has_value());
	GYRO_CHECK_EQ(refused->Code, static_cast<std::uint32_t>(EACCES));
}

// A textual prefix is only a prefix: this path starts with the runtime directory and is not inside it.
GYRO_TEST(SessionControl, APathClimbingOutOfTheRuntimeDirectoryIsRefused)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(fixture.Greet(peer));

	const Fd listener = MakeListener(std::format("{}/../escaped", fixture.RuntimeDirectory()));

	GYRO_REQUIRE(listener.IsValid());
	GYRO_REQUIRE(peer.SendMessage(Offer{}, listener.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Refused);

	const std::optional<Refused> refused = peer.Refusal();

	GYRO_REQUIRE(refused.has_value());
	GYRO_CHECK_EQ(refused->Code, static_cast<std::uint32_t>(EACCES));
}

GYRO_TEST(SessionControl, AMessageTravellingTheWrongWayIsRefused)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(peer.ConnectTo(fixture.ControlPath()));
	GYRO_REQUIRE(peer.SendMessage(Welcome{}));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Refused);

	const std::optional<Refused> refused = peer.Refusal();

	GYRO_REQUIRE(refused.has_value());
	GYRO_CHECK_EQ(refused->Code, static_cast<std::uint32_t>(EPROTO));
}

// A descriptor on a message that does not carry one is a file table filling up on the process that
// must not die, so the count is checked before the message is read.
GYRO_TEST(SessionControl, ADescriptorOnTheGreetingIsRefused)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(peer.ConnectTo(fixture.ControlPath()));

	const Fd listener = MakeListener(std::format("{}/wayland-0", fixture.RuntimeDirectory()));

	GYRO_REQUIRE(listener.IsValid());
	GYRO_REQUIRE(peer.SendMessage(Hello{}, listener.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Refused);

	const std::optional<Refused> refused = peer.Refusal();

	GYRO_REQUIRE(refused.has_value());
	GYRO_CHECK_EQ(refused->Code, static_cast<std::uint32_t>(EPROTO));
}

// The whole reason the agent holds the connection open: its EOF is the session ending, by
// construction rather than by anybody remembering to say so.
GYRO_TEST(SessionControl, TheAgentGoingAwayEndsTheSession)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(fixture.Greet(peer));

	const Fd listener = MakeListener(std::format("{}/wayland-0", fixture.RuntimeDirectory()));

	GYRO_REQUIRE(listener.IsValid());
	GYRO_REQUIRE(peer.SendMessage(Offer{}, listener.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Accepted);

	const std::optional<Accepted> accepted = peer.Acceptance();

	GYRO_REQUIRE(accepted.has_value());

	peer.Disconnect();

	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE_EQ(fixture.Watcher().Ended.size(), std::size_t{ 1 });
	GYRO_CHECK(fixture.Watcher().Ended.front() == accepted->Id);
	GYRO_CHECK_EQ(fixture.Control().Connections(), std::size_t{ 0 });
}

// And a connection that never established one ends nothing, so a retrying agent does not look like a
// session flapping.
GYRO_TEST(SessionControl, AConnectionThatNeverOfferedEndsNothing)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(fixture.Greet(peer));

	peer.Disconnect();

	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_CHECK(fixture.Watcher().Ended.empty());
	GYRO_CHECK_EQ(fixture.Control().Connections(), std::size_t{ 0 });
}

// A user who can open one connection can open thirty-two, so the per-user cap is the one that bounds
// the global one.
GYRO_TEST(SessionControl, TooManyConnectionsForOneUserAreRefused)
{
	Fixture fixture;
	std::array<Peer, MaxConnectionsPerUid> held;

	for (Peer& peer : held)
	{
		GYRO_REQUIRE(peer.ConnectTo(fixture.ControlPath()));
	}

	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE_EQ(fixture.Control().Connections(), MaxConnectionsPerUid);

	Peer extra;

	GYRO_REQUIRE(extra.ConnectTo(fixture.ControlPath()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_CHECK(extra.Answer() == Opcode::Refused);
	GYRO_CHECK_EQ(fixture.Control().Connections(), MaxConnectionsPerUid);
}

// A path holding something that is not a socket is refused rather than removed: gyro is a boot service
// and deleting a file somebody else put there is not a thing to do unasked.
GYRO_TEST(SessionControl, ARegularFileAtTheControlPathIsRefused)
{
	TemporaryDirectory directory;
	const std::string path = directory.In("control");

	{
		const Fd file{ ::open(path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600) };

		GYRO_REQUIRE(file.IsValid());
	}

	GYRO_CHECK(!SessionControl::Open(path, directory.In("run")).has_value());
}
