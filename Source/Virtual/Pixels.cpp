#include "Virtual/Pixels.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace
{
// The one place a format's memory layout is written down.
//
// **Little-endian words rather than byte offsets, because that is what DRM's names mean.** `XR24` is
// `DRM_FORMAT_XRGB8888`, and the `XRGB` is the order of the *bits in a 32-bit little-endian word* —
// so the bytes in memory run `B, G, R, X`. Spelling it as a word and shifting is the reading that
// matches the kernel's own definition; spelling it as `bytes[2], bytes[1], bytes[0]` is the same
// answer arrived at by a coincidence that stops holding for the ten-bit formats, where no channel is
// byte-aligned at all.
[[nodiscard]] std::uint32_t LoadWord(const std::byte* at) noexcept
{
	std::uint32_t word = 0;
	std::memcpy(&word, at, sizeof word);

	return word;
}

void StoreWord(std::byte* at, std::uint32_t word) noexcept
{
	std::memcpy(at, &word, sizeof word);
}

[[nodiscard]] Rgba16 Decode(std::uint32_t word, std::uint32_t code) noexcept
{
	switch (code)
	{
		case FormatXrgb8888:
		case FormatArgb8888:
			return { FromEightBit(static_cast<std::uint8_t>((word >> 16) & 0xFFU)),
				     FromEightBit(static_cast<std::uint8_t>((word >> 8) & 0xFFU)),
				     FromEightBit(static_cast<std::uint8_t>(word & 0xFFU)),
				     HasAlpha(code) ? FromEightBit(static_cast<std::uint8_t>((word >> 24) & 0xFFU)) :
				                      std::uint16_t{ 0xFFFF } };

		case FormatXrgb2101010:
		case FormatArgb2101010:
			return { FromTenBit(static_cast<std::uint16_t>((word >> 20) & 0x3FFU)),
				     FromTenBit(static_cast<std::uint16_t>((word >> 10) & 0x3FFU)),
				     FromTenBit(static_cast<std::uint16_t>(word & 0x3FFU)),
				     HasAlpha(code) ? FromTwoBit(static_cast<std::uint8_t>((word >> 30) & 0x3U)) :
				                      std::uint16_t{ 0xFFFF } };

		default:
			return {};
	}
}

// The inverse. An `X` channel is written as all ones rather than as zero: the bits are ignored by
// definition, and a scanout path that a driver decided to read anyway should find opaque rather than
// transparent.
[[nodiscard]] std::uint32_t Encode(Rgba16 pixel, std::uint32_t code) noexcept
{
	switch (code)
	{
		case FormatXrgb8888:
		case FormatArgb8888:
		{
			const std::uint32_t alpha = HasAlpha(code) ? ToEightBit(pixel.Alpha) : 0xFFU;

			return (alpha << 24) | (static_cast<std::uint32_t>(ToEightBit(pixel.Red)) << 16) |
			       (static_cast<std::uint32_t>(ToEightBit(pixel.Green)) << 8) | ToEightBit(pixel.Blue);
		}

		case FormatXrgb2101010:
		case FormatArgb2101010:
		{
			const std::uint32_t alpha = HasAlpha(code) ? ToTwoBit(pixel.Alpha) : 0x3U;

			return (alpha << 30) | (static_cast<std::uint32_t>(ToTenBit(pixel.Red)) << 20) |
			       (static_cast<std::uint32_t>(ToTenBit(pixel.Green)) << 10) | ToTenBit(pixel.Blue);
		}

		default:
			return 0;
	}
}

// What `Over` checks, for both views, so that the mutable one cannot drift from the read-only one.
//
// The last row is the interesting bound: a buffer holds `stride * (height - 1) + width * bytes` and
// not `stride * height`, because the trailing padding of the final row need not be mapped. Checking
// the larger figure would refuse a legitimate tightly-allocated image.
[[nodiscard]] Result<void>
CheckShape(std::size_t bytes, PixelSize<DeviceSpace> size, std::uint32_t stride, PixelFormat format) noexcept
{
	const std::uint32_t bytesPerPixel = DecodableBytesPerPixel(format.Code);

	if (bytesPerPixel == 0)
	{
		return Failure(EINVAL, "no decode for this pixel format");
	}

	if (!size.IsValid() || size.IsEmpty())
	{
		return Failure(EINVAL, "an image view needs a non-empty extent");
	}

	const std::size_t row = static_cast<std::size_t>(size.Width) * bytesPerPixel;

	if (stride < row)
	{
		return Failure(EINVAL, "stride is shorter than a row of pixels");
	}

	const std::size_t needed = static_cast<std::size_t>(stride) * static_cast<std::size_t>(size.Height - 1) + row;

	if (bytes < needed)
	{
		return Failure(EINVAL, "the mapping is shorter than the image it describes");
	}

	return {};
}
} // namespace

Result<ImageView>
ImageView::Over(std::span<const std::byte> bytes, PixelSize<DeviceSpace> size, std::uint32_t stride, PixelFormat format)
{
	if (const Result<void> shape = CheckShape(bytes.size(), size, stride, format); !shape)
	{
		return std::unexpected{ shape.error() };
	}

	ImageView view;
	view.m_Bytes = bytes;
	view.m_Size = size;
	view.m_Stride = stride;
	view.m_Format = format;

	return view;
}

