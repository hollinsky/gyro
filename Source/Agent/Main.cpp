#include <poll.h>
#include <signal.h>
#include <spdlog/spdlog.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Fd.h"
#include "Session/Agent.h"
#include "Session/Child.h"
#include "Session/Listener.h"
#include "Session/Options.h"

// `gyro-session`: the session agent, and the whole of what gyro cannot do for itself.
//
// It binds the Wayland listener for the user it runs as, offers it to gyro over the control socket,
// holds that connection for the session's lifetime — its EOF is the session ending, which is what
// lets gyro learn one is over without subscribing to anything — and starts the session's first client
// with `WAYLAND_DISPLAY` already in its environment.
//
// **Under `--shell` it binds a second listener and that is what makes a session have a shell.** Trust
// belongs to the socket a client arrived on (decision 23), so the party that decides which process
// gets the run of a session is the party that can create a socket in that user's runtime directory —
// which gyro cannot (decision 22) and this can. The agent connects to it itself and hands the child
// the connected descriptor, so the path never enters an environment anything could inherit.
//
// **The pieces are in `Session` and only the sequencing is here**, which is Source/Main.cpp's
// arrangement: the parse is total and tested, each mechanism is tested against a real socket, and
// what is left is a wait and a handful of transitions.
//
// **This is not the login agent.** No PAM, no `setuid`, no greetd protocol: it runs as whoever
// started it and serves that one user. Docs/Architecture.md#the-login-agent is the privileged half
// that will `fork` one of these per session, and it is not written — which is why a development run
// starts this by hand from a shell that is already the user.

namespace
{
// SPEC: how long to wait before trying the control socket again, and the ceiling it doubles to. gyro
// not being up yet is the ordinary state of a machine during boot and of a development tree between
// restarts, so the first retry is quick enough that nobody sees it and the last is slow enough that
// an agent left running against a compositor that is never coming back costs nothing.
constexpr int FirstRetryMilliseconds = 100;
constexpr int LastRetryMilliseconds = 2000;

void PrintUsage()
{
	std::cout << "Usage: gyro-session [options] [--] [command [argument...]]\n"
			  << "\n"
			  << "Creates a Wayland listener for the user it runs as, offers it to gyro, and holds\n"
			  << "the connection open for the session's lifetime.\n"
			  << "\n"
			  << "Options:\n"
			  << "  --help              Show this help message\n"
			  << "  --control=PATH      Where gyro is listening for offers; the default is\n"
			  << "                      /run/gyro/control\n"
			  << "  --runtime-dir=PATH  Where to bind the listener; the default is XDG_RUNTIME_DIR\n"
			  << "  --display=NAME      What to call the display; the default is the first free\n"
			  << "                      wayland-N\n"
			  << "  --respawn           Start the command again whenever the session has no client,\n"
			  << "                      which covers both the client exiting and gyro restarting.\n"
			  << "                      Without it the agent exits when the command does, with the\n"
			  << "                      status the command exited with\n"
			  << "  --shell             The command is this session's shell. Binds a second listener\n"
			  << "                      beside the display, offers it to gyro as the socket that\n"
			  << "                      grants the run of the session, and starts the command on a\n"
			  << "                      connection to it. Without this the session has no shell and\n"
			  << "                      nothing in it can claim a chord or draw chrome\n"
			  << "\n";
}

// A descriptor the wanted signals arrive on, with those signals blocked so they arrive nowhere else.
//
// **A file rather than a handler, because the alternative is a race the loop cannot close.** A
// handler setting a flag between the check and the `poll` leaves the agent asleep holding a session
// whose client has already exited; blocking them and reading them as a descriptor puts them in the
// same wait as everything else.
[[nodiscard]] Fd OpenSignals()
{
	::sigset_t wanted;
	::sigemptyset(&wanted);
	::sigaddset(&wanted, SIGCHLD);
	::sigaddset(&wanted, SIGINT);
	::sigaddset(&wanted, SIGTERM);

	if (::sigprocmask(SIG_BLOCK, &wanted, nullptr) != 0)
	{
		return Fd{};
	}

	return Fd{ ::signalfd(-1, &wanted, SFD_CLOEXEC | SFD_NONBLOCK) };
}

// What the runtime directory is, given what the command line said. `XDG_RUNTIME_DIR` is where
// `pam_systemd` puts a user's own directory, and gyro checks that an offered listener is bound inside
// the one belonging to the uid the *kernel* reported — so an agent whose environment is missing it
// cannot produce an offer that would be accepted, and says so here rather than being refused three
// steps later.
[[nodiscard]] std::string RuntimeDirectory(const Session::AgentOptions& options)
{
	if (!options.RuntimeDirectory.empty())
	{
		return options.RuntimeDirectory;
	}

	const char* const named = std::getenv("XDG_RUNTIME_DIR");

	return named != nullptr ? std::string{ named } : std::string{};
}

// What the agent exits with, having run one command to completion. The command's own status, so that
// `gyro-session -- some-test` is a thing a script can read the answer of.
[[nodiscard]] int StatusFrom(int wait)
{
	return WIFEXITED(wait) ? WEXITSTATUS(wait) : 1;
}
} // namespace

