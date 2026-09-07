#include "Session/Control.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
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

#include "Core/Clock.h"
#include "Core/Fd.h"
#include "Core/Signal.h"
#include "Core/Trace.h"
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
		const std::array<RawFd, 1> one{ attached };

		return SendMessage(message, std::span<const RawFd>{ one });
	}

	template<typename Message>
	[[nodiscard]] bool SendMessage(const Message& message, std::span<const RawFd> attached)
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

// What the machine peer has asked for, which crosses the same way and for the same reason.
struct RequestWatcher
{
	std::vector<MachineRequest> Requested;

	void Observe(MachineRequest request) { Requested.push_back(request); }
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
			m_RequestLink.ConnectTo<&RequestWatcher::Observe>(m_Control->Requested, m_Requests);
		}
	}

	[[nodiscard]] SessionControl& Control() const noexcept { return *m_Control; }

	[[nodiscard]] EndWatcher& Watcher() noexcept { return m_Watcher; }

	[[nodiscard]] RequestWatcher& Requests() noexcept { return m_Requests; }

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

	RequestWatcher m_Requests;
	Connection<MachineRequest> m_RequestLink;
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

// A session is offered whole, so a shell listener arrives on the same message as the applications one
// and is handed over beside it. There is nothing to do afterwards to give a session a shell.
GYRO_TEST(SessionControl, AShellListenerIsAcceptedWithTheSessionThatHasIt)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(fixture.Greet(peer));

	const Fd applications = MakeListener(std::format("{}/wayland-0", fixture.RuntimeDirectory()));
	const Fd shell = MakeListener(std::format("{}/wayland-0-shell", fixture.RuntimeDirectory()));

	GYRO_REQUIRE(applications.IsValid());
	GYRO_REQUIRE(shell.IsValid());

	const std::array<RawFd, 2> both{ applications.Borrow(), shell.Borrow() };
	const Offer offer{ .Roles = KnownRoles };

	GYRO_REQUIRE(peer.SendMessage(offer, std::span<const RawFd>{ both }));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Accepted);

	std::optional<AcceptedOffer> taken = fixture.Control().TakeOffer();

	GYRO_REQUIRE(taken.has_value());
	GYRO_CHECK(taken->Listener.IsValid());
	GYRO_CHECK(taken->Shell.IsValid());
}

// The ordinary session, and the one an agent that starts no shell offers: nothing on the machine can
// then reach the System tier in it, which is the answer rather than an omission.
GYRO_TEST(SessionControl, ASessionWithNoShellListenerHasNone)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(fixture.Greet(peer));

	const Fd listener = MakeListener(std::format("{}/wayland-0", fixture.RuntimeDirectory()));

	GYRO_REQUIRE(listener.IsValid());
	GYRO_REQUIRE(peer.SendMessage(Offer{}, listener.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Accepted);

	std::optional<AcceptedOffer> taken = fixture.Control().TakeOffer();

	GYRO_REQUIRE(taken.has_value());
	GYRO_CHECK(taken->Listener.IsValid());
	GYRO_CHECK(!taken->Shell.IsValid());
}

