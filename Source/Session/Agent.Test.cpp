#include "Session/Agent.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <string>

#include "Core/Fd.h"
#include "Session/Control.h"
#include "Session/Listener.h"
#include "Testing/Test.h"

// Both ends of the handover, in one process and over real sockets.
//
// **This is the test the two halves were written for.** Session/Control.Test.cpp plays an agent by
// hand and Session/Listener.Test.cpp binds what one would offer; neither can catch the two ends
// agreeing on a message and disagreeing about the order it arrives in. Here the real
// `SessionAgent` speaks to the real `SessionControl`, and the only thing faked is the wait — a test
// pumps both by calling `Drain`, where the loop would `poll`.
//
// **Same-uid, so `SO_PEERCRED` still rejects nothing**, exactly as Session/Control.Test.cpp records.
// What this adds is the sequencing either side of that check.

namespace
{
using namespace Session;

class TemporaryDirectory
{
public:
	TemporaryDirectory()
	{
		std::string pattern = (std::filesystem::temp_directory_path() / "gyro-agent-XXXXXX").string();

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

	[[nodiscard]] const std::string& Path() const noexcept { return m_Path; }

	[[nodiscard]] std::string In(std::string_view name) const { return std::format("{}/{}", m_Path, name); }

private:
	std::string m_Path;
};

// A control socket, a runtime directory for this user under it, and the ability to take gyro away
// and bring it back — which is the case the agent exists to survive.
class Fixture
{
public:
	Fixture()
	{
		std::error_code ignored;
		std::filesystem::create_directories(RuntimeDirectory(), ignored);

		Start();
	}

	void Start()
	{
		Result<std::unique_ptr<SessionControl>> opened = SessionControl::Open(ControlPath(), m_Directory.In("run"));

		m_Control = opened ? std::move(*opened) : nullptr;
	}

	// gyro going away: the socket is unbound and every connection to it ends.
	void Stop() { m_Control.reset(); }

	[[nodiscard]] SessionControl* Control() const noexcept { return m_Control.get(); }

	[[nodiscard]] std::string ControlPath() const { return m_Directory.In("control"); }

	[[nodiscard]] std::string RuntimeDirectory() const
	{
		return std::format("{}/{}", m_Directory.In("run"), ::geteuid());
	}

private:
	TemporaryDirectory m_Directory;
	std::unique_ptr<SessionControl> m_Control;
};

// Step both ends until the agent stops moving. Four exchanges is the whole handshake with room to
// spare; a state that has not settled by then is one the test is about to fail on.
[[nodiscard]] bool Settle(Fixture& fixture, SessionAgent& agent)
{
	for (int step = 0; step < 4; ++step)
	{
		if (fixture.Control() != nullptr && !fixture.Control()->Drain())
		{
			return false;
		}

		if (!agent.Drain())
		{
			return false;
		}
	}

	return true;
}

// Whether a client could connect to the display the agent bound.
[[nodiscard]] bool Reachable(const std::string& path)
{
	const Fd socket{ ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0) };

	if (!socket.IsValid() || path.size() >= sizeof(::sockaddr_un::sun_path))
	{
		return false;
	}

	::sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, path.data(), path.size());

	return ::connect(socket.Get(), reinterpret_cast<const ::sockaddr*>(&address), sizeof address) == 0;
}
} // namespace

