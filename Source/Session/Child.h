#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "Core/Result.h"

// The session's first client, started by the party that is allowed to start it.
//
// **This is the second job that keeps an agent alive**, and Docs/Architecture.md#the-login-agent
// names it before there is a shell to want it: Xwayland must run as the user and gyro cannot
// `setuid`, so the fork belongs to the agent. A terminal started here is that mechanism with a
// simpler argument in front of it — until a shell exists there is no launcher, and a terminal is what
// a person launches things from.
//
// **gyro must not be the one forking, and the reason is not tidiness.** gyro runs as its own uid
// holding DRM master; a process it spawned would inherit both, and the escape hatch
// (Input/Chord.h) would have become a way to start programs as the compositor's user. The agent is
// already the user, which is the whole of what makes it the right party.
//
// **`WAYLAND_DISPLAY` is in the environment before the child exists**, which the handover section
// calls out by name because getting it wrong presents as "sometimes applications cannot find the
// display". Here that ordering is structural rather than remembered: the variable is composed into
// the environment this class builds, so there is no window in which a child could be running without
// it.

namespace Session
{
// A forked process, from `fork` to the status it exited with.
class Child
{
public:
	Child() = default;

	~Child();

	// Neither copied nor moved: a copy would be two owners of one pid, and the second `waitpid` lands
	// on whatever the number was recycled for.
	Child(const Child&) = delete;
	Child& operator=(const Child&) = delete;
	Child(Child&&) = delete;
	Child& operator=(Child&&) = delete;

	// Start `command` with `WAYLAND_DISPLAY` set to `display`, inheriting everything else.
	//
	// Searches `PATH`, because what a person types on the agent's command line is a program name and
	// not a path. Refused where a child is already running.
	[[nodiscard]] Result<void> Start(std::span<const std::string> command, std::string_view display);

	[[nodiscard]] bool IsRunning() const noexcept { return m_Pid > 0; }

	// Collect the child if it has exited, without waiting. Answers its wait status the once, and
	// nothing on every other call.
	//
	// **It reaps only this child**, by pid rather than by `-1`: an agent that reaped indiscriminately
	// would swallow the status of anything a future version of it also forks, and the bug that
	// produces is a session that never notices its shell died.
	[[nodiscard]] std::optional<int> Reap() noexcept;

	// Ask it to exit, and do not wait for it. `SIGTERM` rather than `SIGKILL` because a terminal with
	// a shell in it has children of its own, and the signal that lets it take them with it is the one
	// a person expects.
	void Stop() noexcept;

private:
	// -1 rather than 0: 0 is *this process's group* to `kill`, so a pid field that defaulted to it
	// would signal the agent and everything it started on the first `Stop` after a failed fork.
	int m_Pid = -1;
};

// Describe how a wait status ended, for the one log line that says so. `exited 0`, `killed by 15`.
[[nodiscard]] std::string DescribeExit(int status);
} // namespace Session