// The bitmap is the descriptor count, so the two have to agree — and a peer that says two and sends
// one would otherwise have gyro adopt a socket at a role nobody named.
GYRO_TEST(SessionControl, AnOfferWhoseRolesDoNotMatchItsDescriptorsIsRefused)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(fixture.Greet(peer));

	const Fd listener = MakeListener(std::format("{}/wayland-0", fixture.RuntimeDirectory()));

	GYRO_REQUIRE(listener.IsValid());
	GYRO_REQUIRE(peer.SendMessage(Offer{ .Roles = KnownRoles }, listener.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_CHECK(peer.Answer() == Opcode::Refused);
}

// A bit this build has no name for is refused rather than masked off, and an offer with no
// applications listener is a session no client could reach.
GYRO_TEST(SessionControl, AnOfferNamingRolesGyroDoesNotHaveIsRefused)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(fixture.Greet(peer));

	const Fd listener = MakeListener(std::format("{}/wayland-0", fixture.RuntimeDirectory()));

	GYRO_REQUIRE(listener.IsValid());
	GYRO_REQUIRE(peer.SendMessage(Offer{ .Roles = 1U << 8U }, listener.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_CHECK(peer.Answer() == Opcode::Refused);
}

// Every listener is judged before any is taken, so a shell socket bound where it should not be costs
// the session rather than being quietly dropped — which would leave a shell connected to a queue
// nobody will ever accept from.
GYRO_TEST(SessionControl, AnOfferIsRefusedWholeWhenOnlyTheShellListenerIsWrong)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(fixture.Greet(peer));

	const Fd applications = MakeListener(std::format("{}/wayland-0", fixture.RuntimeDirectory()));
	const Fd shell = MakeListener(fixture.OutsidePath("elsewhere-shell"));

	GYRO_REQUIRE(applications.IsValid());
	GYRO_REQUIRE(shell.IsValid());

	const std::array<RawFd, 2> both{ applications.Borrow(), shell.Borrow() };

	GYRO_REQUIRE(peer.SendMessage(Offer{ .Roles = KnownRoles }, std::span<const RawFd>{ both }));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_CHECK(peer.Answer() == Opcode::Refused);
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

namespace
{
// A ring, its storage, and what landed on the session row. The buffer holds a span, so the storage has
// to outlive it — Core/Trace.Test.cpp's shape, kept here rather than shared because a test helper
// travelling between modules is a dependency edge for the sake of six lines.
class SessionTrace
{
public:
	SessionTrace() { m_Buffer.Arm(m_Records, m_Clock); }

	~SessionTrace() { EnrollTracing(nullptr); }

	SessionTrace(const SessionTrace&) = delete;
	SessionTrace& operator=(const SessionTrace&) = delete;
	SessionTrace(SessionTrace&&) = delete;
	SessionTrace& operator=(SessionTrace&&) = delete;

	// Recording starts when a test says so rather than in the constructor, so that standing the fixture
	// up is not on the row the assertions read.
	void Record() { EnrollTracing(&m_Buffer); }

	void Stop() { EnrollTracing(nullptr); }

	// Every mark on the session row, in the order it was made, with the number the slice carries.
	[[nodiscard]] std::vector<std::pair<std::string_view, std::uint64_t>> Marks() const
	{
		std::array<TraceEvent, 128> events{};
		const std::size_t count = m_Buffer.Copy(events);

		std::vector<std::pair<std::string_view, std::uint64_t>> marks;

		for (std::size_t index = 0; index < count; ++index)
		{
			const TraceEvent& event = events[index];

			if (event.Scope == TraceSession() && event.Kind == TraceKind::Mark)
			{
				marks.emplace_back(std::string_view{ event.Name }, event.Payload);
			}
		}

		return marks;
	}

	[[nodiscard]] std::vector<std::string_view> Names() const
	{
		std::vector<std::string_view> names;

		for (const auto& [name, payload] : Marks())
		{
			names.push_back(name);
		}

		return names;
	}

private:
	ManualClock m_Clock;
	std::array<TraceRecord, 128> m_Records{};
	TraceBuffer m_Buffer;
};
} // namespace

// **The row has to carry the states and not only the ends**, because the failure it exists to describe
// is a session that never started: a capture holding `agent connected` and nothing after it says gyro
// took the connection and never answered, which is the one thing a person staring at a blank screen
// cannot otherwise tell from a machine where no agent ran at all.
GYRO_TEST(SessionControl, TheHandshakeIsOnTheSessionRow)
{
	Fixture fixture;
	Peer peer;
	SessionTrace trace;

	trace.Record();

	GYRO_REQUIRE(fixture.Greet(peer));

	const Fd listener = MakeListener(std::format("{}/wayland-0", fixture.RuntimeDirectory()));

	GYRO_REQUIRE(listener.IsValid());
	GYRO_REQUIRE(peer.SendMessage(Offer{}, listener.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Accepted);

	peer.Disconnect();

	GYRO_REQUIRE(fixture.Control().Drain().has_value());

	trace.Stop();

	const std::vector<std::pair<std::string_view, std::uint64_t>> marks = trace.Marks();

	GYRO_REQUIRE_EQ(marks.size(), std::size_t{ 4 });
	GYRO_CHECK(marks[0].first == "agent connected");
	GYRO_CHECK(marks[1].first == "agent greeted");
	GYRO_CHECK(marks[2].first == "session established");
	GYRO_CHECK(marks[3].first == "session ended");

	// The tag turns over where the identity does: a uid until an offer has been taken, and the session
	// id from there on, which is what makes one session's events findable on a row three of them share.
	GYRO_CHECK_EQ(marks[0].second, std::uint64_t{ ::geteuid() });
	GYRO_CHECK_EQ(marks[1].second, std::uint64_t{ ::geteuid() });
	GYRO_CHECK_EQ(marks[2].second, marks[3].second);
	GYRO_CHECK(marks[2].second != 0);
}

// A refusal names itself, which is the whole reason the sentence is a literal: these are the
// security-relevant events, and a row saying only *refused* would need the source open beside it.
GYRO_TEST(SessionControl, ARefusalIsRecordedWithItsReason)
{
	Fixture fixture;
	Peer peer;
	SessionTrace trace;

	trace.Record();

	GYRO_REQUIRE(fixture.Greet(peer));

	// A listener bound outside the offering user's runtime directory, which is one of the four
	// questions `InspectOffer` asks and the one that would otherwise hand somebody else's windows over.
	const Fd elsewhere = MakeListener(fixture.OutsidePath("wayland-0"));

	GYRO_REQUIRE(elsewhere.IsValid());
	GYRO_REQUIRE(peer.SendMessage(Offer{}, elsewhere.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Refused);

	trace.Stop();

	const std::vector<std::string_view> names = trace.Names();

	GYRO_CHECK(
		std::ranges::find(names, "the offered listener is bound outside the offering user's runtime directory") !=
		names.end()
	);

	// And no session was established, so nothing on the row claims one was.
	GYRO_CHECK(std::ranges::find(names, "session established") == names.end());
}

// A greeting that never arrives leaves the agent stuck in `Offering`'s predecessor, and the connection
// going away is the only thing gyro sees. It has to be on the row, or a boot where every agent died
// halfway is a capture with nothing in it.
GYRO_TEST(SessionControl, AnAgentThatPartsBeforeOfferingIsRecorded)
{
	Fixture fixture;
	Peer peer;
	SessionTrace trace;

	trace.Record();

	GYRO_REQUIRE(fixture.Greet(peer));

	peer.Disconnect();

	GYRO_REQUIRE(fixture.Control().Drain().has_value());

	trace.Stop();

	const std::vector<std::string_view> names = trace.Names();

	GYRO_CHECK(std::ranges::find(names, "agent parted") != names.end());
	GYRO_CHECK(std::ranges::find(names, "session ended") == names.end());
}

// **A `.pftrace` gets mailed to somebody who was not at the machine.** A uid is a number on the machine
// that allocated it; a username is a person, and a runtime directory or a socket path is a username
// with a prefix. So the row is swept for both, over every branch a session can take: the accept, the
// greeting, the establishment, four kinds of refusal, and the end.
GYRO_TEST(SessionControl, NoMarkNamesAUserOrAPath)
{
	Fixture fixture;
	SessionTrace trace;

	trace.Record();

	{
		Peer established;

		GYRO_REQUIRE(fixture.Greet(established));

		const Fd listener = MakeListener(std::format("{}/wayland-0", fixture.RuntimeDirectory()));

		GYRO_REQUIRE(listener.IsValid());
		GYRO_REQUIRE(established.SendMessage(Offer{}, listener.Borrow()));
		GYRO_REQUIRE(fixture.Control().Drain().has_value());

		// Two more refusals over the same control socket: an offer that arrived before the greeting, and
		// a second session for a uid that already has one.
		Peer early;

		GYRO_REQUIRE(early.ConnectTo(fixture.ControlPath()));
		GYRO_REQUIRE(early.SendMessage(Offer{}, listener.Borrow()));
		GYRO_REQUIRE(fixture.Control().Drain().has_value());

		Peer outside;

		GYRO_REQUIRE(fixture.Greet(outside));

		const Fd elsewhere = MakeListener(fixture.OutsidePath("wayland-1"));

		GYRO_REQUIRE(elsewhere.IsValid());
		GYRO_REQUIRE(outside.SendMessage(Offer{}, elsewhere.Borrow()));
		GYRO_REQUIRE(fixture.Control().Drain().has_value());
	}

	GYRO_REQUIRE(fixture.Control().Drain().has_value());

	trace.Stop();

	const std::vector<std::string_view> names = trace.Names();

	GYRO_REQUIRE(!names.empty());

	const char* const user = ::getenv("USER");

	for (const std::string_view name : names)
	{
		// A path is the thing to look for rather than any particular one: nothing on this row has an
		// honest reason to hold a separator, so the check does not have to guess which path leaked.
		GYRO_CHECK(name.find('/') == std::string_view::npos);
		GYRO_CHECK(name.find(fixture.RuntimeDirectory()) == std::string_view::npos);
		GYRO_CHECK(name.find("wayland-") == std::string_view::npos);

		if (user != nullptr && *user != '\0')
		{
			GYRO_CHECK(name.find(user) == std::string_view::npos);
		}
	}
}

// The machine peer, which is the other kind of connection this socket serves. It offers no session and
// says instead which user's session belongs on which screen.
//
// **This is the first check in this file the uid actually decides**, and it is worth saying out loud
// beside the note at the top. A session offer is same-uid by construction here, so `SO_PEERCRED` never
// refuses one; the machine claim requires root, and a test binary is almost never root — so the
// refusal below is exercised on every ordinary run, and the grant is exercised only where somebody
// runs the suite as root. Both are asserted, by asking the kernel which case this is rather than by
// pretending it is one of them.
GYRO_TEST(SessionControl, OnlyRootMayRunTheMachine)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(fixture.Greet(peer));
	GYRO_REQUIRE(peer.SendMessage(Manage{}));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());

	if (::geteuid() == 0)
	{
		GYRO_CHECK(peer.Answer() == Opcode::Managing);
		GYRO_CHECK(fixture.Control().HasMachine());

		return;
	}

	GYRO_REQUIRE(peer.Answer() == Opcode::Refused);

	const std::optional<Refused> refused = peer.Refusal();

	GYRO_REQUIRE(refused.has_value());
	GYRO_CHECK_EQ(refused->Code, std::uint32_t{ EACCES });
	GYRO_CHECK(!fixture.Control().HasMachine());
}

// Nobody is running the machine until somebody claims it, which is what tells the composition root it
// is still the party placing sessions.
GYRO_TEST(SessionControl, NobodyRunsTheMachineUntilItIsClaimed)
{
	Fixture fixture;
	Peer peer;

	GYRO_CHECK(!fixture.Control().HasMachine());
	GYRO_REQUIRE(fixture.Greet(peer));
	GYRO_CHECK(!fixture.Control().HasMachine());
}

// An assignment on a connection that has not claimed the machine is refused before the uid is
// consulted, so the two roles stay apart for the one uid that could hold either.
GYRO_TEST(SessionControl, AssigningWithoutTheMachineIsRefused)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(fixture.Greet(peer));
	GYRO_REQUIRE(peer.SendMessage(Assign{ .Uid = 1000, .Connector = {} }));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Refused);

	const std::optional<Refused> refused = peer.Refusal();

	GYRO_REQUIRE(refused.has_value());
	GYRO_CHECK_EQ(refused->Code, std::uint32_t{ EACCES });
	GYRO_CHECK(fixture.Requests().Requested.empty());
}

