// `accept4` and `SO_PEERCRED` are Linux's rather than POSIX's, and glibc gates the first behind a
// feature-test macro. Named here rather than inherited, which is the rule Core/Clock.cpp states for
// the portable tier and this module keeps for the sake of one spelling across the tree.
#define _GNU_SOURCE 1

#include "Protocol/Server.h"

#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
// What says a client has gone, and it is a `new` with no owner on purpose: libwayland's destroy
// listener is intrusive, so the record has to outlive every scope in this file and be freed from the
// notify it registers. `Server` keeps only the session id, in a map of complete types.
//
// **The listener is first, and that is load-bearing rather than tidy.** libwayland hands a notify
// nothing but the `wl_listener*` it was registered with, and a standard-layout struct's first member
// shares the struct's address — so the cast back is defined. `wl_container_of` is the library's own
// answer to this and is not used: it is a `__typeof__` macro, and the assertions below say the same
// thing in the language rather than in the preprocessor.
struct ClientWatch
{
	wl_listener Destroyed{};

	std::unordered_map<wl_client*, Session::SessionId>* Watched = nullptr;

	wl_client* Client = nullptr;
};

static_assert(std::is_standard_layout_v<ClientWatch>);
static_assert(offsetof(ClientWatch, Destroyed) == 0);

void OnClientGone(wl_listener* listener, void* data) noexcept
{
	(void)data;

	auto* const watch = reinterpret_cast<ClientWatch*>(listener);

	// Out of libwayland's list before the record holding it is freed, which is the one ordering the
	// intrusive shape obliges.
	wl_list_remove(&watch->Destroyed.link);
	watch->Watched->erase(watch->Client);

	delete watch;
}
} // namespace

Server::~Server()
{
	if (m_Display != nullptr)
	{
		// Destroys the globals, drops every client, and closes the sockets libwayland bound itself. The
		// event loop is the display's own and goes with it, which takes every adopted listener's event
		// source with it too — so nothing below removes one, and the descriptors are closed by the
		// vector going away afterwards.
		//
		// Every client is destroyed in here, so every `OnClientGone` runs, each watch is freed and
		// `m_Watched` empties on the way through. Clearing it again below is what covers a display that
		// was never opened.
		wl_display_destroy(m_Display);
	}

	for (const std::unique_ptr<Listener>& listener : m_Listeners)
	{
		listener->Source = nullptr;
	}

	m_Watched.clear();
	m_Listeners.clear();
}

Result<void> Server::Open()
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

	m_Display = display;
	m_EventLoop = wl_display_get_event_loop(display);

	return {};
}

Result<void> Server::Bind(std::string_view name)
{
	if (m_Display == nullptr)
	{
		return Failure(EBADF, "binding a socket on a Wayland server that was never opened");
	}

	if (!m_SocketName.empty())
	{
		return Failure(EALREADY, "the Wayland server has already bound a socket");
	}

	if (name.empty())
	{
		// Picks the first free `wayland-N`, or null where the runtime directory is unset, unwritable,
		// or full of taken names. libwayland does not set `errno` for this, so the message carries the
		// cause a person can act on rather than a code that would be a guess.
		const char* const chosen = wl_display_add_socket_auto(m_Display);

		if (chosen == nullptr)
		{
			return Failure(EADDRNOTAVAIL, "binding a Wayland socket; is XDG_RUNTIME_DIR set and writable?");
		}

		m_SocketName = chosen;

		return {};
	}

	// `wl_display_add_socket` wants a NUL-terminated name and a `string_view` does not promise one.
	const std::string requested{ name };

	if (wl_display_add_socket(m_Display, requested.c_str()) != 0)
	{
		// Not the name, because an `Error` carries a `string_view` and a temporary that named the
		// socket would dangle the moment this returned. The caller has the name it passed.
		return Failure(EADDRINUSE, "binding the named Wayland socket");
	}

	m_SocketName = requested;

	return {};
}

