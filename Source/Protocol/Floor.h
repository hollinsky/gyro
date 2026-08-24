#pragma once

#include "Core/Handle.h"
#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Scene/Commit.h"
#include "Scene/Store.h"

// Where a client's window hangs, and who puts it somewhere when no shell will.
//
// **Every container a window can occupy is gyro's, and a client's window is parented into its
// session's floor at commit (141).** The shell declares containers and moves windows between them; it
// never authors one. The reason is what happens when the shell dies: with custody in the shell, a
// shell crash retires a subtree full of nodes somebody else authored, and the rule that a retiring
// subtree is single-authored (114) has to grow a case for foreign children. With the containers
// gyro's, the chrome blinks out and back and not one window moves — which is what a person
// experiences as *the crash I did not cause did not cost me my arrangement*.
//
// **A window is shown when it is placed, and placement has an author.** With a shell, the shell
// places, and the entrance starts when the placement lands. With none, the Floorplanner below places
// immediately — so there is no *is there a shell?* branch on the window and no second visibility
// state, and the no-shell gap closes to nothing because the placer runs at once.
//
// **There is one floor here and it stands for a session.** Sessions have no model yet — no agent, no
// per-session tree — so the host authors one floor at startup and every client's window goes under
// it. The shape is the one 141 describes and the missing piece is above it rather than inside it.

// The default container, authored once and outliving every client under it.
class SessionFloor
{
public:
	// Author the container. Called once, from the host's `Open`, which is after the composition root
	// has given the store its outputs and before any client can have committed anything.
	[[nodiscard]] Result<void> Open(SceneStore& scene);

	// What a window is parented into. Null before `Open`.
	[[nodiscard]] EntityId Container() const noexcept { return m_Container; }

private:
	EntityId m_Container{};
};

// Decision 141's Floorplanner: **centred on the output holding the pointer, at its natural size,
// newest on top.**
//
// With no input devices the pointer is taken to be centred on the first output, which is the whole of
// what *holding the pointer* can mean today. Newest on top costs nothing to honour because the store
// appends — the sibling list is the z order (55), so the window just created is already the frontmost.
//
// **No cascade offset and no tiling, and the refusal is the guard doing its work.** A cascade needs a
// number and a tile needs a rule, and either is gyro making the window-management decision that
// belongs to a shell (51). Centre-on-pointer is the one placement that takes no parameter. Two windows
// landing exactly on top of each other is acceptable and is honest signal that nothing is managing
// them; in the two configurations this runs in — development with no session agent, and the gap while
// a shell restarts — there are one or two windows anyway.
//
// Does nothing where the world has no outputs, which is a scene nothing is drawing in any case.
void PlaceOnFloor(SceneCommit& commit, const SceneStore& scene, EntityId window, Size<SurfaceSpace, float> natural);
