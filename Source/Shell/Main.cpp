#include <poll.h>
#include <spdlog/spdlog.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>

#include "Shell/Bar.h"
#include "Shell/Launch.h"
#include "Shell/Session.h"

// gyro's shell, and the third binary this project ships.
//
// **A separate process for the reason decision 51 gives, rather than for convenience.** The shell
// declares and configures and is never in a per-event loop: gyro owns every continuous manipulation,
// so a shell that died mid-drag would take nothing with it. Making it a process is what makes that
// claim checkable — this one can be killed at any point and the compositor keeps drawing.
//
// It reaches gyro through `WAYLAND_DISPLAY` like any other client. What makes it a shell is which
// socket that names: `gyro-system-N` is bound by `Server::BindSystem` and grants System trust to
// every client that arrives there (183), which is where `gyro_bindings_v1` and `gyro_chrome_v1` are
// offered at all. Starting it against the socket applications reach fails at `Session::Open` with the
// interface that was missing, rather than half-working.
namespace
{
// `Alt` and `Space`. Written here rather than read from a file because there is no configuration
// yet and inventing a format before there is a second thing to put in it is how a format ends up
// wrong. Both are arguments to `Open`, so the file this becomes has somewhere to land.
constexpr std::uint32_t SummonModifiers = 4;
constexpr std::uint32_t SummonKeysym = XKB_KEY_space;
} // namespace

int main(int argument, char** arguments)
{
	bool summon = false;
	std::optional<std::string_view> display;

	// **Read before the connection, because opening it takes the variable away.** Wire/Connection.h
	// `unsetenv`s `WAYLAND_SOCKET` the moment it adopts the descriptor, and whether it was there is
	// exactly the question below.
	const bool handed = ::getenv("WAYLAND_SOCKET") != nullptr;

	for (int index = 1; index < argument; ++index)
	{
		const std::string_view option{ arguments[index] };

		if (option.starts_with("--display="))
		{
			display = option.substr(std::string_view{ "--display=" }.size());

			continue;
		}

		if (option == "--summon")
		{
			summon = true;

			continue;
		}

		if (option == "--verbose")
		{
			spdlog::set_level(spdlog::level::debug);

			continue;
		}

		spdlog::error("gyro-shell: unknown option {}", option);

		return EXIT_FAILURE;
	}

	Session session;

	if (Result<void> opened = session.Open(); !opened)
	{
		spdlog::error("gyro-shell: {}", opened.error());

		return EXIT_FAILURE;
	}

	spdlog::info("gyro-shell: connected");

	// **What everything this shell starts will be told the display is, and it is not always the one
	// this process connected on.** The agent puts the applications' socket in `WAYLAND_DISPLAY` and
	// hands the shell its own connection as `WAYLAND_SOCKET` (Session/Child.h), so inheriting is right
	// there and is what happens. A development run has no agent: the shell is started by name against
	// `gyro-system-N`, and that name is the socket that grants the run of the session — passing it on
	// would give a browser everything a shell is trusted with. So it is deliberately dropped, and
	// `--display=NAME` is how such a run says what applications should reach instead.
	Launcher launcher;

	if (display.has_value())
	{
		launcher.Adopt(*display);
	}
	else if (handed)
	{
		const char* const inherited = ::getenv("WAYLAND_DISPLAY");

		launcher.Adopt(inherited != nullptr ? std::string_view{ inherited } : std::string_view{});
	}
	else
	{
		launcher.Adopt({});

		spdlog::warn(
			"gyro-shell: connected by name, so nothing started from the bar is told a display; "
			"pass --display=NAME to say which socket applications should reach"
		);
	}

	Bar bar;

	if (Result<void> opened = bar.Open(session, launcher, SummonModifiers, SummonKeysym); !opened)
	{
		spdlog::error("gyro-shell: {}", opened.error());

		return EXIT_FAILURE;
	}

	if (summon)
	{
		bar.Summon();
	}

	spdlog::info("gyro-shell: ready, alt and space");

	// The whole of the loop, and the shape is the one every Wayland client has: write what the last
	// round produced, sleep on the socket, read what arrived. Everything this shell does happens inside
	// the drain — a chord fires, a configure is answered, a key is typed — so there is no work here to
	// do between the two.
	//
	// **It sleeps indefinitely and that is the claim being made.** A shell with a timeout would be one
	// polling for something, and there is nothing to poll for: gyro wakes this process when a person
	// presses the key and at no other time. A run bar that cost a wakeup a second would be a laptop
	// battery spent on a launcher nobody opened.
	while (true)
	{
		if (Result<void> flushed = session.Flush(); !flushed)
		{
			spdlog::error("gyro-shell: {}", flushed.error());

			return EXIT_FAILURE;
		}

		pollfd waiting{ .fd = session.Connection().Descriptor().Value, .events = POLLIN, .revents = 0 };

		if (::poll(&waiting, 1, -1) < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}

			spdlog::error("gyro-shell: waiting for the compositor: {}", std::strerror(errno));

			return EXIT_FAILURE;
		}

		if (Result<void> drained = session.Connection().Drain(); !drained)
		{
			// The compositor closing the connection is the ordinary end of a session, and it is not a
			// failure of this process: gyro released the listener the shell arrived on.
			if (const std::optional<Wire::ProtocolFault>& fault = session.Connection().Fault(); fault.has_value())
			{
				// **The fault's own sentence rather than the drain's**, because the drain's is a fixed
				// string — `Error` carries a `string_view` over static storage and cannot hold what gyro
				// said. The object and the code beside it are what turn *a protocol error* into a line
				// naming the request that caused it, which is the only diagnostic a client ever gets.
				spdlog::error(
					"gyro-shell: protocol error on object {}, code {}: {}",
					static_cast<std::uint32_t>(fault->Object),
					fault->Code,
					fault->Message
				);

				return EXIT_FAILURE;
			}

			spdlog::info("gyro-shell: the session ended");

			return EXIT_SUCCESS;
		}
	}
}
