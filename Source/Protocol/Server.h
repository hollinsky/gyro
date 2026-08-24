#pragma once

#include <string>
#include <string_view>

#include "Core/Result.h"

struct wl_display;
struct wl_event_loop;

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

	// Bring up the display and bind a listening socket. An empty `name` takes the first free
	// `wayland-N` under `XDG_RUNTIME_DIR`, which is what a client with nothing set finds; a non-empty
	// one is bound verbatim, for a second compositor on one machine or a test that wants a name it
	// chose. Refused if already open, if the runtime directory is unset or unwritable, or if a named
	// socket is already taken.
	[[nodiscard]] Result<void> Open(std::string_view name = {});

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

private:
	wl_display* m_Display = nullptr;
	wl_event_loop* m_EventLoop = nullptr;
	std::string m_SocketName;
};
