#pragma once

#include <optional>

#include "Geometry/Shape.h"

// What a node accepts of the pointer.
//
// **Nothing accepts by default, and that is the direction that fails safely.** A node the world
// authored and said nothing about swallows no clicks: the boot splash, the recovery console's text,
// a gym's lanes, the container decision 141 parents a window into, and — the one that would be a bug
// somebody spends an afternoon on — the cursor itself, which is a node like any other and is directly
// under the pointer by construction. A default of *the whole extent accepts* would make every one of
// those a target, and the symptom is a click landing on the arrow instead of on the window it is
// pointing at.
//
// **Absent is not the same as empty, which is why the shape is optional rather than merely unset.**
// Wayland fixes both meanings and they are opposite: `set_input_region(NULL)` is *the whole surface*,
// and a `wl_region` a client created and never added to is *nothing at all* — a click-through window,
// which is how a notification overlay is built. `Geometry/Shape.h`'s `IsUnset` cannot tell them apart
// because both are an empty op list, so the distinction is carried here.
struct NodeInput
{
	// Whether the pointer can land on this node at all. False is the whole of what an unauthored node
	// is, and it is also what a surface that has lost its role goes back to.
	bool Accepts = false;

	// The shape, in the node's own coordinates, or nothing for *the whole of the node's extent*. The
	// extent bounds it either way: a client may state a shape larger than its surface and the protocol
	// says the surplus is ignored, so the extent is tested first and the shape only narrows it.
	std::optional<SurfaceShape> Shape;
};
