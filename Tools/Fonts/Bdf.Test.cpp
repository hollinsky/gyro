#include "Bdf.h"

#include <string>

#include "Testing/Test.h"

namespace
{
// One 8x2 font with two glyphs, written out rather than read from Vendor/Spleen, because a parser
// test asserting on a real font asserts on the font. The corpus test below is the one that reads the
// vendored files, and it asks a different question.
std::string OneFont(std::string_view glyphs)
{
	return std::string{ "STARTFONT 2.1\nFONTBOUNDINGBOX 8 2 0 -1\nSTARTPROPERTIES 2\nFONT_ASCENT 1\n"
		                "FONT_DESCENT 1\nENDPROPERTIES\nCHARS 1\n" } +
	       std::string{ glyphs } + "ENDFONT\n";
}

constexpr std::string_view Letter = "STARTCHAR A\nENCODING 65\nSWIDTH 500 0\nDWIDTH 8 0\nBBX 8 2 0 -1\n"
									"BITMAP\nFF\n81\nENDCHAR\n";
} // namespace

GYRO_TEST(Bdf, CarriesTheCellAndTheBaseline)
{
	std::string diagnostic;
	const Result<BdfFont> font = ParseBdf(OneFont(Letter), &diagnostic);

	GYRO_REQUIRE(font.has_value());
	GYRO_CHECK(font->CellWidth == 8);
	GYRO_CHECK(font->CellHeight == 2);
	GYRO_CHECK(font->Ascent == 1);
	GYRO_CHECK(font->RowBytes() == 1);
	GYRO_REQUIRE(font->Glyphs.size() == 1);
	GYRO_CHECK(font->Glyphs[0].Code == U'A');
	GYRO_CHECK(font->Glyphs[0].Rows == std::vector<std::uint8_t>{ 0xFF, 0x81 });
}

GYRO_TEST(Bdf, RefusesAProportionalFont)
{
	// The defect this exists for: a font whose advance is not its cell renders as columns that drift
	// by a texel per character, which is reported as "the text looks wrong" and found nowhere near
	// the font.
	std::string diagnostic;
	const Result<BdfFont> font =
		ParseBdf(OneFont("STARTCHAR A\nENCODING 65\nDWIDTH 6 0\nBBX 8 2 0 -1\nBITMAP\nFF\n81\nENDCHAR\n"), &diagnostic);

	GYRO_CHECK(!font.has_value());
	GYRO_CHECK(diagnostic.contains("monospaced"));
}

GYRO_TEST(Bdf, RefusesAGlyphOffTheCell)
{
	std::string diagnostic;
	const Result<BdfFont> font =
		ParseBdf(OneFont("STARTCHAR A\nENCODING 65\nDWIDTH 8 0\nBBX 8 1 0 -1\nBITMAP\nFF\nENDCHAR\n"), &diagnostic);

	GYRO_CHECK(!font.has_value());
	GYRO_CHECK(diagnostic.contains("cell-aligned"));
}

GYRO_TEST(Bdf, RefusesATruncatedBitmap)
{
	std::string diagnostic;
	const Result<BdfFont> font =
		ParseBdf(OneFont("STARTCHAR A\nENCODING 65\nDWIDTH 8 0\nBBX 8 2 0 -1\nBITMAP\nFF\n"), &diagnostic);

	GYRO_CHECK(!font.has_value());
}

GYRO_TEST(Bdf, DropsAnUnencodedGlyph)
{
	std::string diagnostic;
	const Result<BdfFont> font = ParseBdf(
		OneFont(
			std::string{ "STARTCHAR none\nENCODING -1\nDWIDTH 8 0\nBBX 8 2 0 -1\nBITMAP\n00\n00\nENDCHAR\n" } +
			std::string{ Letter }
		),
		&diagnostic
	);

	GYRO_REQUIRE(font.has_value());
	GYRO_CHECK(font->Glyphs.size() == 1);
}

GYRO_TEST(Bdf, RefusesAFileThatIsNotOne)
{
	std::string diagnostic;

	GYRO_CHECK(!ParseBdf("this is not a font\n", &diagnostic).has_value());
}