int main(int argc, char** argv)
{
	std::vector<std::string_view> arguments;

	for (int index = 1; index < argc; ++index)
	{
		if (std::string_view{ argv[index] } == "--help")
		{
			PrintUsage();

			return 0;
		}

		arguments.emplace_back(argv[index]);
	}

	const Result<Session::AgentOptions> options = Session::ParseAgentOptions(arguments);

	if (!options)
	{
		std::cerr << std::format("{}\n", options.error());
		PrintUsage();

		return 1;
	}

	const std::string directory = RuntimeDirectory(*options);

	if (directory.empty())
	{
		std::cerr << "XDG_RUNTIME_DIR is not set and --runtime-dir was not given, so there is nowhere to bind a "
					 "listener\n";

		return 1;
	}

	// **Signals before the listener**, so that a Ctrl+C during the retry loop is read rather than
	// killing an agent that has already bound a socket and would leave the lock on it held.
	const Fd signals = OpenSignals();

	if (!signals.IsValid())
	{
		std::cerr << "the agent could not take over its own signals\n";

		return 1;
	}

	const Result<Session::WaylandListener> listener = Session::BindWaylandListener(directory, options->Display);

	if (!listener)
	{
		std::cerr << std::format("{}\n", listener.error());

		return 1;
	}

	spdlog::info("listening on {}", listener->Path);

	// **Bound before the first offer and never rebound.** gyro restarting loses every listener it held
	// and the agent offers the same two again — Session/Listener.h's argument for the display's socket
	// applies unchanged to this one, and a shell whose connection survived the gap is served out of this
	// queue when gyro adopts it.
	Session::ShellListener shell;

	if (options->Shell)
	{
		Result<Session::ShellListener> bound = Session::BindShellListener(directory, listener->Name);

		if (!bound)
		{
			std::cerr << std::format("{}\n", bound.error());

			return 1;
		}

		shell = std::move(*bound);

		spdlog::info("the session's shell reaches gyro on {}, and nothing else is told where that is", shell.Path);
	}

	Session::SessionAgent agent{ listener->Socket.Borrow(), shell.Socket.Borrow() };
	Session::Child client;

	// Whether a client should be started when there is a session and none running. **One flag covering
	// the first start, the client exiting and gyro restarting**, because all three are the same
	// condition — which is what `--respawn` is a policy about rather than a mechanism.
	bool starting = !options->Command.empty();

	Session::AgentState reported = Session::AgentState::Apart;
	int retry = FirstRetryMilliseconds;
	int status = 0;
	bool running = true;

	while (running)
	{
		if (agent.State() == Session::AgentState::Apart)
		{
			if (const Result<void> connected = agent.Connect(options->ControlPath); connected)
			{
				retry = FirstRetryMilliseconds;
			}
			else if (retry == FirstRetryMilliseconds)
			{
				// Once per gap rather than once per attempt: an agent waiting through a boot would
				// otherwise write the same line a dozen times before anything happened.
				spdlog::info("waiting for gyro on {}: {}", options->ControlPath, connected.error());
			}
		}

		if (agent.State() == Session::AgentState::Established && starting && !client.IsRunning())
		{
			// **Connected here rather than when the socket was bound**, and a fresh connection every
			// start: a respawned shell needs one of its own, and one made before gyro had adopted the
			// listener would be a connection sitting in a queue for however long the compositor took to
			// arrive. The descriptor goes no further than the child's environment — see
			// Session/Child.h — so it is closed here as soon as the fork has been done with it.
			Fd connection;

			if (options->Shell)
			{
				Result<Fd> opened = Session::ConnectTo(shell.Path);

				if (!opened)
				{
					std::cerr << std::format("{}\n", opened.error());

					status = 1;

					break;
				}

				connection = std::move(*opened);
			}

			if (const Result<void> started = client.Start(options->Command, listener->Name, connection.Borrow());
			    !started)
			{
				std::cerr << std::format("{}\n", started.error());

				status = 1;

				break;
			}

			starting = options->Respawn;
		}

		const bool apart = agent.State() == Session::AgentState::Apart;

		// The agent's descriptor is invalid while apart, and `poll` skips a negative one — so the set is
		// the same two entries throughout and the only thing that changes is how long to wait.
		std::array<::pollfd, 2> waiting{
			::pollfd{ .fd = signals.Get(), .events = POLLIN, .revents = 0 },
			::pollfd{ .fd = agent.Descriptor().Value, .events = POLLIN, .revents = 0 },
		};

		const int woken = ::poll(waiting.data(), waiting.size(), apart ? retry : -1);

		if (woken < 0 && errno != EINTR)
		{
			std::cerr << std::format("the agent's wait failed: {}\n", std::strerror(errno));

			return 1;
		}

		if (woken == 0 && apart)
		{
			retry = retry < LastRetryMilliseconds ? retry * 2 : LastRetryMilliseconds;
		}

		bool reaping = false;

		while (true)
		{
			::signalfd_siginfo raised{};

			if (::read(signals.Get(), &raised, sizeof raised) != static_cast<::ssize_t>(sizeof raised))
			{
				break;
			}

			if (raised.ssi_signo == SIGCHLD)
			{
				reaping = true;

				continue;
			}

			spdlog::info("stopping on signal {}", raised.ssi_signo);

			client.Stop();

			running = false;
		}

		// **Reaped after the loop rather than inside it**, because signals coalesce: two children
		// exiting between one wakeup and the next raise one `SIGCHLD`, so a reap per signal read would
		// leave the second unreaped until something else happened to wake the agent.
		if (reaping)
		{
			if (const std::optional<int> ended = client.Reap(); ended)
			{
				spdlog::info("the session's client {}", Session::DescribeExit(*ended));

				if (!options->Respawn)
				{
					status = StatusFrom(*ended);
					running = false;
				}
			}
		}

		if (const Result<void> drained = agent.Drain(); !drained)
		{
			spdlog::warn("the control connection failed: {}", drained.error());
		}

		if (agent.Rejection() != 0)
		{
			std::cerr << std::format("gyro refused the handover: {}\n", agent.Rejected());

			status = 1;

			break;
		}

		if (agent.State() != reported)
		{
			if (agent.State() == Session::AgentState::Established)
			{
				spdlog::info("session {} established", static_cast<std::uint32_t>(agent.Session()));
			}
			else if (reported == Session::AgentState::Established)
			{
				spdlog::warn("gyro went away; the listener will be offered again");
			}

			reported = agent.State();
		}
	}

	return status;
}
