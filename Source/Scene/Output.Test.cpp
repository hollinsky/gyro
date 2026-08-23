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

GYRO_TEST(SceneOutput, AQuarterTurnOrientsBeforeItScalesAndTranslates)
{
	SceneOutput turned = Secondary();
	turned.Orientation = AxisOrientation::Rotate90;

	const OutputAdapter placement = turned.Placement();

	// The adapter's own order is orient, scale, translate — so the offset that has to be stored is the
	// *oriented* origin negated. Mapping the origin back through it is the statement of that, and the
	// wrong order leaves the second monitor's picture a screen and a half away from where it is.
	const Point<DeviceSpace> corner = placement.Map(Point<GlobalSpace>{ 1920.0, 0.0 });

	GYRO_CHECK_EQ(corner.X, 0.0F);
	GYRO_CHECK_EQ(corner.Y, 0.0F);
	GYRO_CHECK(placement.Orientation == AxisOrientation::Rotate90);
}

GYRO_TEST(SceneOutput, ThePlacementIsValidAndSaysWhatItComposedFrom)
{
	const OutputAdapter placement = Secondary().Placement();

	GYRO_CHECK(placement.IsValid());
	GYRO_CHECK_EQ(placement.ScaleX, 1.5);
	GYRO_CHECK_EQ(placement.ScaleY, 1.5);
	GYRO_CHECK_EQ(placement.Translation.X, -2880.0);
}
