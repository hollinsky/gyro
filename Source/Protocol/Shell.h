#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Handle.h"
#include "Geometry/Space.h"
#include "Protocol/Context.h"
#include "Protocol/Drag.h"
#include "Protocol/Positioner.h"
#include "Protocol/Seat.h"
#include "Protocol/Surface.h"
#include "Wayland/Server/Wayland.h"
#include "Wayland/Server/XdgShell.h"
#include "World/Material.h"

// `xdg_wm_base`: the role that turns a client's rectangle of pixels into a window.
//
// **Advertised at version 1, on `wl_compositor`'s rule: the number is a promise about what gyro
// sends.** Version 2 adds the tiled states, 3 `xdg_popup.reposition` and the reactive positioner, 4 a
// `configure_bounds` event, 5 a `wm_capabilities` event telling the client which of maximise,
// minimise, fullscreen and the window menu it may offer. A client told 5 and sent no capabilities is
// entitled to assume it has all four, which is a titlebar full of buttons that do nothing. Every
// toolkit binds this at whatever version it finds; none requires more than one.
//
// **4 is what the number is now, and it was raised because 1 was making windows the wrong size.**
// `configure_bounds` is how a client is told how much screen it has, and a client that is never told
// derives it from `wl_output`: the mode in device pixels over the integer scale. Decision 164 runs
// panels at fractional scales and `wl_output.scale` can carry only the ceiling of one (56), so on a
// laptop panel deriving 1.37 a toolkit works out a screen half the width of the one the world is laid
// out on and will not let a window be more than about two thirds of it — a person drags the corner out
// and the window springs back, and every window on that panel opens undersized.
//
// **Getting there meant honouring version 3, which is what `SyncPopups` below is.** `reposition` was
// already implemented rather than stubbed, and the positioner already recorded `set_parent_size` and
// `set_parent_configure`; what was missing was `set_reactive` — a mapped popup resolved again when its
// parent moves under it. That was worth nothing while the Floorplanner placed a window once and never
// again (141) and became worth something the day a person could drag one, which is the same decision
// (51) that made this whole file grow gestures.
//
// **5 is not taken and the reason is unchanged**: a client told 5 and sent no `wm_capabilities` is
// entitled to assume it has maximise, minimise, fullscreen and the window menu, which is a titlebar
// full of buttons that do nothing. Version 2's tiled states cost nothing — they are states gyro may
// send and does not — and came along with the rest. Every toolkit binds this at whatever version it
// finds.
//
// **The whole of the mapping protocol is here because the whole of it is one state machine.** A
// client gets an `xdg_surface`, gets an `xdg_toplevel` or an `xdg_popup` from it, commits with *no*
// buffer to ask for a configure, is configured, acks, and only then commits a buffer — and the window
// is mapped at that last step and nowhere else. Splitting those across files would put half the
// sequence where the other half's invariants are not readable. What is *not* here is the arithmetic a
// popup's position comes out of ([Positioner.h](Positioner.h)) and the grab stack a menu is dismissed
// through ([Popup.h](Popup.h)): neither is part of the sequence, and both are testable with no client
// in front of them.

// The advertised version. See above before raising it.
inline constexpr std::uint32_t ShellVersion = 4;

class ClientChrome;
class ClientXdgSurface;

