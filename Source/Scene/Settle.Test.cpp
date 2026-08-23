#include "Scene/Settle.h"

#include <cmath>
#include <span>

#include "Animation/Author/Animatable.h"
#include "Animation/Author/Motion.h"
#include "Animation/Author/Retarget.h"
#include "Animation/Solve/Spring.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Scene/Output.h"
#include "Testing/Test.h"

// The numbers, and the two properties that make them safe rather than merely chosen.
//
// What a threshold *is* is a judgement Docs/Open.md has not closed, so nothing here asserts a value
// against an argument about perception — that would be pinning taste with a test. What is asserted is
// the pair of things a wrong number breaks silently: that every threshold is strictly positive, since a
// zero is never crossed and a channel handed one never settles; and that resolving against a finer
// output set only ever produces a *smaller* threshold, since the substitution the resolver makes for
// decision 54's *finest grid a node intersects* is only conservative if that direction holds.

namespace
{
[[nodiscard]] SceneOutput Panel(std::int32_t width, std::int32_t height, Scale density)
{
	return { .Density = density, .Grid = { width, height } };
}
} // namespace

GYRO_TEST(SettleThresholds, EveryThresholdIsPositiveOnAnEmptyOutputSet)
{
	const SettleThresholdSet thresholds{ {} };

	// A scene with nowhere to be drawn still has springs running in it — a headless test, a machine
	// between modesets, a session whose outputs have not been assigned. A threshold of zero there is not
	// a smaller number, it is a channel that never settles, and the compositor draws forever.
	GYRO_CHECK(thresholds.Translation().Position > 0.0);
	GYRO_CHECK(thresholds.Translation().Velocity > 0.0);
	GYRO_CHECK(thresholds.Scaling().Position > 0.0F);
	GYRO_CHECK(thresholds.Scaling().Velocity > 0.0F);
	GYRO_CHECK(thresholds.Rotation().Position > 0.0F);
	GYRO_CHECK(thresholds.Opacity().Position > 0.0F);
	GYRO_CHECK(thresholds.Opacity().Velocity > 0.0F);

	GYRO_CHECK_EQ(thresholds.Density(), Scale::FromInteger(1));
}

GYRO_TEST(SettleThresholds, TheGeometricThresholdIsInDevicePixelsOfTheFinestGrid)
{
	const SceneOutput set[] = { Panel(1920, 1080, Scale::FromInteger(1)),
		                        Panel(2560, 1440, Scale::FromNumerator(180)) };

	const SettleThresholdSet thresholds{ set };

	// The finest grid in the set, not the first one and not the coarsest: settle against the coarse
	// panel and the fine one is left crawling, which is the failure decision 54 names.
	GYRO_REQUIRE_EQ(thresholds.Density(), Scale::FromNumerator(180));

	// A sixteenth of a device pixel, expressed in the global units a translation is in — so on a 1.5x
	// panel the world-space tolerance is two thirds of the pixel one.
	GYRO_CHECK_EQ(thresholds.Translation().Position, (1.0 / 16.0) / 1.5);
	GYRO_CHECK_EQ(thresholds.Translation().Velocity, 1.0 / 1.5);
}

GYRO_TEST(SettleThresholds, AFinerOutputSetOnlyEverTightens)
{
	const SceneOutput coarse[] = { Panel(1920, 1080, Scale::FromInteger(1)) };
	const SceneOutput fine[] = { Panel(1920, 1080, Scale::FromInteger(1)), Panel(3840, 2160, Scale::FromInteger(2)) };

	const SettleThresholdSet one{ coarse };
	const SettleThresholdSet both{ fine };

	// This is the whole of why *every output* is an honest stand-in for *the outputs this node touches*.
	// The set the resolver is handed is a superset of the set decision 54 asks for, and a superset can
	// only make the grid finer, so the threshold can only get smaller and the settle can only get later.
	// Later is redundant composites at the tail of a transition; earlier is an animation freezing.
	GYRO_CHECK(both.Translation().Position <= one.Translation().Position);
	GYRO_CHECK(both.Translation().Velocity <= one.Translation().Velocity);
	GYRO_CHECK(both.Scaling().Position <= one.Scaling().Position);
	GYRO_CHECK(both.Rotation().Position <= one.Rotation().Position);

	// The radius a dimensionless residual is judged at is the largest half-diagonal in the set, so a
	// second panel widens it and tightens the angular tolerance with it.
	GYRO_CHECK(both.ConversionRadius() >= one.ConversionRadius());
	GYRO_CHECK_EQ(both.ConversionRadius(), std::hypot(3840.0, 2160.0) / 2.0);
}

