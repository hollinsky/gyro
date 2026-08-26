#include "Text/Font.h"

#include <algorithm>
#include <cstddef>
#include <span>

#include "Testing/Test.h"

GYRO_TEST(Font, HasTheWholeLadderAscending)
{
	const std::span<const Face> ladder = Faces();

	GYRO_REQUIRE(ladder.size() == 6);

	for (std::size_t index = 1; index < ladder.size(); ++index)
	{
		GYRO_CHECK(ladder[index - 1].CellSize().Height < ladder[index].CellSize().Height);
	}

	GYRO_CHECK(ladder.front().Name() == "spleen-5x8");
	GYRO_CHECK(ladder.back().Name() == "spleen-32x64");
}

GYRO_TEST(Font, TheBaselineSitsInsideTheCell)
{
	for (const Face& face : Faces())
	{
		GYRO_CHECK(face.Ascent() > 0);
		GYRO_CHECK(face.Ascent() < face.CellSize().Height);
		GYRO_CHECK(face.RowBytes() * 8 >= face.CellSize().Width);
	}
}

GYRO_TEST(Font, NearestClampsAndBreaksTiesDownwards)
{
	// The ends clamp, because a caller asking for four texels or four hundred is asking for the
	// smallest and the largest thing gyro can draw rather than for a failure.
	GYRO_CHECK(Nearest(1).Name() == "spleen-5x8");
	GYRO_CHECK(Nearest(4000).Name() == "spleen-32x64");
	GYRO_CHECK(Nearest(16).Name() == "spleen-8x16");

	// Ten is two from 6x12 and two from 5x8; the smaller wins, because a label that asked for ten and
	// got twelve is a label overlapping its neighbour.
	GYRO_CHECK(Nearest(10).Name() == "spleen-5x8");
	GYRO_CHECK(Nearest(11).Name() == "spleen-6x12");
}

GYRO_TEST(Font, EveryFaceCarriesPrintableLatin)
{
	for (const Face& face : Faces())
	{
		for (char32_t code = U' '; code <= U'~'; ++code)
		{
			GYRO_CHECK(face.Has(code));
		}
	}
}

GYRO_TEST(Font, AMissingCodePointIsTheNotdefBox)
{
	const Face& face = Nearest(16);

	// Braille is outside Tools/Fonts/Bake.h's declared coverage, so no face has it however many the
	// BDF carried.
	GYRO_CHECK(!face.Has(0x2800));

	const Glyph notdef = face.Find(0x2800);
	const Glyph alsoNotdef = face.Find(0x2801);

	GYRO_CHECK(!notdef.Rows.empty());
	GYRO_CHECK(std::ranges::equal(notdef.Rows, alsoNotdef.Rows));

	// It is a mark rather than a blank, which is the whole point of it: a missing character has to be
	// visible to whoever is reading the screen.
	GYRO_CHECK(std::ranges::any_of(notdef.Rows, [](std::uint8_t row) { return row != 0; }));

	// And it is not what a letter looks like.
	GYRO_CHECK(!std::ranges::equal(notdef.Rows, face.Find(U'A').Rows));
}

GYRO_TEST(Font, TheSmallSizesCarryOnlyPartOfTheBoxDrawing)
{
	// 5x8 has thirteen box-drawing characters and 8x16 has the block. The bake cuts runs around the
	// holes rather than padding them, and this is the assertion that says the holes are real.
	GYRO_CHECK(Nearest(8).Has(0x2500));
	GYRO_CHECK(!Nearest(8).Has(0x2501));
	GYRO_CHECK(Nearest(16).Has(0x2501));
}
