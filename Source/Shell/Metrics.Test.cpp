#include "Shell/Metrics.h"

#include "Testing/Test.h"

namespace
{
// A 1080p panel and a 4K one, which between them are the two ends the ladder has to hold: the first is
// where the card must not be so large it dominates the screen, the second where a bar sized off pixels
// used to come out at four times the intended size.
constexpr PixelSize<BufferSpace> Small{ 1920, 1080 };
constexpr PixelSize<BufferSpace> Wide{ 3840, 2160 };
} // namespace

// **The card is the same size at the eye whatever the panel is**, which is the whole claim of
// measuring in logical pixels rather than in a fraction of the screen. A 4K panel a person sits the
// same distance from derives scale 2 and gets a logical extent of 1920x1080 — the same room the 1080p
// panel has — so the card must come out at twice the device pixels and identical logical ones.
GYRO_TEST(BarMetrics, PutsTheSameCardOnAPanelOfTwiceTheDensity)
{
	const BarMetrics single = BarMetrics::For(Small, Scale::FromInteger(1));
	const BarMetrics doubled = BarMetrics::For(Small, Scale::FromInteger(2));

	GYRO_REQUIRE(single.Fits && doubled.Fits);

	GYRO_CHECK_EQ(doubled.Surface.Width, 2 * single.Surface.Width);
	GYRO_CHECK_EQ(doubled.Card.Width, 2 * single.Card.Width);
	GYRO_CHECK_EQ(doubled.PadX, 2 * single.PadX);

	// The card is capped rather than proportional, so a screen twice as wide in logical pixels does not
	// get a card twice as wide — it gets the same card with more space around it.
	const BarMetrics wide = BarMetrics::For(Wide, Scale::FromInteger(1));

	GYRO_CHECK_EQ(wide.Card.Width, single.Card.Width);
	GYRO_CHECK(wide.Origin.X > single.Origin.X);
}

// The exact rational is the case the integer path cannot express, and it is the one `--ui-size` puts
// people on: 1.25 and 1.5 are ordinary derived scales. What matters is that the arithmetic stays
// exact — the surface has to cover the logical extent with nothing left over.
GYRO_TEST(BarMetrics, CoversTheOutputAtAFractionalScale)
{
	for (const std::int32_t numerator : { 120, 150, 180, 210, 240 })
	{
		const Scale scale = Scale::FromNumerator(numerator);
		const BarMetrics metrics = BarMetrics::For(Small, scale);

		GYRO_REQUIRE(metrics.Fits);

		// Covers, and by less than a whole logical pixel — the surface rounds up, so it is never short and
		// never a pixel of scrim missing along an edge.
		GYRO_CHECK(scale.LogicalFromDevice(metrics.Surface.Width, Rounding::Down) >= Small.Width);
		GYRO_CHECK(metrics.Surface.Width - scale.DeviceFromLogical(Small.Width, Rounding::Down) <= 1);

		// The card sits inside the screen with its margin intact, at every rung.
		GYRO_CHECK(metrics.Origin.X > 0);
		GYRO_CHECK(metrics.Origin.X + metrics.Card.Width <= metrics.Surface.Width);
		GYRO_CHECK(metrics.Origin.Y + metrics.Card.Height <= metrics.Surface.Height);
	}
}

// The card hugs whichever rung of the bitmap ladder the scale landed on, rather than the height that
// was asked for. A card sized from the nominal height would leave the text sitting high in it at every
// scale where the two disagree, which is most of them — see Metrics.h.
GYRO_TEST(BarMetrics, SizesTheCardFromTheFaceItActuallyGot)
{
	for (const std::int32_t numerator : { 120, 150, 180, 240 })
	{
		const BarMetrics metrics = BarMetrics::For(Small, Scale::FromNumerator(numerator));

		GYRO_REQUIRE(metrics.Text != nullptr);
		GYRO_CHECK_EQ(metrics.Card.Height, metrics.Text->CellSize().Height + 2 * metrics.PadY);
	}
}

// A screen with no room for the smallest card says so instead of returning one that would be drawn off
// both edges. The bar reads this and stays down — a launcher running off the sides of a screen is
// worse than one that did not appear.
GYRO_TEST(BarMetrics, RefusesAScreenTooNarrowForACard)
{
	GYRO_CHECK(!BarMetrics::For({ 320, 240 }, Scale::FromInteger(1)).Fits);
	GYRO_CHECK(BarMetrics::For({ 1024, 768 }, Scale::FromInteger(1)).Fits);
}

// The card sits above the middle of the screen, which is where a person's eyes rest and where it
// covers least of what is behind it. Stated as a test because it is the one number here that is taste
// rather than arithmetic, so it should be a deliberate change when somebody moves it.
GYRO_TEST(BarMetrics, PutsTheCardAboveTheMiddleOfTheScreen)
{
	const BarMetrics metrics = BarMetrics::For(Small, Scale::FromInteger(1));

	GYRO_REQUIRE(metrics.Fits);
	GYRO_CHECK(metrics.Origin.Y + metrics.Card.Height / 2 < metrics.Surface.Height / 2);
	GYRO_CHECK(metrics.Origin.Y > 0);
}
