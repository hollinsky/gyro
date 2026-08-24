#include "Protocol/Region.h"

#include "Geometry/Space.h"
#include "Testing/Test.h"

// What is worth testing about a region is the one question anything ever asks it — *is this point
// inside* — and the answer under the sequence of adds and subtracts a client actually sends. The
// representation is the op list, so a test that asserted on the rectangles would be asserting on the
// storage; every case below asks about points.
//
// The order-dependence is the point of most of them. A region is not a set of rectangles with a sign,
// it is a sequence, and add-then-subtract and subtract-then-add over the same two rectangles are
// different shapes. A compositor that normalised into a rectangle set would have to get that right in
// the normaliser instead, which is the code this design does not have.

namespace
{
[[nodiscard]] PixelRect<SurfaceSpace> Box(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
	return { { x, y }, { width, height } };
}

[[nodiscard]] PixelPoint<SurfaceSpace> At(std::int32_t x, std::int32_t y)
{
	return { x, y };
}
} // namespace

GYRO_TEST(Region, AnUnsetRegionContainsNothing)
{
	const SurfaceRegion region;

	GYRO_CHECK(region.IsUnset());
	GYRO_CHECK(!region.Contains(At(0, 0)));
	GYRO_CHECK(region.Bounds().IsEmpty());
}

GYRO_TEST(Region, ARectangleIsHalfOpen)
{
	SurfaceRegion region;
	region.Add(Box(10, 10, 20, 20));

	// The left and top edges are in and the right and bottom are out, which is the convention every
	// rectangle in this codebase carries and the one that makes two abutting windows share no pixel.
	GYRO_CHECK(region.Contains(At(10, 10)));
	GYRO_CHECK(region.Contains(At(29, 29)));
	GYRO_CHECK(!region.Contains(At(30, 20)));
	GYRO_CHECK(!region.Contains(At(20, 30)));
	GYRO_CHECK(!region.Contains(At(9, 20)));
}

GYRO_TEST(Region, SubtractionCutsAHoleAndOrderDecides)
{
	SurfaceRegion cut;
	cut.Add(Box(0, 0, 100, 100));
	cut.Subtract(Box(40, 40, 20, 20));

	GYRO_CHECK(cut.Contains(At(10, 10)));
	GYRO_CHECK(!cut.Contains(At(50, 50)));

	// The same two rectangles the other way round: subtracting from nothing removes nothing, and the
	// add that follows covers the whole square. A representation that summed signed rectangles would
	// make these two regions equal, and a window with a hole in it would be a window without one.
	SurfaceRegion filled;
	filled.Subtract(Box(40, 40, 20, 20));
	filled.Add(Box(0, 0, 100, 100));

	GYRO_CHECK(filled.Contains(At(50, 50)));
}

GYRO_TEST(Region, AHoleCanBeFilledBackIn)
{
	SurfaceRegion region;
	region.Add(Box(0, 0, 100, 100));
	region.Subtract(Box(40, 40, 20, 20));
	region.Add(Box(45, 45, 5, 5));

	GYRO_CHECK(!region.Contains(At(41, 41)));
	GYRO_CHECK(region.Contains(At(46, 46)));
}

GYRO_TEST(Region, BoundsIgnoreWhatWasSubtracted)
{
	SurfaceRegion region;
	region.Add(Box(10, 10, 10, 10));
	region.Add(Box(50, 50, 10, 10));

	// Subtraction cannot put a point outside the union of the adds, so a subtracted rectangle reaching
	// past them describes nothing and must not widen the bound. A bound that grew with it would defeat
	// the only thing a bound is for, which is rejecting a point without walking the list.
	region.Subtract(Box(-1000, -1000, 5000, 5000));

	GYRO_CHECK_EQ(region.Bounds(), Box(10, 10, 50, 50));
}

GYRO_TEST(Region, AnEmptyRectangleContributesNothing)
{
	SurfaceRegion region;
	region.Add(Box(10, 10, 0, 0));
	region.Add(Box(20, 20, 10, 10));

	GYRO_CHECK(!region.Contains(At(10, 10)));
	GYRO_CHECK_EQ(region.Bounds(), Box(20, 20, 10, 10));
}

GYRO_TEST(Region, EmptyIsNotUnset)
{
	SurfaceRegion region;
	region.Add(Box(0, 0, 100, 100));
	region.Subtract(Box(0, 0, 100, 100));

	// The shape covers nothing and the client has still said something, and the two are different
	// facts: `wl_surface.set_input_region` with an empty region is a window deliberately letting
	// clicks through, and never setting one is a window accepting them everywhere.
	GYRO_CHECK(!region.IsUnset());
	GYRO_CHECK(!region.Contains(At(50, 50)));
}

GYRO_TEST(Region, TheCapIsReachedBeforeTheAllocationIs)
{
	SurfaceRegion region;

	for (std::uint32_t index = 0; index < MaxRegionRects; ++index)
	{
		GYRO_REQUIRE(!region.IsFull());

		region.Add(Box(0, 0, 1, 1));
	}

	// The full region is the caller's cue to end the client, so what matters is that it says so before
	// the rectangle that would have overrun rather than after.
	GYRO_CHECK(region.IsFull());
}
