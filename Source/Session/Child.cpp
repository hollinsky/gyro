#include "Session/Child.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <format>
#include <vector>

extern char** environ;

namespace Session
{
namespace
{
constexpr std::string_view DisplayVariable = "WAYLAND_DISPLAY=";

// The child's environment: this process's, with any `WAYLAND_DISPLAY` replaced.
//
// **Built before the fork and never after it.** Between `fork` and `exec` only async-signal-safe
// calls are permitted, and `setenv` is not one — it allocates, and in a process that forked from a
// thread holding the allocator's lock it deadlocks a child that no longer has the thread that would
// release it. This agent is single-threaded today, which is precisely the kind of premise that stops
// being true quietly; composing the environment in the parent means it never has to be checked.
[[nodiscard]] std::vector<std::string> ComposeEnvironment(std::string_view display)
{
	std::vector<std::string> composed;

	for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry)
	{
		const std::string_view variable{ *entry };

		if (!variable.starts_with(DisplayVariable))
		{
			composed.emplace_back(variable);
		}
	}

	composed.emplace_back(std::format("{}{}", DisplayVariable, display));

	return composed;
}
} // namespace

Child::~Child()
{
	// Deliberately no kill and no wait. A `Child` going out of scope happens on the way out of `main`,
	// where the process is exiting anyway and the child is about to be reparented to init — and a
	// destructor that waited would hang the agent on a terminal somebody left open.
}

Result<void> Child::Start(std::span<const std::string> command, std::string_view display)
{
	if (IsRunning())
	{
		return Failure(EBUSY, "a client is already running");
	}

	if (command.empty())
	{
		return Failure(EINVAL, "there is no command to start");
	}

	const std::vector<std::string> environment = ComposeEnvironment(display);

	// The two `char*` arrays, built here so the child does nothing but `execvpe`. `const_cast` is the
	// API's rather than a choice: `execvpe` does not write through either of them.
	std::vector<char*> arguments;
	arguments.reserve(command.size() + 1);

	for (const std::string& word : command)
	{
		arguments.push_back(const_cast<char*>(word.c_str()));
	}

	arguments.push_back(nullptr);

	std::vector<char*> variables;
	variables.reserve(environment.size() + 1);

	for (const std::string& variable : environment)
	{
		variables.push_back(const_cast<char*>(variable.c_str()));
	}

	variables.push_back(nullptr);

	const ::pid_t forked = ::fork();

	if (forked < 0)
	{
		return FailFromErrno("forking the session's client");
	}

	if (forked == 0)
	{
		// **The signal mask goes back to the default, and it is not optional.** The agent blocks
		// `SIGCHLD`, `SIGINT` and `SIGTERM` so it can read them off a `signalfd`, and a mask is
		// inherited across `exec` — so without this every program the session starts would be one that
		// cannot be interrupted, which a person discovers by pressing Ctrl+C in the terminal and having
		// nothing happen.
		::sigset_t empty;
		::sigemptyset(&empty);
		::sigprocmask(SIG_SETMASK, &empty, nullptr);

		::execvpe(arguments[0], arguments.data(), variables.data());

		// `_exit` rather than `exit`, which would run this process's atexit handlers in a copy of it.
		// 127 is what a shell reports for a command it could not run, and is the number a person
		// reading the agent's log has already seen elsewhere.
		::_exit(127);
	}

	m_Pid = forked;

	return {};
}

std::optional<int> Child::Reap() noexcept
{
	if (!IsRunning())
	{
		return std::nullopt;
	}

	int status = 0;
	const ::pid_t reaped = ::waitpid(m_Pid, &status, WNOHANG);

	if (reaped != m_Pid)
	{
		// Zero is *still running*, which is the ordinary answer on a `SIGCHLD` raised by somebody else's
		// child. A failure is `ECHILD` — already reaped, which cannot happen while this is the only
		// party waiting on it, and is answered the same way regardless: the pid is not ours any more.
		if (reaped < 0)
		{
			m_Pid = -1;
		}

		return std::nullopt;
	}

	m_Pid = -1;

	return status;
}

void Child::Stop() noexcept
{
	if (IsRunning())
	{
		::kill(m_Pid, SIGTERM);
	}
}

std::string DescribeExit(int status)
{
	if (WIFEXITED(status))
	{
		return std::format("exited {}", WEXITSTATUS(status));
	}

	if (WIFSIGNALED(status))
	{
		return std::format("killed by signal {}", WTERMSIG(status));
	}

	return "ended";
}
} // namespace Session
