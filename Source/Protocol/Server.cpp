// `accept4` and `SO_PEERCRED` are Linux's rather than POSIX's, and glibc gates the first behind a
// feature-test macro. Named here rather than inherited, which is the rule Core/Clock.cpp states for
// the portable tier and this module keeps for the sake of one spelling across the tree.
#define _GNU_SOURCE 1

#include "Protocol/Server.h"

#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
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

	std::unordered_map<wl_client*, Server::Admitted>* Watched = nullptr;

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

// How many `gyro-system-N` names are tried before `BindSystem` gives up. Sixteen simultaneous
// development compositors is already an odd machine, and the bound is what keeps a directory somebody
// has filled with matching names from being a loop instead of a message.
constexpr int SystemSocketNames = 16;

// Whether something is listening on a socket path already. **This is the whole of how a crashed run's
// leftovers are told from a running compositor's socket**: an `AF_UNIX` path outlives the process that
// bound it, so its existence says nothing, and a `connect` that is refused is the only thing that
// does. Answers *live* for any other failure, because a path gyro cannot reach is one it must not take
// away from whoever can.
[[nodiscard]] bool SocketIsLive(const std::string& path) noexcept
{
	::sockaddr_un address{};
	address.sun_family = AF_UNIX;

	if (path.size() >= sizeof address.sun_path)
	{
		return true;
	}

	std::memcpy(address.sun_path, path.c_str(), path.size());

	const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (probe < 0)
	{
		return true;
	}

	const bool connected = ::connect(probe, reinterpret_cast<const ::sockaddr*>(&address), sizeof address) == 0;
	const int failure = errno;

	::close(probe);

	return connected || failure != ECONNREFUSED;
}

// Bind and listen on one candidate path, or answer an invalid descriptor for a name that is taken.
[[nodiscard]] Fd ListenOn(const std::string& path) noexcept
{
	::sockaddr_un address{};
	address.sun_family = AF_UNIX;

	if (path.size() >= sizeof address.sun_path)
	{
		return Fd{};
	}

	std::memcpy(address.sun_path, path.c_str(), path.size());

	Fd socket{ ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0) };

	if (!socket.IsValid())
	{
		return Fd{};
	}

	if (::bind(socket.Get(), reinterpret_cast<const ::sockaddr*>(&address), sizeof address) != 0)
	{
		return Fd{};
	}

	// The same backlog libwayland gives its own sockets. It bounds connections the loop has not
	// accepted yet, and one dispatch accepts one — so it is the depth of a burst rather than a limit on
	// clients.
	if (::listen(socket.Get(), 128) != 0)
	{
		::unlink(path.c_str());

		return Fd{};
	}

	return socket;
}
} // namespace

