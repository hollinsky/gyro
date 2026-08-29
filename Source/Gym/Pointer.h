#pragma once

#include <cstdint>

#include "Core/Result.h"
#include "Geometry/NodeTransform.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"

// The instrument the pointer glyph was chosen with: every candidate on screen in one frame.
//
// **The glyph gyro actually draws is [Scene/Cursor.h](../Scene/Cursor.h)**, because the splash, the
// recovery console and the client host all draw the same cursor and none of them may name a gym. What
// is here is everything that *lost*, kept because a comparison with one specimen in it is not one: the
// column staircase at three step counts, the bracket, and the baked image beside them, with two that
// slide so a glyph that crawls as it moves says so.
//
// **The staircase is the construction the whole exercise started from, and its premise expired.** It
// approximates the arrow's one diagonal with upright rectangles because `Blit` was thought to express
// an axis-aligned solid quad and nothing else — which was true of the renderer when this was written
// and is not true of it now: `Blit/Blit.cpp` samples textures, so the baked glyph draws on the CPU
// renderer too and there was never a reason for the cursor to be several hundred nodes. It stays here
// as an instrument rather than in `Scene` as a drawing, and the two findings it produced are worth
// having on screen next to the answer.
//
// **The first is the seam, and it cost a rewrite to find.** Two opaque quads sharing a vertical edge
// that falls inside a device pixel each cover part of it and blend `over`: 0.4 then 0.6 comes out 0.76
// rather than 1.0. Every join does it, so the glyph wears a lattice of faint dark lines. The fix is a
// *bridge* — one more rectangle over each join, one step wide and carrying the two columns' shared
// vertical run — so any device pixel narrower than a step falls entirely inside an opaque quad.
// Widening the columns instead also covers the join and drags the silhouette outward by whatever the
// edge rose over that distance, which at eight steps is a blunter arrow rather than a coarser one.
//
// **The second is that the step count therefore has a floor.** A bridge works while a step is at least
// a device pixel wide; below that it cannot cover the straddled pixel and the seam comes back, spread
// thinner but there — which makes *just under one step per pixel* the worst place to be rather than the
// best. Neither finding survives into the answer, and that is the point of keeping the instrument: the
// baked glyph has no seam because it never writes a texel twice, which is a property to be able to see
// beside the construction that had to work for it.
//
// **`Bracket` owes the staircase nothing and is here for a different argument.** Two arms of a corner,
// upright by construction, exact at every scale, and directional without a diagonal — kept because a
// designed glyph does not have to be the arrow everybody ships (decision 152 says so in as many words).
//
// **The specimen heights are the output's own units and not fractions of it**, which is the one place
// this gym parts company with [Lanes.h](Lanes.h)'s rule. A lane is a fraction because the instrument
// should be the same shape on a 1080p panel and a 4K one; a glyph is the opposite, because the
// question is how many steps land in a device pixel and a cursor that scaled with the panel would
// answer the same everywhere.

// A candidate glyph, as nodes.
enum class Glyph : std::uint8_t
{
	// The classic pointing arrow, approximated by upright columns.
	Arrow,

	// A corner bracket. No diagonal, so no approximation and no step count.
	Bracket,
};

// One glyph as a subtree under `parent`, with its hotspot at the parent's origin.
//
// `steps` is ignored for `Bracket`, which has no diagonal to approximate.
[[nodiscard]] Result<EntityId>
AuthorGlyph(SceneStore& scene, EntityId parent, Glyph glyph, double height, std::int32_t steps);

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

	// The still grid: `SpecimenRows` staircases by `SpecimenSizes`, then the baked glyph and the bracket
	// at the same sizes. The baked row is read against the staircases directly above it, which is the
	// comparison the grid is laid out to make.
	EntityId Arrows[SpecimenRows][SpecimenSizes]{};
	EntityId BakedArrows[SpecimenSizes]{};
	EntityId Brackets[SpecimenSizes]{};

	// The carriage that translates, at the size a cursor actually is, and the bracket riding beside it
	// on the same one. A staircase that shimmers as it moves is the failure a still frame cannot show:
	// the columns land on different device pixels each frame, and an edge that is a few percent light
	// in a different place every frame is a glyph that crawls. The bracket is the control, being exact
	// at every phase — and it is a child of the carriage rather than a second driven node, so the two
	// are at the same sub-pixel offset in every frame and the comparison is between the glyphs.
	EntityId MovingArrow{};
	EntityId MovingBracket{};
	EntityId MovingBaked{};

	// Where the two moving specimens sit at each end of their travel, in the stage's space. Derived
	// from what was authored rather than from the fractions it came from, for `LaneScene`'s reason.
	Vector3<double> SlideNear{};
	Vector3<double> SlideFar{};
};

// Author the specimen grid into an empty store, against the first output the store carries.
//
// `textures` is what the baked specimens' pixels are adopted through, and this gym never retires them:
// the grid is authored once and lives as long as the store does.
//
// Refused with a sentence where the store carries no outputs or the first has no bounds, which is
// `AuthorLanes`' refusal for its reason: a scene laid out against a degenerate rectangle is an output
// that is black for a cause nobody can see from the frame.
[[nodiscard]] Result<PointerScene> AuthorPointers(SceneStore& scene, ITextures& textures);