GYRO_TEST(SessionAgent, OffersItsListenerAndEstablishesASession)
{
	Fixture fixture;
	GYRO_REQUIRE(fixture.Control() != nullptr);

	const Result<WaylandListener> listener = BindWaylandListener(fixture.RuntimeDirectory());
	GYRO_REQUIRE(listener.has_value());

	SessionAgent agent{ listener->Socket.Borrow() };
	GYRO_CHECK(agent.State() == AgentState::Apart);

	GYRO_REQUIRE(agent.Connect(fixture.ControlPath()).has_value());
	GYRO_CHECK(agent.State() == AgentState::Greeting);

	GYRO_REQUIRE(Settle(fixture, agent));
	GYRO_CHECK(agent.State() == AgentState::Established);
	GYRO_CHECK(agent.Session() != SessionId::None);
	GYRO_CHECK(agent.Rejection() == 0);

	// And what gyro is holding is the socket a client will connect to, rather than something that
	// merely passed the inspection.
	std::optional<AcceptedOffer> offer = fixture.Control()->TakeOffer();
	GYRO_REQUIRE(offer.has_value());
	GYRO_CHECK(offer->Id == agent.Session());
	GYRO_CHECK(offer->Uid == ::geteuid());
	GYRO_CHECK(offer->Listener.IsValid());
	GYRO_CHECK(Reachable(listener->Path));
}

// An agent starting a shell offers both sockets in one message, and gyro holds a session with both.
// **This is the end-to-end shape of decision 189**: there is no second handshake, no ordering, and no
// state in which a session exists without the shell socket its agent has already been given.
GYRO_TEST(SessionAgent, OffersTheShellListenerBesideTheDisplayInOneMessage)
{
	Fixture fixture;
	GYRO_REQUIRE(fixture.Control() != nullptr);

	const Result<WaylandListener> listener = BindWaylandListener(fixture.RuntimeDirectory());
	GYRO_REQUIRE(listener.has_value());

	const Result<ShellListener> shell = BindShellListener(fixture.RuntimeDirectory(), listener->Name);
	GYRO_REQUIRE(shell.has_value());

	SessionAgent agent{ listener->Socket.Borrow(), shell->Socket.Borrow() };

	GYRO_REQUIRE(agent.Connect(fixture.ControlPath()).has_value());
	GYRO_REQUIRE(Settle(fixture, agent));
	GYRO_CHECK(agent.State() == AgentState::Established);

	std::optional<AcceptedOffer> offer = fixture.Control()->TakeOffer();

	GYRO_REQUIRE(offer.has_value());
	GYRO_CHECK(offer->Listener.IsValid());
	GYRO_CHECK(offer->Shell.IsValid());
	GYRO_CHECK(Reachable(shell->Path));
}

// gyro restarting loses every listener it held, and what the agent offers on the next connection is
// the same pair — so a shell that was connected across the gap is served out of a queue that was
// never torn down.
GYRO_TEST(SessionAgent, ReoffersBothListenersAfterGyroRestarts)
{
	Fixture fixture;
	GYRO_REQUIRE(fixture.Control() != nullptr);

	const Result<WaylandListener> listener = BindWaylandListener(fixture.RuntimeDirectory());
	GYRO_REQUIRE(listener.has_value());

	const Result<ShellListener> shell = BindShellListener(fixture.RuntimeDirectory(), listener->Name);
	GYRO_REQUIRE(shell.has_value());

	SessionAgent agent{ listener->Socket.Borrow(), shell->Socket.Borrow() };

	GYRO_REQUIRE(agent.Connect(fixture.ControlPath()).has_value());
	GYRO_REQUIRE(Settle(fixture, agent));

	fixture.Stop();

	GYRO_REQUIRE(agent.Drain().has_value());
	GYRO_REQUIRE(agent.State() == AgentState::Apart);

	fixture.Start();
	GYRO_REQUIRE(fixture.Control() != nullptr);

	GYRO_REQUIRE(agent.Connect(fixture.ControlPath()).has_value());
	GYRO_REQUIRE(Settle(fixture, agent));
	GYRO_CHECK(agent.State() == AgentState::Established);

	std::optional<AcceptedOffer> offer = fixture.Control()->TakeOffer();

	GYRO_REQUIRE(offer.has_value());
	GYRO_CHECK(offer->Shell.IsValid());
}

