#include "Text/Raster.h"

#include <algorithm>
#include <vector>

#include "Testing/Test.h"

namespace
{
// 8x16, because it is the size every assertion below can be written against by hand and the one a
// console on an ordinary panel actually picks.
const Face& Console()
{
	return Nearest(16);
}

std::vector<std::uint8_t> Draw(std::string_view text, std::int32_t stride = 0)
{
	const TextExtent extent = Measure(Console(), text);
	const std::int32_t row = stride == 0 ? extent.Size.Width : stride;

	std::vector<std::uint8_t> coverage(
		static_cast<std::size_t>(row) * static_cast<std::size_t>(extent.Size.Height), 0xAB
	);

	GYRO_CHECK(Rasterize(Console(), text, coverage, row).has_value());

	return coverage;
}
} // namespace

GYRO_TEST(Raster, MeasuresACellPerCharacter)
{
	GYRO_CHECK(Measure(Console(), "hello").Size == PixelSize<BufferSpace>{ 40, 16 });
	GYRO_CHECK(Measure(Console(), "hello").Lines == 1);
}

GYRO_TEST(Raster, MeasuresTheLongestLine)
{
	const TextExtent extent = Measure(Console(), "ab\ncdef\ng");

	GYRO_CHECK(extent.Size == PixelSize<BufferSpace>{ 32, 48 });
	GYRO_CHECK(extent.Lines == 3);
}

GYRO_TEST(Raster, ATrailingNewlineIsALine)
{
	// A caller stacking labels wants the cursor to have moved, and an empty string is one empty line
	// rather than nothing.
	GYRO_CHECK(Measure(Console(), "a\n").Lines == 2);
	GYRO_CHECK(Measure(Console(), "").Lines == 1);
	GYRO_CHECK(Measure(Console(), "").Size == PixelSize<BufferSpace>{ 0, 16 });
}

GYRO_TEST(Raster, TabsLandOnTheColumn)
{
	// Every log line that will ever reach the recovery console was written against eight.
	GYRO_CHECK(Measure(Console(), "a\tb").Size.Width == 9 * 8);
	GYRO_CHECK(Measure(Console(), "abcdefgh\tb").Size.Width == 17 * 8);
	GYRO_CHECK(Measure(Console(), "\r\n").Lines == 2);
}

GYRO_TEST(Raster, CoverageIsOneBitAndFillsTheExtent)
{
	const std::vector<std::uint8_t> coverage = Draw("Ag");

	GYRO_CHECK(coverage.size() == 16U * 16U);

	// Every texel is defined, including the ones no glyph wrote — the buffer went in filled with a
	// value neither of those is.
	GYRO_CHECK(std::ranges::all_of(coverage, [](std::uint8_t value) { return value == 0 || value == 255; }));
	GYRO_CHECK(std::ranges::any_of(coverage, [](std::uint8_t value) { return value == 255; }));
}

GYRO_TEST(Raster, ASpaceIsBlankAndTwoCharactersDiffer)
{
	const std::vector<std::uint8_t> blank = Draw(" ");

	GYRO_CHECK(std::ranges::all_of(blank, [](std::uint8_t value) { return value == 0; }));
	GYRO_CHECK(!std::ranges::equal(Draw("A"), Draw("B")));
}

GYRO_TEST(Raster, GlyphsLandInTheirOwnCell)
{
	// The defect this catches is the one that makes text look like it drifts: a second character
	// written at the wrong offset. Drawing "A " and " A" and comparing halves says the cell is exactly
	// the advance.
	const std::vector<std::uint8_t> left = Draw("A ");
	const std::vector<std::uint8_t> right = Draw(" A");

	for (std::int32_t y = 0; y < 16; ++y)
	{
		for (std::int32_t x = 0; x < 8; ++x)
		{
			const std::size_t row = static_cast<std::size_t>(y) * 16U;

			GYRO_CHECK(left[row + static_cast<std::size_t>(x)] == right[row + static_cast<std::size_t>(x) + 8U]);
			GYRO_CHECK(right[row + static_cast<std::size_t>(x)] == 0);
		}
	}
}

GYRO_TEST(Raster, LeavesTexelsOutsideTheExtentAlone)
{
	// What lets a caller compose several strings into one image.
	const std::vector<std::uint8_t> coverage = Draw("A", 12);

	for (std::int32_t y = 0; y < 16; ++y)
	{
		GYRO_CHECK(coverage[static_cast<std::size_t>(y) * 12U + 9U] == 0xAB);
	}
}

GYRO_TEST(Raster, RefusesABufferItWouldNotFitIn)
{
	// Clipping would be a debug view that silently lost its last three characters, and the size is
	// knowable before the call.
	std::vector<std::uint8_t> coverage(16U * 16U);

	GYRO_CHECK(!Rasterize(Console(), "hello", coverage, 40).has_value());
	GYRO_CHECK(!Rasterize(Console(), "hello", coverage, 4).has_value());
}
