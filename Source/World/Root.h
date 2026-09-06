#pragma once

#include <cstdint>
#include <type_traits>

#include "Core/Session.h"
#include "World/Node.h"

// Which session each top-level node belongs to, as bytes in the snapshot's root run.
//
// **This module exists for World/Node.h's reason and not a weaker one.** Docs/Decisions.md decision
// 21 keeps every connected session alive and presents one of them locally, so a published scene holds
// the roots of every session at once and the frame thread shows the ones the output it is drawing is
// assigned to. `Scene` writes that partition and `Frame` reads it, neither may name the other
// (decision 91), and decision 87 puts a record two parties across a waist both name below both of
// them rather than in `Seam`.
//
// **It is a run beside the node run rather than a field on `Node`, which is decision 95's shape
// applied to a second fact.** A session is meaningful at depth one and nowhere else — every
// descendant of a root is in the session its root is in, or the partition would be a tree of its own
// — so a field on the record would be four bytes every node in the scene drags through cache in
// order not to use, which is the argument World/Node.h already makes for keeping content out of the
// record. Roots number in the sessions on the machine plus what gyro authors for itself; nodes number
// in the windows.
//
// **The node index is carried rather than implied by position.** The run is in the same order the
// preorder walk meets its roots, so the frame thread could count roots as it goes and index by
// ordinal — and then a run that had drifted from the scene by one entry would silently show a
// session's windows on somebody else's screen, which is the one failure this partition exists to make
// impossible. Carrying the index means the walk can compare, and decision 90's rule that the frame
// thread validates what it walks rather than trusting it is the same rule one run over.
struct SceneRoot
{
	// The root's position in the node run. Strictly increasing across the run, because a preorder walk
	// meets its roots in order and emits them as it meets them.
	std::uint32_t Node = 0;

	// The session whose scene this root is part of.
	//
	// **`None` is not an unfilled field: it is gyro's own, and it is shown on every output.** The
	// pointer glyph is the standing case — Scene/Cursor.h authors it as the last root so that it is
	// frontmost (55), and it has to draw over whichever session an output is showing rather than
	// belonging to one of them. The boot splash and the recovery console are `None` for the same
	// reason and stay honest without a third state, because gyro authors and *retires* them around
	// the moment they are on screen rather than keeping them and hiding them.
	//
	// **It holds both ends of the stack rather than one, which is what the second member of it makes
	// clear.** Scene/Background.h is `None` and is the *first* root — authored before any author opens,
	// because a list makes the last root the frontmost and a wallpaper handed over an hour into a
	// session would otherwise land in front of every window on the machine (179). So `None` is not a
	// layer: it is *whose*, and where a root of gyro's own sits is decided by when it was authored,
	// exactly as it is for a session's.
	SessionId Session = SessionId::None;
};

static_assert(std::is_trivially_copyable_v<SceneRoot> && std::is_standard_layout_v<SceneRoot>);
static_assert(sizeof(SceneRoot) == 8, "Two uint32s, and no padding to leave uninitialised");

// What one output is showing, as bytes in the snapshot's session run.
//
// **The transition belongs to the output, which is decision 188, and it is why this is a record
// rather than the bare `SessionId` it was.** Decision 21 says an output is assigned to at most one
// session at a time; a transition is exactly the interval in which that is false, so it cannot be a
// property of either session — a session is shown on several outputs and a switch need not move them
// together, so one number per session would fade both of somebody's monitors when they were asked to
// switch user on one.
//
// **A reassignment carrying no coefficient is the cut, and that is the base case rather than a second
// mode.** `Outgoing` is `None` and `Fade` is `NoCoefficient` for every output that is not mid
// transition, which is every output today and every output a suspend leaves behind — decision 59
// needs the locked state on the glass at the next flip, so it takes the cut. Nothing records which
// kind of transition this is, because the absence *is* the kind.
//
// The two fields go together and the frame thread may assume neither: `Scene/Serializer.h` retires
// the pair as one, so an `Outgoing` with no coefficient behind it is a run that has drifted, and
// [Frame/Evaluator.h](../Frame/Evaluator.h) draws the steady state instead — which fails towards the
// screen a person is arriving at rather than towards the one they are leaving.
struct SceneAssignment
{
	// The session this output is showing, and the one it is moving *to* while a transition is running.
	// Input follows this from the instant the transition is authored rather than from when it settles
	// (188), so the first characters of a password cannot land in the terminal a person just left.
	SessionId Shown = SessionId::None;

	// The session this output is moving away from, still composited live for the length of the fade,
	// or `None` outside one. Live rather than a photograph because the incoming side is live in every
	// design and a snapshot would be a second mechanism for one transition — so a film playing when a
	// laptop is locked goes on playing as it leaves the screen.
	SessionId Outgoing = SessionId::None;

	// Where the outgoing session's opacity is, in the same opacity run a node's own fade is in, or
	// `NoCoefficient` outside a transition. One coefficient rather than two: what a person sees is a
	// departure from the screen they are already looking at, and the arriving session is drawn at
	// full strength underneath from the first frame.
	//
	// It is a *compositor-owned* channel — gyro authors it, no protocol reaches it, and no node names
	// it — which is one of the three properties decision 43's anti-spoofing argument survives the fade
	// on, the other two being that it only ever falls and that the catalog bounds how long it runs.
	std::uint32_t Fade = NoCoefficient;
};

static_assert(std::is_trivially_copyable_v<SceneAssignment> && std::is_standard_layout_v<SceneAssignment>);
static_assert(sizeof(SceneAssignment) == 12, "Two sessions and an index, and no padding to leave uninitialised");