// One `xdg_toplevel`: a window with a title, and the requests a person's window manager would answer.
//
// **Most of them are accepted and do nothing, and that is not a stub.** Maximise, minimise and
// fullscreen are *window management*, which belongs to a shell (51) — gyro's answer to a client that
// asks for them is the same whether there is no shell yet or a shell that declined: the state does not
// change, and the protocol's contract is that a client learns what it got from the next configure
// rather than from a reply. So a request that changes nothing is answered by changing nothing, which is
// a legal outcome rather than an unimplemented one.
//
// **`move` is the exception, and decision 51 is why it is not on that list.** Continuous manipulation
// is gyro's mechanism rather than a shell's policy: a drag that round tripped through a shell would
// reach the window two hops after the hand moved, on the interaction a person judges most harshly. So
// the request starts a gesture the compositor runs at pointer rate ([Drag.h](Drag.h)) and the shell's
// half of it — snap targets, tiling gravity, the edges a window may not cross — is declared ahead of
// time rather than per event, and is empty until there is a shell to declare it. `resize` is the same
// mechanism and is not built: it is the first thing that would make a configure carry a size, which is
// the one number this file currently refuses to invent.
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

	// Decision 187: this toplevel is a surface the shell draws rather than a window on the desktop.
	//
	// **Set once and never cleared, which is the protocol's rule and not an implementation shortcut.**
	// `gyro_chrome_manager_v1.get_chrome` refuses a toplevel that has already mapped, so what is being
	// asked at every point that reads this is settled before the surface enters the world — and a
	// surface that could stop being chrome halfway through its life would be a launcher falling behind
	// the windows while a person was typing into it.
	[[nodiscard]] bool IsChrome() const noexcept { return m_Chrome; }

	// Whether this window is in the world, asked of the `xdg_surface` that owns the answer. False for a
	// toplevel whose surface was destroyed out of order, which is the same answer for the same reason:
	// there is nothing on screen.
	[[nodiscard]] bool IsMapped() const noexcept;

	// The declaration itself, and the back link that keeps the two objects from outliving each other in
	// either order. Refused where this toplevel is already chrome, which is the caller's error to post.
	[[nodiscard]] bool BecomeChrome(ClientChrome& object) noexcept;

	// The chrome object went away. The toplevel stays chrome — see `IsChrome` — and simply loses the
	// ability to be redressed.
	void ForgetChrome() noexcept { m_ChromeObject = nullptr; }

	// The material staged by `gyro_chrome_v1.set_material`, landing at the next commit like everything
	// else a client says. `Dress` is what actually arrived.
	void StageMaterial(Material material) noexcept
	{
		m_PendingMaterial = material;
		m_MaterialStaged = true;
	}

	void ApplyMaterial() noexcept;

	[[nodiscard]] Material Dress() const noexcept { return m_Material; }

	void OnSetParent(Wayland::Server::XdgToplevel parent) override { (void)parent; }

	void OnSetTitle(std::string_view title) override { m_Title = title; }

	void OnSetAppId(std::string_view appId) override { m_AppId = appId; }

	void OnShowWindowMenu(Wayland::Server::WlSeat seat, std::uint32_t serial, std::int32_t x, std::int32_t y) override;

	void OnMove(Wayland::Server::WlSeat seat, std::uint32_t serial) override;

	void
	OnResize(Wayland::Server::WlSeat seat, std::uint32_t serial, Wayland::Server::XdgToplevelResizeEdge edges) override;

	void OnSetMaxSize(std::int32_t width, std::int32_t height) override;

	void OnSetMinSize(std::int32_t width, std::int32_t height) override;

	// **The four state requests gyro declines, and declining them is still an answer.** Maximise and
	// fullscreen are window management a shell owns (51), so gyro's policy is that the state does not
	// change — which the protocol permits in as many words: *whether the client is actually put into a
	// fullscreen state is subject to compositor policies*. What it does not permit is silence. The same
	// paragraph opens *the compositor will respond by emitting a configure event*, and a client that
	// asked and heard nothing is not a client that learned it was refused: it is one still waiting.
	//
	// **What that cost was a browser whose fullscreen button did nothing at all.** Firefox asks, holds
	// its transition open for the configure that says what it got, and never completes it — so the
	// page's own `fullscreenchange` never fires and a person clicking the control on a video sees the
	// frame they were already looking at. The window not growing is the policy; the button being dead
	// is the bug, and only the second one is visible to them.
	//
	// So each of these answers with the window exactly as it is. `ClientXdgSurface::Configure` sends the
	// current size and states under a fresh serial, which is precisely *you asked, and here is what you
	// have* — and a toolkit reads the absent state and puts its own UI back.
	//
	// **`set_minimized` is the one that stays silent, and the protocol is why**: it is the request with
	// no state to carry back and no way to observe the result, so there is nothing a configure could
	// say. A client is told in the interface description that it cannot know.
	void OnSetMaximized() override;

	void OnUnsetMaximized() override;

	void OnSetFullscreen(Wayland::Server::WlOutput output) override;

	void OnUnsetFullscreen() override;

	void OnSetMinimized() override {}

	// Whether the client has been told it has the keyboard, which is the whole of what makes a toolkit
	// draw its titlebar live rather than grey.
	//
	// **It is what has been *said* rather than what is true**, which is the distinction
	// [Seat.h](Seat.h) draws over focus for the same reason: `Scene/Focus.h` holds the fact, this holds
	// the last thing sent, and a configure goes out exactly where the two disagree. Keeping only the
	// fact would mean either a configure per dispatch iteration or a signal out of `Scene`, and keeping
	// only this one would mean a window that is focused and does not know it.
	[[nodiscard]] bool Activated() const noexcept { return m_Activated; }

	// Record what the next configure will carry, answering whether it changed — which is the whole of
	// the question *is a configure owed*.
	bool SetActivated(bool activated) noexcept
	{
		const bool changed = activated != m_Activated;

		m_Activated = activated;

		return changed;
	}

	// Whether the client has been told it is being resized, held for `m_Activated`'s reason and read
	// the same way: the fact is the seat's gesture and this is the last thing sent.
	bool SetResizing(bool resizing) noexcept
	{
		const bool changed = resizing != m_Resizing;

		m_Resizing = resizing;

		return changed;
	}

	// The size the next configure will carry, clamped to what the client itself declared it can be.
	//
	// **Zero is *pick your own size* and is where every window starts** — decision 141 places a window
	// at its natural size, so gyro has nothing to say about how big it should be until a person takes
	// hold of an edge. From the first resize onward it does, and it keeps saying it: going back to zero
	// afterwards would tell a client it may return to its natural size, which is a window that snaps
	// back the moment anything else makes gyro configure it.
	bool SetSize(PixelSize<SurfaceSpace> size) noexcept
	{
		const PixelSize<SurfaceSpace> clamped = Clamp(size);
		const bool changed = clamped != m_Size;

		m_Size = clamped;

		return changed;
	}

	// The room this window is told it has, which is the screen it is on: `xdg_toplevel.configure_bounds`,
	// and *not* the `ApplyBounds` below — that one is the client's own minimum and maximum, coming the
	// other way.
	//
	// **This is the number a toolkit sizes its first frame against, and gyro withholding it is what
	// makes a window open the wrong size.** A client that is told nothing works the bounds out from
	// `wl_output`: the mode in device pixels divided by the integer scale. Decision 164 runs panels at
	// fractional scales and `wl_output.scale` can only carry the ceiling of one (56), so on a laptop
	// panel deriving 1.37 a toolkit concludes the screen is half of 1920 rather than the 1405 the world
	// is actually laid out in — and refuses to be more than 68% of it, however far a person drags the
	// corner. Saying it directly is the only fix that does not involve lying about the mode.
	//
	// Zero is *unknown*, which is what the protocol says to send when there is nothing to say and is
	// what a window whose session is on no output gets.
	bool SetRoom(PixelSize<SurfaceSpace> room) noexcept
	{
		const bool changed = room != m_Room;

		m_Room = room;

		return changed;
	}

	// The client's own minimum and maximum, applied to a size the pointer asked for. Zero in either
	// axis of either bound is *no limit*, which is what almost every client sends and what an unset
	// bound means on the wire.
	//
	// **These are the one constraint a resize honours, and they are not the shell's** — decision 51 has
	// a shell declaring snap targets and tiling gravity, and there is none, but a minimum size arrives
	// from the party being resized rather than from a policy. A compositor that ignored it would ask a
	// text editor to be forty pixels wide and get back a window that is not, which is the same picture
	// as a resize that does not work.
	[[nodiscard]] PixelSize<SurfaceSpace> Clamp(PixelSize<SurfaceSpace> size) const noexcept;

	// Send `xdg_toplevel.configure` with the size and the states this window is in. No arguments,
	// because both are this object's: the `xdg_surface` decides *when* and the toplevel decides *what*.
	void Configure() const;

	// The staged minimum and maximum arriving in the world, called by the `xdg_surface` from the commit
	// that carries them — they are double buffered like everything else a client says, so a toolkit
	// that shrinks its minimum and redraws in one commit is never seen half applied.
	void ApplyBounds();

