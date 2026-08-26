#pragma once

#include <cstdint>

#include "Core/Result.h"
#include "Geometry/NodeTransform.h"
#include "Scene/Store.h"

// The pointer glyph, drawn as scene nodes, at several step counts and several sizes at once.
//
// **Decision 152 settles that gyro draws its own glyph rather than adopting an XCursor theme, and
// leaves the shape to somebody.** This is the instrument for choosing it. The constraint that makes it
// a gym rather than a sketch is `Blit/Blit.cpp`: the CPU renderer expresses an axis-aligned solid quad
// and nothing else — no rotation, no corner radius, no material, no shadow — and one refused item
// loses the whole `Record`. So a pointer that draws under `Blit` is a pointer built out of upright
// rectangles, and the only question worth an instrument is what that costs the diagonal every arrow
// has two of.
//
// **The answer is a column staircase, and the step count is the knob this gym exists to turn.** Each
// column is one upright rectangle spanning the glyph's vertical extent at that x. Coarse steps are
// pixel art and read as such on a 4K panel; fine steps are a diagonal nobody can tell from a real one,
// paid for in nodes at two per step. Both are on screen here in the same frame at three sizes, because
// the number that matters is *steps per device pixel* and no single size shows it.
//
// **Abutting rectangles do not composite to a straight edge, and this cost a rewrite to find.** Two
// opaque quads sharing a vertical edge that falls inside a device pixel each cover part of it and
// blend `over`: 0.4 then 0.6 comes out 0.76 rather than 1.0. Every join does it, so the glyph wears a
// lattice of faint dark lines, and the first run of this gym drew one at fifty-six steps that is
// plainly visible in a dump. The fix is a *bridge* — one more rectangle over each join, one step wide
// and carrying the two columns' shared vertical run — so any device pixel narrower than a step falls
// entirely inside an opaque quad. Widening the columns instead also covers the join and drags the
// silhouette outward by whatever the edge rose over that distance, which at eight steps is a blunter
// arrow rather than a coarser one; a bridge is the intersection of what it joins and cannot reach
// outside either.
//
// **So the step count has a floor, and it is the finding this gym exists to have produced.** A bridge
// works while a step is at least a device pixel wide. Below that it cannot cover the straddled pixel
// and the seam comes back — spread thinner, but there — which makes *just under one step per pixel*
// the worst place to be rather than the best. A glyph is therefore authored against the scale it will
// be drawn at, with steps no finer than about a pixel and a half, and gyro is in a position to do that
// because decision 152 has it authoring the cursor node itself.
//
// **The outline needs no overlap, because growing a shape overlaps it.** The dark staircase is the
// light one expanded by `Outline` on every side, so consecutive dark columns overlap by twice that and
// the seam cannot arise. It is also what hides the last of the sub-pixel softness: the light columns
// are under-covered against *black* rather than against a person's desktop, which is the direction the
// edge was going anyway.
//
// **`Bracket` is the alternative that owes the staircase nothing.** Two arms of a corner, upright by
// construction, exact at every scale, and directional without a diagonal. It is here because a
// designed glyph does not have to be the arrow everybody ships (decision 152 says so in as many
// words), and because if it reads as a pointer then rotation in `Blit` is a cost gyro need not take.

// A candidate glyph.
enum class Glyph : std::uint8_t
{
	// The classic pointing arrow, approximated by upright columns.
	Arrow,

	// A corner bracket. No diagonal, so no approximation and no step count.
	Bracket,
};

// The nominal heights the specimens are drawn at, in the output's own units. A cursor is around the
// first of these and the others are the same glyph under a scaled output — which is the axis the step
// count has to survive, since steps per device pixel is what a person actually sees.
inline constexpr double SpecimenHeights[3]{ 24.0, 48.0, 96.0 };

inline constexpr std::size_t SpecimenSizes = 3;

// The step counts the arrow is specimened at. Two per step in nodes, so the third row costs seven
// times the first — which is the trade this gym is for looking at rather than arguing about.
inline constexpr std::int32_t SpecimenSteps[3]{ 8, 20, 56 };

inline constexpr std::size_t SpecimenRows = 3;

// The scene: a grid of specimens, plus two that move.
struct PointerScene
{
	// Everything hangs off this, parked at the output's origin.
	EntityId Stage{};

	// A mid-grey field. Neither black nor white, because a glyph is a light body with a dark outline
	// and both edges have to be readable against the same ground — which is also the situation a real
	// pointer is in over a real desktop.
	EntityId Backdrop{};

	// The still grid: `SpecimenRows` arrows by `SpecimenSizes`, then the bracket at the same sizes.
	EntityId Arrows[SpecimenRows][SpecimenSizes]{};
	EntityId Brackets[SpecimenSizes]{};

	// The carriage that translates, at the size a cursor actually is, and the bracket riding beside it
	// on the same one. A staircase that shimmers as it moves is the failure a still frame cannot show:
	// the columns land on different device pixels each frame, and an edge that is a few percent light
	// in a different place every frame is a glyph that crawls. The bracket is the control, being exact
	// at every phase — and it is a child of the carriage rather than a second driven node, so the two
	// are at the same sub-pixel offset in every frame and the comparison is between the glyphs.
	EntityId MovingArrow{};
	EntityId MovingBracket{};

	// Where the two moving specimens sit at each end of their travel, in the stage's space. Derived
	// from what was authored rather than from the fractions it came from, for `LaneScene`'s reason.
	Vector3<double> SlideNear{};
	Vector3<double> SlideFar{};
};

// Author the specimen grid into an empty store, against the first output the store carries.
//
// Refused with a sentence where the store carries no outputs or the first has no bounds, which is
// `AuthorLanes`' refusal for its reason: a scene laid out against a degenerate rectangle is an output
// that is black for a cause nobody can see from the frame.
[[nodiscard]] Result<PointerScene> AuthorPointers(SceneStore& scene);

// One glyph as a subtree under `parent`, with its hotspot at the parent's origin.
//
// Exposed rather than kept to the file below because it is the part a real pointer node will want: the
// gym's grid is scaffolding and this is the glyph. `steps` is ignored for `Bracket`, which has no
// diagonal to approximate.
[[nodiscard]] Result<EntityId>
AuthorGlyph(SceneStore& scene, EntityId parent, Glyph glyph, double height, std::int32_t steps);
