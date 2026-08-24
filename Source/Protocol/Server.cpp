#include "Protocol/Server.h"

#include <wayland-server-core.h>

#include <cerrno>
#include <string>

Server::~Server()
{
	if (m_Display != nullptr)
	{
		// Destroys the globals, drops every client, and closes the socket. The event loop is the
		// display's own and goes with it.
		wl_display_destroy(m_Display);
	}
}

Result<void> Server::Open(std::string_view name)
{
	if (m_Display != nullptr)
	{
		return Failure(EALREADY, "the Wayland server is already open");
	}

	wl_display* const display = wl_display_create();

	if (display == nullptr)
	{
		return Failure(ENOMEM, "creating the Wayland display");
	}

	if (name.empty())
	{
		// Picks the first free `wayland-N`, or null where the runtime directory is unset, unwritable,
		// or full of taken names. libwayland does not set `errno` for this, so the message carries the
		// cause a person can act on rather than a code that would be a guess.
		const char* const chosen = wl_display_add_socket_auto(display);

		if (chosen == nullptr)
		{
			wl_display_destroy(display);

			return Failure(EADDRNOTAVAIL, "binding a Wayland socket; is XDG_RUNTIME_DIR set and writable?");
		}

		m_SocketName = chosen;
	}
	else
	{
		// `wl_display_add_socket` wants a NUL-terminated name and a `string_view` does not promise one.
		const std::string requested{ name };

		if (wl_display_add_socket(display, requested.c_str()) != 0)
		{
			wl_display_destroy(display);

			// Not the name, because an `Error` carries a `string_view` and a temporary that named the
			// socket would dangle the moment this returned. The caller has the name it passed.
			return Failure(EADDRINUSE, "binding the named Wayland socket");
		}

		m_SocketName = requested;
	}

	m_Display = display;
	m_EventLoop = wl_display_get_event_loop(display);

	return {};
}

int Server::PollFd() const noexcept
{
	return m_EventLoop != nullptr ? wl_event_loop_get_fd(m_EventLoop) : -1;
}

Result<void> Server::Poll()
{
	if (m_EventLoop == nullptr)
	{
		return Failure(EBADF, "polling a Wayland server that was never opened");
	}

	// Zero timeout is non-blocking: drain what is ready and return. A negative result is the loop
	// itself faulting rather than a client misbehaving — a client's own fault is answered by ending
	// that client, inside libwayland, and never reaches here.
	if (wl_event_loop_dispatch(m_EventLoop, 0) < 0)
	{
		return Failure(errno, "dispatching the Wayland event loop");
	}

	return {};
}

void Server::Flush() noexcept
{
	if (m_Display != nullptr)
	{
		wl_display_flush_clients(m_Display);
	}
}
