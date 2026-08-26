#include "Bake.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>

#include "Bdf.h"
#include "Testing/Test.h"

namespace
{
BdfFont TinyFont(std::string_view glyphs)
{
	const std::string text =
		std::string{ "STARTFONT 2.1\nFONTBOUNDINGBOX 8 2 0 -1\nFONT_ASCENT 1\n" } + std::string{ glyphs } + "ENDFONT\n";
	std::string diagnostic;

	return ParseBdf(text, &diagnostic).value_or(BdfFont{});
}

std::string Glyph(std::uint32_t code, std::string_view rows)
{
	return std::format("STARTCHAR g\nENCODING {}\nDWIDTH 8 0\nBBX 8 2 0 -1\nBITMAP\n{}ENDCHAR\n", code, rows);
}

std::string ReadFile(const std::filesystem::path& path)
{
	std::ifstream file{ path, std::ios::binary };

	return std::string{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
}
} // namespace

GYRO_TEST(Bake, CutsRunsAroundTheHoles)
{
	// 'A' and 'C' with 'B' missing is two runs, and the second one's index has to skip the glyph that
	// is not there. Getting this wrong shifts every character after a hole by one, which reads as a
	// font that renders the wrong letters rather than as a table bug.
	std::string diagnostic;
	const Result<std::string> baked = Bake(
		{ FaceSource{ .Name = "tiny", .Font = TinyFont(Glyph(65, "FF\n81\n") + Glyph(67, "81\nFF\n")) } }, &diagnostic
	);

	GYRO_REQUIRE(baked.has_value());
	GYRO_CHECK(baked->contains("{ 0x41, 0x41, 1 },"));
	GYRO_CHECK(baked->contains("{ 0x43, 0x43, 2 },"));
}

GYRO_TEST(Bake, PutsTheNotdefBoxFirst)
{
	// Index 0 belongs to the box the bake wrote, so the first real glyph is index 1 whatever it is.
	std::string diagnostic;
	const Result<std::string> baked =
		Bake({ FaceSource{ .Name = "tiny", .Font = TinyFont(Glyph(65, "FF\n81\n")) } }, &diagnostic);

	GYRO_REQUIRE(baked.has_value());
	GYRO_CHECK(baked->contains("{ 0x41, 0x41, 1 },"));
}

GYRO_TEST(Bake, RefusesAFontWithNoneOfTheCoverage)
{
	std::string diagnostic;

	GYRO_CHECK(
		!Bake({ FaceSource{ .Name = "tiny", .Font = TinyFont(Glyph(0x2800, "FF\n81\n")) } }, &diagnostic).has_value()
	);
}

GYRO_TEST(Bake, RefusesAnEmptyLadder)
{
	std::string diagnostic;

	GYRO_CHECK(!Bake({}, &diagnostic).has_value());
}

GYRO_TEST(Bake, SortsTheLadderAscending)
{
	// Nearest breaks its tie towards the smaller face, which is only the smaller face if the ladder
	// ascends — and the CMake list is not obliged to be in order.
	std::string diagnostic;
	BdfFont taller = TinyFont(Glyph(65, "FF\n81\n"));
	taller.CellHeight = 4;
	taller.Glyphs[0].Rows.resize(4);

	const Result<std::string> baked = Bake(
		{ FaceSource{ .Name = "second", .Font = taller },
	      FaceSource{ .Name = "first", .Font = TinyFont(Glyph(65, "FF\n81\n")) } },
		&diagnostic
	);

	GYRO_REQUIRE(baked.has_value());
	GYRO_CHECK(baked->find("\"second\"") > baked->find("\"first\""));
}

// The vendored fonts as this checkout has them, rather than a fixture beside the test. A font upgrade
// that changed the format, or a file that arrived truncated through a checkout, should fail here on
// the day it lands rather than as a console that draws boxes.
GYRO_TEST(Bake, BakesEveryVendoredSpleen)
{
	const std::filesystem::path directory{ GYRO_FONT_DIR };
	std::vector<FaceSource> faces;

	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator{ directory })
	{
		if (entry.path().extension() != ".bdf")
		{
			continue;
		}

		std::string diagnostic;
		Result<BdfFont> font = ParseBdf(ReadFile(entry.path()), &diagnostic);

		GYRO_REQUIRE(font.has_value());

		faces.push_back(FaceSource{ .Name = entry.path().stem().string(), .Font = std::move(*font) });
	}

	GYRO_REQUIRE(faces.size() == 6);

	std::string diagnostic;
	const Result<std::string> baked = Bake(std::move(faces), &diagnostic);

	GYRO_REQUIRE(baked.has_value());
	GYRO_CHECK(baked->contains("\"spleen-5x8\""));
	GYRO_CHECK(baked->contains("\"spleen-32x64\""));
}
