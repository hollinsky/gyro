#include "Bdf.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <optional>

namespace
{
struct Line
{
	std::string_view Text;
	std::size_t Number = 0;
};

// The keyword and the rest, which is all the structure BDF has: every line is a word, then
// whitespace-separated values or a quoted string nothing here reads.
std::string_view Keyword(std::string_view line)
{
	const std::size_t end = line.find(' ');

	return end == std::string_view::npos ? line : line.substr(0, end);
}

std::vector<std::int32_t> Numbers(std::string_view line)
{
	std::vector<std::int32_t> values;

	for (std::size_t index = 0; index < line.size();)
	{
		if (line[index] == ' ' || line[index] == '\t')
		{
			++index;

			continue;
		}

		std::int32_t value = 0;
		const char* const first = line.data() + index;
		const std::from_chars_result parsed = std::from_chars(first, line.data() + line.size(), value);

		if (parsed.ec != std::errc{})
		{
			// A word rather than a number: the keyword itself, or a property's quoted text. Skipped to
			// the next separator, since a caller only ever asks for the numbers it expects.
			while (index < line.size() && line[index] != ' ' && line[index] != '\t')
			{
				++index;
			}

			continue;
		}

		values.push_back(value);
		index = static_cast<std::size_t>(parsed.ptr - line.data());
	}

	return values;
}

std::optional<std::uint8_t> HexByte(std::string_view text)
{
	std::uint8_t value = 0;

	for (const char digit : text)
	{
		value = static_cast<std::uint8_t>(value << 4);

		if (digit >= '0' && digit <= '9')
		{
			value = static_cast<std::uint8_t>(value | (digit - '0'));
		}
		else if (digit >= 'a' && digit <= 'f')
		{
			value = static_cast<std::uint8_t>(value | (digit - 'a' + 10));
		}
		else if (digit >= 'A' && digit <= 'F')
		{
			value = static_cast<std::uint8_t>(value | (digit - 'A' + 10));
		}
		else
		{
			return std::nullopt;
		}
	}

	return value;
}

std::vector<Line> Split(std::string_view text)
{
	std::vector<Line> lines;
	std::size_t number = 0;

	for (std::size_t index = 0; index <= text.size();)
	{
		const std::size_t end = std::min(text.find('\n', index), text.size());
		std::string_view line = text.substr(index, end - index);

		// CRLF, because a font file is data somebody may well have moved through a Windows checkout.
		if (line.ends_with('\r'))
		{
			line.remove_suffix(1);
		}

		lines.push_back(Line{ .Text = line, .Number = ++number });
		index = end + 1;
	}

	return lines;
}
} // namespace

