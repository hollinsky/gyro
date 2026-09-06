#pragma once

#include <cstdint>
#include <span>

#include "Animation/Author/Animatable.h"
#include "Core/Handle.h"
#include "Core/Session.h"
#include "Core/Time.h"
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

// One bit per output, in the order the world holds them — which is the same positional convention
// decision 84 puts on every per-output run, on the way out and on the way back.
//
// Here rather than beside the one question that computes it, because more than one thing now answers
// per output over the same set: `Scene/Reach.h` works out which outputs a node lands on, and
// `Scene/Atlas.h` reserves a snapshot on each of them (190). A mask is a property of the set.
using OutputReach = std::uint32_t;

// SPEC: how many outputs a reach can name. It is the width of the mask above rather than a policy, and
// it sits above `OutputsPerReport`'s sixteen so that the two limits cannot disagree about a display
// that exists — the report is what would truncate first, and it says so on its own terms.
inline constexpr std::size_t MaxReachableOutputs = 32;

// One output, as the world sees it.
struct SceneOutput
{
	// Generational for `Core/Handle.h`'s reason rather than a weaker one: a monitor is unplugged and
	// the next one takes the slot, and anything still keyed to the old one is wrong about a display
	// that is physically no longer there.
	OutputId Id{};

	// Which session this output is showing, or `SessionId::None` for gyro's own scene.
	//
	// **An output belongs to a session or to gyro, and there is no third state.** Decision 21 keeps
	// every connected session alive and presents one of them locally, so *which one* is a property of
	// the output rather than of the session — switching users moves this field and tears nothing down.
	// `None` is not an output nobody has got round to: it is the boot splash, the background between one
	// session and the next, and the recovery console, all of which gyro authors for itself.
	//
	// **The composition root fills it in, and Docs/Open.md requires that it stay there.** Assignment
	// must not be client-reachable — a System-tier client that could move an output between sessions
	// walks straight through decision 43's locking — so configuration and assignment are separate
	// operations with separate reachability, and the root is the only party that sees both the sessions
	// `Session/Control.h` mints and the outputs the world holds.
	//
	// **Nothing draws differently by it yet**, and it is minted now for the reason the composition root
	// mints an `OutputId` before anything reads one: the reader is a per-session root for the walk to
	// gate on, and a field added alongside it would be a second scheme to reconcile with the assignment
	// policy written first.
	SessionId Session = SessionId::None;

	// The session drawn at `Fade` below, or `None` outside a transition — decision 188's cross-fade,
	// authoring side.
	//
	// **It is here rather than on either session because the transition is exactly the interval in
	// which decision 21's *one session per output* is false.** A session is shown on several outputs
	// and a user switch need not move them together, so a coefficient per session would fade both of
	// somebody's monitors when they asked to switch on one.
	//
	// **It is whichever participant is in front rather than whichever one is leaving**, which is
	// `World/Root.h`'s revision: the one underneath is drawn at full strength, so a coefficient on it
	// would be a fade nobody can see. `Session` and this being equal is the legal shape that says so —
	// the arriving session is the one on top and is resolving onto what is already there.
	//
	// It is retired by `Scene/Serializer.h` together with the coefficient below, on the walk that
	// already decides whether a channel still owes a frame: the two are one fact and a published
	// faded session with no coefficient behind it would be a session composited for ever.
	SessionId Fading = SessionId::None;

	// The faded session's opacity, running down to nothing or up to full.
	//
	// **A compositor-owned animatable, which is the shape Architecture.md anticipates for the idle dim
	// ramp**: a channel gyro authors for itself, moving under the catalog, belonging to no client and
	// reachable by no protocol. That last part is load-bearing rather than tidy — decision 43's
	// anti-spoofing argument survives a transition that composites a user's session beside a lock
	// prompt only because a client cannot start one, extend one, re-enter one, or push this back up.
	//
	// It is not a node's channel and so does not go through `Scene/Commit.h`: a commit is a scope with
	// an author and an origin over the entities a client changed (112), and there is no entity here.
	// `SceneStore::FadeOutputSession` is the one door onto it.
	//
	// Zero at rest, which is the same statement as `Fading` being `None`: nothing is between the two
	// desktops, so there is nothing to draw at a strength of its own.
	Animatable<float> Fade{ 0.0F };

	// The session this output is holding behind a lock, or `None` where it is not locked.
	//
	// **This is decision 188's refusal, and it is one field rather than a bit because the bit alone
	// would lose the thing being protected.** A locked output is showing somebody else — gyro's own
	// scene today and the greeter when there is one — so `Session` no longer names the person whose
	// screen it is, and a flag saying *locked* would leave nothing to unlock back to. Holding the
	// session id is the refusal and the destination at once, which is what makes them impossible to
	// disagree.
	//
	// **The refusal it carries is `ShowSession`'s**: an output showing nobody is the one thing the
	// composition root hands to a session that has just connected, and a locked screen is exactly an
	// output showing nobody. Without this, somebody logging in on a second seat would be put straight
	// onto a locked panel — the screen would unlock itself for a person who never authenticated. The
	// check is in the root rather than here because *who is entitled to unlock* is policy and this is
	// a record; `SetOutputSession` clears this field on the way past for that reason, being the
	// unconditional assignment only the root can reach.
	//
	// **It does not cross the waist.** The frame thread draws what an output is showing and a lock
	// changes nothing about that — what is on the glass while a screen is locked is a session
	// assignment like any other, which is decision 43's *locking is an output reassignment* being
	// taken literally rather than becoming a mode the walk has to know about.
	SessionId Locked = SessionId::None;

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

