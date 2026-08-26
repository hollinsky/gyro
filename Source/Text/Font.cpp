#include "Text/Font.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>

namespace
{
// The notdef box, and the reason it is index 0 in every face rather than a code point looked up: a
// face that lacked the replacement character would have nothing to answer with, and a face that had
// one would answer differently from its neighbour on the ladder.
constexpr std::uint32_t NotdefIndex = 0;
} // namespace

Glyph Face::At(std::uint32_t index) const noexcept
{
	const std::size_t stride = static_cast<std::size_t>(RowBytes()) * static_cast<std::size_t>(m_CellHeight);
	const std::size_t offset = static_cast<std::size_t>(index) * stride;

	// A face whose bitmaps do not hold the glyph its own runs point at is a bake that emitted a table
	// and an array that disagree, which is a corrupt font compiled into the binary rather than
	// anything a caller did. There is no sensible pixel to return, so it is caught here.
	if (offset + stride > m_Bitmaps.size())
	{
		std::abort();
	}

	return Glyph{ m_Bitmaps.subspan(offset, stride) };
}

bool Face::Has(char32_t code) const noexcept
{
	return std::ranges::any_of(m_Runs, [code](const CodeRun& run) { return code >= run.First && code <= run.Last; });
}

Glyph Face::Find(char32_t code) const noexcept
{
	// Linear over the runs, which is a handful per face — ninety-five printable Latin, the Latin-1
	// supplement, and however much of the box-drawing block the size carries. A binary search would be
	// the same number of cache lines and one more thing to get wrong.
	for (const CodeRun& run : m_Runs)
	{
		if (code >= run.First && code <= run.Last)
		{
			return At(run.Index + (code - run.First));
		}
	}

	return At(NotdefIndex);
}

const Face& Nearest(std::int32_t cellHeight) noexcept
{
	const std::span<const Face> ladder = Faces();

	if (ladder.empty())
	{
		std::abort();
	}

	const Face* best = &ladder.front();
	std::int32_t bestDistance = std::abs(best->CellSize().Height - cellHeight);

	for (const Face& face : ladder.subspan(1))
	{
		const std::int32_t distance = std::abs(face.CellSize().Height - cellHeight);

		// Strictly closer, so a tie between the size below and the size above keeps the one already
		// held — and since the ladder ascends, that is the smaller. A label that asked for twenty and
		// got twenty-four would be a label overlapping its neighbour.
		if (distance < bestDistance)
		{
			best = &face;
			bestDistance = distance;
		}
	}

	return *best;
}
