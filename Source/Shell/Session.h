#pragma once

#include <cstdint>
#include <memory>

#include "Core/Result.h"
#include "Wayland/FractionalScaleV1.h"
#include "Wayland/GyroBindingsV1.h"
#include "Wayland/GyroChromeV1.h"
#include "Wayland/Viewporter.h"
#include "Wayland/Wayland.h"
#include "Wayland/XdgShell.h"
#include "Wire/Connection.h"

// What the shell is connected to, and everything it was given when it arrived.
//
// **A shell is an ordinary Wayland client.** It opens the socket the environment names, walks the
// registry and binds what it needs, and nothing it sends says it is a shell — trust is a property of
// the listener it reached rather than of the connection (Protocol/Tier.h). So the whole of *am I a
// shell* is whether `gyro_bindings_v1` and `gyro_chrome_v1` were announced, and `Open` treats their
// absence as the failure it is rather than carrying on without them.
//
// That refusal is worth the line it costs. A shell started against the socket applications reach
// binds a compositor, a shm, a shell and a seat exactly as it expected to, opens a window nobody
// asked for in the middle of the screen, and never answers its key — which reads as a broken chord
// rather than as the wrong socket. Naming the interface that was missing says which it was.
class Session
{
public:
	Session();
	~Session();

	Session(const Session&) = delete;
	Session& operator=(const Session&) = delete;

	// Connects, walks the registry once, and binds. `WAYLAND_DISPLAY` selects the socket, so a shell
	// is pointed at gyro's own listener by naming the one the compositor logs at startup.
	[[nodiscard]] Result<void> Open();

	// A round trip: `wl_display.sync` is answered behind everything already queued, so a callback that
	// has fired means every event sent before it has been dispatched. It is how the registry is known
	// to be complete, and how a caller waits for a configure without inventing a second protocol.
	[[nodiscard]] Result<void> Roundtrip();

	// Everything buffered goes out. A shell calls this immediately before it sleeps, for the reason
	// Protocol/Host.h calls its own: a request still in the output buffer is a frame that never
	// arrives, and the loop is about to stop writing.
	[[nodiscard]] Result<void> Flush();

	// The connection, for the loop that owns the descriptor.
	[[nodiscard]] Wire::Connection& Connection() noexcept { return m_Connection; }

	[[nodiscard]] Wayland::WlCompositor Compositor() const noexcept { return m_Compositor; }

	[[nodiscard]] Wayland::WlShm Shm() const noexcept { return m_Shm; }

	[[nodiscard]] Wayland::XdgWmBase Shell() const noexcept { return m_Shell; }

	[[nodiscard]] Wayland::WlSeat Seat() const noexcept { return m_Seat; }

	[[nodiscard]] Wayland::GyroBindingsV1 Bindings() const noexcept { return m_Bindings; }

	[[nodiscard]] Wayland::GyroChromeManagerV1 Chrome() const noexcept { return m_Chrome; }

	[[nodiscard]] Wayland::WpViewporter Viewporter() const noexcept { return m_Viewporter; }

	[[nodiscard]] Wayland::WpFractionalScaleManagerV1 FractionalScale() const noexcept { return m_Fractional; }

	// **There is no `Width` here any more, and no `wl_output` behind it.** What used to be here was the
	// size of the first panel announced, which the bar laid itself out against — and on a mixed desk it
	// was the wrong panel, because gyro puts chrome on the output holding the pointer (141) and never
	// told the shell which that was. Both halves of it now come from the compositor's answer *about the
	// bar's own surface*: `xdg_toplevel.configure_bounds` for the room, `wp_fractional_scale_v1` for the
	// density. A shell that still bound an output would have a second, worse answer available to
	// whoever read it next.

private:
	// The listeners, which are the objects the connection dispatches into and so must outlive every
	// proxy made with them. Held behind a pointer because none of them is anything a caller of this
	// header has business naming: what a registry announced and what a ping asked for are this file's
	// own business, and putting them in the header would put four classes in front of every reader.
	struct Events;

	std::unique_ptr<Events> m_Events;
	Wire::Connection m_Connection;
	Wayland::WlDisplay m_Display;
	Wayland::WlRegistry m_Registry;
	Wayland::WlCompositor m_Compositor;
	Wayland::WlShm m_Shm;
	Wayland::XdgWmBase m_Shell;
	Wayland::WlSeat m_Seat;
	Wayland::GyroBindingsV1 m_Bindings;
	Wayland::GyroChromeManagerV1 m_Chrome;
	Wayland::WpViewporter m_Viewporter;
	Wayland::WpFractionalScaleManagerV1 m_Fractional;
};
