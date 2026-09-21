#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

// What the world asks each output to be, as bytes in the snapshot's configuration run.
//
// **This is decision 73's trigger, which was written down and never built.** The frame thread initiates
// a reconfiguration when a per-output generation carried in the snapshot moves, so that the ordering of
// an output change against the scene that assumes it is defined by the publication rather than arranged
// beside it. `Scene` writes the record and `Frame` acts on it, neither may name the other, and `Scene`
// may not name `Seam/OutputConfiguration.h` either — so it lives below both for decision 87's reason,
// exactly where `World/Root.h`'s assignment already does.
//
// **A run of its own rather than a field on `SceneAssignment`, because the two are read by different
// parties at different rates.** The assignment is drawn from on every frame by the walk; this is read by
// the loop and acted on once, when the generation moves, and a walk handed a field it must ignore is the
// cost `World/Root.h` already declines to put on a node.
//
// **Power is all it carries, because the idle ladder's display-off rung is the only thing that asks.** A
// mode, a transform or variable refresh is a field added here when something in the world asks for one,
// which is what the generation buys: the frame thread's half does not change shape when it does.
struct SceneConfiguration
{
	// Moved by the world when it asks this output for something different. The frame thread compares it
	// against the last generation it handed the presenter, so a scene republished for any other reason
	// asks for nothing.
	std::uint64_t Generation = 0;

	// False is the ladder's display-off rung: the panel dark and owing no frames.
	bool Powered = true;

	std::array<std::uint8_t, 7> Reserved{};
};

static_assert(std::is_trivially_copyable_v<SceneConfiguration> && std::is_standard_layout_v<SceneConfiguration>);
static_assert(sizeof(SceneConfiguration) == 16, "A generation, a flag, and padding that is spelled");