	// The mode's nominal period, echoed from what the backend achieved.
	//
	// **The one field here that nothing in the world reads, and it is here because there is nowhere
	// else it can be said from.** `wl_output.mode` carries a refresh rate and `Protocol` sees only this
	// store, so the alternative is telling every client zero — which is not *we decline to answer*, it
	// is the only cadence figure a client has before it has drawn anything, withheld. A media player reading zero falls
	// back to 60 and judders on a 144 Hz panel, which is a wrong number rather than an absent one.
	//
	// **Nominal rather than measured, which is what keeps it a fact rather than a prediction.**
	// `Frame/FrameClock.h` learns what the panel is actually doing and that number belongs to the frame
	// thread; this one is the mode's own, filled from the same `OutputConfiguration` the fields around
	// it are, so it is one more echo rather than a second source of truth to hold in agreement. A
	// client must not schedule against it in any case — the frame callback is the contract, and it is
	// answered from what reached the glass.
	//
	// **It has a second reader now**, which is `wp_presentation_feedback.presented`: that event's
	// `refresh` is a prediction of how long until the next one, and this is what
	// [Protocol/Host.h](../Protocol/Host.h) substitutes where the backend measured nothing. The
	// observed period wins wherever there is one, so this stays the mode's statement rather than
	// becoming a measurement by being read.
	Duration Period{};

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
	// The order is the adapter's own — a point is oriented, then scaled, then translated — so what has
	// to be stored is whatever carries the *oriented* rectangle onto the grid's own top-left corner.
	// Spelled through `Orient` rather than through a bare `Map`, because `Map` lands its result on the
	// destination's scalar and device space is single precision: the translation this adapter carries
	// is `double`, and rounding it to float before storing it there would put a fifth of a device pixel
	// of error on every coordinate of a distant output.
	//
	// **The corner that has to land on nothing is the rectangle's, not the origin's, and those are
	// different points on six of the eight orientations.** `Detail::Orient` is a pure turn about the
	// coordinate origin with no re-centring in it, so turning a rectangle that starts in the positive
	// quadrant sends part of it negative — a quarter turn puts every row of a panel at a negative
	// device `y`. Carrying only the oriented origin was therefore right for `Normal` and for the one
	// flip that happens to fix its own corner, and wrong for the rest: a monitor stood on end drew
	// entirely off its own glass, and the whole screen was black. Only the origin was ever mapped in a
	// test, and the origin lands on nothing under every orientation by construction, which is why this
	// survived being pinned.
	//
	// The correction is the *minimum* of the four oriented corners rather than a table of eight cases,
	// and it is two comparisons because a turn is a signed permutation: the oriented offsets are the
	// zero corner and three others, so the least of them on each axis is whichever of zero and the
	// oriented extent is smaller.
	[[nodiscard]] constexpr OutputAdapter Placement() const noexcept
	{
		const double density = Density.ToDouble();
		const Detail::Axes<double> origin = Detail::Orient(Orientation, Bounds.Origin.X, Bounds.Origin.Y);
		const Detail::Axes<double> extent = Detail::Orient(Orientation, Bounds.Extent.Width, Bounds.Extent.Height);

		const double leastX = origin.X + (extent.X < 0.0 ? extent.X : 0.0);
		const double leastY = origin.Y + (extent.Y < 0.0 ? extent.Y : 0.0);

		return { .Orientation = Orientation,
			     .ScaleX = density,
			     .ScaleY = density,
			     .Translation = { -density * leastX, -density * leastY } };
	}
};

// Which session an output showing this point is showing, and `None` where no output is.
//
// **This is what makes a session reachable exactly where it is presented.** Decision 21 keeps every
// connected session alive at once, so the world holds the roots of all of them and *which one a
// person is pointing at* is a property of the screen the point is on rather than of the scene. The
// frame walk already asks this question per output; [Hit.h](Hit.h) asks it per point, which is the
// same gate applied to the other thing a root can be on the receiving end of.
//
// **`None` for a point on no output is the answer rather than a failure**, and it falls out right:
// gyro's own roots are `None` and are shown everywhere (`World/Root.h`), so a coordinate off the side
// of every screen still finds the pointer glyph and the background and finds nobody's windows. The
// pointer is confined to the union of the outputs, so the case arises for a grab whose coordinates
// have travelled past the frame and for a machine with no outputs at all.
//
// Half-open, which is `Scene/Pointer.h`'s convention and `Geometry/Shape.h`'s: two panels abutting do
// not both answer for the column between them.
[[nodiscard]] inline SessionId SessionShownAt(std::span<const SceneOutput> outputs, Point<GlobalSpace> point) noexcept
{
	for (const SceneOutput& output : outputs)
	{
		const Rect<GlobalSpace>& bounds = output.Bounds;

		if (point.X >= bounds.Origin.X && point.X < bounds.Origin.X + bounds.Extent.Width &&
		    point.Y >= bounds.Origin.Y && point.Y < bounds.Origin.Y + bounds.Extent.Height)
		{
			return output.Session;
		}
	}

	return SessionId::None;
}
