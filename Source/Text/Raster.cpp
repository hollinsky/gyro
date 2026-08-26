#include "Text/Raster.h"

#include <algorithm>
#include <cstddef>

#include "Text/Utf8.h"

namespace
{
constexpr std::uint8_t Inside = 255;
constexpr std::uint8_t Outside = 0;

// The one walk both Measure and Rasterize are made of: it hands out a cell column and row per code
// point and swallows the characters that move the cursor instead of drawing. Written once because two
// copies of it is how a label ends up measured one cell wider than it is drawn, which reads as a
// margin nobody chose.
template<typename Visitor>
void WalkCells(std::string_view text, Visitor&& visit)
{
	std::int32_t column = 0;
	std::int32_t row = 0;

	for (std::size_t index = 0; index < text.size();)
	{
		const Utf8Code decoded = DecodeUtf8(text.substr(index));
		index += decoded.Length;

		if (decoded.Code == U'\n')
		{
			column = 0;
			++row;

			continue;
		}

		// Swallowed rather than drawn. A string that arrived over a wire or out of a file carries
		// them, and a carriage return rendered as a notdef box is a line of log output with a mark on
		// the end of it that means nothing to the reader.
		if (decoded.Code == U'\r')
		{
			continue;
		}

		if (decoded.Code == U'\t')
		{
			column = ((column / TabColumns) + 1) * TabColumns;

			continue;
		}

		visit(decoded.Code, column, row);
		++column;
	}
}
} // namespace

TextExtent Measure(const Face& face, std::string_view text) noexcept
{
	std::int32_t columns = 0;
	std::int32_t rows = 0;

	WalkCells(text, [&columns, &rows](char32_t, std::int32_t column, std::int32_t row) {
		columns = std::max(columns, column + 1);
		rows = std::max(rows, row + 1);
	});

	// A string of nothing but newlines still occupies the rows it names, and an empty string is one
	// empty line rather than nothing: a caller stacking labels wants the cursor to have moved.
	rows = std::max(rows, 1 + static_cast<std::int32_t>(std::ranges::count(text, '\n')));

	const PixelSize<BufferSpace> cell = face.CellSize();

	return TextExtent{ .Size = { columns * cell.Width, rows * cell.Height }, .Lines = rows };
}

Result<void>
Rasterize(const Face& face, std::string_view text, std::span<std::uint8_t> coverage, std::int32_t stride) noexcept
{
	const TextExtent extent = Measure(face, text);

	if (stride < extent.Size.Width)
	{
		return Failure(EINVAL, "rasterizing text into a row shorter than the text");
	}

	const std::size_t needed = extent.Size.Height == 0 ?
	                               0 :
	                               static_cast<std::size_t>(stride) * static_cast<std::size_t>(extent.Size.Height - 1) +
	                                   static_cast<std::size_t>(extent.Size.Width);

	if (coverage.size() < needed)
	{
		return Failure(EINVAL, "rasterizing text into a buffer smaller than its extent");
	}

	// Cleared first, and over the extent rather than over the buffer. Every cell the walk skips — the
	// short end of a line, the run a tab jumped — has to be defined, and defining it as the walk goes
	// would mean the walk knowing what it did not visit.
	for (std::int32_t y = 0; y < extent.Size.Height; ++y)
	{
		const std::size_t start = static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);

		std::ranges::fill(coverage.subspan(start, static_cast<std::size_t>(extent.Size.Width)), Outside);
	}

	const PixelSize<BufferSpace> cell = face.CellSize();
	const std::int32_t rowBytes = face.RowBytes();

	WalkCells(text, [&](char32_t code, std::int32_t column, std::int32_t row) {
		const Glyph glyph = face.Find(code);
		const std::int32_t left = column * cell.Width;
		const std::int32_t top = row * cell.Height;

		for (std::int32_t y = 0; y < cell.Height; ++y)
		{
			const std::span<const std::uint8_t> bits =
				glyph.Rows.subspan(static_cast<std::size_t>(y) * static_cast<std::size_t>(rowBytes));
			const std::size_t start =
				static_cast<std::size_t>(top + y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(left);

			for (std::int32_t x = 0; x < cell.Width; ++x)
			{
				// Most significant bit leftmost, which is BDF's own packing carried through the bake
				// untouched — see Text/Font.h.
				const std::uint8_t bit = static_cast<std::uint8_t>(0x80U >> (x % 8));
				const bool set = (bits[static_cast<std::size_t>(x / 8)] & bit) != 0;

				coverage[start + static_cast<std::size_t>(x)] = set ? Inside : Outside;
			}
		}
	});

	return {};
}
