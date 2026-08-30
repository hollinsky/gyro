#pragma once

#include "Core/Handle.h"
#include "Core/Time.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Space.h"

class SceneStore;

// A window a person is moving with the pointer.
//
// **This is decision 51's continuous manipulation, and the whole of the claim is that nothing round
// trips.** A client asks once, with `xdg_toplevel.move`, and from then until the button comes up the
// window's position is written here — one write per dispatch iteration, at the rate the hand is
// sampled at, with no request and no reply in the loop. What a person feels is a window that is stuck
// to the cursor rather than one that follows it a frame or two behind, which is the difference every
// shell-side implementation pays for and the reason the mechanism is gyro's.
//
// **Immediate rather than sprung, and that is not the same as *unanimated*.** A drag is the one motion
// whose target is a fact about where a hand is right now: springing it would put the window behind the
// pointer by the spring's own settling time, so the thing being dragged and the thing doing the
// dragging would visibly separate. The channel is written with `Immediate()` and the entrance, the
// snap-back and everything else around a gesture keep their springs.
//
// **The anchor is where the window was seen rather than where it was going.** A window grabbed while
// it is still animating — an entrance in flight, a shell's move that has not settled — is read at its
// presentation position, so the drag starts from the rectangle the person's finger came down on. Taking
// the model value instead would make the first frame of every interrupted drag a jump.
//
// **No constraints, and the absence is the guard rather than an omission.** Decision 51 has the shell
// declaring snap targets, tiling gravity and the edges a window may not cross, and there is no shell —
// so gyro moves the window exactly as far as the pointer moved and nothing else. Clamping a window to
// its output would be the compositor inventing the one number decision 141 keeps it from inventing, and
// a person who drags a window off the top of a screen has done something they can undo by dragging it
// back. What that costs is stated in Docs/Open.md beside the constraint set it belongs to.
//
// **One drag at a time, because there is one pointer.** A second `move` while one is running is
// refused by the seat rather than queued here: the request has to name a live implicit grab, and the
// grab that opened this drag is the only one there is until the button comes up.
class WindowDrag
{
public:
	// Take `window`, anchored at where it is and where the pointer is. False where the entity names
	// nothing live, which is a client asking to move a window that has already gone.
	bool Begin(const SceneStore& scene, EntityId window);

	// Write this iteration's position, and end the drag where there is no longer anything to move.
	//
	// **The window going away ends the drag here rather than through a hook on the unmap.** A window can
	// leave under a person's hand in three ways — the client destroys the toplevel, the client
	// disconnects, the surface loses its role — and an entity id is generational (15), so the one place
	// that has to notice is the one that looks the id up. A retiring window is gone for this purpose
	// too: it is on screen for as long as its exit takes and dragging a closing window would be moving
	// something nobody can drop.
	void Track(SceneStore& scene, Instant origin);

	void End() noexcept { m_Window = {}; }

	[[nodiscard]] bool IsActive() const noexcept { return !m_Window.IsNull(); }

	[[nodiscard]] EntityId Window() const noexcept { return m_Window; }

private:
	// The window being moved, null when no drag is running.
	EntityId m_Window{};

	// Where it was when the button went down, and where the pointer was. The position written is the
	// first plus how far the second has travelled — an anchor rather than an accumulation, so a drag
	// cannot drift over a long gesture and an iteration that saw no motion writes the same number again.
	Vector3<double> m_Anchor{};
	Point<GlobalSpace> m_From{};
};
