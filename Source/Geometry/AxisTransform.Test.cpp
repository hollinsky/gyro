#include "Geometry/AxisTransform.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include "Testing/Test.h"

// The runtime half of AxisTransform.h's contract. The compile-time half is the static_assert block
// at the foot of that header — the wire's numbering, the orientation group and its inverses, the
// mapping convention, the classification ladder, and the layout the publication boundary reads — and
// is not repeated here. The group law is already exhaustive up there, since eight by eight is a
// constant expression's size rather than a sweep's.
//
// What is left is the two things a constant expression is the wrong shape for: what an adapter looks
// like to whoever reads a log, and the properties that are only convincing swept across the
// orientations, scales, and offsets a real arrangement produces rather than asserted at a point.

// Every scale on the settings ladder that is exact in binary, plus the two that bracket it. Exact
// values are the point of the composition sweep: the claim being checked there is that composing two
// adapters and applying them one after the other are the same computation, and a sweep over values
// that round would be checking the scalar's arithmetic instead.
constexpr std::array<float, 5> DyadicScales{ 0.5F, 1.0F, 1.5F, 2.0F, 4.0F };

// Offsets on the grid, off it by half, and off it by a quarter — which is where the classification's
// three rungs separate. Negative because an output to the left of the global origin is ordinary.
constexpr std::array<float, 5> Offsets{ -12.0F, -0.5F, 0.0F, 0.25F, 7.0F };

GYRO_TEST(AxisTransform, FormatsWithItsSpacesAndItsParts)
{
	const AxisTransform<GlobalSpace, DeviceSpace> adapter{ AxisOrientation::Rotate90, 1.5, 1.5, { 12.0, 40.0 } };

	GYRO_CHECK_EQ(std::format("{}", adapter), std::string{ "global->device[90 1.5x (+12, +40)]" });
	GYRO_CHECK_EQ(
		std::format("{}", AxisTransform<DeviceSpace, DeviceSpace>::Identity()),
		std::string{ "device->device[normal 1x (+0, +0)]" }
	);

	// The pair of spaces is in the output for the reason Space.h puts one space in a point's: the
	// same numbers are a different mapping between a different pair, and "the transform is 1.5x at
	// +12" is exactly the log line that cannot be acted on without knowing which chain it is from.
	GYRO_CHECK_EQ(
		std::format(
			"{}", AxisTransform<BufferSpace, SurfaceSpace>{ AxisOrientation::Flipped180, 1.5F, 1.5F, { 12.0F, 40.0F } }
		),
		std::string{ "buffer->surface[flipped-180 1.5x (+12, +40)]" }
	);

	// The anamorphic surface adapter, where the two factors differ and the short spelling would have
	// to pick one of them. The pair appears exactly where it means something, so a non-uniform scale
	// nobody intended is visible in a log rather than being something to go and look for.
	GYRO_CHECK_EQ(
		std::format("{}", AxisTransform<BufferSpace, SurfaceSpace>{ AxisOrientation::Normal, 2.0F, 1.5F, {} }),
		std::string{ "buffer->surface[normal 2x/1.5x (+0, +0)]" }
	);

	GYRO_CHECK_EQ(std::format("{}", AxisOrientation::Flipped270), std::string{ "flipped-270" });
}

GYRO_TEST(AxisTransform, ClassificationPrintsTheSetAndNotARung)
{
	GYRO_CHECK_EQ(
		std::format("{}", AxisTransform<GlobalSpace, DeviceSpace>::Identity().Classify()),
		std::string{ "class(axis-aligned upright unit-scale integer-offset)" }
	);

	// The fractionally scaled output, which is the case a reader is most often looking at: not
	// sharp, still promotable, and the report has to make both readable at once.
	GYRO_CHECK_EQ(
		std::format("{}", AxisTransform<GlobalSpace, DeviceSpace>{ AxisOrientation::Normal, 1.5, 1.5, {} }.Classify()),
		std::string{ "class(axis-aligned upright integer-offset)" }
	);

	// A transform that reduces to nothing prints as nothing rather than as an empty pair of
	// parentheses somebody has to recognise.
	GYRO_CHECK_EQ(std::format("{}", TransformClass{}), std::string{ "class(none)" });
}

