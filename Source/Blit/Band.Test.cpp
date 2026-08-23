#include "Blit/Band.h"

#include <array>
#include <cstdint>
#include <span>

#include "Core/Result.h"
#include "Testing/Test.h"

// The scratch and the algebra over it.
//
// Everything here is arithmetic over a vector, so it runs where `Blit` runs — which is a machine
// with no GPU, no seat and no panel, and is the whole reason this module is portable.

namespace
{
constexpr Light White{ 65535, 65535, 65535, 65535 };
constexpr Light Black{ 0, 0, 0, 65535 };
constexpr Light Red{ 65535, 0, 0, 65535 };
} // namespace

// The band is sized to stay in cache, so a wide output gets a short band and a narrow one gets the
// ceiling rather than a very tall column.
GYRO_TEST(Band, TheBandIsSizedToTheWidthAndNotToThePanel)
{
	const std::int32_t wide = Band::RowsFor(3840);
	const std::int32_t narrow = Band::RowsFor(64);

	GYRO_CHECK(wide >= 1);
	GYRO_CHECK(narrow > wide);

	// The bytes a band occupies do not grow with the panel, which is the property that keeps this
	// module's footprint off the resolution.
	GYRO_CHECK(static_cast<std::size_t>(wide) * 3840 * sizeof(Light) <= 256U * 1024U);

	GYRO_CHECK_EQ(Band::RowsFor(0), 0);
	GYRO_CHECK_EQ(Band::RowsFor(-1), 0);
}

GYRO_TEST(Band, ReserveTakesAWidthAndRefusesWhatIsNotOne)
{
	Band band;

	GYRO_CHECK_EQ(band.Width(), 0);
	GYRO_CHECK_EQ(band.Rows(), 0);

	GYRO_CHECK_EQ(band.Reserve(0).has_value(), false);
	GYRO_CHECK_EQ(band.Reserve(-8).has_value(), false);
	GYRO_CHECK_EQ(band.Reserve(1 << 20).has_value(), false);

	GYRO_REQUIRE(band.Reserve(16).has_value());
	GYRO_CHECK_EQ(band.Width(), 16);
	GYRO_CHECK(band.Rows() > 0);

	band.Release();
	GYRO_CHECK_EQ(band.Width(), 0);
	GYRO_CHECK(band.Row(0).empty());
}

// The clear puts opaque black down, which is the bottom of every composite and the reason nothing
// here ever reads the target.
GYRO_TEST(Band, TheClearIsOpaqueBlackAndOnlyWhereItWasAsked)
{
	Band band;
	GYRO_REQUIRE(band.Reserve(8).has_value());

	band.Clear(2, 2, 5);

	GYRO_CHECK_EQ(band.At(0, 1), Light{});
	GYRO_CHECK_EQ(band.At(0, 2), Black);
	GYRO_CHECK_EQ(band.At(0, 4), Black);
	GYRO_CHECK_EQ(band.At(0, 5), Light{});
	GYRO_CHECK_EQ(band.At(1, 3), Black);

	// The third row was not asked for and is untouched, which is what makes a band shorter than the
	// scratch safe to leave dirty.
	GYRO_CHECK_EQ(band.At(2, 3), Light{});
}

// An opaque run replaces and a translucent one composites, and the difference is a branch rather
// than a second entry point — most of a boot screen is the opaque case.
GYRO_TEST(Band, AnOpaqueRunReplacesAndATranslucentOneComposites)
{
	Band band;
	GYRO_REQUIRE(band.Reserve(8).has_value());

	band.Clear(1, 0, 8);
	band.BlendRun(0, 2, 5, Red);

	GYRO_CHECK_EQ(band.At(0, 1), Black);
	GYRO_CHECK_EQ(band.At(0, 2), Red);
	GYRO_CHECK_EQ(band.At(0, 4), Red);
	GYRO_CHECK_EQ(band.At(0, 5), Black);

	// Half-covered white over red: half the white plus half of what was there, in linear light.
	band.BlendRun(0, 2, 3, Attenuate(White, 32768));

	const Light blended = band.At(0, 2);
	GYRO_CHECK_EQ(blended.Alpha, std::uint16_t{ 65535 });
	GYRO_CHECK(blended.Red > 65000);
	GYRO_CHECK(blended.Green > 32000 && blended.Green < 33500);

	// Nothing to add is nothing written, which is what an item's own bounding box produces outside
	// its coverage.
	band.BlendRun(0, 0, 8, Light{});
	GYRO_CHECK_EQ(band.At(0, 4), Red);
}

// A run outside the band writes nothing rather than walking off the end, which is what lets the
// caller hand over a rectangle in the output's coordinates without narrowing it first.
GYRO_TEST(Band, ARunOutsideTheBandWritesNothing)
{
	Band band;
	GYRO_REQUIRE(band.Reserve(4).has_value());

	band.Clear(1, 0, 4);

	band.BlendRun(0, -10, 0, Red);
	band.BlendRun(0, 4, 40, Red);
	band.BlendRun(-1, 0, 4, Red);
	band.BlendRun(band.Rows(), 0, 4, Red);

	for (std::int32_t column = 0; column < 4; ++column)
	{
		GYRO_REQUIRE_EQ(band.At(0, column), Black);
	}

	// A run that straddles an edge paints the half that is inside.
	band.BlendRun(0, -2, 2, Red);
	GYRO_CHECK_EQ(band.At(0, 0), Red);
	GYRO_CHECK_EQ(band.At(0, 2), Black);

	// Outside the band entirely, `At` answers transparent black — a value the composite cannot hold,
	// so a misindexed read is unambiguous rather than plausible.
	GYRO_CHECK_EQ(band.At(0, 99), Light{});
}

