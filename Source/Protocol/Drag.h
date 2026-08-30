#pragma once

#include "Core/Handle.h"
#include "Core/Time.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Space.h"

class SceneStore;

// A window a person is moving or resizing with the pointer.
//
// **This is decision 51's continuous manipulation: a client asks once and gyro runs the gesture.**
// `xdg_toplevel.move` and `xdg_toplevel.resize` each start one, and from then until the button comes up
// nothing goes back to the client asking what to do next. What that buys is two hops of latency on the
// interaction a person judges most harshly — a shell in the loop would make it
// gyro → shell → gyro → client → gyro — and it is the whole of the argument, because the two halves are
// otherwise nothing alike.
//
// **Moving is gyro's outright and resizing is a request.** Decision 166 is the line and it is worth
// reading before either half is changed: a position is a fact this process owns, so a drag writes it
// here, immediately, every dispatch iteration; an extent is the *client's* — only it knows what its
// layout does at a given width — so a resize computes a size, hands it over in a configure, and the
// window becomes whatever the client actually drew. So `Track` writes the world for a move and writes
// nothing at all for a resize.
//
// **Immediate rather than sprung, and that is not the same as *unanimated*.** A drag is the one motion
// whose target is a fact about where a hand is right now: springing it would put the window behind the
// pointer by the spring's own settling time, so the thing being dragged and the thing doing the
// dragging would visibly separate. The channel is written with `Immediate()` and the entrance, the
// snap-back and everything else around a gesture keep their springs.
//
// **The anchor is where the window was seen rather than where it was going.** A window grabbed while
// it is still animating — an entrance in flight, a shell's move that has not settled — is read at its
// presentation position, so the gesture starts from the rectangle the person's finger came down on.
// Taking the model value instead would make the first frame of every interrupted drag a jump.
//
// **No constraints, and the absence is the guard rather than an omission.** Decision 51 has the shell
// declaring snap targets, tiling gravity and the edges a window may not cross, and there is no shell —
// so gyro moves the window exactly as far as the pointer moved and nothing else. Clamping a window to
// its output would be the compositor inventing the one number decision 141 keeps it from inventing, and
// a person who drags a window off the top of a screen has done something they can undo by dragging it
// back. The one bound a resize *does* honour is the client's own minimum and maximum size, which is not
// a shell's declaration at all — it arrives on the wire from the party being resized.
//
// **One gesture at a time, because there is one pointer.** A second `move` or `resize` while one is
// running is refused by the seat rather than queued here: the request has to name a live implicit grab,
// and the grab that opened this gesture is the only one there is until the button comes up.

// Which sides of the window the pointer is pulling.
//
// A value of gyro's own rather than the protocol's bitmask, so that this file names no wire type — and
// because the question asked of it four times below is *does this edge move*, which a mask answers by
// arithmetic and this answers by reading. Opposite edges are never both set: the wire enum has no such
// value, and the shell refuses one that is not in it.
struct ResizeEdges
{
	bool Left = false;
	bool Right = false;
	bool Top = false;
	bool Bottom = false;

	[[nodiscard]] constexpr bool Any() const noexcept { return Left || Right || Top || Bottom; }

	friend constexpr bool operator==(ResizeEdges, ResizeEdges) noexcept = default;
};

class WindowDrag
{
public:
	// Take `window` for a move, anchored at where it is and where the pointer is. False where the entity
	// names nothing live, which is a client asking to move a window that has already gone.
	bool BeginMove(const SceneStore& scene, EntityId window);

	// The same for a resize, which additionally records the extent the window started at: every size
	// this gesture asks for is that one plus how far the pointer has travelled, so a resize cannot drift
	// over a long gesture and cannot compound the client's own rounding.
	//
	// False for the reasons above and for edges naming nothing, which is `xdg_toplevel.resize` with
	// `none` — legal on the wire and a gesture with no direction to run in.
	bool BeginResize(const SceneStore& scene, EntityId window, ResizeEdges edges);

	// Advance the gesture, and end it where there is no longer anything to act on.
	//
	// **A move writes the window's position and a resize writes nothing**, which is decision 166 in one
	// line: what a resize produces is a number for the shell to put in a configure, and the window does
	// not change shape until the client has drawn one.
	//
	// **The window going away ends the gesture here rather than through a hook on the unmap.** A window
	// can leave under a person's hand in three ways — the client destroys the toplevel, the client
	// disconnects, the surface loses its role — and an entity id is generational (15), so the one place
	// that has to notice is the one that looks the id up. A retiring window is gone for this purpose
	// too: it is on screen for as long as its exit takes, and dragging a closing window would be moving
	// something nobody can drop.
	void Track(SceneStore& scene, Instant origin);

	void End() noexcept { m_Window = {}; }

	[[nodiscard]] bool IsActive() const noexcept { return !m_Window.IsNull(); }

	// Whether the gesture running is a resize. False when nothing is running at all, so this is the one
	// question the shell asks rather than two.
	[[nodiscard]] bool IsResizing() const noexcept { return IsActive() && m_Resizing; }

	[[nodiscard]] EntityId Window() const noexcept { return m_Window; }

	// The size the pointer is asking for, in the window's own geometry space and before the client's
	// declared minimum and maximum are applied to it — those are the shell's, because they arrive on the
	// wire and are double buffered like everything else a client says.
	//
	// Floored at one so that a person who drags an edge past the far side of the window asks for a
	// window rather than for nothing: zero on the wire means *pick your own size*, which would hand the
	// client back the very freedom the gesture is taking away.
	[[nodiscard]] Size<SurfaceSpace, float> Wanted() const noexcept { return m_Wanted; }

	// Where the window goes now that the client has drawn `actual`.
	//
	// **This is the half of a resize that cannot be delegated, and it is why decision 166 exists.** The
	// edge a person is holding must not move, so the position is derived from the size that *arrived*
	// rather than from the size that was asked for: a client is free to round, to clamp to its own
	// increment, or to ignore the request outright, and a window positioned from the request would jitter
	// its own fixed edge by the difference on every frame of the drag. Pulling the right or the bottom
	// edge moves no origin at all and this answers with the anchor unchanged.
	[[nodiscard]] Vector3<double> Anchored(Size<SurfaceSpace, float> actual) const noexcept;

private:
	// The window being moved or resized, null when no gesture is running.
	EntityId m_Window{};

	bool m_Resizing = false;
	ResizeEdges m_Edges{};

	// Where the window was when the button went down, how big it was, and where the pointer was. Every
	// answer below is one of the first two plus how far the third has travelled — an anchor rather than
	// an accumulation, so an iteration that saw no motion produces the same number again.
	Vector3<double> m_Anchor{};
	Size<SurfaceSpace, float> m_Extent{};
	Point<GlobalSpace> m_From{};

	// The last size `Track` computed, which is what the shell turns into a configure.
	Size<SurfaceSpace, float> m_Wanted{};
};
