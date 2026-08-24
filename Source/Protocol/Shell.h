#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "Core/Handle.h"
#include "Geometry/Space.h"
#include "Protocol/Context.h"
#include "Protocol/Surface.h"
#include "Wayland/Server/Wayland.h"
#include "Wayland/Server/XdgShell.h"

// `xdg_wm_base`: the role that turns a client's rectangle of pixels into a window.
//
// **Advertised at version 1, on `wl_compositor`'s rule: the number is a promise about what gyro
// sends.** Version 2 adds the tiled states, 4 a `configure_bounds` event, 5 a `wm_capabilities` event
// telling the client which of maximise, minimise, fullscreen and the window menu it may offer. gyro
// has no shell, no seat and no output model reaching this module, so it has nothing true to say in any
// of them — and a client told 5 and sent no capabilities is entitled to assume it has all four, which
// is a titlebar full of buttons that do nothing. Every toolkit binds this at whatever version it
// finds; none requires more than one.
//
// **The whole of the mapping protocol is here because the whole of it is one state machine.** A
// client gets an `xdg_surface`, gets an `xdg_toplevel` from it, commits with *no* buffer to ask for a
// configure, is configured, acks, and only then commits a buffer — and the window is mapped at that
// last step and nowhere else. Splitting those across files would put half the sequence where the other
// half's invariants are not readable.

// The advertised version. See above before raising it.
inline constexpr std::uint32_t ShellVersion = 1;

class ClientXdgSurface;

// One `xdg_toplevel`: a window with a title, and the requests a person's window manager would answer.
//
// **Almost all of them are accepted and do nothing, and that is not a stub.** Maximise, minimise,
// fullscreen, interactive move and resize are all *window management*, which belongs to a shell
// (51) — gyro's answer to a client that asks for them is the same whether there is no shell yet or a
// shell that declined: the state does not change, and the protocol's contract is that a client learns
// what it got from the next configure rather than from a reply. So a request that changes nothing is
// answered by changing nothing, which is a legal outcome rather than an unimplemented one.
class ClientXdgToplevel final : public Wayland::Server::XdgToplevelHandler
{
public:
	explicit ClientXdgToplevel(ClientXdgSurface& surface) noexcept : m_Surface{ &surface } {}

	void OnGone() override;

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	// What the client calls its window. Held rather than dropped because it is the one piece of a
	// toplevel a person reads, and the recovery console and every future switcher want it.
	[[nodiscard]] std::string_view Title() const noexcept { return m_Title; }

	[[nodiscard]] std::string_view AppId() const noexcept { return m_AppId; }

	// The `xdg_surface` went first, which the protocol calls a client error and gyro still has to
	// survive: this object stays alive until its own destroy request arrives, and must not be holding a
	// pointer into freed memory while it waits.
	void Forget() noexcept { m_Surface = nullptr; }

	void OnSetParent(Wayland::Server::XdgToplevel parent) override { (void)parent; }

	void OnSetTitle(std::string_view title) override { m_Title = title; }

	void OnSetAppId(std::string_view appId) override { m_AppId = appId; }

	void OnShowWindowMenu(Wayland::Server::WlSeat seat, std::uint32_t serial, std::int32_t x, std::int32_t y) override;

	void OnMove(Wayland::Server::WlSeat seat, std::uint32_t serial) override;

	void
	OnResize(Wayland::Server::WlSeat seat, std::uint32_t serial, Wayland::Server::XdgToplevelResizeEdge edges) override;

	void OnSetMaxSize(std::int32_t width, std::int32_t height) override;

	void OnSetMinSize(std::int32_t width, std::int32_t height) override;

	void OnSetMaximized() override {}

	void OnUnsetMaximized() override {}

	void OnSetFullscreen(Wayland::Server::WlOutput output) override { (void)output; }

	void OnUnsetFullscreen() override {}

	void OnSetMinimized() override {}

private:
	ClientXdgSurface* m_Surface = nullptr;

	std::string m_Title;
	std::string m_AppId;
};

// One `xdg_popup`. Accepted, never configured, and therefore never shown.
//
// **A menu that does not appear is worse than a client that will not start, and this is still the
// right answer for now.** A popup needs a positioner resolved against the anchor rectangle of its
// parent and constrained to an output — decision 141's guard puts placement policy squarely outside
// gyro, and the positioner's constraint adjustment is the one piece of placement the *protocol*
// specifies rather than the shell. Until that is built, refusing the object would take down every
// toolkit at its first tooltip; accepting it costs the menu and keeps the application running.
class ClientXdgPopup final : public Wayland::Server::XdgPopupIgnoring
{
public:
	void OnGone() override { delete this; }
};

// One `xdg_positioner`. Recorded and unread, for `ClientXdgPopup`'s reason.
class ClientXdgPositioner final : public Wayland::Server::XdgPositionerIgnoring
{
public:
	void OnGone() override { delete this; }
};

