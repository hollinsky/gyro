#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.h"
#include "Session/Handover.h"

// What `gyro-session` was asked to do, parsed from the command line and nothing else.
//
// Compositor/Options.h's shape one binary over, and for one of its two reasons: the parse is pure, so
// the part of startup a person types into can be tested without a compositor to connect to, a
// runtime directory to bind in, or a program to run. The other reason — a boot service must not take
// a silent default — applies here too, since the login agent will `exec` this with a command line it
// composed and nobody will be watching.

namespace Session
{
struct AgentOptions
{
	// Where gyro is listening for offers.
	std::string ControlPath{ DefaultControlPath };

	// Where the Wayland listener is bound. Empty means `XDG_RUNTIME_DIR`, resolved by the caller
	// rather than here, because reading the environment is not parsing a command line — and a parse
	// that reached for it could not be tested on a machine where it is not set.
	std::string RuntimeDirectory;

	// The display's name. Empty takes the first free `wayland-N`.
	std::string Display;

	// Start the command again whenever there is a session and nothing running.
	//
	// **It is one rule covering three events**, which is why it is a flag rather than a policy: the
	// first start, the client exiting, and gyro restarting are all *a session exists and no child does*.
	// Without it the agent runs the command once and exits when it does, which is what makes
	// `gyro-session -- foot` mean what a person expects it to.
	bool Respawn = false;

	// What to start once the session exists, argv[0] first. Empty is an agent that offers a listener
	// and starts nothing — which is the login agent's case, where the shell is started by the session
	// manager rather than by this.
	std::vector<std::string> Command;
};

// Total: every argument is accepted or named. The first argument that is not a recognised option
// begins the command, as does `--`, so an option-looking word can still be passed to the program.
[[nodiscard]] Result<AgentOptions> ParseAgentOptions(std::span<const std::string_view> arguments);
} // namespace Session
