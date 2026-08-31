#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Session.h"

struct wl_client;
struct wl_display;
struct wl_event_loop;
struct wl_event_source;
struct wl_listener;

// gyro's Wayland server: the socket a client connects to, and the event loop its requests arrive on.
//
// **The codec underneath is libwayland's, and this owns everything around it.**
// [Decision 2](../../Docs/Decisions.md#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec)
// gives the server-side wire codec to `libwayland-server` — the framing, the object map, the dispatch
// into handlers — so what is left for gyro to own is the display those objects live in, the loop they
// are demarshalled on, and the socket clients reach them through. `WlDisplay` in the generated
// bindings names the display but implements nothing; the lifecycle is the core library's own verbs,
// called here.
//
// **The loop is one descriptor, and that is what the composition root wants of it.**
// `wl_event_loop_get_fd` is a single pollable fd behind which libwayland multiplexes every client, so
// `PollFd` hands the root one more thing to add to its `ppoll` set rather than a set of client
// sockets to round-robin.
// [Decision
// 126](../../Docs/Decisions.md#126-the-dispatch-threads-wait-is-a-ppoll-on-one-descriptor-and-the-root-converts-the-wake)
// argued the dispatch wait stays a `ppoll` for as long as there is a small fixed set to wait on, and
// worried the ring would arrive "with the first client socket"; because the codec is libwayland's,
// that socket never reaches gyro — the collapsing happens inside the library, behind this one fd. The
// root owns the descriptor, per that decision; this only names it, and never closes it, because it is
// the loop's and not ours.
//
// **The listener is somebody else's, and gyro accepts on it rather than handing it to libwayland.**
// Docs/Architecture.md#listener-handover has a session's agent create the Wayland socket in the user's
// own runtime directory, because gyro cannot `chown` one into place; `Adopt` is where that descriptor
// arrives. `wl_display_add_socket_fd` would take it in one call and is *not* used, for two reasons and
// the first is decisive: **libwayland has no way to remove a socket from a display.** A `wl_socket` is
// freed when the display is, so a session ending could not stop gyro accepting on that user's path —
// the agent is gone, the listener is not, and the session-end event the whole handover is shaped
// around would be a log line rather than a fact. Accepting here makes ending a session `Release`,
// which closes the file.
//
// The second reason is where the uid check lands. Docs/Architecture.md calls the per-connection check
// load-bearing rather than defence in depth: the *user* creates the socket, so a user can create a
// permissive one, and a connection through it would otherwise be attributed to the offering user's
// session — inside their clipboard and their surfaces. Checking on the accepted descriptor means no
// `wl_client` is ever constructed for a stranger, where `wl_display_add_socket_fd` would oblige gyro
// to build one and then destroy it from a client-created listener.
//
// What is *not* rewritten is the accept itself: one connection per readable callback, on a blocking
// descriptor, which is `socket_data()` in libwayland's own `wayland-server.c` line for line. The event
// source is level-triggered, so the callback runs only when a connection is already pending and the
// second one is served on the next dispatch — which is why there is no `O_NONBLOCK` here. Setting it
// would be visible to the *agent*, since a descriptor passed by `SCM_RIGHTS` shares its open file
// description with the one that sent it.
//
// **`Poll` then `Flush` is the shape a step wraps around the world.** A client's requests are read and
// their handlers run in `Poll` — which is where, once there are globals, a `wl_surface.commit`
// becomes a change to the scene — and the events gyro owes back, a `wl_buffer.release` or a frame
// callback, leave in `Flush` after the scene has been serialised. Both are non-blocking: the root has
// already decided when to sleep, in its own wait, and neither of these is allowed to make that
// decision for it. Decision 45 makes this a thread rather than a task, and the frame thread never
// waits on it.

class Server
{
public:
	Server() = default;

	~Server();

	// Neither copied nor moved: libwayland records the display's address in every resource it creates,
	// so a move would leave those pointing at a husk and a copy would be a second display nothing knows
	// about.
	Server(const Server&) = delete;
	Server& operator=(const Server&) = delete;
	Server(Server&&) = delete;
	Server& operator=(Server&&) = delete;

	// Bring up the display and the event loop, binding nothing.
	//
	// **Separate from binding, because under the handover there is nothing to bind yet.** A run that
	// takes its listeners from session agents has no socket at startup and may never get one — an agent
	// that never connects is an ordinary state of a machine — so the globals, the loop and the
	// descriptor the composition root waits on all have to exist before the first offer arrives. That
	// ordering is free: `PollFd` is the *event loop's* file, which the display creates, and a client
	// cannot be dispatched before there is a client.
	[[nodiscard]] Result<void> Open();

	// Bind a listening socket of gyro's own making. An empty `name` takes the first free `wayland-N`
	// under `XDG_RUNTIME_DIR`, which is what a client with nothing set finds; a non-empty one is bound
	// verbatim, for a second compositor on one machine or a test that wants a name it chose. Refused
	// where the server is not open, where the runtime directory is unset or unwritable, or where a
	// named socket is already taken.
	//
	// **Every client on it belongs to no session**, which is why the composition root refuses to do
	// this and take offers in the same run: there is no uid to check a connection against and no
	// session for one to end with. It is the development and single-user path, and it is the one this
	// binary has had until now.
	[[nodiscard]] Result<void> Bind(std::string_view name = {});