private:
	// The answer the four declined state requests share: configure with what this window already is.
	//
	// Gated on the surface still being there for `OnMove`'s reason — an `xdg_surface` destroyed out of
	// order leaves this object alive with nothing behind it.
	void AnswerUnchanged();

	ClientXdgSurface* m_Surface = nullptr;

	std::string m_Title;
	std::string m_AppId;

	bool m_Activated = false;
	bool m_Resizing = false;

	// What the last configure said, so that a comparison rather than a signal decides whether one is
	// owed — the same shape `m_Activated` has.
	PixelSize<SurfaceSpace> m_Size{};

	// The screen this window was last told it has, held for `m_Size`'s reason: a person dragging a
	// window onto a second monitor changes it, and nothing else does.
	PixelSize<SurfaceSpace> m_Room{};

	// What the client says it can be, staged and current.
	PixelSize<SurfaceSpace> m_PendingMin{};
	PixelSize<SurfaceSpace> m_PendingMax{};
	PixelSize<SurfaceSpace> m_Min{};
	PixelSize<SurfaceSpace> m_Max{};
	bool m_BoundsStaged = false;

	// Decision 187. The flag and the object are deliberately two things: the flag is what this toplevel
	// *is* and outlives every object, and the pointer is only the client's handle on it.
	bool m_Chrome = false;
	ClientChrome* m_ChromeObject = nullptr;

	// What the shell says this surface is made of, staged and current, double buffered for the reason
	// the bounds are.
	Material m_PendingMaterial = Material::None;
	Material m_Material = Material::None;
	bool m_MaterialStaged = false;
};

