#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.h"

// What the run bar does with a line somebody typed: turns it into a program and starts it.
//
// **It is the shell that forks and not gyro, which is Session/Child.h's argument seen from one step
// further out.** gyro runs as its own uid holding DRM master, so anything it spawned would inherit
// both; the agent forks the shell because the agent is already the person. The shell is that person
// too, and it is the process a launcher's children should hang off — not the compositor, which must
// still be drawing after the browser somebody started has crashed.
//
// **The child's display is decided here rather than inherited, and that is the whole reason this is a
// class.** A shell reaches gyro over a socket that grants it the run of the session
// (Protocol/Tier.h), and where it reached that socket by name — a development run, where
// `WAYLAND_DISPLAY` names `gyro-system-N` — passing the environment straight through would hand a
// browser everything the shell is trusted with. So children are told a display this object was given
// and never the one this process connected on.
class Launcher
{
public:
	// `display` is what `WAYLAND_DISPLAY` will say for everything started from here: the applications'
	// socket, which is what the agent puts in the shell's own environment before it forks it. Empty
	// starts children with no display at all — the honest answer where nothing knows one, and a
	// program that fails to connect says so, where a program handed the System socket would not.
	void Adopt(std::string_view display);

	// Split `query` on whitespace and start it, searching `PATH`. Returns once the program is running
	// or once it is known that the fork failed.
	//
	// **What it cannot report is a program that does not exist**, because the failing `execvpe` happens
	// in a process this one has already stopped waiting for. That is the price of the double fork
	// below and it is the right way round: a launcher that reported a typo but left a zombie per launch
	// would be a shell that leaks for as long as a session lasts.
	[[nodiscard]] Result<void> Run(std::string_view query);

	// What `Run` would start, for the test that would rather not fork. Whitespace-separated words, so
	// nothing here quotes and nothing here globs — see the header comment in Launch.cpp.
	[[nodiscard]] static std::vector<std::string> Words(std::string_view query);

private:
	// Composed once rather than per launch, and copied for the same reason Session/Child.h composes
	// its own before the fork: between `fork` and `exec` only async-signal-safe calls are allowed, and
	// building an environment is not one of them.
	std::vector<std::string> m_Environment;
};
