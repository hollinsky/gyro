#pragma once

#include <vector>

#include "Core/Handle.h"
#include "Core/Result.h"
#include "Core/Session.h"
#include "Geometry/Space.h"
#include "Scene/Commit.h"
#include "Scene/Output.h"
#include "Scene/Store.h"

// Where a client's window hangs, and who does the shell's job while there is none.
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
// **One floor per session, authored when its agent's listener is adopted and retired when the agent
// goes away.** Decision 21 keeps every connected session alive at once, so *the floor* is not a thing
// the machine has one of — two people logged in are two floors, two sets of windows, and one of them
// on screen. The floor is the session's root, which is what `World/Root.h` names and the frame thread
// gates on: a session nobody is showing costs one comparison per frame rather than one per window in
// it.
//
// **The floor is the root rather than a container under one, and that is a refusal to build ahead.**
// A session with a workspace and a panel in it wants more containers, and decision 141 has gyro
// authoring them on the shell's declaration — but they will be *roots carrying the same session*,
// which the run already represents, so there is nothing an extra node above the floor would buy today
// beyond a node in every walk.
//
// **A development run has one floor and its session is `None`**, which is the same object doing the
// same job: `HostListener::Own` binds a socket with no agent behind it and no session to attribute a
// client to, and a root of no session is gyro's own and is drawn on every output. So the no-agent path
// is not a branch here, it is the ordinary case with the ordinary answer.

// Every session's floor, and the one lookup a commit does to find the right one.
class SessionFloors
{
public:
	// Author a session's floor. Called when its listener is adopted, before that listener can have
	// admitted a client, so that a window never arrives before the container it hangs under.
	//
	// **Before the socket rather than at the first window**, which is `ClientHost::Open`'s reason one
	// session down: a container created on demand is one whose failure lands in the middle of a
	// client's commit, where the only answer left is to end that client for something gyro did. Here a
	// session that cannot be given a floor is a session that is not served at all, and the agent is
	// told its offer failed.
	//
	// Refused for a session that already has one, which would be a second offer accepted for a uid that
	// `Session/Control.h` allows only one of.
	[[nodiscard]] Result<void> Open(SceneStore& scene, SessionId session);

	// The session ended, so its floor does. Retires the subtree rather than destroying it, which is the
	// path a client exiting already takes (114) — the windows on it animate out and are freed when they
	// have settled, so logging out is a screen emptying rather than a frame with everything gone.
	//
	// Does nothing for a session that has no floor, which is every session under `HostListener::Own`
	// except the one this authored for `None`.
	void Close(SceneStore& scene, SessionId session) noexcept;

	// What a window of this session is parented into, or null where the session has no floor. Null is
	// the answer a commit from a client whose session has already ended gets, and it is why the caller
	// tests rather than parenting into whatever came back.
	[[nodiscard]] EntityId Container(SessionId session) const noexcept;

private:
	// A session and its floor. A vector and a scan because the count is the people logged into this
	// machine — a map would be a hash and an allocation to search two entries.
	struct Floor
	{
		SessionId Session = SessionId::None;
		EntityId Container{};
	};

	std::vector<Floor> m_Floors;
};

// The screen a window of this session belongs to, or null where the session is being shown on none.
//
// **Two rules and the second is the fallback**, which is the order a window's life runs in: a mapped
// window is on the output it covers most of, and a window with no place in the world yet is on the one
// the Floorplanner is about to put it on. The first is what a person would say — *the monitor the
// window is on* — and the second is the only honest answer before there is a rectangle to ask about,
// which is where `xdg_toplevel.configure_bounds` needs one most: a toolkit sizes its first frame
// against whatever it is told here, and hearing nothing is what has it open two thirds the size it
// meant to on a fractionally scaled panel.
//
// A null `window` asks the second question directly, which is what `PlaceOnFloor` itself does.
[[nodiscard]] const SceneOutput* OutputFor(const SceneStore& scene, SessionId session, EntityId window);

void PlaceOnFloor(
	SceneCommit& commit,
	const SceneStore& scene,
	SessionId session,
	EntityId window,
	Size<SurfaceSpace, float> natural
);

// Decision 162's click-to-focus: **the press that begins a gesture focuses the window under it and
// brings it to the front.**
//
// The second stand-in of the Floorplanner's shape and here for that reason — a policy gyro holds only
// because no shell has declared one, so both of them are in one file and leave in one commit.
//
// `hit` is the node the pointer was on when the button went down, which is a surface some way inside a
// window; `Scene/Hit.h` resolves it to the window, since that is what focus and z order are about.
//
// **It focuses *and* raises, which is two verbs called by one policy.** With no shell there is nothing
// drawing a focus ring — decision 96 gives gyro the ring and nothing authors one yet — and the
// Floorplanner centres every window on the same point, so a click that focused without raising would
// be a click whose whole effect is invisible until the next keystroke.
//
// Does nothing where the hit resolves to no window: the background, the floor itself, or anything gyro
// drew for itself. **Focus stays where it was rather than clearing** — there is nothing else on this
// machine to type into, and a person who clicks empty space and then types means the window they were
// already using.
void FocusByClick(SceneStore& scene, EntityId hit);