	// Serve one session's clients on the listener its agent offered.
	//
	// `uid` is what the kernel said about the offering process rather than anything it claimed, and it
	// is the whole of what a connection is admitted against. The descriptor is taken: it is closed by
	// `Release`, by the destructor, or here if the event source cannot be made.
	[[nodiscard]] Result<void> Adopt(Fd listener, std::uint32_t uid, SessionId session);

	// Stop serving a session: close its listener, and end every client that arrived on it.
	//
	// **Ending the clients is the point rather than tidying up after it.** The connection to the agent
	// closing *is* the session ending (Docs/Architecture.md#listener-handover), and a session whose
	// windows stayed on screen with nothing behind them would make that event advisory. A client is
	// ended the way libwayland ends one it has fallen out with, so its surfaces retire through
	// [decision
	// 114](../../Docs/Decisions.md#114-retirement-is-the-author-going-away-and-resurrection-is-the-authors-alone)'s
	// path and not through a second one written for this.
	//
	// Does nothing for a session that has no listener here, which is every session under `Bind`.
	void Release(SessionId session) noexcept;

	// The event loop's single descriptor, for the composition root to add to its `ppoll` set. Borrowed
	// from libwayland and never closed here; valid only while the server is open, and `-1` before it is.
	[[nodiscard]] int PollFd() const noexcept;

	// The display the globals are advertised in, or null before `Open`.
	//
	// **Handed out rather than wrapped**, because a global's lifetime is not this object's to manage: a
	// `wl_output` appears and disappears with a monitor and a `wl_seat` with a session, so a `Server`
	// that owned the advertisements would have to grow a verb per protocol and know when each one is
	// due. What it owns is the display, and `Wayland::Server::<Interface>::Advertise` takes one — so
	// the party that knows when a global should exist calls it directly, and this stays the three
	// things it says it is.
	[[nodiscard]] wl_display* Display() const noexcept { return m_Display; }

	// The loop `PollFd` is the file of. Handed out so that a party with a descriptor of its own to
	// watch — a client's acquire point, per [ExplicitSync.h](ExplicitSync.h) — lands behind the same
	// number the root already sleeps on, rather than becoming a second thing the composition root has
	// to be told to wait for.
	[[nodiscard]] wl_event_loop* EventLoop() const noexcept { return m_EventLoop; }

	// Read from every client that has data waiting and run the handlers their requests reach. Returns
	// at once when nothing is ready — the root's wait is what parks the thread. A refused client is
	// ended inside libwayland and is not a failure here; a failure is the loop itself faulting.
	[[nodiscard]] Result<void> Poll();

	// Push queued events out to their clients. Safe before `Open` and does nothing then.
	void Flush() noexcept;

	// The socket name `Open` bound, for the one log line that tells a person where to point a client.
	// Empty until a successful `Open`.
	[[nodiscard]] std::string_view SocketName() const noexcept { return m_SocketName; }

	[[nodiscard]] bool IsOpen() const noexcept { return m_Display != nullptr; }

	// Which session a client arrived under, or `None` for one that came in on a socket gyro bound
	// itself — which under `HostListener::Own` is every client there is.
	//
	// **It is the accepted connection's attribution rather than the request's**, which is the whole
	// value of keeping the map: a `wl_surface.commit` names no session and a handler two calls deep
	// cannot ask the socket, so the answer has to have been recorded when the connection was admitted
	// and the peer's uid checked.
	[[nodiscard]] SessionId SessionOf(wl_client* client) const noexcept
	{
		const auto found = m_Watched.find(client);

		return found == m_Watched.end() ? SessionId::None : found->second;
	}

	// How many clients are live across every session. For a test; nothing in the loop asks.
	[[nodiscard]] std::size_t Clients() const noexcept { return m_Watched.size(); }

private:
	// One adopted listener, and the session it serves. Held behind a pointer because its address is the
	// event source's user data and the vector it lives in is appended to while sources exist.
	struct Listener
	{
		Server* Owner = nullptr;

		wl_event_source* Source = nullptr;

		Fd Socket;

		std::uint32_t Uid = 0;

		SessionId Session = SessionId::None;
	};

	// libwayland's `wl_event_loop_fd_func_t`: a connection is pending on an adopted listener.
	static int OnConnection(int descriptor, std::uint32_t mask, void* data) noexcept;

	// Admit an accepted connection, or close it. Answers nothing because there is no caller that could
	// act on the difference: a stranger is a log line and a client that failed to construct is one too.
	void Admit(int connection, const Listener& listener) noexcept;

	wl_display* m_Display = nullptr;
	wl_event_loop* m_EventLoop = nullptr;
	std::string m_SocketName;

	std::vector<std::unique_ptr<Listener>> m_Listeners;

	// Which session each live client arrived under, which is the whole of what `Release` needs of one.
	// **The destroy listener that keeps this current is not here**: libwayland's is intrusive, so the
	// record holding it has to contain a `wl_listener` by value, and that is a type this header goes out
	// of its way not to name. Server.cpp owns it and frees it from its own notify.
	std::unordered_map<wl_client*, SessionId> m_Watched;
};
