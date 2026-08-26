#include "Text/Label.h"

#include <cstdint>

#include "Testing/Test.h"
#include "Text/Raster.h"

namespace
{
constexpr std::uint32_t White = 0xFFFFFFFF;
constexpr std::uint32_t Clear = 0x00000000;
} // namespace

GYRO_TEST(Label, IsTheTextsOwnBox)
{
	const Result<Label> label = Label::Draw(Nearest(16), "gyro", White);

	GYRO_REQUIRE(label.has_value());
	GYRO_CHECK(label->Size() == PixelSize<BufferSpace>{ 32, 16 });
	GYRO_CHECK(label->Stride() == 128);
	GYRO_CHECK(label->Bytes().size() == 32U * 16U * 4U);
}

GYRO_TEST(Label, SelectsRatherThanBlends)
{
	// One bit of coverage means every texel is one of the two words handed in — which is what makes a
	// label exact, and why nothing here takes an AlphaMode.
	const Result<Label> label = Label::Draw(Nearest(16), "A", White, 0xFF204060);

	GYRO_REQUIRE(label.has_value());

	for (std::int32_t y = 0; y < 16; ++y)
	{
		for (std::int32_t x = 0; x < 8; ++x)
		{
			const std::uint32_t word = label->At(x, y);

			GYRO_CHECK(word == White || word == 0xFF204060);
		}
	}
}

GYRO_TEST(Label, DefaultsToGlyphsOverWhateverIsBehindThem)
{
	const Result<Label> label = Label::Draw(Nearest(16), " ", White);

	GYRO_REQUIRE(label.has_value());
	GYRO_CHECK(label->At(0, 0) == Clear);
}

GYRO_TEST(Label, OutsideIsTransparentBlack)
{
	const Result<Label> label = Label::Draw(Nearest(16), "A", White);

	GYRO_REQUIRE(label.has_value());
	GYRO_CHECK(label->At(-1, 0) == Clear);
	GYRO_CHECK(label->At(0, 16) == Clear);
}

GYRO_TEST(Label, AgreesWithTheCoverageItWasDrawnFrom)
{
	const Face& face = Nearest(24);
	const TextExtent extent = Measure(face, "Hi\nthere");
	std::vector<std::uint8_t> coverage(
		static_cast<std::size_t>(extent.Size.Width) * static_cast<std::size_t>(extent.Size.Height)
	);

	GYRO_REQUIRE(Rasterize(face, "Hi\nthere", coverage, extent.Size.Width).has_value());

	const Result<Label> label = Label::Draw(face, "Hi\nthere", White);

	GYRO_REQUIRE(label.has_value());
	GYRO_REQUIRE(label->Size() == extent.Size);

	for (std::int32_t y = 0; y < extent.Size.Height; ++y)
	{
		for (std::int32_t x = 0; x < extent.Size.Width; ++x)
		{
			const std::size_t index =
				static_cast<std::size_t>(y) * static_cast<std::size_t>(extent.Size.Width) + static_cast<std::size_t>(x);

			GYRO_CHECK(label->At(x, y) == (coverage[index] != 0 ? White : Clear));
		}
	}
}

GYRO_TEST(Label, RefusesAStringWithNoGlyphsInIt)
{
	// A texture with no texels is something an importer has to have an answer for, and "the string was
	// empty" is a question the caller can ask before it gets here.
	GYRO_CHECK(!Label::Draw(Nearest(16), "", White).has_value());
	GYRO_CHECK(!Label::Draw(Nearest(16), "\n\n", White).has_value());
}
