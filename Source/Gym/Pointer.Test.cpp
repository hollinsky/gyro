#include "Gym/Pointer.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <vector>

#include "Core/Clock.h"
#include "Core/Time.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Testing/Test.h"
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

	[[nodiscard]] double Right() const noexcept { return Left + Width; }
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
		          .Height = static_cast<double>(entity->Extent.Height) }
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
