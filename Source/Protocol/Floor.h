#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Core/Handle.h"
#include "Core/Result.h"
#include "Core/Session.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Space.h"
#include "Scene/Commit.h"
#include "Scene/Output.h"
#include "Scene/Store.h"

struct wl_client;

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
// **The floor is a root rather than a container under one, and the second root is the shell's.**
// Decision 141 has gyro authoring more containers on a shell's declaration and says they will be
// *roots carrying the same session*, which the run already represents — and decision 187 is the first
// of them: a chrome root per session, created beside the floor and after it, holding the surfaces a
// shell draws.
//
// **It is a second root rather than a raised child of the floor**, and one line decides it: decision
// 55 makes the sibling list the paint order, so a click that raises a window (162) moves that window
// to the end of the chain it is in. With a launcher in the same chain, clicking any window behind it
// would put that window in front of it — the person's own click hiding the thing they were typing
// into. Two roots make the ordering structural instead: everything on the floor is behind everything
// on the chrome root whatever either chain does to itself, and neither `Raise` can reach across.
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
	//
	// **Both roots are authored here, in this order**, because the order *is* the z order: the chrome
	// root is created second and so is the later root, which decision 55 makes the frontmost. Nothing
	// re-raises it and nothing needs to — a root is only appended when a session opens, and the pointer
	// glyph, which is the one node that must stay in front of even the shell, re-raises itself every
	// dispatch iteration for exactly this reason (`Scene/Cursor.h`).
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

	// What a *chrome* surface of this session is parented into (187), or null on the same terms as
	// `Container` — a session that has ended, or one that never had a floor.
	//
	// A shell's launcher and an application's window are told apart by exactly this call and nothing
	// else in the scene: there is no flag on the entity, because what being chrome means is which of
	// two chains a node hangs in, and a node already carries the chain it is in.
	[[nodiscard]] EntityId Chrome(SessionId session) const noexcept;

	// The container this session's shell declared under that name, or null where it has not declared
	// one (190).
	//
	// **The name is the shell's and the container is gyro's, which is the whole of why there is a
	// lookup here at all.** A shell that crashes takes every wire object it held with it and not one
	// window moves; when it comes back it asks for `workspace-2` again and is handed the same node,
	// with the same windows still in it and still where they were. What a person gets out of that is
	// that a crash they did not cause does not cost them their arrangement — which is decision 141's
	// argument for gyro owning containers, delivered.
	[[nodiscard]] EntityId Declared(SessionId session, std::string_view name) const noexcept;

	// Declare one under that name, or hand back the one already declared under it. Null where the
	// session has no floor, and where the entity space is exhausted.
	//
	// **The position is applied at birth and never animated**, which is the one place a shell states a
	// coordinate with no transition over it: a container being created has nowhere to have come from,
	// so there is nothing for a motion to describe. That is what lets `gyro_scene_v1.commit` refuse to
	// carry *no transition* at all — see decision 190.
	//
	// **The arguments are ignored for a container that already exists**, and that is not a shortcut. A
	// restarting shell asks with whatever it last knew; what survived is the truth, and overwriting it
	// would teleport every window in the container to where the shell guessed.
	[[nodiscard]] EntityId Declare(SceneStore& scene, SessionId session, std::string_view name, Vector3<double> at);

	// The container goes. Whatever is still in it is handed back to the floor at the position it had
	// inside the container, so a window is never left under a node that is on its way out.
	//
	// **Retired rather than destroyed**, which is the path everything in this file takes (114): the
	// node stops being authorable and is freed on the pass its last channel settles. An emptied
	// container has nothing running, so that is the very next pass.
	//
	// Does nothing for a container this session did not declare, which is a shell removing one twice.
	void Undeclare(SceneCommit& commit, SceneStore& scene, SessionId session, EntityId container) noexcept;

	// This client decides where a new window of this session goes, so the Floorplanner stands down
	// (141). False where somebody already holds it, which `Protocol/Scene.h` turns into a protocol
	// error rather than a silent second placer.
	[[nodiscard]] bool ClaimPlacement(SessionId session, wl_client* client) noexcept;

	// Give it back, which is the shell exiting or its `gyro_scene_v1` being destroyed. Ignored where
	// this client is not the holder, so one of a client's two scene objects going does not take the
	// other's claim with it.
	void ReleasePlacement(SessionId session, const wl_client* client) noexcept;

	// Whether anything is placing this session's windows. **The one branch decision 141 says is not on
	// the window**, and it is not: the window is invisible until it is placed either way, and this
	// decides only which author places it. A session nobody has claimed is placed by the Floorplanner
	// at the instant the window arrives, which is why the no-shell gap is nothing rather than a wait.
	[[nodiscard]] bool IsPlacing(SessionId session) const noexcept;

private:
	// A session and its floor. A vector and a scan because the count is the people logged into this
	// machine — a map would be a hash and an allocation to search two entries.
	// One container a shell declared, and the name it minted for it.
	struct Declaration
	{
		std::string Name;
		EntityId Container{};
	};

	struct Floor
	{
		SessionId Session = SessionId::None;
		EntityId Container{};
		EntityId Chrome{};

		// The containers this session's shell declared. A vector and a scan because the count is the
		// workspaces on one desktop, and the lookup happens when a shell says a name rather than per
		// frame or per window.
		std::vector<Declaration> Declared{};

		// Who places this session's new windows, or null for the Floorplanner. Keyed on the connection
		// rather than on the object, because what the claim actually says is *this shell is the window
		// manager* and a shell that held two scene objects would still be one shell.
		wl_client* Placer = nullptr;
	};

	[[nodiscard]] const Floor* Find(SessionId session) const noexcept;
	[[nodiscard]] Floor* Find(SessionId session) noexcept;

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

// A window is put somewhere for the first time, which is decision 141's *shown when placed* and is
// therefore also its entrance.
//
// **Two transactions and neither of them is the caller's**, which is the shape `Protocol/Shell.cpp`
// already had for the Floorplanner and which decision 190 hands to a shell without handing over what
// it means. The position lands with no motion — a window has nowhere to travel from before it has
// been anywhere — and the window then arrives at it under `Transition::WindowOpen`, both stamped with
// the same instant, because a person opened one window rather than two things happening to it.
//
// **The transition a shell named is deliberately not consulted here.** A shell that placed a new
// window inside a `WorkspaceSwitch` would have it slide in from wherever the node happened to sit,
// and one that named `WindowOpen` would find its position silently dropped — that entry is silent
// about translation on purpose, since a window that grew *and* slid would have to slide from
// somewhere and nothing in the model says where. So the shell says where and gyro says how it
// arrives, and the timestamp is what the shell is really contributing: the entrance starts when the
// person pressed the key rather than when the shell got round to answering.
//
// `origin` is that instant. `parent` is the container it lands in, which is the session's floor where
// the shell named none.
void PlaceWindow(SceneStore& scene, EntityId window, EntityId parent, Vector3<double> at, Instant origin);

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