GYRO_TEST(SessionAgent, ConnectingBeforeGyroIsUpFails)
{
	const TemporaryDirectory directory;

	const Result<WaylandListener> listener = BindWaylandListener(directory.Path());
	GYRO_REQUIRE(listener.has_value());

	SessionAgent agent{ listener->Socket.Borrow() };

	// The failure a retry loop is written against, and it is reported as `connect` gave it rather than
	// flattened: a person whose session did not start needs to know whether the socket was missing or
	// whether nobody was behind it.
	const Result<void> apart = agent.Connect(directory.In("control"));
	GYRO_REQUIRE(!apart.has_value());
	GYRO_CHECK(apart.error().Code() == ENOENT);
	GYRO_CHECK(agent.State() == AgentState::Apart);
}

GYRO_TEST(SessionAgent, ReoffersTheSameListenerAfterGyroRestarts)
{
	Fixture fixture;
	GYRO_REQUIRE(fixture.Control() != nullptr);

	const Result<WaylandListener> listener = BindWaylandListener(fixture.RuntimeDirectory());
	GYRO_REQUIRE(listener.has_value());

	SessionAgent agent{ listener->Socket.Borrow() };
	GYRO_REQUIRE(agent.Connect(fixture.ControlPath()).has_value());
	GYRO_REQUIRE(Settle(fixture, agent));
	GYRO_REQUIRE(agent.State() == AgentState::Established);

	const SessionId first = agent.Session();

	fixture.Stop();

	// The connection ending is the session ending, and the agent is back where it started — with the
	// listener still bound, which is the whole point: the display name a person exported is still the
	// one clients connect to.
	GYRO_REQUIRE(agent.Drain().has_value());
	GYRO_CHECK(agent.State() == AgentState::Apart);
	GYRO_CHECK(agent.Session() == SessionId::None);
	GYRO_CHECK(agent.Rejection() == 0);
	GYRO_CHECK(Reachable(listener->Path));

	fixture.Start();
	GYRO_REQUIRE(fixture.Control() != nullptr);

	GYRO_REQUIRE(agent.Connect(fixture.ControlPath()).has_value());
	GYRO_REQUIRE(Settle(fixture, agent));
	GYRO_CHECK(agent.State() == AgentState::Established);

	// **A session id belongs to the gyro that minted it and does not survive one.** A new compositor
	// numbers from the start, so the second session reuses the first's number while being a different
	// session — which is worth pinning here rather than discovering from a log line that reads as a
	// session having outlived the process that owned it.
	GYRO_CHECK(agent.Session() == first);

	const std::optional<AcceptedOffer> offer = fixture.Control()->TakeOffer();
	GYRO_REQUIRE(offer.has_value());
	GYRO_CHECK(Reachable(listener->Path));
}

GYRO_TEST(SessionAgent, ARefusalIsTerminal)
{
	Fixture fixture;
	GYRO_REQUIRE(fixture.Control() != nullptr);

	const Result<WaylandListener> first = BindWaylandListener(fixture.RuntimeDirectory());
	const Result<WaylandListener> second = BindWaylandListener(fixture.RuntimeDirectory());
	GYRO_REQUIRE(first.has_value());
	GYRO_REQUIRE(second.has_value());

	SessionAgent established{ first->Socket.Borrow() };
	GYRO_REQUIRE(established.Connect(fixture.ControlPath()).has_value());
	GYRO_REQUIRE(Settle(fixture, established));
	GYRO_REQUIRE(established.State() == AgentState::Established);

	// A second agent for a user who already has a session. gyro refuses, and the agent records it
	// rather than reconnecting — the offer is not going to be judged differently the next time.
	SessionAgent refused{ second->Socket.Borrow() };
	GYRO_REQUIRE(refused.Connect(fixture.ControlPath()).has_value());
	GYRO_REQUIRE(Settle(fixture, refused));

	GYRO_CHECK(refused.State() == AgentState::Apart);
	GYRO_CHECK(refused.Rejection() == EBUSY);
	GYRO_CHECK(!refused.Rejected().empty());

	// And the session that was already standing is untouched by the one that was turned away.
	GYRO_CHECK(established.State() == AgentState::Established);
}