Server::~Server()
{
	if (m_Display != nullptr)
	{
		// **The clients are ended here as a backstop rather than as the ordering**, because
		// `wl_display_destroy` leaves every `wl_client` standing — it takes the globals, the sockets and
		// the loop, and nothing else — so a server that was not torn down through `EndClients` would
		// otherwise leave each of its clients holding a connection nobody closes. `ClientHost` calls that
		// verb first, while the clipboards a client's teardown reaches into are still alive, so this walks
		// an empty list on the path that matters; what it covers is a `Server` standing on its own, which
		// is every test that builds one.
		//
		// Before the display, so that each `wl_client_destroy` still has the loop its event source is
		// registered on.
		wl_display_destroy_clients(m_Display);

		// The event loop is the display's own and goes with it, which takes every adopted listener's
		// event source with it too — so nothing below removes one, and the descriptors are closed by the
		// vector going away afterwards. Every client went through `OnClientGone` above, so `m_Watched` is
		// empty by the time it is cleared below; clearing it anyway is what covers a display that was
		// never opened.
		wl_display_destroy(m_Display);
	}

	for (const std::unique_ptr<Listener>& listener : m_Listeners)
	{
		listener->Source = nullptr;

		// Only a socket gyro created carries a path, which today is `BindSystem`'s alone. Unlinked after
		// the display is destroyed and therefore after the last client on it is gone, so nothing is
		// connecting through a name that has already been taken away.
		if (!listener->Path.empty())
		{
			::unlink(listener->Path.c_str());
		}
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

	// **One hook rather than a check in every global's bind path**, which is what makes filtered globals
	// a day's work rather than the rewrite Docs/Architecture.md#filtered-globals priced it as before the
	// server half existed. libwayland calls this while advertising a registry *and* while binding off
	// one, so a global a client never saw is also a global it cannot name, and no `Advertise` call site
	// changes.
	wl_display_set_global_filter(display, &Server::OnGlobalFilter, this);

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

Result<void> Server::Adopt(Fd listener, std::uint32_t uid, SessionId session, Trust trust)
{
	if (m_EventLoop == nullptr)
	{
		return Failure(EBADF, "adopting a listener on a Wayland server that was never opened");
	}

	if (!listener.IsValid())
	{
		return Failure(EBADF, "adopting a listener that is not a descriptor");
	}

	if (session == SessionId::None)
	{
		return Failure(EINVAL, "adopting a listener for no session");
	}

	// No path, because the file is the offering user's: it lives in that user's runtime directory and
	// removing it is not gyro's to do.
	return Watch(std::move(listener), {}, uid, session, trust);
}

Result<void> Server::BindSystem()
{
	if (m_EventLoop == nullptr)
	{
		return Failure(EBADF, "binding a System listener on a Wayland server that was never opened");
	}

	if (!m_SystemSocketName.empty())
	{
		return Failure(EALREADY, "the Wayland server has already bound a System listener");
	}

	const char* const directory = std::getenv("XDG_RUNTIME_DIR");

	if (directory == nullptr || *directory == '\0')
	{
		return Failure(ENOENT, "binding a System listener; is XDG_RUNTIME_DIR set?");
	}

	for (int index = 0; index < SystemSocketNames; ++index)
	{
		const std::string name = "gyro-system-" + std::to_string(index);
		const std::string path = std::string{ directory } + "/" + name;

		Fd socket = ListenOn(path);

		if (!socket.IsValid())
		{
			// Taken, and the only question left is by what. A live compositor keeps its name; the file a
			// crashed one left behind is unlinked and the same name tried once more, which is what stops a
			// machine that has been developed on all day from walking further up the range every run.
			if (SocketIsLive(path))
			{
				continue;
			}

			::unlink(path.c_str());

			socket = ListenOn(path);

			if (!socket.IsValid())
			{
				continue;
			}
		}

		if (const Result<void> watched = Watch(std::move(socket), path, ::getuid(), SessionId::None, Trust::System);
		    !watched)
		{
			// The descriptor went with the failure, so the file it bound is now a path with nothing behind
			// it — removed here rather than left for the next run's probe to find.
			::unlink(path.c_str());

			return watched;
		}

		m_SystemSocketName = name;

		return {};
	}

	return Failure(EADDRINUSE, "binding a System listener; every gyro-system-N name is taken");
}

Result<void> Server::Watch(Fd socket, std::string path, std::uint32_t uid, SessionId session, Trust trust)
{
	auto held = std::make_unique<Listener>();
	held->Owner = this;
	held->Path = std::move(path);
	held->Uid = uid;
	held->Session = session;
	held->Level = trust;
	held->Socket = std::move(socket);

	// The address is the user data, which is why the record is behind a pointer: the vector below is
	// appended to while sources made from earlier entries are still registered.
	held->Source =
		wl_event_loop_add_fd(m_EventLoop, held->Socket.Get(), WL_EVENT_READABLE, &Server::OnConnection, held.get());

	if (held->Source == nullptr)
	{
		// The descriptor goes with the record, which is what the caller wants: an offer gyro could not
		// serve is one whose listener it must not keep holding open.
		return Failure(ENOMEM, "watching a Wayland listener");
	}

	m_Listeners.push_back(std::move(held));

	return {};
}

void Server::EndClients() noexcept
{
	if (m_Display != nullptr)
	{
		wl_display_destroy_clients(m_Display);
	}
}

void Server::Release(SessionId session) noexcept
{
	// **No session is not a session, and asking to end it would end the development run instead.** Every
	// client on a socket gyro bound itself is `SessionId::None`, and so is the System listener
	// `BindSystem` creates — so a `Release(None)` reaching the walks below would close the shell's
	// socket and destroy every window on the machine. Nothing calls it that way today, since a session
	// ending names the session that ended; this is what stops the first caller that does.
	if (session == SessionId::None)
	{
		return;
	}

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
		if (held.Session == session)
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

bool Server::OnGlobalFilter(const wl_client* client, const wl_global* global, void* data) noexcept
{
	const auto* const server = static_cast<const Server*>(data);
	const wl_interface* const interface = wl_global_get_interface(global);

	// A global with no interface is not a thing libwayland makes; answering `false` rather than
	// asserting keeps a state nobody can reach from being the one that takes the compositor down.
	if (interface == nullptr || interface->name == nullptr)
	{
		return false;
	}

	return Visible(TierOf(interface->name), server->TrustOf(client));
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

	m_Watched.emplace(client, Admitted{ .Session = listener.Session, .Level = listener.Level });
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
