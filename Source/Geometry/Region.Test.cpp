#include "Geometry/Region.h"

#include <cstdint>
#include <format>
#include <limits>
#include <string>

#include "Geometry/Space.h"
#include "Testing/Test.h"

// What a damage region has to get right is narrow and it is all about the direction of error. A
// region that reports too much costs bandwidth; a region that reports too little leaves stale pixels
// on the glass. Every case below is either a check that the cover is conservative or a check that the
// cheap merging does not accidentally make it not so.

namespace
{
using Damage = Region<DeviceSpace>;

[[nodiscard]] PixelRect<DeviceSpace> Damaged(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
	return { { x, y }, { width, height } };
}

// Whether every pixel of `inner` is inside some rectangle of the region. Deliberately a different
// algorithm from the one under test — it asks the covering question directly, one rectangle at a
// time, rather than reusing the containment predicate the merge path uses.
[[nodiscard]] bool Covers(const Damage& region, PixelRect<DeviceSpace> inner)
{
	for (const PixelRect<DeviceSpace>& rect : region.Rects())
	{
		if (rect.Left() <= inner.Left() && rect.Top() <= inner.Top() && rect.Right() >= inner.Right() &&
		    rect.Bottom() >= inner.Bottom())
		{
			return true;
		}
	}

	return false;
}
} // namespace

GYRO_TEST(Region, DefaultIsEmptyAndCoversNothing)
{
	const Damage region;

	GYRO_CHECK(region.IsEmpty());
	GYRO_CHECK(!region.IsCollapsed());
	GYRO_CHECK(region.Rects().empty());
	GYRO_CHECK(region.Bounds().IsEmpty());
}

GYRO_TEST(Region, AnEmptyRectIsNotDamage)
{
	Damage region;

	region.Add(Damaged(10, 10, 0, 40));
	region.Add(Damaged(10, 10, 40, 0));

	GYRO_CHECK(region.IsEmpty());
}

GYRO_TEST(Region, DisjointRectanglesAreKeptApart)
{
	Damage region;

	region.Add(Damaged(0, 0, 16, 16));
	region.Add(Damaged(100, 100, 8, 8));

	GYRO_REQUIRE_EQ(region.Rects().size(), std::size_t{ 2 });
	GYRO_CHECK(Covers(region, Damaged(0, 0, 16, 16)));
	GYRO_CHECK(Covers(region, Damaged(100, 100, 8, 8)));
	GYRO_CHECK_EQ(region.Bounds(), Damaged(0, 0, 108, 108));
}

