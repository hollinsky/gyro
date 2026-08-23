#include "Blit/Band.h"

#include <algorithm>
#include <cerrno>

namespace
{
// What the scratch is allowed to occupy. See `RowsFor`.
constexpr std::size_t Budget = 256U * 1024U;

// A ceiling on the band whatever the width, so that a small output does not get a scratch taller
// than the picture. Also the bound that keeps `Reserve` from being handed a width of one and
// allocating a very tall column.
constexpr std::int32_t MaxRows = 64;

// The widest target this will size a band for. Not a limit on what a presenter may allocate — it is
// the point at which a width has stopped being a width, and `Reserve` says so rather than
// multiplying it out.
constexpr std::int32_t MaxWidth = 1 << 16;
} // namespace

std::int32_t Band::RowsFor(std::int32_t width) noexcept
{
	if (width <= 0)
	{
		return 0;
	}

	const std::size_t row = static_cast<std::size_t>(width) * sizeof(Light);
	const std::size_t rows = std::max<std::size_t>(Budget / row, 1);

	return static_cast<std::int32_t>(std::min<std::size_t>(rows, static_cast<std::size_t>(MaxRows)));
}

Result<void> Band::Reserve(std::int32_t width)
{
	Release();

	if (width <= 0 || width > MaxWidth)
	{
		return Failure(EINVAL, "a band needs a width the output could actually have");
	}

	const std::int32_t rows = RowsFor(width);

	m_Pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(rows));
	m_Width = width;
	m_Rows = rows;

	return {};
}

void Band::Release() noexcept
{
	m_Pixels.clear();
	m_Pixels.shrink_to_fit();
	m_Width = 0;
	m_Rows = 0;
}

std::span<const Light> Band::Row(std::int32_t row) const noexcept
{
	if (row < 0 || row >= m_Rows)
	{
		return {};
	}

	return { m_Pixels.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(m_Width),
		     static_cast<std::size_t>(m_Width) };
}

bool Band::Clip(std::int32_t row, std::int32_t& left, std::int32_t& right) const noexcept
{
	if (row < 0 || row >= m_Rows)
	{
		return false;
	}

	left = std::max(left, 0);
	right = std::min(right, m_Width);

	return right > left;
}

void Band::Clear(std::int32_t rows, std::int32_t left, std::int32_t right) noexcept
{
	// Opaque black, which is the one value that makes `Over` exact forever: every composite above it
	// keeps an alpha of exactly full range, so nothing has to be unpremultiplied on the way out.
	constexpr Light Bottom{ 0, 0, 0, 65535 };

	for (std::int32_t row = 0; row < std::min(rows, m_Rows); ++row)
	{
		std::int32_t from = left;
		std::int32_t to = right;

		if (!Clip(row, from, to))
		{
			continue;
		}

		Light* const at = m_Pixels.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(m_Width);
		std::fill(at + from, at + to, Bottom);
	}
}

void Band::BlendRun(std::int32_t row, std::int32_t left, std::int32_t right, Light source) noexcept
{
	if (!Clip(row, left, right))
	{
		return;
	}

	// Nothing to add. Worth the branch rather than the loop: a fully transparent run is what every
	// item's own bounding box produces outside its coverage, and it is the common case at an edge.
	if (source == Light{})
	{
		return;
	}

	Light* const at = m_Pixels.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(m_Width);

	// The opaque case is a store rather than a blend, and it is most of a boot screen: a logo's
	// interior, a console's background, a solid panel. `Over` would compute the same answer through
	// four multiplies whose other operand is zero.
	if (source.Alpha == 65535)
	{
		std::fill(at + left, at + right, source);

		return;
	}

	for (std::int32_t column = left; column < right; ++column)
	{
		at[column] = Over(source, at[column]);
	}
}

void Band::BlendPixel(std::int32_t row, std::int32_t column, Light source) noexcept
{
	BlendRun(row, column, column + 1, source);
}

Light Band::At(std::int32_t row, std::int32_t column) const noexcept
{
	if (row < 0 || row >= m_Rows || column < 0 || column >= m_Width)
	{
		return {};
	}

	return m_Pixels
		[static_cast<std::size_t>(row) * static_cast<std::size_t>(m_Width) + static_cast<std::size_t>(column)];
}
