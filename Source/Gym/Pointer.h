#pragma once

#include <cstdint>

#include "Core/Result.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Scale.h"
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
// **The glyph carries `Node::Snap`, and this gym is what found out that it had to.** The sliding
// specimen below boiled: the outline is a stroke a pixel and a bit wide, and at a fractional device
// position it is one dark pixel at one sub-pixel phase and two grey ones at the next, so its weight
// pumped every frame. Decision 156 is the answer — the subtree's origin lands on the device grid
// whenever it is drawn rather than only once it has settled, and the grid is taken once at the root so
// the rectangles keep their proportions. With it the glyph is byte-identical from frame to frame and
// simply translates, which is what the sliding pair is now the demonstration of rather than the
// symptom.
//
// **`Exact` is the third construction, and it owes the staircase nothing either.** `Blit` already
// weights a solid by the *exact* area of its rectangle inside each device pixel — `Overlap` in
// `Blit/Blit.cpp` is a product of two one-dimensional overlaps and says so — so a quad one device pixel
// tall and one wide, carrying an opacity, is an exact coverage sample rather than an approximation of
// one. That is enough to draw the diagonal properly: clip the glyph's polygon to each device pixel,
// take the area, and emit it. The result is the box-filtered analytic coverage of the shape, which is
// the exact prefiltered answer and not a blur — a blur is a *wider* filter, and this is the narrowest
// one that integrates rather than samples.
//
// **What makes it correct where the staircase is not is that it partitions the pixels instead of
// overlapping them.** The seam above is two quads sharing a pixel and compositing `over`. Here every
// quad's edges land on device pixel boundaries by construction, so no pixel is ever written twice
// within a pass, and there is no join to bridge and no floor under the step count.
//
// **The outline is a second pass and its alpha is not its coverage**, which is the one piece of
// arithmetic in the construction. Let `d` be the dark silhouette's area in a pixel and `l` the light
// body's, with the dark shape containing the light one. Drawing dark at `p` and then light at `q` over
// a background `B` leaves `q·W + (1−q)(1−p)·B`, and what is wanted is `l·W + (d−l)·0 + (1−d)·B`. So
// `q = l` and `p = (d−l)/(1−l)` — the outline's *conditional* coverage, given that the body did not
// already take the pixel. Emitting `d` and `l` as two plain coverages would blend the outline through
// the body and grey the glyph's inside edge, which is the seam again wearing a different hat.
//
// **It is authored against a scale and snapped, exactly as the staircase is.** Coverage is computed
// for one alignment of the shape to the grid, so the glyph is right at that alignment and nowhere else
// — which decision 156's `Node::Snap` is what guarantees, by landing the subtree's origin on a device
// pixel whenever it is drawn. The consequence is worth stating plainly: this makes the glyph's *shape*
// exact, not its motion. A pointer still advances a whole device pixel at a time, and authoring
// coverage per sub-pixel phase is a different change than this one.
//
// **The cost is nodes, and it is not cheap.** Roughly six to eight rectangles per device pixel row —
// an opaque run and a partial pixel at each end, twice over for the two passes — so a 24-unit glyph on
// an unscaled panel is under two hundred nodes against a 56-step staircase's two hundred and twenty
// with its bridges, and a 96-unit one is several times that. That is the trade this row is in the grid
// to be looked at rather than argued about, and `MaxParts` is the refusal that keeps a mistaken
// density from authoring a scene nobody can draw.
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

	// The same arrow, decomposed into exact per-device-pixel coverage. No step count either, and a
	// density instead — see `AuthorExactGlyph`.
	Exact,
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

// How many rectangles one exactly covered glyph may decompose into. Six to eight per device pixel row,
// so this is comfortably past the largest specimen in the grid on a 2x panel and well short of
// `Blit`'s four thousand item ceiling, which the whole scene has to fit inside rather than one glyph.
//
// A bound for Core/SlotAllocator.h's reason rather than an estimate of a working set: it turns
// "authored at the wrong scale" from a frame the renderer refuses into a sentence naming the glyph.
inline constexpr std::size_t MaxParts = 2048;

// The scene: a grid of specimens, plus two that move.
struct PointerScene
{
	// Everything hangs off this, parked at the output's origin.
	EntityId Stage{};

	// A mid-grey field. Neither black nor white, because a glyph is a light body with a dark outline
	// and both edges have to be readable against the same ground — which is also the situation a real
	// pointer is in over a real desktop.
	EntityId Backdrop{};

	// The still grid: `SpecimenRows` arrows by `SpecimenSizes`, then the exactly covered arrow and the
	// bracket at the same sizes. The exact row is read against the staircases directly above it, which
	// is the comparison the grid is laid out to make.
	EntityId Arrows[SpecimenRows][SpecimenSizes]{};
	EntityId ExactArrows[SpecimenSizes]{};
	EntityId Brackets[SpecimenSizes]{};

	// The carriage that translates, at the size a cursor actually is, and the bracket riding beside it
	// on the same one. A staircase that shimmers as it moves is the failure a still frame cannot show:
	// the columns land on different device pixels each frame, and an edge that is a few percent light
	// in a different place every frame is a glyph that crawls. The bracket is the control, being exact
	// at every phase — and it is a child of the carriage rather than a second driven node, so the two
	// are at the same sub-pixel offset in every frame and the comparison is between the glyphs.
	EntityId MovingArrow{};
	EntityId MovingBracket{};
	EntityId MovingExact{};

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

// The arrow as exact per-device-pixel coverage, as a subtree under `parent`, hotspot at the origin.
//
// `density` is the output's own scale, and it is a parameter rather than a lookup because the glyph
// does not know which output it is on and the answer has to be the one the pixels are: coverage
// computed against the wrong grid is a shape that is neither exact nor a staircase.
//
// Refused with a sentence where the height or the resulting part count is out of range, which is the
// one place a caller can get this wrong — a glyph authored at a panel's height would decompose into
// more rectangles than any renderer will take.
[[nodiscard]] Result<EntityId> AuthorExactGlyph(SceneStore& scene, EntityId parent, double height, Scale density);
