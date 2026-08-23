#include "Blit/Transfer.h"

#include <cmath>
#include <cstdint>

#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Testing/Test.h"

// The table against the closed form, everywhere.
//
// **A table is the one kind of code that can be wrong only in the middle.** Its ends are what an
// author checks by eye and what a spot test asserts, and a segment index off by one or an
// interpolation that rounds the wrong way is invisible at both ends and shows up as a band across a
// gradient. So the sweep below walks all 65536 inputs rather than sampling them: it costs a
// millisecond and it is the only assertion that means anything about a lookup table.

namespace
{
// A hundredth of an eight-bit code, in the sixteen-bit units the table answers in. The
// interpolation's error is bounded by the curve's second derivative over the segment width, which is
// largest just above the linear segment's knee; this is two orders above what that bound works out
// to, and it is the number to tighten if the sample count ever changes.
constexpr float Tolerance = 65535.0F / 255.0F / 100.0F;
} // namespace

// Both directions of the closed form are inverses, which is what the table is then held to.
GYRO_TEST(Transfer, TheClosedFormsAreInverses)
{
	for (std::uint32_t code = 0; code <= 1023; ++code)
	{
		const float encoded = static_cast<float>(code) / 1023.0F;

		GYRO_REQUIRE(std::abs(LinearToSrgb(SrgbToLinear(encoded)) - encoded) < 1.0e-5F);
	}

	// The two ends are exact rather than nearly so, because they are what black and white are.
	GYRO_CHECK_EQ(SrgbToLinear(0.0F), 0.0F);
	GYRO_CHECK_EQ(LinearToSrgb(0.0F), 0.0F);
	GYRO_CHECK_EQ(SrgbToLinear(1.0F), 1.0F);
	GYRO_CHECK_EQ(LinearToSrgb(1.0F), 1.0F);

	// The knee is where the piecewise definition joins, and a curve that used a bare 2.2 power
	// instead would miss it by several codes — which is the darkest few values of every shadow.
	GYRO_CHECK(std::abs(LinearToSrgb(0.0031308F) - 0.04045F) < 1.0e-5F);
}

// Every input, against the closed form. The assertion that a segment index or a rounding direction
// is right in the middle of the range and not only at its ends.
GYRO_TEST(Transfer, TheTableTracksTheClosedFormAcrossTheWholeRange)
{
	TransferTable table;
	GYRO_REQUIRE(table.Build(TransferFunction::Srgb).has_value());

	float worst = 0.0F;

	for (std::uint32_t value = 0; value <= 65535; ++value)
	{
		const float linear = static_cast<float>(value) / 65535.0F;
		const float exact = LinearToSrgb(linear) * 65535.0F;
		const float given = static_cast<float>(table.Encode(static_cast<std::uint16_t>(value)));

		worst = std::max(worst, std::abs(given - exact));
	}

	GYRO_CHECK(worst < Tolerance);

	// Black and white are exact, which no tolerance would have caught: a table that landed white at
	// 65534 puts a visible seam between the logo and a full-range background.
	GYRO_CHECK_EQ(table.Encode(0), std::uint16_t{ 0 });
	GYRO_CHECK_EQ(table.Encode(65535), std::uint16_t{ 65535 });

	// Monotone, which is what keeps a gradient from reversing at a segment boundary.
	std::uint16_t previous = 0;

	for (std::uint32_t value = 0; value <= 65535; ++value)
	{
		const std::uint16_t given = table.Encode(static_cast<std::uint16_t>(value));

		GYRO_REQUIRE(given >= previous);

		previous = given;
	}
}

// A linear output needs no conversion at all, and an unbuilt table is the same thing — which is what
// makes the identity case cost nothing rather than cost a lookup that happens to be flat.
GYRO_TEST(Transfer, ALinearOutputIsTheIdentity)
{
	TransferTable table;

	GYRO_CHECK_EQ(table.Encode(12345), std::uint16_t{ 12345 });

	GYRO_REQUIRE(table.Build(TransferFunction::Linear).has_value());

	GYRO_CHECK_EQ(table.Encode(0), std::uint16_t{ 0 });
	GYRO_CHECK_EQ(table.Encode(12345), std::uint16_t{ 12345 });
	GYRO_CHECK_EQ(table.Encode(65535), std::uint16_t{ 65535 });
}

// An absolute transfer function is refused rather than approximated. A PQ code is a number of nits,
// so encoding into one means deciding what the composite's full range was worth in light, and that
// is a question Docs/Open.md is still holding rather than one this file should answer by accident.
GYRO_TEST(Transfer, AnAbsoluteTransferFunctionIsRefused)
{
	TransferTable table;

	GYRO_CHECK_EQ(table.Build(TransferFunction::Pq).has_value(), false);
	GYRO_CHECK_EQ(table.Build(TransferFunction::Hlg).has_value(), false);
}

// The number the whole linear-light argument reduces to, stated once where a reader can see it: half
// the light is not half the code.
GYRO_TEST(Transfer, HalfTheLightIsNotHalfTheCode)
{
	// What a half-covered white edge should encode to, which is well above the midpoint.
	GYRO_CHECK(LinearToSrgb(0.5F) > 0.73F);

	// And what a blend in the target's own encoding would have put there instead: code 128 of 255,
	// which is 22% of the light. That gap is the dark rim around every antialiased edge.
	GYRO_CHECK(SrgbToLinear(128.0F / 255.0F) < 0.23F);
}