Result<void> Server::Adopt(Fd listener, std::uint32_t uid, Session::SessionId session)
{
	if (m_EventLoop == nullptr)
	{
		return Failure(EBADF, "adopting a listener on a Wayland server that was never opened");
	}

	if (!listener.IsValid())
	{
		return Failure(EBADF, "adopting a listener that is not a descriptor");
	}

	if (session == Session::SessionId::None)
	{
		return Failure(EINVAL, "adopting a listener for no session");
	}

	auto held = std::make_unique<Listener>();
	held->Owner = this;
	held->Uid = uid;
	held->Session = session;
	held->Socket = std::move(listener);

	// The address is the user data, which is why the record is behind a pointer: the vector below is
	// appended to while sources made from earlier entries are still registered.
	held->Source =
		wl_event_loop_add_fd(m_EventLoop, held->Socket.Get(), WL_EVENT_READABLE, &Server::OnConnection, held.get());

	if (held->Source == nullptr)
	{
		// The descriptor goes with the record, which is what the caller wants: an offer gyro could not
		// serve is one whose listener it must not keep holding open.
		return Failure(ENOMEM, "watching an offered Wayland listener");
	}

	m_Listeners.push_back(std::move(held));

	return {};
}

void Server::Release(Session::SessionId session) noexcept
{
	for (std::size_t index = m_Listeners.size(); index > 0; --index)
	{
		std::unique_ptr<Listener>& listener = m_Listeners[index - 1];

		if (listener->Session != session)
		{
			continue;
		}

		if (listener->Source != nullptr)
		{
			// Before the descriptor closes, because the source names it: a loop left holding a removed
			// file is one epoll reports on after somebody else has been given the number.
			wl_event_source_remove(listener->Source);
			listener->Source = nullptr;
		}

		m_Listeners.erase(m_Listeners.begin() + static_cast<std::ptrdiff_t>(index - 1));
	}

	// Gathered before any of them is destroyed, because `OnClientGone` erases from the map that is
	// being walked.
	std::vector<wl_client*> ending;

	for (const auto& [client, held] : m_Watched)
	{
		if (held == session)
		{
			ending.push_back(client);
		}
	}

	for (wl_client* const client : ending)
	{
		wl_client_destroy(client);
	}
}

int Server::OnConnection(int descriptor, std::uint32_t mask, void* data) noexcept
{
	// The mask is unread on purpose: the source is registered for readability alone, and a hangup on a
	// listening socket is not a thing that happens — the file is gyro's and closing it is `Release`.
	(void)mask;

	auto* const listener = static_cast<Listener*>(data);

	// **One connection per callback, on a blocking descriptor**, which is `socket_data()` in
	// libwayland's `wayland-server.c` exactly. The source is level-triggered, so this runs only with a
	// connection already pending and the next one is served on the next dispatch — and the loop that
	// would drain to empty is what would need `O_NONBLOCK`, which a `SCM_RIGHTS` descriptor shares with
	// the agent that sent it.
	const int connection = ::accept4(descriptor, nullptr, nullptr, SOCK_CLOEXEC);

	if (connection < 0)
	{
		spdlog::warn(
			"accepting a client on session {}: {}", static_cast<std::uint32_t>(listener->Session), ::strerror(errno)
		);

		return 1;
	}

	listener->Owner->Admit(connection, *listener);

	return 1;
}

void Server::Admit(int connection, const Listener& listener) noexcept
{
	// **The kernel's answer rather than the peer's**, and it is the check
	// Docs/Architecture.md#listener-handover calls load-bearing rather than defence in depth: the user
	// creates the socket, so a user can create a permissive one, and a connection through it would
	// otherwise be attributed to the offering user's session — which is that person's clipboard and
	// that person's surfaces.
	::ucred credentials{};
	::socklen_t size = sizeof credentials;

	if (::getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0)
	{
		spdlog::warn("reading the credentials of a client on session {}", static_cast<std::uint32_t>(listener.Session));
		::close(connection);

		return;
	}

	if (credentials.uid != listener.Uid)
	{
		// **Said out loud, because it is silent at both ends otherwise.** The client sees its connection
		// close with nothing on it, which every toolkit reports as *the compositor is not running*, and
		// the person it happens to is by construction not the person who owns the session it happened
		// on.
		spdlog::warn(
			"refusing a client of uid {} on session {}, which belongs to uid {}",
			credentials.uid,
			static_cast<std::uint32_t>(listener.Session),
			listener.Uid
		);
		::close(connection);

		return;
	}

	wl_client* const client = wl_client_create(m_Display, connection);

	if (client == nullptr)
	{
		// libwayland closes nothing it did not take, which its own `socket_data()` answers the same way.
		::close(connection);
		spdlog::warn("creating a client on session {}", static_cast<std::uint32_t>(listener.Session));

		return;
	}

	auto* const watch = new ClientWatch{};
	watch->Watched = &m_Watched;
	watch->Client = client;
	watch->Destroyed.notify = &OnClientGone;

	wl_client_add_destroy_listener(client, &watch->Destroyed);

	m_Watched.emplace(client, listener.Session);
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