GYRO_TEST(Region, ARectangleAlreadyCoveredIsDropped)
{
	Damage region;

	region.Add(Damaged(0, 0, 64, 64));
	region.Add(Damaged(10, 10, 4, 4));

	GYRO_CHECK_EQ(region.Rects().size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(region.Rects().front(), Damaged(0, 0, 64, 64));
}

// The caret case, which is the whole reason the merge exists: the same small rectangle reported every
// frame must not consume the capacity one entry at a time.
GYRO_TEST(Region, RepeatingOneRectangleDoesNotFillTheSet)
{
	Damage region;

	for (int frame = 0; frame < 1000; ++frame)
	{
		region.Add(Damaged(20, 20, 2, 16));
	}

	GYRO_CHECK_EQ(region.Rects().size(), std::size_t{ 1 });
	GYRO_CHECK(!region.IsCollapsed());
}

GYRO_TEST(Region, ACoveringRectangleReplacesWhatItCovers)
{
	Damage region;

	region.Add(Damaged(0, 0, 8, 8));
	region.Add(Damaged(40, 40, 8, 8));
	region.Add(Damaged(0, 0, 256, 256));

	GYRO_REQUIRE_EQ(region.Rects().size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(region.Rects().front(), Damaged(0, 0, 256, 256));
}

GYRO_TEST(Region, OverflowCollapsesToTheBoundsAndStillCoversEverything)
{
	Damage region;

	// Capacity + 1 rectangles, none containing another, so no merging can save any of them.
	for (std::size_t index = 0; index <= Damage::Capacity; ++index)
	{
		region.Add(Damaged(static_cast<std::int32_t>(index) * 32, 0, 16, 16));
	}

	GYRO_REQUIRE(region.IsCollapsed());
	GYRO_REQUIRE_EQ(region.Rects().size(), std::size_t{ 1 });

	// The property that matters. Every rectangle that went in is still covered by what came out, which
	// is what makes the collapse a cost rather than a defect.
	for (std::size_t index = 0; index <= Damage::Capacity; ++index)
	{
		GYRO_CHECK(Covers(region, Damaged(static_cast<std::int32_t>(index) * 32, 0, 16, 16)));
	}
}

GYRO_TEST(Region, ACollapsedRegionKeepsAbsorbing)
{
	Damage region;

	for (std::size_t index = 0; index <= Damage::Capacity; ++index)
	{
		region.Add(Damaged(static_cast<std::int32_t>(index) * 32, 0, 16, 16));
	}

	GYRO_REQUIRE(region.IsCollapsed());

	region.Add(Damaged(-100, -100, 4, 4));

	GYRO_CHECK_EQ(region.Rects().size(), std::size_t{ 1 });
	GYRO_CHECK(Covers(region, Damaged(-100, -100, 4, 4)));
	GYRO_CHECK(Covers(region, Damaged(0, 0, 16, 16)));
}

GYRO_TEST(Region, ClearForgetsTheCollapse)
{
	Damage region;

	for (std::size_t index = 0; index <= Damage::Capacity; ++index)
	{
		region.Add(Damaged(static_cast<std::int32_t>(index) * 32, 0, 16, 16));
	}

	region.Clear();

	GYRO_CHECK(region.IsEmpty());
	GYRO_CHECK(!region.IsCollapsed());
}

GYRO_TEST(Region, AddingARegionAddsItsRectangles)
{
	Damage region{ Damaged(0, 0, 16, 16) };
	Damage other{ Damaged(64, 64, 16, 16) };

	other.Add(Damaged(128, 0, 8, 8));
	region.Add(other);

	GYRO_CHECK_EQ(region.Rects().size(), std::size_t{ 3 });
	GYRO_CHECK(Covers(region, Damaged(128, 0, 8, 8)));
}

GYRO_TEST(Region, IntersectClipsAndDropsWhatFallsOutside)
{
	Damage region;

	region.Add(Damaged(-10, -10, 40, 40));
	region.Add(Damaged(1000, 1000, 8, 8));

	region.Intersect(Damaged(0, 0, 640, 480));

	GYRO_REQUIRE_EQ(region.Rects().size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(region.Rects().front(), Damaged(0, 0, 30, 30));
}

GYRO_TEST(Region, IntersectingEverythingAwayLeavesNothing)
{
	Damage region{ Damaged(0, 0, 16, 16) };

	region.Intersect(Damaged(100, 100, 16, 16));

	GYRO_CHECK(region.IsEmpty());
	GYRO_CHECK(region.Bounds().IsEmpty());
}

// The filter footprint. Content that moved is resampled, the kernel reads outside the rectangle it
// writes, and damage that ignores that leaves a one-pixel trail no screenshot shows.
GYRO_TEST(Region, ExpandGrowsOutwardOnEverySide)
{
	Damage region{ Damaged(100, 100, 20, 20) };

	region.Expand(2);

	GYRO_CHECK_EQ(region.Rects().front(), Damaged(98, 98, 24, 24));
}

GYRO_TEST(Region, ExpandingByNothingChangesNothing)
{
	Damage region{ Damaged(100, 100, 20, 20) };

	region.Expand(0);
	region.Expand(-4);

	GYRO_CHECK_EQ(region.Rects().front(), Damaged(100, 100, 20, 20));
}

// An extent near the representable edge is a bug upstream, and the failure mode to refuse is an
// overflow that turns damage into an empty rectangle — the one direction of error this type exists
// to prevent.
GYRO_TEST(Region, ExpandSaturatesRatherThanWrapping)
{
	constexpr std::int32_t High = std::numeric_limits<std::int32_t>::max();
	constexpr std::int32_t Low = std::numeric_limits<std::int32_t>::min();

	Damage region;

	region.Add(Damaged(Low + 1, Low + 1, 1'000, 1'000));
	region.Add(Damaged(High - 1'001, High - 1'001, 1'000, 1'000));

	region.Expand(16);

	GYRO_REQUIRE_EQ(region.Rects().size(), std::size_t{ 2 });

	const PixelRect<DeviceSpace> nearFloor = region.Rects()[0];
	const PixelRect<DeviceSpace> nearCeiling = region.Rects()[1];

	GYRO_CHECK_EQ(nearFloor.Left(), Low);
	GYRO_CHECK_EQ(nearFloor.Top(), Low);
	GYRO_CHECK(!nearFloor.IsEmpty());

	GYRO_CHECK_EQ(nearCeiling.Right(), High);
	GYRO_CHECK_EQ(nearCeiling.Bottom(), High);
	GYRO_CHECK(!nearCeiling.IsEmpty());
}

GYRO_TEST(Region, BoundsSpanEveryRectangle)
{
	Damage region;

	region.Add(Damaged(100, 50, 10, 10));
	region.Add(Damaged(0, 200, 10, 10));
	region.Add(Damaged(300, 0, 10, 10));

	GYRO_CHECK_EQ(region.Bounds(), PixelRect<DeviceSpace>::FromEdges({ 0, 0 }, { 310, 210 }));
}

GYRO_TEST(Region, FormatsForALog)
{
	Damage region;

	GYRO_CHECK_EQ(std::format("{}", region), std::string{ "device{empty}" });

	region.Add(Damaged(0, 0, 64, 64));
	region.Add(Damaged(100, 100, 8, 16));

	GYRO_CHECK_EQ(std::format("{}", region), std::string{ "device{2: [0, 0 64x64], [100, 100 8x16]}" });

	for (std::size_t index = 0; index <= Damage::Capacity; ++index)
	{
		region.Add(Damaged(static_cast<std::int32_t>(index) * 32, 500, 16, 16));
	}

	GYRO_CHECK(std::format("{}", region).starts_with("device{collapsed: "));
}

// Buffer-space damage is a different type from device-space damage, which is the whole reason this is
// a template. Compiled here so the instantiation is not only exercised through the alias above.
GYRO_TEST(Region, TheSpaceTravelsWithTheDamage)
{
	Region<BufferSpace> buffer{ { { 0, 0 }, { 32, 32 } } };

	GYRO_CHECK(!buffer.IsEmpty());
	GYRO_CHECK_EQ(std::format("{}", buffer), std::string{ "buffer{1: [0, 0 32x32]}" });
}