// One `xdg_surface`: the role a `wl_surface` takes, and the object that owns the window in the scene.
//
// **The node is created at map and destroyed at unmap, and neither is the same event as the object
// existing.** A window exists in the world from the commit that first carries a buffer *after* a
// configure has been acknowledged, which is the protocol's own definition of mapped; everything before
// that is a client negotiating a size. Authoring a node at `get_toplevel` would put an empty rectangle
// on screen for however long the client takes to draw its first frame.
//
// **Two entities per window, not one.** The container is the window — it carries the placement, and it
// is what the shell will one day move between workspaces — and the surface's pixels are an image child
// of it. That is the shape decision 111 describes for a toplevel: a container holding its
// below-subsurfaces, its own surface, and its above-subsurfaces. There are no subsurfaces yet and the
// container still earns its place, because the offset between the window's declared geometry and the
// surface's own origin lives on the child.
class ClientXdgSurface final : public Wayland::Server::XdgSurfaceHandler, public SurfaceRole
{
public:
	// `surface` is null only where the client named something that was not a `wl_surface` at all. The
	// object still has to exist — the client is holding an id and libwayland needs something behind it
	// — and every request on it then does nothing, which costs nothing because the connection has
	// already been ended by the caller.
	ClientXdgSurface(HostContext& context, ClientSurface* surface) noexcept
		: m_Context{ &context }, m_Surface{ surface }
	{}

	~ClientXdgSurface() override;

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	Wayland::Server::XdgToplevelHandler* OnGetToplevel() override;

	Wayland::Server::XdgPopupHandler*
	OnGetPopup(Wayland::Server::XdgSurface parent, Wayland::Server::XdgPositioner positioner) override;

	void OnSetWindowGeometry(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override;

	void OnAckConfigure(std::uint32_t serial) override;

	// `SurfaceRole`.
	void OnSurfaceCommitted(ClientSurface& surface) override;

	void OnSurfaceGone() override;

	// The window in the scene, or null while the surface is unmapped. A test's way of asking whether a
	// window exists without inferring it from a node count.
	[[nodiscard]] EntityId Window() const noexcept { return m_Window; }

	// The role object let go, which unmaps the window: a toplevel that is destroyed is a window that is
	// gone, whatever the surface still has attached.
	void Orphan() noexcept;

private:
	// The client is entitled to a size and gyro has no opinion about what it should be, which the
	// protocol spells `0 x 0` — *pick your own*. That is the honest configure for a compositor with no
	// shell placing windows at their natural size, rather than a number invented here that every client
	// would then be obliged to obey.
	void Configure();

	// Turn a committed surface into a window, or update the one that is already there.
	void Map(ClientSurface& surface);

	// Take the window out of the world. Retires rather than removes, per decision 114 — the subtree
	// keeps its position so that an exit animation has something to run on, and the store frees it on
	// the pass its last channel settles.
	void Unmap();

	// The window's natural size in its own space: what the client declared as its geometry, or the
	// whole surface where it declared none.
	[[nodiscard]] Size<SurfaceSpace, float> Natural(const ClientSurface& surface) const noexcept;

	HostContext* m_Context = nullptr;

	// The surface this role was taken on, or null once it has gone. Not owned.
	ClientSurface* m_Surface = nullptr;

	// The role object, or null while the client has an `xdg_surface` and has not yet said what kind of
	// thing it is. Not owned — a toplevel is a protocol object with its own lifetime.
	ClientXdgToplevel* m_Toplevel = nullptr;

	// The window's own two nodes. Null while unmapped.
	EntityId m_Window{};
	EntityId m_Content{};

	// What the client last declared through `set_window_geometry`, staged and current — it is
	// double-buffered like everything else a surface carries, so it lands with the commit that follows
	// it rather than mid-frame.
	PixelRect<SurfaceSpace> m_PendingGeometry{};
	PixelRect<SurfaceSpace> m_Geometry{};
	bool m_GeometryStaged = false;

	// The serial of the configure the client owes an acknowledgement for, and whether it has ever given
	// one. A buffer before the first ack is `unconfigured_buffer`.
	std::uint32_t m_Serial = 0;
	bool m_Configured = false;
	bool m_Acked = false;
};

// One client's `xdg_wm_base`.
class ClientShell final : public Wayland::Server::XdgWmBaseHandler
{
public:
	explicit ClientShell(HostContext& context) noexcept : m_Context{ &context } {}

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	Wayland::Server::XdgPositionerHandler* OnCreatePositioner() override { return new ClientXdgPositioner{}; }

	Wayland::Server::XdgSurfaceHandler* OnGetXdgSurface(Wayland::Server::WlSurface surface) override;

	// The answer to a ping gyro has not sent. Nothing pings yet — the unresponsive-client check wants a
	// timer and a policy about what to do with the answer, and both belong with input.
	void OnPong(std::uint32_t serial) override { (void)serial; }

private:
	HostContext* m_Context = nullptr;
};

// The global itself, owned by whoever advertises it and outliving every client that binds it.
class ShellGlobal final : public Wayland::Server::XdgWmBaseBinding
{
public:
	explicit ShellGlobal(HostContext& context) noexcept : m_Context{ &context } {}

	Wayland::Server::XdgWmBaseHandler* OnBind(wl_client& client, std::uint32_t version) override;

private:
	HostContext* m_Context = nullptr;
};
