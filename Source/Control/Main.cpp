#include <poll.h>
#include <signal.h>
#include <spdlog/spdlog.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Fd.h"
#include "Core/Signal.h"
#include "Session/Handover.h"
#include "Session/Machine.h"

// `gyro-control`: the machine peer by hand, and the stand-in for a login agent that is not written.
//
// **It is the sequence the login agent will run with the authentication removed**, which is exactly the
// argument `Deploy/gyro-autologin@.service` makes one layer up: the shape being exercised is the shape
// that stays, and what a greeter adds later is the missing half rather than a different arrangement.
// Docs/Architecture.md#the-login-agent has the whole of it — a PAM conversation over a greetd socket,
// `setuid`, and a session agent forked per person — and the part that is here is the part that talks to
// gyro: claim the machine, say who goes on which screen, and hold the claim open.
//
// **Holding it open is not an implementation detail.** The machine role belongs to a connection, so a
// tool that claimed, assigned and exited would hand placement straight back to gyro's own stand-in
// rule — and a request for somebody who has not logged in yet would go with it. So this waits, which is
// what the login agent does for the life of the machine anyway.
//
// **It is not installed.** A binary that assigns screens is one a login agent should be doing, and
// shipping a second way to do it would make the deployment ambiguous about which is in charge. It is
// built, it is for a person at a terminal, and it goes away when there is a login agent to replace it.

namespace
{
// The same schedule `gyro-session` retries on and for the same reason: gyro not being up yet is the
// ordinary state of a machine during boot, so the first retry is quick enough that nobody sees it and
// the last is slow enough that a peer left against a compositor that is never coming back costs
// nothing.
constexpr int FirstRetryMilliseconds = 100;
constexpr int LastRetryMilliseconds = 2000;

void PrintUsage()
{
	std::cout << "Usage: gyro-control [options] UID[:CONNECTOR] [UID[:CONNECTOR]...]\n"
			  << "\n"
			  << "Claims the machine on gyro's control socket and says which user's session belongs\n"
			  << "on which screen. Holds the claim until stopped, because the role belongs to the\n"
			  << "connection: exiting gives placement back to gyro's own stand-in rule.\n"
			  << "\n"
			  << "Options:\n"
			  << "  --help              Show this help message\n"
			  << "  --control=PATH      Where gyro is listening for offers; the default is\n"
			  << "                      /run/gyro/control\n"
			  << "\n"
			  << "A bare UID asks for that user's session on every output. UID:CONNECTOR asks for it\n"
			  << "on one, named as the kernel names it: eDP-1, DP-7, HDMI-A-1.\n"
			  << "\n"
			  << "Only root may claim the machine. Assignment is what locking is built out of, so a\n"
			  << "uid that could reach it could put its own session onto a locked screen.\n"
			  << "\n"
			  << "  sudo gyro-control 1000\n"
			  << "  sudo gyro-control 1000:eDP-1 1001:DP-7\n"
			  << "\n";
}

// One `UID[:CONNECTOR]` argument, or nothing where it is not one.
[[nodiscard]] std::optional<Session::MachineRequest> ParseRequest(std::string_view argument)
{
	const std::size_t colon = argument.find(':');
	const std::string_view user = argument.substr(0, colon);
	const std::string_view connector =
		colon == std::string_view::npos ? std::string_view{} : argument.substr(colon + 1);

	if (user.empty() || user.find_first_not_of("0123456789") != std::string_view::npos)
	{
		return std::nullopt;
	}

	const std::optional<Session::ConnectorName> named = Session::ConnectorName::From(connector);

	if (!named)
	{
		return std::nullopt;
	}

	return Session::MachineRequest{ .Uid = static_cast<std::uint32_t>(std::stoul(std::string{ user })),
		                            .Connector = *named };
}

// The descriptor the wanted signals arrive on, with those signals blocked so they arrive nowhere else.
// `Source/Agent/Main.cpp`'s arrangement and its reason: a handler setting a flag between the check and
// the `poll` leaves this asleep holding a claim nobody meant it to keep.
[[nodiscard]] Fd OpenSignals()
{
	::sigset_t wanted;
	::sigemptyset(&wanted);
	::sigaddset(&wanted, SIGINT);
	::sigaddset(&wanted, SIGTERM);

	if (::sigprocmask(SIG_BLOCK, &wanted, nullptr) != 0)
	{
		return Fd{};
	}

	return Fd{ ::signalfd(-1, &wanted, SFD_CLOEXEC | SFD_NONBLOCK) };
}

// What gyro said had landed, so that a person watching can see the screen move and the answer arrive
// as one event rather than trusting the log.
struct Landings
{
	std::size_t Count = 0;