bool ImageView::Contains(PixelRect<DeviceSpace> rect) const noexcept
{
	return !rect.Extent.IsEmpty() && rect.Left() >= 0 && rect.Top() >= 0 && rect.Right() <= m_Size.Width &&
	       rect.Bottom() <= m_Size.Height;
}

Rgba16 ImageView::At(std::int32_t x, std::int32_t y) const noexcept
{
	if (!Contains(x, y))
	{
		return {};
	}

	const std::size_t offset =
		static_cast<std::size_t>(y) * m_Stride + static_cast<std::size_t>(x) * DecodableBytesPerPixel(m_Format.Code);

	return Decode(LoadWord(m_Bytes.data() + offset), m_Format.Code);
}

bool ImageView::IsUniform(PixelRect<DeviceSpace> rect, Rgba16 colour, std::uint16_t tolerance) const noexcept
{
	if (!Contains(rect))
	{
		return false;
	}

	for (std::int32_t y = rect.Top(); y < rect.Bottom(); ++y)
	{
		for (std::int32_t x = rect.Left(); x < rect.Right(); ++x)
		{
			if (!At(x, y).Within(colour, tolerance))
			{
				return false;
			}
		}
	}

	return true;
}

std::size_t ImageView::CountMatching(PixelRect<DeviceSpace> rect, Rgba16 colour, std::uint16_t tolerance) const noexcept
{
	if (!Contains(rect))
	{
		return 0;
	}

	std::size_t matched = 0;

	for (std::int32_t y = rect.Top(); y < rect.Bottom(); ++y)
	{
		for (std::int32_t x = rect.Left(); x < rect.Right(); ++x)
		{
			matched += At(x, y).Within(colour, tolerance) ? 1U : 0U;
		}
	}

	return matched;
}

std::optional<PixelRect<DeviceSpace>>
ImageView::BoundsOfDiffering(Rgba16 background, std::uint16_t tolerance) const noexcept
{
	std::int32_t left = m_Size.Width;
	std::int32_t top = m_Size.Height;
	std::int32_t right = 0;
	std::int32_t bottom = 0;

	for (std::int32_t y = 0; y < m_Size.Height; ++y)
	{
		for (std::int32_t x = 0; x < m_Size.Width; ++x)
		{
			if (At(x, y).Within(background, tolerance))
			{
				continue;
			}

			left = std::min(left, x);
			top = std::min(top, y);
			right = std::max(right, x + 1);
			bottom = std::max(bottom, y + 1);
		}
	}

	if (right <= left || bottom <= top)
	{
		return std::nullopt;
	}

	return PixelRect<DeviceSpace>::FromEdges({ left, top }, { right, bottom });
}

Result<MutableImageView> MutableImageView::Over(
	std::span<std::byte> bytes,
	PixelSize<DeviceSpace> size,
	std::uint32_t stride,
	PixelFormat format
)
{
	Result<ImageView> read = ImageView::Over({ bytes.data(), bytes.size() }, size, stride, format);

	if (!read)
	{
		return std::unexpected{ read.error() };
	}

	MutableImageView view;
	view.m_Bytes = bytes;
	view.m_Read = *read;

	return view;
}

void MutableImageView::Set(std::int32_t x, std::int32_t y, Rgba16 colour) const noexcept
{
	if (!m_Read.Contains(x, y))
	{
		return;
	}

	const std::size_t offset = static_cast<std::size_t>(y) * m_Read.Stride() +
	                           static_cast<std::size_t>(x) * DecodableBytesPerPixel(m_Read.Format().Code);

	StoreWord(m_Bytes.data() + offset, Encode(colour, m_Read.Format().Code));
}

std::size_t MutableImageView::Fill(PixelRect<DeviceSpace> rect, Rgba16 colour) const noexcept
{
	const PixelSize<DeviceSpace> size = m_Read.Size();

	const std::int32_t left = std::max(rect.Left(), 0);
	const std::int32_t top = std::max(rect.Top(), 0);
	const std::int32_t right = std::min(rect.Right(), size.Width);
	const std::int32_t bottom = std::min(rect.Bottom(), size.Height);

	if (right <= left || bottom <= top)
	{
		return 0;
	}

	// Encoded once and stored per pixel, which is the shape a `Blit` fill wants too: the arithmetic
	// is per colour and the loop is per pixel, and doing it the other way round is how a solid fill
	// becomes the most expensive thing in a composite.
	const std::uint32_t word = Encode(colour, m_Read.Format().Code);
	const std::uint32_t bytesPerPixel = DecodableBytesPerPixel(m_Read.Format().Code);

	for (std::int32_t y = top; y < bottom; ++y)
	{
		std::byte* row = m_Bytes.data() + static_cast<std::size_t>(y) * m_Read.Stride();

		for (std::int32_t x = left; x < right; ++x)
		{
			StoreWord(row + static_cast<std::size_t>(x) * bytesPerPixel, word);
		}
	}

	return static_cast<std::size_t>(right - left) * static_cast<std::size_t>(bottom - top);
}

BufferReader::BufferReader(const DmabufBuffer& buffer) : m_Read{ buffer }
{
	if (!buffer.IsMapped())
	{
		m_Status = Failure(ENODEV, "the buffer carries no CPU mapping");

		return;
	}

	Result<ImageView> view = ImageView::Over(m_Read.Bytes(), buffer.Size(), buffer.Stride(), buffer.Format());

	if (!view)
	{
		m_Status = std::unexpected{ view.error() };

		return;
	}

	m_View = *view;
}