// One `xdg_positioner`: the client's description of where a popup should go, accumulated across as
// many requests as it likes and read once, by the `get_popup` that names it.
//
// **It is a value that is copied out rather than an object a popup keeps a pointer to**, which is the
// protocol's own rule and not a convenience: a client is explicitly permitted to destroy the
// positioner the instant `get_popup` returns, and every toolkit does. What survives is
// [Positioner.h](Positioner.h)'s `PopupPlacement`, which is the whole of what was said.
class ClientXdgPositioner final : public Wayland::Server::XdgPositionerHandler
{
public:
	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	[[nodiscard]] const PopupPlacement& Rules() const noexcept { return m_Rules; }

	void OnSetSize(std::int32_t width, std::int32_t height) override;

	void OnSetAnchorRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override;

	void OnSetAnchor(Wayland::Server::XdgPositionerAnchor anchor) override { m_Rules.Anchor = anchor; }

	void OnSetGravity(Wayland::Server::XdgPositionerGravity gravity) override { m_Rules.Gravity = gravity; }

	void OnSetConstraintAdjustment(Wayland::Server::XdgPositionerConstraintAdjustment adjustment) override
	{
		m_Rules.Adjustment = adjustment;
	}

	void OnSetOffset(std::int32_t x, std::int32_t y) override { m_Rules.Offset = { x, y }; }

	// The three a client bound below version 3 cannot send, which since `ShellVersion` reached 4 is only
	// one that bound low on purpose. [Positioner.h](Positioner.h) says what each is for: `set_reactive` is
	// acted on by `ClientXdgSurface::SyncPopups`, and the other two together by `PendingShift`.
	void OnSetReactive() override { m_Rules.Reactive = true; }

	void OnSetParentSize(std::int32_t width, std::int32_t height) override
	{
		m_Rules.ParentExtent = PixelSize<SurfaceSpace>{ width, height };
	}

	void OnSetParentConfigure(std::uint32_t serial) override { m_Rules.ParentConfigure = serial; }

private:
	PopupPlacement m_Rules;
};

// One `xdg_popup`: a menu, a tooltip, a combo box's list — a surface positioned against a rectangle of
// its parent rather than placed by anybody.
//
// **A popup is a window in the store like any other, and the only thing that makes it a popup is where
// it hangs.** It is parented into its parent's own container rather than into the floor, so it moves
// with the window it belongs to, draws over it because the sibling list is the z order (55), and is
// not clipped by it because the scene vocabulary has no clipping — which `Scene/Hit.h` relies on in
// exactly the same direction, so what is drawn outside the parent is also what catches the pointer.
//
// **What gyro decides here is nothing, and that is the point.** Decision 141's guard keeps placement
// policy out of the compositor, and a popup is the one window whose position is not a policy at all:
// the client states an anchor, a gravity and what to do at the edge of a screen, and the compositor
// does arithmetic. The only judgement is which rectangle counts as *the screen*, and with no panels
// and no struts that is the output the anchor is on.
class ClientXdgPopup final : public Wayland::Server::XdgPopupHandler
{
public:
	ClientXdgPopup(HostContext& context, ClientXdgSurface& surface, ClientXdgSurface* parent, PopupPlacement rules);