GYRO_TEST(AxisTransform, ComposingIsApplyingOneAfterTheOther)
{
	// The property the restricted type exists for, over the chain the design actually has: global
	// onto an output's device grid, then that grid onto a rotated panel. If these two ever disagree
	// then damage was computed against one mapping and the surface sampled through the other, which
	// presents as a trail behind a window on a rotated display and nowhere else.
	//
	// Exact equality, deliberately. Every value here is dyadic and small, so both routes are exact
	// and a tolerance would only be hiding the case where they are not.
	//
	// Every scale here is non-uniform, and unequal between the two adapters, so no pair of factors
	// can survive being exchanged when it should not have been or left alone when it should have
	// been. Detail::CompositionHolds makes the same claim as a constant expression over the same
	// sixty-four pairs; what this adds is the chain the header cannot reach — double onto single,
	// with the narrowing happening in the middle on one route and at the end on the other.
	for (std::uint8_t a = 0; a < OrientationCount; ++a)
	{
		for (std::uint8_t b = 0; b < OrientationCount; ++b)
		{
			for (std::size_t index = 0; index < DyadicScales.size(); ++index)
			{
				const float scaleX = DyadicScales[index];
				const float scaleY = DyadicScales[(index + 2) % DyadicScales.size()];

				for (const float offset : Offsets)
				{
					const AxisTransform<GlobalSpace, DeviceSpace> output{ static_cast<AxisOrientation>(a),
						                                                  static_cast<double>(scaleX),
						                                                  static_cast<double>(scaleY),
						                                                  { static_cast<double>(offset), 20.0 } };
					const AxisTransform<DeviceSpace, DeviceSpace> panel{
						static_cast<AxisOrientation>(b), 2.0F, 0.5F, { 0.5F, offset }
					};
					const AxisTransform<GlobalSpace, DeviceSpace> composed = Compose(output, panel);

					for (const Point<GlobalSpace> probe : { Point<GlobalSpace>{ 0.0, 0.0 },
					                                        Point<GlobalSpace>{ 3.0, -4.5 },
					                                        Point<GlobalSpace>{ -1024.0, 768.0 } })
					{
						GYRO_REQUIRE_EQ(composed.Map(probe), panel.Map(output.Map(probe)));
					}

					// And the same for a rectangle, which is the one damage mapping asks about. Two
					// routes to the same rectangle is the claim; the corners exchange places under
					// half of these orientations, so this is where an assumed top-left corner shows.
					const Rect<GlobalSpace> area{ { -8.0, 2.0 }, { 640.0, 480.0 } };

					GYRO_REQUIRE_EQ(composed.Map(area), panel.Map(output.Map(area)));
				}
			}
		}
	}
}

