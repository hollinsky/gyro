#include "Geometry/Scale.h"

#include <array>
#include <cstdint>
#include <string>

#include "Testing/Test.h"

// The runtime half of Scale.h's contract. The compile-time half is the static_assert block at the
// foot of that header — the worked example, the rounding directions, the clamping — and is not
// repeated here. What is left is the two things a constant expression is the wrong shape for: what
// a scale looks like to whoever reads a log, and the properties that are only convincing swept
// across a range rather than asserted at a point.
//
// Every scale a settings ladder offers, plus the two that break floating point. 132 is 1.1, which
// is the value decision 53 is written about; 133 divides nothing and is here so that no property
// below is passing because its inputs were friendly. 60 is a minifying scale, which is the only
// place the round trip is genuinely lossy.
constexpr std::array Numerators{ 60, 120, 132, 133, 150, 180, 210, 240, 360 };

// Wide enough to cross the global origin in both directions, since that is where the sign-dependent
// bugs live, and a prime-ish stride so the sweep does not sample only friendly residues.
constexpr std::int32_t SweepLow = -2003;
constexpr std::int32_t SweepHigh = 2003;

GYRO_TEST(Scale, FormatsAsAMultiplier)
{
	GYRO_CHECK_EQ(std::format("{}", Scale::FromNumerator(180)), std::string{ "1.5x" });
	GYRO_CHECK_EQ(std::format("{}", Scale{}), std::string{ "1x" });

	// 1.1 is not representable in binary and the stored value is exact regardless, so the rendering
	// is the shortest double that round-trips. A log reader wants to recognise the setting they
	// typed; they do not want 1.0999999999999999.
	GYRO_CHECK_EQ(std::format("{}", Scale::FromNumerator(132)), std::string{ "1.1x" });
}

GYRO_TEST(Scale, EdgesTileButExtentsDoNot)
{
	// Docs/Experience.md promises that two tiled windows meet with no line of background showing
	// between them, and that nothing is one pixel too big or too small at 110%. At this layer that
	// promise is not a property of the conversion at all — it is a rule about *what* gets converted.
	// Edges convert; extents are the differences between converted edges. Never the other way.
	//
	// Three one-logical-pixel windows on a 1.5x output. Converting each extent on its own rounds
	// 1.5 up three times and produces a row six device pixels wide. Converting the edges and taking
	// the differences produces five, which is what 4.5 rounds to. The extra pixel in the first form
	// is the one the next window was going to start at, so the row overflows whatever it was
	// allotted and the last window loses a column — or, tiled against a screen edge, the wallpaper
	// shows through at the far end.
	constexpr Scale Fractional = Scale::FromNumerator(180);

	std::int32_t fromExtents = 0;
	for (std::int32_t window = 0; window < 3; ++window)
	{
		fromExtents += Fractional.DeviceFromLogical(1, Rounding::Nearest);
	}

	const std::int32_t fromEdges =
		Fractional.DeviceFromLogical(3, Rounding::Nearest) - Fractional.DeviceFromLogical(0, Rounding::Nearest);

	GYRO_CHECK_EQ(fromEdges, 5);
	GYRO_CHECK_EQ(fromExtents, 6);
}

GYRO_TEST(Scale, AnEdgeDerivedExtentIsWithinOneDevicePixel)
{
	// What the rule above buys, stated as a bound rather than as an example. An extent taken as the
	// difference of two rounded edges is off by less than a whole device pixel from the exact
	// value, because each edge is off by at most half of one. That is the guarantee a layout needs:
	// a window is never a pixel too wide *and* its neighbour a pixel too narrow, which is what
	// independently converted extents can do and what shows up as a seam that moves as you resize.
	//
	// Compared in integers. Routing the exact value through a double to check it would be checking
	// the conversion against the arithmetic it exists to avoid.
	for (const std::int32_t numerator : Numerators)
	{
		const Scale scale = Scale::FromNumerator(numerator);

		for (std::int32_t left = SweepLow; left <= SweepHigh; left += 7)
		{
			for (const std::int32_t span : { 1, 2, 3, 17, 640, 1281 })
			{
				const std::int64_t extent = scale.DeviceFromLogical(left + span, Rounding::Nearest) -
				                            scale.DeviceFromLogical(left, Rounding::Nearest);

				// |extent - span * numerator / 120| < 1, without the division.
				const std::int64_t error = extent * Scale::Denominator - static_cast<std::int64_t>(span) * numerator;

				GYRO_REQUIRE(error < Scale::Denominator && error > -Scale::Denominator);
			}
		}
	}
}