Result<BdfFont> ParseBdf(std::string_view text, std::string* diagnostic)
{
	const std::vector<Line> lines = Split(text);

	const auto fail = [diagnostic](const Line& line, std::string_view what) {
		if (diagnostic != nullptr)
		{
			*diagnostic = std::format("{}: {}", line.Number, what);
		}

		return Failure(EINVAL, "parsing a BDF font");
	};

	BdfFont font;
	std::int32_t boundingXOffset = 0;
	std::int32_t boundingYOffset = 0;
	bool haveBounds = false;
	bool haveAscent = false;

	for (std::size_t index = 0; index < lines.size(); ++index)
	{
		const Line& line = lines[index];
		const std::string_view keyword = Keyword(line.Text);

		if (keyword == "FONTBOUNDINGBOX")
		{
			const std::vector<std::int32_t> values = Numbers(line.Text);

			if (values.size() != 4 || values[0] <= 0 || values[1] <= 0)
			{
				return fail(line, "FONTBOUNDINGBOX wants a positive width and height and two offsets");
			}

			font.CellWidth = values[0];
			font.CellHeight = values[1];
			boundingXOffset = values[2];
			boundingYOffset = values[3];
			haveBounds = true;

			continue;
		}

		if (keyword == "FONT_ASCENT")
		{
			const std::vector<std::int32_t> values = Numbers(line.Text);

			if (values.size() != 1)
			{
				return fail(line, "FONT_ASCENT wants one number");
			}

			font.Ascent = values[0];
			haveAscent = true;

			continue;
		}

		if (keyword != "STARTCHAR")
		{
			continue;
		}

		// A glyph, and from here the lines are in a fixed order that the format guarantees and this
		// checks: an encoding, an advance, a bounding box, then BITMAP and exactly the rows.
		if (!haveBounds)
		{
			return fail(line, "a glyph before FONTBOUNDINGBOX");
		}

		BdfGlyph glyph;
		bool haveCode = false;
		bool skip = false;

		for (++index; index < lines.size(); ++index)
		{
			const Line& inner = lines[index];
			const std::string_view innerKeyword = Keyword(inner.Text);
			const std::vector<std::int32_t> values = Numbers(inner.Text);

			if (innerKeyword == "ENCODING")
			{
				if (values.size() != 1)
				{
					return fail(inner, "ENCODING wants one number");
				}

				// A BDF may carry a glyph with no code point at all, which nothing can ever ask for.
				// Dropped rather than refused: it is a legal font, and a bake that rejected one would
				// be a build that fails on a font upgrade for a glyph gyro would not have used.
				skip = values[0] < 0;
				glyph.Code = static_cast<char32_t>(std::max(values[0], 0));
				haveCode = true;

				continue;
			}

			if (innerKeyword == "DWIDTH")
			{
				if (values.size() != 2 || values[0] != font.CellWidth || values[1] != 0)
				{
					return fail(inner, "DWIDTH is not the cell width, so the font is not monospaced");
				}

				continue;
			}

			if (innerKeyword == "BBX")
			{
				if (values.size() != 4 || values[0] != font.CellWidth || values[1] != font.CellHeight ||
				    values[2] != boundingXOffset || values[3] != boundingYOffset)
				{
					return fail(inner, "BBX is not the font bounding box, so the glyphs are not cell-aligned");
				}

				continue;
			}

			if (innerKeyword != "BITMAP")
			{
				if (innerKeyword == "ENDCHAR")
				{
					return fail(inner, "a glyph with no BITMAP");
				}

				continue;
			}

			if (!haveCode)
			{
				return fail(inner, "a bitmap before its ENCODING");
			}

			const std::size_t digits = static_cast<std::size_t>(font.RowBytes()) * 2;

			for (std::int32_t row = 0; row < font.CellHeight; ++row)
			{
				if (++index >= lines.size())
				{
					return fail(inner, "a bitmap that ends before the cell does");
				}

				const std::string_view bits = lines[index].Text;

				if (bits.size() != digits)
				{
					return fail(lines[index], "a bitmap row that is not the cell's width in hex digits");
				}

				for (std::size_t byte = 0; byte < static_cast<std::size_t>(font.RowBytes()); ++byte)
				{
					const std::optional<std::uint8_t> value = HexByte(bits.substr(byte * 2, 2));

					if (!value.has_value())
					{
						return fail(lines[index], "a bitmap row that is not hexadecimal");
					}

					glyph.Rows.push_back(*value);
				}
			}

			break;
		}

		if (!skip && !glyph.Rows.empty())
		{
			font.Glyphs.push_back(std::move(glyph));
		}
	}

	if (!haveBounds)
	{
		return fail(lines.front(), "no FONTBOUNDINGBOX, so this is not a BDF font");
	}

	if (!haveAscent)
	{
		// Not a defect in the font — FONT_ASCENT is a property rather than a requirement — but a face
		// with no baseline is one the console cannot put a cursor on, so it is refused rather than
		// guessed at from the bounding box.
		return fail(lines.front(), "no FONT_ASCENT, so the baseline is unknown");
	}

	std::ranges::sort(font.Glyphs, {}, &BdfGlyph::Code);

	return font;
}
