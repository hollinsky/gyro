#pragma once

#include <string>
#include <string_view>

#include "Core/Fd.h"
#include "Core/Result.h"

// The Wayland listener an agent creates for its own user, before it offers it to gyro.
//
// **This is the half of the inversion gyro cannot perform.**
// Docs/Architecture.md#listener-handover: the socket has to be owned by the user whose clients will
// connect to it, and gyro cannot `chown` one into a user's runtime directory without a capability
// [decision
// 22](../../Docs/Decisions.md#22-gyro-runs-as-a-dedicated-unprivileged-uid-with-cap_sys_nice-and-nothing-else) refuses
// to hold. So the agent binds it, and Session/Control.h's `InspectOffer` is the far side checking that what arrived is
// what this file makes.
//
// **The lock file is libwayland's convention and is not optional.** `wl_display_add_socket` takes an
// exclusive `flock` on `<name>.lock` before binding, and `wl_display_add_socket_auto` walks the
// display numbers looking for one it can take — so an agent that bound the socket without the lock
// would be invisible to that search, and the next compositor started on the machine would unlink a
// live session's socket and bind its own over the top. The person at the keyboard sees every
// application stop being able to start. gyro no longer calls either function on the handover path,
// which is exactly why the convention has to be honoured here instead: the lock is what the *rest of
// the world* reads, and this is now the only party holding one.
//
// **The listener outlives every connection to gyro, which is why it is not `Session/Agent.h`'s.** A
// gyro restart loses every listener it held and the agents re-offer — so what the agent offers on the
// second connection has to be the same socket it offered on the first, with the same path and the
// same queue. A client that connected during the gap is *in* that queue and is served on adoption,
// which is what makes `WAYLAND_DISPLAY` survive a compositor restart rather than being a name that
// stops resolving.

namespace Session
{
// What a display is called before its number. libwayland's, and it is not a preference: a client
// with nothing set tries `wayland-0`, and `WAYLAND_DISPLAY` naming anything else is resolved
// relative to the same directory.
inline constexpr std::string_view DisplayPrefix = "wayland-";

// How far the search for a free name goes. libwayland's own limit, and reaching it means something
// on the machine is leaking sockets rather than that a person has thirty-two sessions.
inline constexpr unsigned MaxDisplayNumber = 32;

// A bound, listening Wayland socket and the lock that reserves its name.
//
// **The lock is held for the socket's whole life** rather than dropped after the bind, because that
// is what the convention means — a released lock is a free display number, and the socket would still
// be sitting on it.
struct WaylandListener
{
	Fd Socket;

	Fd Lock;

	// `wayland-1`, which is what goes in `WAYLAND_DISPLAY`.
	std::string Name;

	// Where it is bound, for a log line and for nothing else. A client is told the name, not this.
	std::string Path;
};

// Bind a listener in `directory`, taking `name` if it is given and the first free display number if
// it is not.
//
// A name that is already locked is refused with `EADDRINUSE`; the automatic form treats the same
// condition as *try the next one* and only fails once every number is spoken for. A stale socket left
// by a process that died is unlinked, because holding the lock is what says nobody owns it — which is
// the ordinary state of the path after a machine loses power.
[[nodiscard]] Result<WaylandListener> BindWaylandListener(std::string_view directory, std::string_view name = {});
} // namespace Session
