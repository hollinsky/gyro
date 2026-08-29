#include "Gym/Pointer.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <vector>

#include "Core/Clock.h"
#include "Core/Time.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Testing/Test.h"
#include "World/Content.h"
#include "World/Elevation.h"
#include "World/Material.h"
#include "World/Node.h"

// What a wrong answer makes *unreadable*, on Gym/Lanes.Test.cpp's rule: the proportions and the
// palette are taste and are not asserted, because a test that pinned them would fail every time the
// glyph is improved with a screen in front of somebody.
//
// What is asserted is the two things this gym exists to be true of. The glyph is drawable by the CPU
// renderer — every node upright, unrotated, undressed, unlifted — because a pointer that is not is one
// the recovery console cannot have, which is decision 152's whole argument for drawing our own. And
// the columns overlap rather than abut, because two abutting opaque quads composite to a seam and a
// lattice of faint lines up the inside of an arrow is the failure this construction exists to avoid.

namespace
{
constexpr double PanelWidth = 1920.0;
constexpr double PanelHeight = 1080.0;

struct Fixture
{
	ManualClock Clock{ Monotonic::FromNanoseconds(1'000'000'000) };
	SceneStore Store{ Clock };

	Fixture()
	{
		const SceneOutput outputs[] = {
			{ .Bounds = { {}, { PanelWidth, PanelHeight } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } }
		};

		Store.SetOutputs(outputs);
	}
};

// One rectangle of a glyph, in the glyph's own space. A plain record rather than `Rect`, because what
// is asked of it here is one comparison between neighbours and typing it would be scaffolding.
struct Part
{
	double Left = 0.0;
	double Top = 0.0;
	double Width = 0.0;
	double Height = 0.0;
	double Alpha = 1.0;

	// The fill's red component, which is all that is needed to tell the two passes apart: the body is
	// white and the outline is black, so this is *which pass* without a field saying so.
	double Fill = 0.0;

	[[nodiscard]] double Right() const noexcept { return Left + Width; }
	[[nodiscard]] double Bottom() const noexcept { return Top + Height; }
};

// The rectangles under `root`, in the order the store holds them. A glyph is one flat sibling list by
// construction, so there is no walk to do.
[[nodiscard]] std::vector<Part> Parts(const SceneStore& store, EntityId root)
{
	std::vector<Part> parts;

	for (EntityId child = store.Find(root)->FirstChild; !child.IsNull();)
	{
		const Entity* const entity = store.Find(child);

		parts.push_back(
			Part{ .Left = entity->Translation.Model().X,
		          .Top = entity->Translation.Model().Y,
		          .Width = static_cast<double>(entity->Extent.Width),
		          .Height = static_cast<double>(entity->Extent.Height),
		          .Alpha = static_cast<double>(entity->Opacity.Model()),
		          .Fill =
		              entity->Kind == NodeKind::Solid ? static_cast<double>(store.Solids()[entity->Content].Red) : 0.0 }
		);

		child = entity->NextSibling;
	}

	return parts;
}

// Every entity in the subtree, visited once. `SceneStore` offers no sweep of its own, which is right —
// nothing outside a test wants one — so this is the ordinary preorder walk over the three links.
void Walk(const SceneStore& store, EntityId root, auto&& visit)
{
	const Entity* const entity = store.Find(root);

	if (entity == nullptr)
	{
		return;
	}

	visit(*entity);

	for (EntityId child = entity->FirstChild; !child.IsNull();)
	{
		const Entity* const next = store.Find(child);

		Walk(store, child, visit);

		child = next->NextSibling;
	}
}
} // namespace

// The refusal a degenerate store earns, which is the one failure that would otherwise reach a person
// as an output that is black for no stated reason.
GYRO_TEST(GymPointer, RefusesAnEmptyOutputSet)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1'000'000'000) };
	SceneStore store{ clock };

	const Result<PointerScene> pointers = AuthorPointers(store);