GYRO_TEST(Scale, RoundingIsSymmetricAboutTheGlobalOrigin)
{
	// Where the global origin sits is an accident of which monitor was called primary, so an output
	// to the left of it holds negative coordinates as an ordinary matter. If rounding went toward
	// zero, a window would land on a different device pixel depending on which side of that
	// accident it was on, and the bug would present as "it is crisp on my left monitor".
	for (const std::int32_t numerator : Numerators)
	{
		const Scale scale = Scale::FromNumerator(numerator);

		for (std::int32_t value = SweepLow; value <= SweepHigh; ++value)
		{
			// Floor and ceiling are reflections of each other. This is the property truncating
			// division does not have, and it is the whole reason Detail::FloorDivide exists.
			GYRO_REQUIRE_EQ(
				scale.DeviceFromLogical(-value, Rounding::Down), -scale.DeviceFromLogical(value, Rounding::Up)
			);

			// Nearest breaks ties toward positive infinity on both sides rather than away from
			// zero, so it reflects everywhere except on an exact half, where both sides round up
			// and the pair sums to one rather than to zero. Ties away from zero would sum to zero
			// here and put the sign dependence back half a pixel further along, where it would be
			// found by a user and not by this test.
			const std::int32_t sum =
				scale.DeviceFromLogical(value, Rounding::Nearest) + scale.DeviceFromLogical(-value, Rounding::Nearest);

			GYRO_REQUIRE(sum == 0 || sum == 1);
		}
	}
}

GYRO_TEST(Scale, ConversionIsMonotonic)
{
	// A window's right edge must not be able to cross its left edge. Non-monotonic conversion would
	// make that possible for one value in one rounding mode, and the negative extent it produced
	// would be caught much later and much further away — as a scissor rectangle the renderer
	// refuses, on one scale, at one position.
	for (const std::int32_t numerator : Numerators)
	{
		const Scale scale = Scale::FromNumerator(numerator);

		for (const Rounding rounding : { Rounding::Down, Rounding::Nearest, Rounding::Up })
		{
			std::int32_t previous = scale.DeviceFromLogical(SweepLow - 1, rounding);

			for (std::int32_t value = SweepLow; value <= SweepHigh; ++value)
			{
				const std::int32_t current = scale.DeviceFromLogical(value, rounding);

				GYRO_REQUIRE(current >= previous);
				previous = current;
			}
		}
	}
}

GYRO_TEST(Scale, RoundTripStaysWithinOneLogicalPixel)
{
	// The conversion is lossy in the minifying direction and cannot be otherwise — two logical
	// pixels genuinely share a device pixel at scale 0.5. What matters is that the loss is bounded
	// at a pixel rather than accumulating, because a surface moving between outputs is converted
	// through this and back, and a drag along a monitor boundary does it repeatedly.
	for (const std::int32_t numerator : Numerators)
	{
		const Scale scale = Scale::FromNumerator(numerator);

		for (std::int32_t value = SweepLow; value <= SweepHigh; ++value)
		{
			const std::int32_t device = scale.DeviceFromLogical(value, Rounding::Nearest);
			const std::int32_t logical = scale.LogicalFromDevice(device, Rounding::Nearest);

			GYRO_REQUIRE(logical - value <= 1 && value - logical <= 1);
		}
	}
}