	~ClientXdgPopup() override;

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	void OnGrab(Wayland::Server::WlSeat seat, std::uint32_t serial) override;

	void OnReposition(Wayland::Server::XdgPositioner positioner, std::uint32_t token) override;

	[[nodiscard]] const PopupPlacement& Rules() const noexcept { return m_Rules; }

	[[nodiscard]] ClientXdgSurface* Parent() const noexcept { return m_Parent; }

	// The `xdg_surface` this popup is the role of, which is what a *is this the topmost popup* question
	// is really asking about — the protocol names the surface in `get_popup`, not the role object.
	[[nodiscard]] ClientXdgSurface* Surface() const noexcept { return m_Surface; }

	[[nodiscard]] bool IsGrabbing() const noexcept { return m_Grabbing; }

	// The container this popup's window is, or null while it is unmapped. What `PopupStack` compares an
	// ancestor walk against.
	[[nodiscard]] EntityId Node() const noexcept;

	// **The compositor taking the menu away**, which is what a press outside a grab means and what a
	// parent going first leaves no alternative to. The client is told with `popup_done` and takes its
	// own surface down; gyro unmaps immediately rather than waiting, because a menu that stayed on
	// screen until the client got round to it is a menu that outlives the click that dismissed it.
	void Dismiss();

	// The window it hangs on went away. Distinct from `Dismiss` only in that there is nothing left to
	// hang on, so the parent pointer is dropped before the unmap rather than after.
	void ForgetParent() noexcept;

	// The `xdg_surface` went first, which is `ClientXdgToplevel::Forget`'s case and needs the same
	// answer for the same reason.
	void Forget() noexcept { m_Surface = nullptr; }

	// The placement this popup was last configured at, in its parent's window-geometry space.
	[[nodiscard]] PixelRect<SurfaceSpace> Placement() const noexcept { return m_Placement; }

	// Whether that placement has still to be written into the world.
	//
	// **A menu redrawing is not a menu moving**, and the two have to be told apart here because a client
	// commits every frame it animates: writing the position back each time would be a retarget per
	// commit on a channel whose target never changed, which is the cost
	// Docs/Architecture.md#doing-nothing-must-cost-nothing exists to keep out of a window that is only
	// repainting itself.
	[[nodiscard]] bool PlacementPending() const noexcept { return m_PlacementPending; }

	void PlacementApplied() noexcept { m_PlacementPending = false; }

	// Resolve the rules against the world and remember the answer. Called by the `xdg_surface` when it
	// is about to configure, because the client is told a position and a size in the same event.
	void Resolve();

	// Send the `repositioned` a `reposition` is owed, if one is. **Immediately before the configure
	// that carries the new position and never after it**, which is the order the protocol fixes and the
	// order a client's own state machine asserts on: GTK warns about an unexpected `repositioned` and
	// then waits forever for the one it wanted.
	void AnswerReposition();

	// Register with the grab stack, which happens at map rather than at `grab` — the protocol's own
	// ordering, since `grab` must arrive before the first commit and a popup that is never committed
	// never grabs anything.
	void Enter();

	// Leave it again, at the unmap. Idempotent, because a popup can be taken down by its client, by a
	// press outside it, and by its parent going away, and two of those can happen in one iteration.
	void Leave() noexcept;

	// Whether the compositor has already taken this one away. A dismissed popup is inert: it maps
	// nothing, configures nothing, and waits for the client to destroy it.
	[[nodiscard]] bool IsDismissed() const noexcept { return m_Dismissed; }

private:
	// The rectangle a popup should stay inside, in its parent's window-geometry space. Empty where
	// there is no world, no parent, or no output — all three of which mean *do not constrain*.
	[[nodiscard]] PixelRect<SurfaceSpace> Bounds() const;

	HostContext* m_Context = nullptr;

	// This popup's own `xdg_surface`, and the one it is anchored on. Both borrowed, and both told to
	// forget this object before they die.
	ClientXdgSurface* m_Surface = nullptr;
	ClientXdgSurface* m_Parent = nullptr;

