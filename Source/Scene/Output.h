#pragma once

#include <cstdint>

#include "Core/Handle.h"
#include "Geometry/AxisTransform.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"

// The output model the world holds, which is not the one the hardware is programmed to.
//
// Docs/Decisions.md decision 87 settles this by *translation rather than relocation*, and the
// difference is the point. `Seam/OutputConfiguration.h` is what `Reconfigure` asks for and
// `Reconfigured` reports achieved — a negotiation between the frame thread and a backend, travelling
// through decision 73's path, which completes as an event some milliseconds later. Almost none of what
// it carries serves the world, and `Scene` may not say `Seam` in any case. So `Scene` declares the
// record it actually wants and the composition root fills it in, which it may do because it is already
// the only thing that knows both sides of both waists.
//
// **The axis that made translation right here is how often the fact moves.** An output's placement
// changes at pointer rate for as long as somebody drags a monitor around a settings panel, and routing
// that through a verb whose own comment forbids it entering the frame thread's non-preemptible chunk
// is what decision 97 rejected `OutputConfiguration` for. A texture identity, by contrast, is per
// surface and per frame, so its translation layer would be a map consulted on the frame path — which
// is why `TextureId` moved to `Core` instead of being translated.
//
// **What is here and what is not.** Decision 87 names the record's contents: identity, the generation,
// the global rectangle, the exact scale, the device grid. The mode's *extent* is not among them —
// decision 97 splits a view in half and gives the extent to the frame side, because the two halves are
// authored at rates far apart and the walk composes them rather than either side carrying both.

// One output, as the world sees it.
struct SceneOutput
{
	// Generational for `Core/Handle.h`'s reason rather than a weaker one: a monitor is unplugged and
	// the next one takes the slot, and anything still keyed to the old one is wrong about a display
	// that is physically no longer there.
	OutputId Id{};

	// Decision 73's per-output reconfiguration generation, echoed from what the composition root last
	// asked the backend for. It answers *is this output's mode request newer than what I have
	// achieved*, which is a different question from decision 84's set generation — that one is the
	// header's and answers *do these runs mean my outputs at all*. Merging the two would have every
	// mode change renumber the world.
	std::uint64_t Generation = 0;

	// Where the output sits in the global space the world is laid out in. The origin is what the
	// placement below subtracts; the extent is the logical size, which is the device grid divided by
	// the scale and is therefore the layout's statement rather than the mode's.
	Rect<GlobalSpace> Bounds{};

	// The exact rational of decision 53, in 120ths. Not a real number, because an output's scale does
	// integer size arithmetic and the whole of `Geometry/Scale.h` is about that arithmetic being
	// exact — a window given half of a 1.25x screen has to come back the width it was given.
	Scale Density{};

	// The device grid: what the panel actually scans out, in its own pixels. Decision 54's settled snap
	// is to this, and decision 32's cadence question — which outputs a surface intersects — is answered
	// against `Bounds` above rather than here.
	PixelSize<DeviceSpace> Grid{};

	// How the panel is turned. It is here rather than left to `OutputConfiguration::Transform` because
	// the placement below cannot be built without it and nothing else carries it into the world — the
	// composition root fills both in from one fact, which is the same job it already does for the rest
	// of this record.
	AxisOrientation Orientation = AxisOrientation::Normal;

	// Decision 97's published half of the view: global space onto this output's device grid.
	//
	// Derived rather than stored, because it is a function of three fields above and a stored copy is a
	// fourth thing to keep in agreement with them — decision 16's *derived, never maintained*, on a
	// record that changes while a person drags a monitor.
	//
	// The order is the adapter's own — a point is oriented, then scaled, then translated — so the
	// offset that carries this output's origin to its own top-left corner is the oriented, scaled
	// origin negated. Spelled through `Orient` rather than through a bare `Map` of the origin, because
	// `Map` lands its result on the destination's scalar and device space is single precision: the
	// translation this adapter carries is `double`, and rounding it to float before storing it there
	// would put a fifth of a device pixel of error on every coordinate of a distant output.
	[[nodiscard]] constexpr OutputAdapter Placement() const noexcept
	{
		const double density = Density.ToDouble();
		const Detail::Axes<double> turned = Detail::Orient(Orientation, Bounds.Origin.X, Bounds.Origin.Y);

		return { .Orientation = Orientation,
			     .ScaleX = density,
			     .ScaleY = density,
			     .Translation = { -density * turned.X, -density * turned.Y } };
	}
};