	void Observe(Session::MachineRequest request)
	{
		++Count;

		spdlog::info(
			"gyro says uid {} is on {}",
			request.Uid,
			request.Connector.IsEveryOutput() ? "every output" : request.Connector.Text()
		);
	}
};
} // namespace

int main(int argc, char** argv)
{
	std::string control{ Session::DefaultControlPath };
	std::vector<Session::MachineRequest> requests;

	for (int index = 1; index < argc; ++index)
	{
		const std::string_view argument = argv[index];

		if (argument == "--help" || argument == "-h")
		{
			PrintUsage();

			return 0;
		}

		if (argument.starts_with("--control="))
		{
			control = argument.substr(std::string_view{ "--control=" }.size());

			continue;
		}

		const std::optional<Session::MachineRequest> request = ParseRequest(argument);

		if (!request)
		{
			std::cerr << std::format("{} is not a uid or a uid and a connector\n", argument);
			PrintUsage();

			return 1;
		}

		requests.push_back(*request);
	}

	if (requests.empty())
	{
		std::cerr << "nothing to assign\n";
		PrintUsage();

		return 1;
	}

	// Said before the connection rather than after the first refusal, because the refusal is `EACCES` on
	// a claim and a person reading that has to know what the claim was for.
	if (::geteuid() != 0)
	{
		spdlog::warn("gyro grants the machine to root only, so this will be refused");
	}

	const Fd signals = OpenSignals();

	if (!signals.IsValid())
	{
		std::cerr << "could not take over this process's signals\n";

		return 1;
	}

	Session::MachinePeer peer;
	Landings landings;
	Connection<Session::MachineRequest> link;
	link.ConnectTo<&Landings::Observe>(peer.Landed, landings);

	// Which requests have been sent on the connection now open. **Sent again on every connection**, for
	// `Session/Listener.h`'s reason one role over: gyro restarting loses the claim and everything behind
	// it, so a peer that stated its requests once would hold a machine that had forgotten them.
	bool stated = false;
	int retry = FirstRetryMilliseconds;
	bool running = true;

	while (running)
	{
		if (peer.State() == Session::MachineState::Apart)
		{
			stated = false;

			if (peer.Rejection() != 0)
			{
				std::cerr << std::format("gyro refused the machine: {}\n", peer.Rejected());

				return 1;
			}

			if (const Result<void> connected = peer.Connect(control); connected)
			{
				retry = FirstRetryMilliseconds;
			}
			else if (retry == FirstRetryMilliseconds)
			{
				// Once per gap rather than once per attempt, so that waiting through a boot does not
				// write the same line a dozen times before anything happens.
				spdlog::info("waiting for gyro on {}: {}", control, connected.error());
			}
		}

		if (peer.State() == Session::MachineState::Running && !stated)
		{
			for (const Session::MachineRequest& request : requests)
			{
				if (const Result<void> asked = peer.Request(request); !asked)
				{
					std::cerr << std::format("{}\n", asked.error());

					return 1;
				}

				spdlog::info(
					"asked gyro for uid {} on {}",
					request.Uid,
					request.Connector.IsEveryOutput() ? "every output" : request.Connector.Text()
				);
			}

			stated = true;
		}

		const bool apart = peer.State() == Session::MachineState::Apart;

		// The peer's descriptor is invalid while apart and `poll` skips a negative one, so the set is the
		// same two entries throughout and only the timeout changes.
		std::array<::pollfd, 2> waiting{
			::pollfd{ .fd = signals.Get(), .events = POLLIN, .revents = 0 },
			::pollfd{ .fd = peer.Descriptor().Value, .events = POLLIN, .revents = 0 },
		};

		const int woken = ::poll(waiting.data(), waiting.size(), apart ? retry : -1);

		if (woken < 0 && errno != EINTR)
		{
			std::cerr << std::format("the wait failed: {}\n", std::strerror(errno));

			return 1;
		}

		if (woken == 0 && apart)
		{
			retry = retry < LastRetryMilliseconds ? retry * 2 : LastRetryMilliseconds;
		}

		while (true)
		{
			::signalfd_siginfo raised{};

			if (::read(signals.Get(), &raised, sizeof raised) != static_cast<::ssize_t>(sizeof raised))
			{
				break;
			}

			spdlog::info("giving the machine back on signal {}", raised.ssi_signo);

			running = false;
		}

		if (!running)
		{
			break;
		}

		if (const Result<void> drained = peer.Drain(); !drained)
		{
			spdlog::warn("the control connection failed: {}", drained.error());
		}
	}

	return 0;
}
