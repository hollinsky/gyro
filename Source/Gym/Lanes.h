#pragma once

#include "Core/Handle.h"
#include "Core/Result.h"
#include "Geometry/NodeTransform.h"
#include "Scene/Store.h"

// The scene the gyms drive, and it is a diagnostic before it is a picture.
//
// **One lane per channel, and a track behind every marker.** A gym exists to be looked at when
// something is wrong, and "the picture is wrong" is worth nothing on its own — what a person needs off
// one frame is *which channel*. So each of the four sprung channels gets a horizontal lane of its own,
// in a colour nothing else uses, and each lane holds two nodes: a still **track** that shows where the
// marker is entitled to be, and the **marker** that moves. A channel that overshoots puts its marker
// past its track; a channel that has stopped leaves its marker parked on one end of a track that says
// how far it should have gone; a channel that is driving the wrong node moves a marker whose colour is
// not the one that is stuck. None of that is legible against a bare rectangle sliding on black.
//
// **The lanes are authored still and driven from outside.** `Gym/Gym.h`'s gyms differ in *what they
// retarget and when*, and not at all in what the scene is — which is what makes two gyms comparable,
// and what makes the settling one and the perpetual one the same instrument read two ways. A gym that
// drives no lane leaves that lane's marker sitting on its track, which is a reference rather than an
// omission.
//
// **The rotation lane is authored and deliberately not driven by default**, and that is a fact about
// the CPU renderer rather than about rotation. `Blit/Blit.cpp`'s `Classify` refuses a quad that is not
// axis-aligned — exact float equality on the corners, which is the honest comparison and which any
// nonzero rotation fails — and one refused item fails the whole `Record`, so the frame is *lost*
// rather than degraded. A turning marker under `--backend=dump` is therefore not a wrong picture, it
// is no picture at all for as long as it turns. The lane is still authored, because a still node at
// the identity orientation draws fine and is the reference the turning one is read against.
//
// **The layout is a fraction of the output rather than a constant**, so the instrument is the same
// shape on a 1080p panel and on a 4K one, and so a marker that has left its lane has left it for a
// reason other than the panel being a size nobody tried.

// One channel's lane: the still reference and the node that moves against it.
struct Lane
{
	// Created first and therefore behind, per decision 55: the sibling list is the z order.
	EntityId Track{};
	EntityId Marker{};
};

// The whole instrument, as the ids a gym drives it through.
//
// It carries the two translation endpoints because they are the only part of the layout a driver needs
// arithmetic for — the other three channels are retargeted between constants below, since a scale, an
// opacity, and an orientation mean the same thing whatever the output is.
struct LaneScene
{
	// The one node everything hangs off, parked at the output's origin so that every position below is
	// read against the output rather than against global space. A gym that wanted to move the whole
	// instrument at once moves this.
	EntityId Stage{};

	// A fill over the whole output, so that a lost frame and a black scene are different pictures.
	EntityId Backdrop{};

	Lane Slide{};
	Lane Grow{};
	Lane Fade{};
	Lane Turn{};

	// Where the slide lane's marker sits at each end of its travel, in the lane's own space. The far
	// end is the track's right edge less the marker's own width, so the marker lands flush with the
	// track rather than hanging off it — which is what makes "past the track" mean something.
	Vector3<double> SlideNear{};
	Vector3<double> SlideFar{};
};

// The two ends of the scale lane. Not the full range: a marker animated to zero is a quad that
// collapsed, which `Blit` treats as drawing nothing rather than as an error, and a lane whose failure
// mode and whose resting state look alike is a lane that says nothing.
inline constexpr Vector3<float> GrowSmall{ 0.35F, 0.35F, 1.0F };
inline constexpr Vector3<float> GrowFull{ 1.0F, 1.0F, 1.0F };

// The two ends of the opacity lane. Dim rather than invisible, for `GrowSmall`'s reason.
inline constexpr float FadeDim = 0.12F;
inline constexpr float FadeFull = 1.0F;

// How far the rotation lane turns per step, and how many steps close the circle. Sixty degrees rather
// than ninety because the marker is a bar: a half turn is a symmetry of it and a quarter turn of a
// square is a symmetry of that, so either would be a rotation nobody can see happening. Six steps
// return the marker exactly to where it started, which is what lets the driver hold an integer count
// instead of an angle that accumulates rounding for as long as the gym runs.
inline constexpr float TurnStepRadians = 1.047197551F; // pi / 3
inline constexpr int TurnSteps = 6;

// Author the instrument into an empty store, against the first output the store carries.
//
// **The output set has to be there already**, which is the one thing this cannot supply for itself: an
// output's placement in global space is the composition root's, being the only thing that knows both
// sides of both waists (decision 87). A store with no outputs, or one whose first output has no bounds,
// is refused with a sentence rather than laid out against zero — a scene authored into a degenerate
// rectangle is an output that is black for a reason nobody can see from the frame.
[[nodiscard]] Result<LaneScene> AuthorLanes(SceneStore& scene);

// The dressed panels the materials gym lays over the lanes: `Glass` on the left of the output and
// `Smoke` on the right, both lifted, both spanning the lanes so that what they are gathering is
// visibly in motion underneath them.
//
// Separate from the layout above rather than a flag on it, because these are the items `Blit` refuses
// outright — a material, an elevation, or both — and every other gym has to be able to author the
// instrument without them. See `Gym/Gym.h` for what that costs a caller who asks for this one under a
// backend that cannot draw it.
struct MaterialOverlay
{
	EntityId Glass{};
	EntityId Smoke{};
};

[[nodiscard]] Result<MaterialOverlay> AuthorMaterialOverlay(SceneStore& scene, const LaneScene& lanes);
