#include "Shell/Launch.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <format>

extern char** environ;

namespace
{
constexpr std::string_view DisplayVariable = "WAYLAND_DISPLAY=";
constexpr std::string_view SocketVariable = "WAYLAND_SOCKET=";

[[nodiscard]] bool IsSpace(char character) noexcept
{
	return character == ' ' || character == '\t';
}
} // namespace

void Launcher::Adopt(std::string_view display)
{
	m_Environment.clear();

	for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry)
	{
		const std::string_view variable{ *entry };

		// **Both are dropped, and `WAYLAND_SOCKET` is the one that would be a bug rather than a wrong
		// name.** Wire/Connection.h unsets it the moment it takes the descriptor, so it is normally gone
		// already; one left behind by a shell that connected some other way names a descriptor in *this*
		// process, which by the time a child ran might have been reused for a file.
		if (!variable.starts_with(DisplayVariable) && !variable.starts_with(SocketVariable))
		{
			m_Environment.emplace_back(variable);
		}
	}

	if (!display.empty())
	{
		m_Environment.emplace_back(std::format("{}{}", DisplayVariable, display));
	}
}

std::vector<std::string> Launcher::Words(std::string_view query)
{
	// **Whitespace and nothing else: no quoting, no variables, no globbing.** The alternative was
	// handing the line to `/bin/sh -c`, which would give all three for free and would also make
	// everything else a shell does reachable from a launcher — a stray `;` or `&` in a URL somebody
	// pasted running as two commands. A run bar is a way to start a program, and the day it needs to
	// start one with a space in its argument is the day it gets quoting written for it deliberately.
	std::vector<std::string> words;
	std::size_t index = 0;

	while (index < query.size())
	{
		while (index < query.size() && IsSpace(query[index]))
		{
			++index;
		}

		const std::size_t begin = index;

		while (index < query.size() && !IsSpace(query[index]))
		{
			++index;
		}

		if (index > begin)
		{
			words.emplace_back(query.substr(begin, index - begin));
		}
	}

	return words;
}

Result<void> Launcher::Run(std::string_view query)
{
	const std::vector<std::string> words = Words(query);

	if (words.empty())
	{
		return Failure(EINVAL, "there is nothing typed to run");
	}

	// The two `char*` arrays, built here so that the forked child does nothing but `execvpe`. The
	// `const_cast` is the API's rather than a choice: `execvpe` does not write through either.
	std::vector<char*> arguments;
	arguments.reserve(words.size() + 1);

	for (const std::string& word : words)
	{
		arguments.push_back(const_cast<char*>(word.c_str()));
	}

	arguments.push_back(nullptr);

	std::vector<char*> variables;
	variables.reserve(m_Environment.size() + 1);

	for (const std::string& variable : m_Environment)
	{
		variables.push_back(const_cast<char*>(variable.c_str()));
	}

	variables.push_back(nullptr);

	// **Forked twice, so that nothing started here is ever this process's child.** A launcher's
	// children outlive the launch by hours and the shell has one thread and one blocking `poll` — it is
	// never anywhere it could reap them, so a single fork would leave a zombie per program until the
	// session ended. The middle process exits immediately, `init` adopts the grandchild, and the shell
	// waits for a child it knows is already on its way out.
	const ::pid_t forked = ::fork();

	if (forked < 0)
	{
		return FailFromErrno("forking the program to run");
	}

	if (forked == 0)
	{
		const ::pid_t inner = ::fork();

		if (inner == 0)
		{
			// **The signal mask goes back to the default.** This shell blocks nothing today, which is
			// exactly the kind of premise that stops being true the first time somebody wants a
			// `signalfd` here — and the failure it would cause is a program a person cannot interrupt,
			// discovered much later and nowhere near this line.
			::sigset_t empty;
			::sigemptyset(&empty);
			::sigprocmask(SIG_SETMASK, &empty, nullptr);

			::execvpe(arguments[0], arguments.data(), variables.data());

			// `_exit` rather than `exit`, which would run this process's atexit handlers in a copy of it.
			// 127 is what a shell reports for a command it could not run, and is the number a person
			// reading a log has already seen elsewhere.
			::_exit(127);
		}

		::_exit(inner < 0 ? 127 : 0);
	}

	// Blocking, and it is not a stall: the process being waited for does nothing but fork and exit.
	int status = 0;

	while (::waitpid(forked, &status, 0) < 0 && errno == EINTR)
	{
	}

	return {};
}