GYRO_TEST(AxisTransform, ResampleFreeIsExactlyTheTexelForPixelCase)
{
	// The sharpness rung, checked against what it is supposed to mean rather than against how it is
	// computed. "The resample is zero" is one geometric statement: the pixel grid goes to the pixel
	// grid, one for one — every integer position lands on an integer position, and the unit cell
	// arrives as the unit cell. That is what makes text sharp, and Docs/Experience.md states it
	// without a qualifier: content sitting still at its natural size is not soft, on any display, at
	// any scale setting.
	//
	// Checked as an equivalence rather than as an implication, in both directions at once. The
	// forward half is that the rung never claims sharpness it does not have. The converse half is
	// the one worth the sweep: a rung that answered false for everything would satisfy the forward
	// half and cost every surface in the system its fast path, and nothing would look wrong.
	//
	// The second condition is not redundant, which is the trap this is shaped around. An integer
	// scale of 2 takes every integer position to an integer position and resamples regardless —
	// one texel arrives as four pixels — so grid alignment alone is not the property, and a rung
	// derived from mapped positions alone would call a magnified surface sharp.
	//
	// The sweep carries both a uniform and a non-uniform scale at each orientation, because the
	// interesting failure for a per-axis scale is the anamorphic surface that is one-to-one along x:
	// a rung that read either factor alone would call it sharp, and half of its area would be soft.
	bool anyResampleFree = false;
	bool anyResampling = false;

	for (std::uint8_t a = 0; a < OrientationCount; ++a)
	{
		for (std::size_t index = 0; index < DyadicScales.size(); ++index)
		{
			for (const bool uniform : { true, false })
			{
				const float scaleX = DyadicScales[index];
				const float scaleY = uniform ? scaleX : DyadicScales[(index + 2) % DyadicScales.size()];

				for (const float offset : Offsets)
				{
					const AxisTransform<DeviceSpace, DeviceSpace> transform{
						static_cast<AxisOrientation>(a), scaleX, scaleY, { offset, 4.0F }
					};

					bool preservesTheGrid =
						transform.Map(PixelSize<DeviceSpace>{ 1, 1 }) == Size<DeviceSpace>{ 1.0F, 1.0F };

					for (std::int32_t x = -3; x <= 3; ++x)
					{
						for (std::int32_t y = -3; y <= 3; ++y)
						{
							const Point<DeviceSpace> mapped = transform.Map(PixelPoint<DeviceSpace>{ x, y });

							preservesTheGrid = preservesTheGrid && mapped.X == std::trunc(mapped.X) &&
							                   mapped.Y == std::trunc(mapped.Y);
						}
					}

					const bool free = transform.Classify().IsResampleFree();

					GYRO_REQUIRE_EQ(free, preservesTheGrid);

					anyResampleFree = anyResampleFree || free;
					anyResampling = anyResampling || !free;
				}
			}
		}
	}

	GYRO_CHECK(anyResampleFree);
	GYRO_CHECK(anyResampling);
}

GYRO_TEST(AxisTransform, TheInverseReturnsTheValueAtAFractionalScale)
{
	// Hit-testing and pointer constraint read the adapter backwards, and they have to arrive where
	// they started or Docs/Experience.md's promise that every column of the screen can be pointed at
	// is off by however far the reciprocal drifted.
	//
	// Composed rather than applied one after the other, and that is not a convenience. Applying the
	// adapter and then its inverse would put a single-precision device coordinate in the middle of
	// the round trip, which is why Docs/Architecture.md#where-the-integers-are says the pointer is
	// rounded into device space once and never round-tripped back through it. Composition keeps the
	// whole chain at global space's precision, which is where the question was asked.
	//
	// 132/120 is decision 53's own example and is not exact in binary, so unlike everything above
	// this is where a tolerance is the honest form. What it is sized against is the failure it is
	// looking for: a reciprocal taken of the wrong quantity, or a translation inverted before the
	// orientation rather than after, is wrong by pixels rather than by ulps. So is a reciprocal pair
	// left uncrossed under a quarter turn, which is why the two axes here are deliberately unequal —
	// 133/120 against 1.5 rather than one scale used twice.
	constexpr double Tolerance = 1.0e-9;

	for (std::uint8_t a = 0; a < OrientationCount; ++a)
	{
		for (const std::int32_t numerator : { 60, 120, 132, 133, 180, 240 })
		{
			const AxisTransform<GlobalSpace, DeviceSpace> adapter{
				static_cast<AxisOrientation>(a), static_cast<double>(numerator) / 120.0, 1.5, { -37.5, 1080.0 }
			};
			const AxisTransform<GlobalSpace, GlobalSpace> roundTrip = Compose(adapter, adapter.Inverse());

			for (const Point<GlobalSpace> probe : { Point<GlobalSpace>{ 0.0, 0.0 },
			                                        Point<GlobalSpace>{ 3.25, -4.5 },
			                                        Point<GlobalSpace>{ -1024.0, 768.0 } })
			{
				const Offset<GlobalSpace> drift = roundTrip.Map(probe) - probe;

				GYRO_REQUIRE(drift.X < Tolerance && drift.X > -Tolerance);
				GYRO_REQUIRE(drift.Y < Tolerance && drift.Y > -Tolerance);
			}

			// The orientation half of the inverse is exact whatever the scale does, since it is a
			// permutation of two axes and a pair of sign changes.
			GYRO_REQUIRE_EQ(roundTrip.Orientation, AxisOrientation::Normal);
		}
	}
}