	GYRO_REQUIRE(!pointers);
	GYRO_CHECK(pointers.error().Code() == ENODEV);
}

// The claim `DrawsOnCpu` makes about this gym, checked rather than asserted in a comment. `Blit`
// refuses a material, an elevation, a corner radius and a quad that is not axis-aligned, and one
// refused item loses the whole `Record` — so a single dressed or turned node here would empty the
// backend the instrument exists to be looked at on.
GYRO_TEST(GymPointer, AuthorsOnlyWhatTheCpuRendererDraws)
{
	Fixture fixture;

	const Result<PointerScene> pointers = AuthorPointers(fixture.Store);

	GYRO_REQUIRE(pointers.has_value());

	std::size_t nodes = 0;

	Walk(fixture.Store, pointers->Stage, [&](const Entity& entity) {
		++nodes;

		GYRO_CHECK(entity.Dress == Material::None);
		GYRO_CHECK(entity.Lift == Elevation::None);
		GYRO_CHECK(entity.Orientation == Quaternion{});
	});

	GYRO_CHECK(nodes > 0);
}

// The seam, which is the whole of why this construction is not just "draw the columns".
//
// Two opaque quads sharing a vertical edge that falls inside a device pixel each cover part of it and
// blend `over` — 0.4 then 0.6 comes out 0.76 rather than 1.0 — so every uncovered join is a faint dark
// line up the inside of the glyph. The property that removes it is stated without reference to which
// rectangle is a column and which is a bridge: no vertical line inside the glyph is an edge of one
// rectangle without being strictly inside another.
GYRO_TEST(GymPointer, NoSeamRunsThroughTheGlyph)
{
	Fixture fixture;

	const Result<EntityId> glyph = AuthorGlyph(fixture.Store, {}, Glyph::Arrow, 96.0, 24);

	GYRO_REQUIRE(glyph.has_value());

	const std::vector<Part> parts = Parts(fixture.Store, *glyph);

	GYRO_REQUIRE(!parts.empty());

	double left = parts.front().Left;
	double right = parts.front().Right();

	for (const Part& part : parts)
	{
		left = std::min(left, part.Left);
		right = std::max(right, part.Right());
	}

	for (const Part& part : parts)
	{
		for (const double join : { part.Left, part.Right() })
		{
			if (join <= left || join >= right)
			{
				continue;
			}

			bool covered = false;

			for (const Part& other : parts)
			{
				covered = covered || (other.Left < join && other.Right() > join);
			}

			GYRO_CHECK(covered);
		}
	}
}

// A glyph's hotspot is its own origin, which is what lets a pointer node be placed at the position
// `Scene/Pointer.h` holds with nothing subtracted from it. The outline reaches above and left of that
// point and is meant to: a hotspot is a place on the screen rather than a corner of the drawing.
GYRO_TEST(GymPointer, GlyphHangsItsOutlineOutsideTheHotspot)
{
	Fixture fixture;

	const Result<EntityId> glyph = AuthorGlyph(fixture.Store, {}, Glyph::Arrow, 24.0, 16);

	GYRO_REQUIRE(glyph.has_value());

	const std::vector<Part> parts = Parts(fixture.Store, *glyph);

	GYRO_REQUIRE(!parts.empty());

	GYRO_CHECK(parts.front().Left < 0.0);
	GYRO_CHECK(parts.front().Top < 0.0);
}

// The grid is taken once, at the glyph's root, and by nothing under it.
//
// A glyph is a body of rectangles whose sub-pixel relationships are the drawing, so flagging the
// rectangles would round each of them apart — which is the difference between a pointer that glides
// and one whose shape ticks as it crosses a pixel.
GYRO_TEST(GymPointer, TheGridIsTakenAtTheGlyphRootAndNowhereBelowIt)
{
	Fixture fixture;

	const Result<EntityId> glyph = AuthorGlyph(fixture.Store, {}, Glyph::Arrow, 24.0, 16);

	GYRO_REQUIRE(glyph.has_value());

	GYRO_CHECK((fixture.Store.Find(*glyph)->Flags & Node::Snap) != 0);

	for (EntityId child = fixture.Store.Find(*glyph)->FirstChild; !child.IsNull();)
	{
		const Entity* const entity = fixture.Store.Find(child);

		GYRO_CHECK((entity->Flags & Node::Snap) == 0);

		child = entity->NextSibling;
	}
}

// The bracket owes the staircase nothing, so it is the same four nodes at every size — which is the
// whole of its case against the arrow.
GYRO_TEST(GymPointer, BracketIsTheSameFourNodesAtEverySize)
{
	Fixture fixture;

	const Result<EntityId> small = AuthorGlyph(fixture.Store, {}, Glyph::Bracket, 24.0, 0);
	const Result<EntityId> large = AuthorGlyph(fixture.Store, {}, Glyph::Bracket, 96.0, 0);

	GYRO_REQUIRE(small.has_value() && large.has_value());

	GYRO_CHECK(Parts(fixture.Store, *small).size() == 4);
	GYRO_CHECK(Parts(fixture.Store, *large).size() == 4);
}

// The refusal that keeps the two constructions from being confused for each other: one takes a step
// count and the other a density, and a caller who hands the wrong one over has asked for a shape
// nobody can compute rather than for a coarser version of the same one.
GYRO_TEST(GymPointer, ExactGlyphIsNotReachedThroughTheStepCount)
{
	Fixture fixture;

	const Result<EntityId> glyph = AuthorGlyph(fixture.Store, {}, Glyph::Exact, 24.0, 16);

	GYRO_REQUIRE(!glyph);
	GYRO_CHECK(glyph.error().Code() == EINVAL);
}

// The property that makes the exact construction correct rather than merely finer, and it is the
// negation of `NoSeamRunsThroughTheGlyph` rather than a stronger form of it.
//
// A staircase avoids the seam by *overlapping*, because two opaque quads sharing a device pixel
// composite to less than either. This one avoids it by never sharing a pixel at all: every rectangle
// is one device pixel tall, its edges land on whole device pixels, and within a pass no two of them
// meet. So the coverage a pixel is written with is the one that was computed for it, once.
GYRO_TEST(GymPointer, ExactGlyphPartitionsTheDeviceGrid)
{
	Fixture fixture;

	const Result<EntityId> glyph = AuthorExactGlyph(fixture.Store, {}, 48.0, Scale::FromInteger(1));

	GYRO_REQUIRE(glyph.has_value());

	const std::vector<Part> parts = Parts(fixture.Store, *glyph);

	GYRO_REQUIRE(!parts.empty());

	for (const Part& part : parts)
	{
		GYRO_CHECK(part.Height == 1.0);
		GYRO_CHECK(part.Left == std::floor(part.Left));
		GYRO_CHECK(part.Top == std::floor(part.Top));
		GYRO_CHECK(part.Width == std::floor(part.Width));
		GYRO_CHECK(part.Width > 0.0);
	}

	// The two passes are consecutive runs of one sibling list — the outline whole, then the body whole
	// — so a pair that overlaps is either two rectangles of one pass or the body over the outline,
	// which is the composite the conditional coverage was derived for. The pass a rectangle belongs to
	// is not recorded on it, so the check is that no *row* carries two overlapping rectangles more than
	// twice over: at every pixel the outline may be under the body and nothing else.
	for (const Part& part : parts)
	{
		for (double column = part.Left; column < part.Right(); column += 1.0)
		{
			std::size_t writers = 0;

			for (const Part& other : parts)
			{
				const bool sameRow = other.Top == part.Top;
				const bool covers = other.Left <= column && other.Right() > column;

				writers += static_cast<std::size_t>(sameRow && covers);
			}

			GYRO_CHECK(writers <= 2);
		}
	}
}

// The claim the word *exact* is carrying, checked as a claim about areas rather than about pixels.
//
// Coverage that is really an area integrates to the shape's area. So the body pass's opacities, each
// weighted by the size of the rectangle carrying it, sum to the arrow's own area — and dividing by the
// square of the glyph's height makes that a number the *shape* has and the sampling does not. Taking
// it at three densities is what separates the two: a construction that sampled the shape rather than
// integrating it would answer differently at each, and by more the coarser the grid.
GYRO_TEST(GymPointer, ExactCoverageIntegratesToTheSameAreaAtEveryDensity)
{
	Fixture fixture;

	const auto areaOf = [&](Scale density) -> double {
		const Result<EntityId> glyph = AuthorExactGlyph(fixture.Store, {}, 32.0, density);

		if (!glyph)
		{
			return 0.0;
		}

		double covered = 0.0;

		for (const Part& part : Parts(fixture.Store, *glyph))
		{
			// The body only. The outline's alpha is a *conditional* coverage — what it needs given that
			// the body did not already take the pixel — so it is not an area and summing it would be
			// asserting something that is not true of it.
			if (part.Fill > 0.5)
			{
				covered += part.Alpha * part.Width * part.Height;
			}
		}

		return covered / (32.0 * 32.0);
	};

	const double one = areaOf(Scale::FromInteger(1));
	const double two = areaOf(Scale::FromInteger(2));
	const double three = areaOf(Scale::FromInteger(3));

	// A thousandth, which is a real assertion rather than a shrug: the areas are the same integral
	// taken over three different partitions of the same plane, and the only thing between them is
	// double-precision arithmetic and the runs the merge folded together.
	GYRO_CHECK(std::abs(two - one) < 0.001);
	GYRO_CHECK(std::abs(three - one) < 0.001);
}

// The exact glyph is snapped for the staircase's reason and one of its own: the coverage was computed
// for the shape sitting on the device grid, so a fractional position draws one alignment's answer at
// another's.
GYRO_TEST(GymPointer, ExactGlyphTakesTheGridAtItsRoot)
{
	Fixture fixture;

	const Result<EntityId> glyph = AuthorExactGlyph(fixture.Store, {}, 24.0, Scale::FromInteger(1));

	GYRO_REQUIRE(glyph.has_value());

	GYRO_CHECK((fixture.Store.Find(*glyph)->Flags & Node::Snap) != 0);

	for (EntityId child = fixture.Store.Find(*glyph)->FirstChild; !child.IsNull();)
	{
		const Entity* const entity = fixture.Store.Find(child);

		GYRO_CHECK((entity->Flags & Node::Snap) == 0);

		child = entity->NextSibling;
	}
}

// The bound doing its job: a glyph authored at a panel's height is a mistake about scale, and it
// arrives as a sentence naming the glyph rather than as a frame the renderer refuses whole.
GYRO_TEST(GymPointer, ExactGlyphRefusesAHeightThatWillNotDecompose)
{
	Fixture fixture;

	const Result<EntityId> glyph = AuthorExactGlyph(fixture.Store, {}, PanelHeight, Scale::FromInteger(2));

	GYRO_REQUIRE(!glyph);
	GYRO_CHECK(glyph.error().Code() == E2BIG);
}

// The outline reaches above and to the left of the hotspot here too, which is `AuthorGlyph`'s property
// arrived at by a different construction — and it is worth checking separately because a mitred offset
// that turned the wrong way would produce a glyph entirely inside its own box and nobody would see it
// until the cursor's point was in the wrong place.
GYRO_TEST(GymPointer, ExactGlyphHangsItsOutlineOutsideTheHotspot)
{
	Fixture fixture;

	const Result<EntityId> glyph = AuthorExactGlyph(fixture.Store, {}, 48.0, Scale::FromInteger(1));

	GYRO_REQUIRE(glyph.has_value());

	const std::vector<Part> parts = Parts(fixture.Store, *glyph);

	GYRO_REQUIRE(!parts.empty());

	double left = parts.front().Left;
	double top = parts.front().Top;
	double bottom = parts.front().Bottom();

	for (const Part& part : parts)
	{
		left = std::min(left, part.Left);
		top = std::min(top, part.Top);
		bottom = std::max(bottom, part.Bottom());
	}

	GYRO_CHECK(left < 0.0);
	GYRO_CHECK(top < 0.0);

	// And the whole glyph is about as tall as it was asked to be, which is what catches an offset that
	// grew by the wrong unit — the mistake that drew a black rectangle around the first staircase.
	GYRO_CHECK(bottom - top > 48.0);
	GYRO_CHECK(bottom - top < 48.0 * 1.5);
}
