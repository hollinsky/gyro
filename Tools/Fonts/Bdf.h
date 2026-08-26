#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.h"

// The BDF front end: a font file in, cells out, and a sentence naming the line when neither.
//
// Hand-rolled, for decision 2's reason applied to a second file format — no scripting-language
// dependency and no third-party library between a vendored font and the table gyro links. BDF is a
// line-oriented format with about a dozen keywords, and the subset accepted here is the part that
// describes a strictly monospaced, cell-aligned font.
//
// **Everything outside that subset stops the parse.** A glyph whose bounding box is not the font's
// own, or whose advance is not the cell width, is a proportional font arriving where a fixed grid is
// assumed — and the failure it would otherwise produce is a console whose columns drift by a texel per
// character, which is the kind of defect somebody reports as "the text looks wrong". Refusing it here
// costs one comparison and reports at the file and the line while somebody is looking at the build.

struct BdfGlyph
{
	char32_t Code = 0;

	// `RowBytes * Height` bytes, most significant bit leftmost — the packing BDF already uses, kept so
	// the bake transcribes rather than repacks.
	std::vector<std::uint8_t> Rows;
};

struct BdfFont
{
	std::int32_t CellWidth = 0;
	std::int32_t CellHeight = 0;

	// Rows from the top of the cell to the baseline, from `FONT_ASCENT`.
	std::int32_t Ascent = 0;

	// Ascending by code point, so the bake can cut runs out of it with one pass.
	std::vector<BdfGlyph> Glyphs;

	[[nodiscard]] std::int32_t RowBytes() const { return (CellWidth + 7) / 8; }
};

// The diagnostic is a second channel for Tools/Bindings/Xml.h's reason: Core/Result.h's Error carries
// a string_view over static storage, and the useful half of "BBX is not the font bounding box at line
// 4211" is the part that differs every time.
[[nodiscard]] Result<BdfFont> ParseBdf(std::string_view text, std::string* diagnostic);