	PopupPlacement m_Rules;
	PixelRect<SurfaceSpace> m_Placement{};

	bool m_PlacementPending = false;
	bool m_Grabbing = false;
	bool m_Entered = false;
	bool m_Dismissed = false;

	// The token a `reposition` is owed a `repositioned` for, which goes out immediately before the
	// configure that carries the new position. Only reachable at version 3.
	std::uint32_t m_RepositionToken = 0;
	bool m_Repositioned = false;
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
//
// **A popup runs the identical sequence and differs in two lines**: the configure carries a position
// and a size instead of *pick your own*, and the container is parented into the popup's parent instead
// of into the floor. Everything between — the ack, the buffer, the geometry, the input region, the
// binding a frame callback travels back along — is the same code, because it is the same state
// machine with a different role object on the end of it.
class ClientXdgSurface final : public Wayland::Server::XdgSurfaceHandler, public SurfaceRole
{
public:
	// `surface` is null only where the client named something that was not a `wl_surface` at all. The
	// object still has to exist — the client is holding an id and libwayland needs something behind it
	// — and every request on it then does nothing, which costs nothing because the connection has
	// already been ended by the caller.
	//
	// `base` is the `xdg_wm_base` this surface came from, held because three of the errors a popup can
	// raise — `invalid_positioner`, `invalid_popup_parent`, `not_the_topmost_popup` — belong to that
	// interface rather than to this one. A client reads an error's code against the interface of the
	// object it arrived on, so posting one of them here would be a compositor telling a client
	// `unconfigured_buffer` when it meant *your menu has the wrong parent*.
	ClientXdgSurface(HostContext& context, Wayland::Server::XdgWmBase base, ClientSurface* surface) noexcept
		: m_Context{ &context }, m_Base{ base }, m_Surface{ surface }
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

	// Where a subsurface of this window hangs, and the node it stacks against. Decision 111's toplevel
	// answered as two questions: the container is the window, and the image under it is the client's
	// own pixels — which `wl_subsurface.place_below` names as a legal reference, and which is the whole
	// reason the window is a container rather than the image itself.
	[[nodiscard]] EntityId RoleContainer() const noexcept override { return m_Window; }

	[[nodiscard]] EntityId RoleContent() const noexcept override { return m_Content; }

	// The window in the scene, or null while the surface is unmapped. A test's way of asking whether a
	// window exists without inferring it from a node count.
	[[nodiscard]] EntityId Window() const noexcept { return m_Window; }

	// The `wl_surface` under this role, or null where the client named something that was not one.
	// Public because `wl_surface.enter` is the surface's event while the reach that decides it is the
	// *window's*, and [Output.h](Output.h) is the one party holding both questions.
	[[nodiscard]] ClientSurface* Content() const noexcept { return m_Surface; }

	// Whether this surface has a window in the world right now, which is what a popup's parent has to be
	// before the popup can hang off it.
	[[nodiscard]] bool IsMapped() const noexcept { return !m_Window.IsNull(); }

	// The `xdg_toplevel` this surface's role is, or null for a popup or for a surface whose client has
	// not said yet. **Public because a menu is in the window registry and is not a window**:
	// `HostContext::Windows()` holds every mapped `xdg_surface`, and
	// [Foreign.h](Foreign.h) enumerates toplevels alone — so the party asking needs the role rather than
	// the mapping, and this is the one question that separates them.
	[[nodiscard]] ClientXdgToplevel* Toplevel() const noexcept { return m_Toplevel; }

	// The role object let go, which unmaps the window: a toplevel that is destroyed is a window that is
	// gone, whatever the surface still has attached.
	void Orphan() noexcept;

	// Take the window off the screen while the role object stays. **The difference from `Orphan` is
	// who is still there afterwards**: a dismissed popup keeps its `xdg_popup` until the client
	// destroys it, and a compositor that dropped the role pointer here would answer that destroy
	// against nothing.
	void Withdraw() noexcept;

	// The popups anchored on this surface, which have to be told when it goes. The protocol requires a
	// client to destroy them first and gyro still has to survive one that does not.
	void Adopt(ClientXdgPopup& popup);
	void Release(ClientXdgPopup& popup) noexcept;

