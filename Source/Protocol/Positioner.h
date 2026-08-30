#pragma once

#include <cstdint>
#include <optional>

#include "Geometry/Space.h"
#include "Wayland/Server/XdgShell.h"

// Where a popup goes: `xdg_positioner` as arithmetic, with no protocol object and no world around it.
//
// **This is the one piece of window placement the *protocol* specifies rather than the shell**, which
// is what makes it gyro's to implement despite decision 51 keeping placement out of the compositor and
// decision 141's guard keeping the Floorplanner down to a rule with no parameter in it. A client that
// opens a menu does not ask to be placed somewhere; it states an anchor rectangle on its own window, a
// corner of that rectangle to hang from, a direction to hang in, and what it would rather have happen
// than run off the edge of a screen. Every one of those is the client's own layout arriving as data,
// and a compositor that resolved them differently from the next would be a menu that lands under the
// pointer on one machine and beside it on another.
//
// **It is a file of its own because none of it needs a client, a store or an output.** The whole of
// the algorithm is two rectangles and a handful of enumerations, so it is a function that can be
// checked against the cases that actually break — a menu at the right edge of a screen, one taller
// than the screen it is on, one flipped and still constrained — with no socket, no node, and no
// panel. That is exactly the split [Floor.h](Floor.h) draws for the Floorplanner and it earns its
// keep here for a second reason: the resolution runs again on every reposition and on every reactive
// recompute, so the arithmetic is on a path a person can feel.
//
// **The behaviour is Mutter's, deliberately, and it is not quite the protocol's prose.** The spec
// describes sliding as a two-step walk towards the gravity and then away from it; every compositor
// that ships implements the clamp below instead, and the two differ only where a popup is larger than
// the screen — where the clamp pins the top-left corner and the walk pins whichever edge the gravity
// names. Toolkits are written against what ships. Reading `src/core/constraints.c` is also what
// settled the question this file would otherwise have got wrong: **the offset is not mirrored when
// the placement flips.** Mirroring it is the tidier reading of *invert the anchor and gravity*, and
// it would put a flipped menu on the wrong side of the control it belongs to on every toolkit that
// offsets by a shadow margin.

// What a client said through `xdg_positioner`, as numbers rather than as a resource.
//
// Every field is in the coordinate space of the *parent's window geometry*, which is what the
// protocol states the anchor rectangle is measured in and what the popup's own position comes back
// in. Nothing here is in global space, so nothing here needs the world.
struct PopupPlacement
{
	// The size the client wants, which is what `set_size` states and is not negotiable — gyro answers a
	// toplevel `0 x 0` for *pick your own*, and there is no such answer for a popup. Only the resize
	// constraint adjustment may shrink it, and only where the client asked for that.
	PixelSize<SurfaceSpace> Extent{};

	// The rectangle on the parent this popup hangs off: a menu bar button, a combo box, a text caret.
	PixelRect<SurfaceSpace> AnchorRect{};

	// Whether the client ever stated one. **A separate bit rather than a test on the rectangle**,
	// because a caret is an anchor rectangle with no width and a control at a window's origin is one at
	// zero, zero — so every value a *stated* anchor rectangle can take is also the value an unstated one
	// has, and there is nothing left to infer completeness from.
	bool AnchorStated = false;

	Wayland::Server::XdgPositionerAnchor Anchor = Wayland::Server::XdgPositionerAnchor::None;
	Wayland::Server::XdgPositionerGravity Gravity = Wayland::Server::XdgPositionerGravity::None;

	Wayland::Server::XdgPositionerConstraintAdjustment Adjustment =
		Wayland::Server::XdgPositionerConstraintAdjustment::None;

	// The client's own nudge off the anchor point, which is almost always a toolkit's shadow margin.
	PixelOffset<SurfaceSpace> Offset{};

	// Whether the client wants this resolved again whenever the parent moves under it, per
	// `set_reactive`. It changes nothing about the arithmetic and is carried here because this struct
	// is the whole of what a popup remembers about how it was placed.
	bool Reactive = false;

	// What the client believed about its parent when it built this, per `set_parent_size` and
	// `set_parent_configure`. Recorded and unread: both exist so that a compositor resizing a window
	// can resolve a popup against the size the client *will* have rather than the one it has, and gyro
	// never resizes a window (51). The `optional` is the distinction that matters if that ever changes
	// — a client that said nothing and a client that said zero are different clients.
	std::optional<PixelSize<SurfaceSpace>> ParentExtent;
	std::optional<std::uint32_t> ParentConfigure;

	// Whether the client said enough for this to mean anything, which the protocol spells as a size and
	// an anchor rectangle both having been set. A positioner missing either is `invalid_positioner` at
	// the request that uses it rather than at the request that left it incomplete, because a client is
	// entitled to build one across as many requests as it likes.
	//
	// A zero-width anchor rectangle is legal and common — a caret is exactly that — so the test on it is
	// that it was *stated* rather than anything about its extent.
	[[nodiscard]] constexpr bool IsComplete() const noexcept { return !Extent.IsEmpty() && AnchorStated; }
};

// The popup's rectangle in the parent's window-geometry space, with nothing done about the edges of
// the screen. This is the placement the client described and would get on an infinite desktop.
[[nodiscard]] PixelRect<SurfaceSpace> PlacePopup(const PopupPlacement& rules) noexcept;

// The same placement, adjusted to fit inside `bounds` by the means the client permitted: flip, then
// slide, then resize, per axis, in the precedence the protocol fixes.
//
// `bounds` is the area a popup should stay inside, in the same space as everything in `rules` — for
// gyro that is an output's rectangle carried down into the parent's coordinates, because there is no
// panel furniture yet for a work area to exclude. **An empty `bounds` means no constraining**, which
// is the honest answer for a parent that is on no output at all rather than a reason to invent one.
[[nodiscard]] PixelRect<SurfaceSpace> PlacePopup(const PopupPlacement& rules, PixelRect<SurfaceSpace> bounds) noexcept;
