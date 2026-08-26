#include "Frame/Assign.h"

#include <array>
#include <vector>

#include "Seam/Renderer.h"
#include "Testing/Test.h"

// What decision 152's partition has to get right, written as the ways it can be wrong *on screen*
// rather than wrong in arithmetic.
//
// A promoted item with something composited over it is that thing disappearing, because the plane
// draws above the composite. A partition wider than the planes is a commit the kernel refuses, which
// costs the whole frame rather than the promotion. A promoted window that is being scaled is a
// sharpness change halfway through its own animation. And a still, plain, fullscreen window that
// refuses to promote is the case the whole mechanism exists for failing silently — the GPU waking
// every refresh on a tablet that had nothing to draw.

namespace
{
// A plain textured item: still, opaque, undressed, landing texel for pixel. The thing a plane wants.
[[nodiscard]] DrawItem Promotable()
{
	DrawItem item{};
	item.Content = DrawTexture{};
	item.Sampling = TransformClass{
		.AxisAligned = true,
		.Upright = true,
		.UnitScale = true,
		.IntegerOffset = true,
	};

	return item;
}

// Anything the GPU has to draw. A solid is the cheapest such thing and needs no fields set.
[[nodiscard]] DrawItem Composited()
{
	DrawItem item{};
	item.Content = DrawSolid{};

	return item;
}
} // namespace

GYRO_TEST(FrameAssign, PromotesTheStillPlainTopOfTheList)
{
	const std::vector<DrawItem> items{ Composited(), Promotable() };

	const Partition partition = Assign(items, 4);

	GYRO_REQUIRE(partition.Count == 1);
	GYRO_CHECK(partition.Items[0] == 1);
	GYRO_CHECK(partition.Composited == 1);
	GYRO_CHECK(partition.NeedsComposite());
	GYRO_CHECK(partition.Layers() == 2);
}

// The suffix rule, and the reason for it. The promotable item at the bottom is drawn *under* the solid
// above it; putting it on a plane would put it over the top instead, and the solid would vanish behind
// a window that is supposed to be behind it.
GYRO_TEST(FrameAssign, WillNotPromoteUnderSomethingComposited)
{
	const std::vector<DrawItem> items{ Promotable(), Composited() };

	const Partition partition = Assign(items, 4);

	GYRO_CHECK(partition.Count == 0);
	GYRO_CHECK(partition.Composited == 2);
	GYRO_CHECK(partition.Layers() == 1);
}

// The arrangement the mechanism exists for: everything promoted, no composite at all, and therefore no
// render pass and no target acquired. The count may reach the ceiling exactly here, because no layer
// has to be kept back for a composite that is empty.
GYRO_TEST(FrameAssign, LeavesNoCompositeWhereEverythingPromoted)
{
	const std::vector<DrawItem> items{ Promotable(), Promotable() };

	const Partition partition = Assign(items, 2);

	GYRO_REQUIRE(partition.Count == 2);
	GYRO_CHECK(partition.Items[0] == 0);
	GYRO_CHECK(partition.Items[1] == 1);
	GYRO_CHECK(partition.Composited == 0);
	GYRO_CHECK(!partition.NeedsComposite());
	GYRO_CHECK(partition.Layers() == 2);
}

// A partition is never proposed wider than the output has planes, and where the composite is not empty
// one plane is kept for it. Three promotable items against two planes is one promotion, not two.
GYRO_TEST(FrameAssign, KeepsAPlaneForTheComposite)
{
	const std::vector<DrawItem> items{ Composited(), Promotable(), Promotable() };

	const Partition partition = Assign(items, 2);

	GYRO_REQUIRE(partition.Count == 1);
	GYRO_CHECK(partition.Items[0] == 2);
	GYRO_CHECK(partition.Composited == 2);
	GYRO_CHECK(partition.Layers() == 2);
}

// An output with one plane composites everything, which is what every backend that has not implemented
// promotion reports, and it must not be a special case anywhere else.
GYRO_TEST(FrameAssign, PromotesNothingOntoOnePlane)
{
	const std::vector<DrawItem> items{ Promotable(), Promotable() };

	const Partition partition = Assign(items, 1);

	GYRO_CHECK(partition.Count == 0);
	GYRO_CHECK(partition.Composited == 2);
	GYRO_CHECK(Assign(items, 0).Count == 0);
	GYRO_CHECK(Assign({}, 4).Layers() == 0);
}

// Each clause of the predicate, one at a time, because every one of them is a thing a display engine
// cannot do and would silently drop.
GYRO_TEST(FrameAssign, RefusesWhatAPlaneCannotDraw)
{
	const auto promotes = [](const DrawItem& item) { return IsPromotable(item); };

	GYRO_CHECK(promotes(Promotable()));

	// No corner radius anywhere in a display engine, and it is not separable: cutting the corner is what
	// reveals what is underneath.
	DrawItem rounded = Promotable();
	rounded.Radius = 8.0F;
	GYRO_CHECK(!promotes(rounded));

	// No blur and no tint.
	DrawItem dressed = Promotable();
	dressed.Dress = Material::Glass;
	GYRO_CHECK(!promotes(dressed));

	// A per-node fade is the composite's arithmetic.
	DrawItem faded = Promotable();
	faded.Opacity = 0.5F;
	GYRO_CHECK(!promotes(faded));

	// A shadow could in principle stay behind in the composite; doing so means re-emitting the item as a
	// dressing with no content, which the assigner may not do to the list it was handed.
	DrawItem lifted = Promotable();
	lifted.Lift.Opacity = 0.4F;
	GYRO_CHECK(!promotes(lifted));

	// Nothing but a texture has pixels a plane could scan out.
	GYRO_CHECK(!promotes(Composited()));

	DrawItem group = Promotable();
	group.Content = DrawGroup{ .Count = 2 };
	GYRO_CHECK(!promotes(group));
}

// The scale clause, which is the one still argued. `IsPlaneExpressible` admits a fractional scale
// deliberately; decision 152 refuses it for now, because a hardware scaler is a visible sharpness
// change on the frame a window is handed over. A half-pixel offset is refused by both.
GYRO_TEST(FrameAssign, RefusesAResample)
{
	DrawItem scaled = Promotable();
	scaled.Sampling.UnitScale = false;

	GYRO_CHECK(scaled.Sampling.IsPlaneExpressible());
	GYRO_CHECK(!IsPromotable(scaled));

	DrawItem offset = Promotable();
	offset.Sampling.IntegerOffset = false;

	GYRO_CHECK(!offset.Sampling.IsPlaneExpressible());
	GYRO_CHECK(!IsPromotable(offset));

	// A general affine that did not reduce reports the whole ladder unset, and is refused with it.
	DrawItem spun = Promotable();
	spun.Sampling = TransformClass{};

	GYRO_CHECK(!IsPromotable(spun));
}