	// Send the configure the client is waiting for. Public because a popup's `reposition` produces one
	// outside the commit sequence that otherwise drives them.
	void Configure();

	// Start an interactive move or resize, per `xdg_toplevel.move` and `xdg_toplevel.resize`. Public
	// because the request arrives on the role object and the window it names is this one's; the seat
	// does the deciding, and [Seat.h](Seat.h) has what it decides against.
	void BeginMove(Wayland::Server::WlSeat seat, std::uint32_t serial);

	void BeginResize(Wayland::Server::WlSeat seat, std::uint32_t serial, ResizeEdges edges);

	// The three of `ClientXdgToplevel`'s comparisons reached through the surface, which is the object the
	// window registry holds. False — nothing changed — for a surface whose role is not a toplevel.
	bool SetActivated(bool activated) noexcept;
	bool SetResizing(bool resizing) noexcept;
	bool SetSize(PixelSize<SurfaceSpace> size) noexcept;

	// The fourth, and the one whose argument is the world's rather than the caller's: it reads the
	// screen this window is on rather than being told. Answers whether the room changed, which is a
	// configure owed — a person dragging a window from one panel to another of a different scale is the
	// case, and it is the same walk `SetActivated` above is folded into.
	bool RefreshRoom();

	// Where this window will be once it has answered a configure it has already been sent, as an offset
	// from where it is now — which is `xdg_positioner.set_parent_size` and `set_parent_configure`
	// honoured, and is zero for every popup that named neither.
	//
	// **A client positioning a menu while its own window is being resized is describing a window that
	// does not exist yet.** It says *I am responding to configure S and I will be W by H*, and the whole
	// use of that is which way a menu flips at the edge of a screen: resolving against the window as it
	// stands puts the flip one frame behind the hand, and on the frame a person crosses the threshold
	// the menu jumps the wrong way and back.
	//
	// **In gyro the answer is one subtraction, because a resize is the only thing that makes a window's
	// future differ from its present.** Decision 166 holds the edge a person is not touching still, so
	// the origin is a function of the size the client produced — and the offset between two futures is
	// `Drag::Anchored` asked twice, once about the size the client says it will be and once about the
	// size it is. It needs no expiry: the day the client produces the size it named, the two answers are
	// the same one and this returns zero on its own.
	[[nodiscard]] PixelOffset<SurfaceSpace> PendingShift(const PopupPlacement& rules) const;

	// `xdg_positioner.set_reactive` honoured: the menus hanging off this surface resolved again against
	// where their parent now is, and configured where the answer moved.
	//
	// **What makes this necessary is that a window can be dragged (51).** A popup is placed once, against
	// the screen edges as they stood in its parent's coordinates at the moment it opened; move the window
	// and a menu that flipped upward to stay on screen should flip back, and one that fitted should slide.
	// Without this the menu keeps the placement it was born with, which a person sees as a submenu opening
	// off the side of the screen after they have moved the window it belongs to.
	//
	// **Only the popups that asked, and the walk descends whether or not they did**: a reactive submenu
	// under a plain menu is an ordinary arrangement, so the recursion is unconditional and the resolve
	// is not. Top down, because a popup is positioned against its parent's own space and the parent
	// has to have settled before the child is asked about.
	void SyncPopups();

	// The `xdg_wm_base` this surface came from, for the errors that belong to that interface.
	[[nodiscard]] Wayland::Server::XdgWmBase Base() const noexcept { return m_Base; }

private:
	// Turn a committed surface into a window, or update the one that is already there.
	void Map(ClientSurface& surface);

	// Take the window out of the world. Retires rather than removes, per decision 114 — the subtree
	// keeps its position so that an exit animation has something to run on, and the store frees it on
	// the pass its last channel settles.
	void Unmap();

	// Say out loud what became of an `xdg_toplevel.move` or `xdg_toplevel.resize`, which is otherwise
	// invisible from both ends — see [Seat.h](Seat.h) for why that is worth a log line at all.
	void Report(std::string_view request, std::uint32_t serial, GestureRefusal why);

	// The window's natural size in its own space: what the client declared as its geometry, or the
	// whole surface where it declared none.
	[[nodiscard]] Size<SurfaceSpace, float> Natural(const ClientSurface& surface) const noexcept;