// An opaque backdrop stays exactly opaque however many items land on it, which is what lets the
// encode skip unpremultiplying and what would otherwise decay a boot screen one bit at a time.
GYRO_TEST(Band, AnOpaqueCompositeStaysExactlyOpaque)
{
	Band band;
	GYRO_REQUIRE(band.Reserve(4).has_value());

	band.Clear(1, 0, 4);

	for (int pass = 0; pass < 64; ++pass)
	{
		band.BlendRun(0, 0, 4, Attenuate(White, 1000));
	}

	GYRO_CHECK_EQ(band.At(0, 0).Alpha, std::uint16_t{ 65535 });
	GYRO_CHECK_EQ(band.At(0, 3).Alpha, std::uint16_t{ 65535 });
}

// One row of the band, which is what the encode reads out of it.
GYRO_TEST(Band, ARowIsWhatTheEncodeReads)
{
	Band band;
	GYRO_REQUIRE(band.Reserve(4).has_value());

	band.Clear(1, 0, 4);
	band.BlendPixel(0, 1, Red);

	const std::span<const Light> row = band.Row(0);
	GYRO_REQUIRE_EQ(row.size(), std::size_t{ 4 });
	GYRO_CHECK_EQ(row[0], Black);
	GYRO_CHECK_EQ(row[1], Red);

	GYRO_CHECK(band.Row(-1).empty());
	GYRO_CHECK(band.Row(band.Rows()).empty());
}

// The per-pixel form of the run, which is what a texture makes it. The source varies along the span
// and is positional over it, so a run whose left edge got clipped away reads from the middle of the
// span rather than from its start — which is the off-by-one that would show up as a texture shifted
// by one pixel only where it meets the edge of the damage.
GYRO_TEST(Band, ASampledRunIsPositionalOverTheRunAndClipsWithIt)
{
	Band band;
	GYRO_REQUIRE(band.Reserve(8).has_value());
	band.Clear(band.Rows(), 0, 8);

	const std::array<Light, 4> ramp{ Light{ 65535, 0, 0, 65535 },
		                             Light{ 0, 65535, 0, 65535 },
		                             Light{ 0, 0, 65535, 65535 },
		                             Light{ 65535, 65535, 65535, 65535 } };

	band.BlendRun(0, 2, 6, std::span<const Light>{ ramp });

	GYRO_CHECK_EQ(band.At(0, 1), Black);
	GYRO_CHECK_EQ(band.At(0, 2), ramp[0]);
	GYRO_CHECK_EQ(band.At(0, 5), ramp[3]);
	GYRO_CHECK_EQ(band.At(0, 6), Black);

	// Clipped on the left: the span is still measured from where the caller said the run started, so
	// the pixel that survives is the one that was always going to land there.
	band.Clear(band.Rows(), 0, 8);
	band.BlendRun(0, -2, 2, std::span<const Light>{ ramp });

	GYRO_CHECK_EQ(band.At(0, 0), ramp[2]);
	GYRO_CHECK_EQ(band.At(0, 1), ramp[3]);
	GYRO_CHECK_EQ(band.At(0, 2), Black);

	// A span shorter than the run it is for writes what it has rather than walking off the end, which
	// is the same answer `Row` gives a row the band does not hold.
	band.Clear(band.Rows(), 0, 8);
	band.BlendRun(0, 0, 8, std::span<const Light>{ ramp.data(), 2 });

	GYRO_CHECK_EQ(band.At(0, 1), ramp[1]);
	GYRO_CHECK_EQ(band.At(0, 2), Black);

	// A row the band does not hold writes nothing at all.
	band.BlendRun(band.Rows(), 0, 4, std::span<const Light>{ ramp });
	band.BlendRun(-1, 0, 4, std::span<const Light>{ ramp });
}

// A translucent sample composites rather than replaces, which is the one thing the per-pixel form
// cannot hoist out of its loop: whether a pixel is opaque is a property of the texel.
GYRO_TEST(Band, ASampledRunCompositesPerPixel)
{
	Band band;
	GYRO_REQUIRE(band.Reserve(4).has_value());
	band.Clear(band.Rows(), 0, 4);
	band.BlendRun(0, 0, 4, Red);

	const std::array<Light, 4> over{ Light{}, Light{ 0, 32768, 0, 32768 }, Light{ 0, 0, 65535, 65535 }, Light{} };

	band.BlendRun(0, 0, 4, std::span<const Light>{ over });

	GYRO_CHECK_EQ(band.At(0, 0), Red);
	GYRO_CHECK_EQ(band.At(0, 2), Light{ 0, 0, 65535, 65535 });
	GYRO_CHECK_EQ(band.At(0, 3), Red);

	// Half green over full red, premultiplied: half the red survives and the alpha stays exactly full
	// range, which is what lets the encode skip unpremultiplying.
	const Light mixed = band.At(0, 1);

	GYRO_CHECK_EQ(mixed.Green, std::uint16_t{ 32768 });
	GYRO_CHECK_EQ(mixed.Alpha, std::uint16_t{ 65535 });
	GYRO_CHECK(mixed.Red > 32000 && mixed.Red < 33000);
}