// The greeting settles the version before anything else is read, and a claim is not an exception to
// that — the version is what says how the next message is encoded.
GYRO_TEST(SessionControl, ClaimingTheMachineBeforeTheGreetingIsRefused)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(peer.ConnectTo(fixture.ControlPath()));
	GYRO_REQUIRE(peer.SendMessage(Manage{}));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Refused);

	const std::optional<Refused> refused = peer.Refusal();

	GYRO_REQUIRE(refused.has_value());
	GYRO_CHECK_EQ(refused->Code, std::uint32_t{ EPROTO });
}

// A claim carrying a descriptor is refused by the frame, which is the rule that only an offer ever
// carries one — and it is what stops a peer filling the file table of the process that must not die.
GYRO_TEST(SessionControl, AClaimCarryingADescriptorIsRefused)
{
	Fixture fixture;
	Peer peer;

	GYRO_REQUIRE(fixture.Greet(peer));

	const Fd listener = MakeListener(std::format("{}/wayland-0", fixture.RuntimeDirectory()));

	GYRO_REQUIRE(listener.IsValid());
	GYRO_REQUIRE(peer.SendMessage(Manage{}, listener.Borrow()));
	GYRO_REQUIRE(fixture.Control().Drain().has_value());
	GYRO_REQUIRE(peer.Answer() == Opcode::Refused);

	const std::optional<Refused> refused = peer.Refusal();

	GYRO_REQUIRE(refused.has_value());
	GYRO_CHECK_EQ(refused->Code, std::uint32_t{ EPROTO });
}

// Answering a request nobody is waiting for does nothing, which is the ordinary end of a login agent
// restarted between asking and being answered.
GYRO_TEST(SessionControl, SatisfyingARequestWithNoMachineIsSilent)
{
	Fixture fixture;

	fixture.Control().Satisfied(MachineRequest{ .Uid = 1000, .Connector = {} });

	GYRO_CHECK(!fixture.Control().HasMachine());
}