	// Whether the client has said what kind of thing this surface is. A commit before it is a client
	// asking to be configured; a *buffer* before it is `not_constructed`.
	[[nodiscard]] bool HasRole() const noexcept { return m_Toplevel != nullptr || m_Popup != nullptr; }

	HostContext* m_Context = nullptr;

	Wayland::Server::XdgWmBase m_Base;

	// The surface this role was taken on, or null once it has gone. Not owned.
	ClientSurface* m_Surface = nullptr;

	// The role object, or null while the client has an `xdg_surface` and has not yet said what kind of
	// thing it is. At most one is ever set. Not owned — a role is a protocol object with its own
	// lifetime.
	ClientXdgToplevel* m_Toplevel = nullptr;
	ClientXdgPopup* m_Popup = nullptr;

	// The popups hanging off this surface. Borrowed, and each removes itself as it dies.
	std::vector<ClientXdgPopup*> m_Children;

	// The press a gesture request was last reported against, so that a client retrying a refused request
	// says it once rather than once per frame. **A refusal is worth a line and a client's retry loop is
	// not**, and the two are told apart by the serial: a retry quotes the same press, and the next thing
	// worth hearing about is by definition a different one.
	std::optional<std::uint32_t> m_ReportedGesture;

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

	// And the newest it has answered, which with `m_Serial` bounds the configures still in flight — the
	// set `PendingShift` asks a positioner's serial against. Zero is *none yet*, and a renegotiation puts
	// it back there beside `m_Acked`.
	std::uint32_t m_AckedSerial = 0;
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

// Whether focus rests on this window, which is a different question from whether this window *is* the
// focused entity.
//
// **A menu belongs to the window it came out of.** A popup that grabbed takes the keyboard —
// `Scene/Focus.h` holds it, because a person typing while a menu is open is typing into the menu — and
// the window underneath must not go grey while its own menu is up, which is what a person would read
// as the application having lost focus mid-click. So the question is asked up the tree: a popup hangs
// under the window it is anchored on (141), so the walk that finds the window is the same relationship
// that puts the menu over it on screen.
//
// Capped at `MaxReachDepth` for `Scene/Hit.h`'s reason, which also bounds a cycle a bad link could
// make.
[[nodiscard]] bool FocusRestsOn(const SceneStore& scene, EntityId window, EntityId focused);

// Bring every window's idea of itself up to date with the world's, and send nothing where nothing has
// changed. Called from `Advance`, beside the seat's own comparison and for the same reason: the
// keystroke, click or motion that changed something and the step that notices are the same wakeup of
// the same thread, so nothing has to be observed and no signal crosses.
//
// **One walk and one configure per window, because a configure is one event carrying every answer.**
// Focus and the resize gesture change independently and a window that learned about both in one
// iteration would otherwise be told twice — and the second event would restate the first, which is a
// client re-laying out its whole window for news it already had.
//
// **A window that has just mapped learns it is focused on this pass rather than in its first
// configure.** The initial configure is answered before the window exists — a client is asking what
// size to draw and has attached nothing — so there is nothing yet for focus to be on, and predicting
// that it is about to take focus would be copying `Scene/Focus.h`'s newest-on-top rule into a second
// place that could disagree with it. What it costs is that the first frame of a new window is drawn
// unfocused; the configure that corrects it goes out in the same iteration the window mapped in.
//
// **Nothing here waits for the client to catch up, and decision 166 records why the version that did
// was wrong.** xdg-shell already says a client may ignore every configure but the newest and
// acknowledge only that one, so the coalescing a fast pointer needs is the client's and is specified;
// a compositor that withheld sizes until the last was acknowledged would be doing it a second time,
// and would stop dead against any client that declined to answer one.
void SyncWindows(HostContext& context, const SceneStore& scene, EntityId focused, EntityId leaving);

// The global itself, owned by whoever advertises it and outliving every client that binds it.
class ShellGlobal final : public Wayland::Server::XdgWmBaseBinding
{
public:
	explicit ShellGlobal(HostContext& context) noexcept : m_Context{ &context } {}

	Wayland::Server::XdgWmBaseHandler* OnBind(wl_client& client, std::uint32_t version) override;

private:
	HostContext* m_Context = nullptr;
};
