#include "Geometry/Space.h"

#include <string>

#include "Testing/Test.h"

// The runtime half of Space.h's contract. The compile-time half is the static_assert block at the
// foot of that header — the spaces being distinct types, global having no grid, a position not
// adding to a position, and the field order the publication boundary reads by — and is not repeated
// here. bit_cast is constexpr, so the layout half stayed there rather than moving down.
//
// What is left is the two things a constant expression cannot reach: what a coordinate looks like
// to whoever reads a log, and whether the precision the global space pays for buys what its comment
// claims it does.

GYRO_TEST(Space, FormatsWithItsSpace)
{
	GYRO_CHECK_EQ(std::format("{}", Point<GlobalSpace>{ 12.5, 40.0 }), std::string{ "global(12.5, 40)" });
	GYRO_CHECK_EQ(std::format("{}", Offset<SurfaceSpace>{ 3.0F, -1.0F }), std::string{ "surface(+3, -1)" });
	GYRO_CHECK_EQ(std::format("{}", Size<BufferSpace>{ 640.0F, 480.0F }), std::string{ "buffer(640x480)" });
	GYRO_CHECK_EQ(
		std::format("{}", PixelRect<DeviceSpace>{ { 0, 0 }, { 640, 480 } }), std::string{ "device[0, 0 640x480]" }
	);
}

GYRO_TEST(Space, TheSpaceIsVisibleToTheReader)
{
	// Same three numbers, different meaning. The compiler already refuses to mix them; if they also
	// printed alike then a log would be the one place that distinction is invisible — and "the
	// window is at 300, 200" is exactly the sentence that is useless without knowing which grid.
	GYRO_CHECK_EQ(std::format("{}", Point<GlobalSpace>{ 300.0, 200.0 }), std::string{ "global(300, 200)" });
	GYRO_CHECK_EQ(std::format("{}", Point<DeviceSpace>{ 300.0F, 200.0F }), std::string{ "device(300, 200)" });
	GYRO_CHECK_EQ(std::format("{}", PixelPoint<DeviceSpace>{ 300, 200 }), std::string{ "device(300, 200)" });
}

GYRO_TEST(Space, GlobalPrecisionSurvivesALargeArrangement)
{
	// Why GlobalSpace is the one space that pays for a double, turned from a claim into a number.
	//
	// wl_fixed delivers 1/256 of a pixel, so that is the smallest offset the wire can even express
	// and therefore the floor global space has to stay above. Single precision reaches exactly
	// 1/256 at 32768 — no headroom at all — and is coarser than the wire beyond it. A desk with
	// several high-resolution monitors, or one virtual output sized for a tablet alongside them,
	// passes that mark without anything unusual happening.
	constexpr double WlFixed = 1.0 / 256.0;
	constexpr double AcrossTheDesk = 65536.0;

	const float single = static_cast<float>(AcrossTheDesk);
	const float singleNudged = single + static_cast<float>(WlFixed);

	// The sub-pixel offset the wire delivered does not survive being stored. Everything this module
	// is about — settling below a threshold, snapping by less than half a pixel, a surface adapter
	// that is exact — happens at or below this magnitude, so losing it here loses all of it.
	GYRO_CHECK_EQ(singleNudged, single);

	const Point<GlobalSpace> position{ AcrossTheDesk, AcrossTheDesk };
	const Point<GlobalSpace> nudged = position + Offset<GlobalSpace>{ WlFixed, WlFixed };

	GYRO_CHECK(nudged != position);

	// And with room underneath it, which is the actual requirement: sub-pixel work has to be
	// representable well below the wire's resolution, not merely at it.
	const Point<GlobalSpace> barely = position + Offset<GlobalSpace>{ WlFixed / 4096.0, 0.0 };

	GYRO_CHECK(barely != position);
}
