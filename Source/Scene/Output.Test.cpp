#include "Scene/Output.h"

#include "Geometry/AxisTransform.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Testing/Test.h"

// Docs/Decisions.md decision 97's published half of the view: what the world says about where an
// output sits, which the frame side composes with the extent the *mode* achieved.

namespace
{
// A second 1080p panel to the right of a first one, at 1.5x — the arrangement where getting the order
// of the orientation, the scale and the offset wrong still puts something on screen.
[[nodiscard]] SceneOutput Secondary()
{
	return { .Bounds = { { 1920.0, 0.0 }, { 1280.0, 720.0 } },
		     .Density = Scale::FromDouble(1.5),
		     .Grid = { 1920, 1080 } };
}
} // namespace

GYRO_TEST(SceneOutput, ThePlacementCarriesTheOutputsOwnOriginToItsOwnCorner)
{
	const OutputAdapter placement = Secondary().Placement();

	const Point<DeviceSpace> corner = placement.Map(Point<GlobalSpace>{ 1920.0, 0.0 });

	GYRO_CHECK_EQ(corner.X, 0.0F);
	GYRO_CHECK_EQ(corner.Y, 0.0F);

	// And the far corner of the logical rectangle lands on the last row of the device grid, which is
	// what makes 1280 x 720 at 1.5x the same panel as 1920 x 1080 — Geometry/Scale.h's exact rational
	// doing the arithmetic rather than a float that is nearly it.
	const Point<DeviceSpace> far = placement.Map(Point<GlobalSpace>{ 3200.0, 720.0 });

	GYRO_CHECK_EQ(far.X, 1920.0F);
	GYRO_CHECK_EQ(far.Y, 1080.0F);
}

GYRO_TEST(SceneOutput, TheFirstOutputAtUnitScaleIsTheIdentity)
{
	const SceneOutput primary{ .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } };

	// Worth pinning because it is the case every test that does not care about placement writes by
	// hand, and a placement that was merely *nearly* the identity would make those tests pass with an
	// offset nobody put there.
	GYRO_CHECK(primary.Placement() == OutputAdapter::Identity());
}

GYRO_TEST(SceneOutput, AQuarterTurnPutsTheWholePanelOnItsOwnGrid)
{
	// A 1920 x 1080 panel at 1.5x stood on end: 720 wide and 1280 high in the world, to the right of a
	// first one. The rectangle is turned along with the panel, which is what makes this a monitor in a
	// portrait stand rather than the same landscape monitor with a transform bolted on.
	const SceneOutput turned{ .Bounds = { { 1920.0, 0.0 }, { 720.0, 1280.0 } },
		                      .Density = Scale::FromDouble(1.5),
		                      .Grid = { 1920, 1080 },
		                      .Orientation = AxisOrientation::Rotate90 };

	const OutputAdapter placement = turned.Placement();

	GYRO_CHECK(placement.Orientation == AxisOrientation::Rotate90);

	// **The origin is not the corner it lands on, and that is the whole of what a turn means.** A
	// quarter turn takes the rectangle's top-left to the panel's bottom-left, so asserting that the
	// origin maps to nothing — which it does under every orientation, because the translation used to
	// be built out of it — is a test that could never fail. All four corners are checked instead.
	const Point<DeviceSpace> topLeft = placement.Map(Point<GlobalSpace>{ 1920.0, 0.0 });
	const Point<DeviceSpace> topRight = placement.Map(Point<GlobalSpace>{ 2640.0, 0.0 });
	const Point<DeviceSpace> bottomLeft = placement.Map(Point<GlobalSpace>{ 1920.0, 1280.0 });
	const Point<DeviceSpace> bottomRight = placement.Map(Point<GlobalSpace>{ 2640.0, 1280.0 });

	GYRO_CHECK_EQ(topLeft.X, 0.0F);
	GYRO_CHECK_EQ(topLeft.Y, 1080.0F);

	GYRO_CHECK_EQ(topRight.X, 0.0F);
	GYRO_CHECK_EQ(topRight.Y, 0.0F);

	GYRO_CHECK_EQ(bottomLeft.X, 1920.0F);
	GYRO_CHECK_EQ(bottomLeft.Y, 1080.0F);

	GYRO_CHECK_EQ(bottomRight.X, 1920.0F);
	GYRO_CHECK_EQ(bottomRight.Y, 0.0F);
}

GYRO_TEST(SceneOutput, NoOrientationSendsAnyPartOfAnOutputOffItsOwnGrid)
{
	// The property the eight cases share, and the one that was false for six of them: whatever the
	// panel is doing in its stand, its rectangle covers its grid exactly. A turn is a permutation about
	// the coordinate origin with no re-centring, so every orientation that carries a corner negative
	// needs the rectangle's own extent putting back — and a monitor that did not get it drew nothing at
	// all, which is a black screen rather than a picture that is merely askew.
	constexpr AxisOrientation Orientations[]{ AxisOrientation::Normal,     AxisOrientation::Rotate90,
		                                      AxisOrientation::Rotate180,  AxisOrientation::Rotate270,
		                                      AxisOrientation::Flipped,    AxisOrientation::Flipped90,
		                                      AxisOrientation::Flipped180, AxisOrientation::Flipped270 };

	for (const AxisOrientation orientation : Orientations)
	{
		const bool swapped = SwapsAxes(orientation);

		const SceneOutput output{ .Bounds = { { 1920.0, 300.0 }, { swapped ? 540.0 : 960.0, swapped ? 960.0 : 540.0 } },
			                      .Density = Scale::FromInteger(2),
			                      .Grid = { 1920, 1080 },
			                      .Orientation = orientation };

		const OutputAdapter placement = output.Placement();

		for (const double x : { output.Bounds.Left(), output.Bounds.Right() })
		{
			for (const double y : { output.Bounds.Top(), output.Bounds.Bottom() })
			{
				const Point<DeviceSpace> at = placement.Map(Point<GlobalSpace>{ x, y });

				GYRO_CHECK(at.X >= 0.0F && at.X <= 1920.0F);
				GYRO_CHECK(at.Y >= 0.0F && at.Y <= 1080.0F);
			}
		}

		// And the middle of the rectangle is the middle of the panel, which no turn may move.
		const Point<DeviceSpace> centre = placement.Map(
			Point<GlobalSpace>{ output.Bounds.Left() + output.Bounds.Extent.Width / 2.0,
		                        output.Bounds.Top() + output.Bounds.Extent.Height / 2.0 }
		);

		GYRO_CHECK_EQ(centre.X, 960.0F);
		GYRO_CHECK_EQ(centre.Y, 540.0F);
	}
}

GYRO_TEST(SceneOutput, ThePlacementIsValidAndSaysWhatItComposedFrom)
{
	const OutputAdapter placement = Secondary().Placement();

	GYRO_CHECK(placement.IsValid());
	GYRO_CHECK_EQ(placement.ScaleX, 1.5);
	GYRO_CHECK_EQ(placement.ScaleY, 1.5);
	GYRO_CHECK_EQ(placement.Translation.X, -2880.0);
}
