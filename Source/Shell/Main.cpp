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
// `Super` and `Space`. Written here rather than read from a file because there is no configuration
// yet and inventing a format before there is a second thing to put in it is how a format ends up
// wrong. Both are arguments to `Open`, so the file this becomes has somewhere to land.
constexpr std::uint32_t SummonModifiers = 8;
constexpr std::uint32_t SummonKeysym = XKB_KEY_space;
} // namespace

int main(int argument, char** arguments)
{
	bool summon = false;

	for (int index = 1; index < argument; ++index)
	{
		const std::string_view option{ arguments[index] };

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

	spdlog::info("gyro-shell: connected, {}x{} at scale {}", session.Width(), session.Height(), session.Scale());

	Bar bar;

	if (Result<void> opened = bar.Open(session, SummonModifiers, SummonKeysym); !opened)
	{
		spdlog::error("gyro-shell: {}", opened.error());

		return EXIT_FAILURE;
	}

	if (summon)
	{
		bar.Summon();
	}

	spdlog::info("gyro-shell: ready, super and space");

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
