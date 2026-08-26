#include "Text/Label.h"

#include "Text/Raster.h"

Result<Label> Label::Draw(const Face& face, std::string_view text, std::uint32_t foreground, std::uint32_t background)
{
	const TextExtent extent = Measure(face, text);

	if (extent.Size.IsEmpty())
	{
		return Failure(EINVAL, "drawing a label for a string with no glyphs in it");
	}

	const std::size_t texels =
		static_cast<std::size_t>(extent.Size.Width) * static_cast<std::size_t>(extent.Size.Height);

	std::vector<std::uint8_t> coverage(texels);

	if (const Result<void> rasterized = Rasterize(face, text, coverage, extent.Size.Width); !rasterized)
	{
		return std::unexpected{ rasterized.error() };
	}

	std::vector<std::uint32_t> words(texels);

	// A select rather than a blend, which is the whole of what one bit of coverage permits and is what
	// makes a label exact: a test asserts the word at a texel and not a distance from one.
	for (std::size_t index = 0; index < texels; ++index)
	{
		words[index] = coverage[index] != 0 ? foreground : background;
	}

	return Label{ extent.Size, std::move(words) };
}

std::uint32_t Label::At(std::int32_t x, std::int32_t y) const noexcept
{
	if (x < 0 || y < 0 || x >= m_Size.Width || y >= m_Size.Height)
	{
		return 0;
	}

	return m_Words[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_Size.Width) + static_cast<std::size_t>(x)];
}
