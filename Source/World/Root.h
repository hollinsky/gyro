#pragma once

#include <cstdint>
#include <type_traits>

#include "Core/Session.h"

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
	SessionId Session = SessionId::None;
};

static_assert(std::is_trivially_copyable_v<SceneRoot> && std::is_standard_layout_v<SceneRoot>);
static_assert(sizeof(SceneRoot) == 8, "Two uint32s, and no padding to leave uninitialised");