GYRO_TEST(SettleThresholds, ADimensionlessResidualIsJudgedAtARadiusAndTheDensityCancels)
{
	const SceneOutput set[] = { Panel(1920, 1080, Scale::FromNumerator(180)) };

	const SettleThresholdSet thresholds{ set };

	// Scale is dimensionless and a log-map rotation is radians, and both displace a point at radius r by
	// about eps*r. So the tolerance is the pixel threshold over the radius *in pixels* — the density
	// appears on both sides of that ratio and drops out, which is why the resolver never applies it here.
	const auto expected = static_cast<float>((1.0 / 16.0) / (std::hypot(1920.0, 1080.0) / 2.0));

	GYRO_CHECK_EQ(thresholds.Scaling().Position, expected);
	GYRO_CHECK_EQ(thresholds.Rotation().Position, expected);
}

GYRO_TEST(SettleThresholds, ARealTransitionSettlesWellAfterItStopsAndNotBefore)
{
	const SceneOutput set[] = { Panel(1920, 1080, Scale::FromInteger(1)) };
	const SettleThresholdSet thresholds{ set };

	// A window moving four hundred global units under the catalog's workhorse motion, which is the
	// commonest thing in the system and therefore the one the numbers have to be sane for.
	Animatable<Vector3<double>> position{ Vector3<double>{ 100.0, 100.0, 0.0 } };
	position.AnimateTo({ 500.0, 100.0, 0.0 }, ParametersFromResponse(0.4, 1.0), Instant{});

	GYRO_REQUIRE(position.NextWake(Instant{}, thresholds.Translation()) == Wake::EveryFrame(Instant{}));

	// Still owed frames a third of the way in, and finished inside a second and a half. The upper bound
	// is loose on purpose and the slack in it is not these thresholds': a four-hundred-unit move under a
	// 0.4 second response is analytically finished in about six tenths of a second, and the extra comes
	// almost entirely from the critical regime's envelope, which decision 11 accepts at up to twice the
	// true settle. Halving the position threshold from an eighth of a pixel to a sixteenth moves this
	// instant by under a tenth of a second, which is the check that the number chosen is not the thing
	// paying for the tail.
	GYRO_CHECK(
		position.NextWake(Monotonic::FromNanoseconds(300'000'000), thresholds.Translation()).Which !=
		Wake::Kind::Settled
	);
	GYRO_CHECK(position.NextWake(Monotonic::FromNanoseconds(1'500'000'000), thresholds.Translation()) == Wake::Never());

	// And the residual at the moment it settles is under the threshold it settled against, which is what
	// makes decision 54's snap to the device grid invisible — the whole reason the number is sub-pixel.
	const SpringState<Vector3<double>> at = position.PresentationState(Monotonic::FromNanoseconds(1'500'000'000));

	GYRO_CHECK(Magnitude(at.Position - position.Model()) < thresholds.Translation().Position);
	GYRO_CHECK(Magnitude(at.Velocity) < thresholds.Translation().Velocity);
}

GYRO_TEST(SettleThresholds, OpacitySettlesBelowAnEightBitCodePoint)
{
	const SettleThresholdSet thresholds{ {} };

	Animatable<float> opacity{ 1.0F };
	opacity.AnimateTo(0.0F, ParametersFromResponse(0.4F, 1.0F), Instant{});

	GYRO_REQUIRE(opacity.NextWake(Instant{}, thresholds.Opacity()).Which == Wake::Kind::Continuous);
	GYRO_REQUIRE(opacity.NextWake(Monotonic::FromNanoseconds(2'000'000'000), thresholds.Opacity()) == Wake::Never());

	// Half a code point of an eight-bit channel, which is the narrowest claim available: below it, two
	// values cannot be the difference between two pixels that leave the machine. It is a
	// representability argument and not a perceptual one, and Docs/Open.md still owns the second.
	const float residual = opacity.Presentation(Monotonic::FromNanoseconds(2'000'000'000)) - opacity.Model();

	GYRO_CHECK(Magnitude(residual) * 255.0F < 0.5F);
}
