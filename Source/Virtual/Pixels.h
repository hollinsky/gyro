#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>

#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Seam/RenderTarget.h"
#include "Virtual/Buffer.h"

// What a consumer sees when it looks at a frame, and the vocabulary for saying what it should have
// been.
//
// **This is the alternative to golden images, and the argument is that a golden image asserts the
// wrong thing.** A checked-in reference frame states every pixel, so it fails on the ones nobody
// meant to promise — a driver that rounds a filtered edge one least-significant bit differently, a
// composite that arrives in a different order and lands on the same picture. What a test actually
// wants to say is *the panel is opaque red where the node is, the background is untouched
// everywhere else, and the boundary is exactly here*, which is a handful of predicates over the
// bytes and is true on every driver that is not broken. The image on disk stays useful for the case
// it is good at, which is a human looking at a failure: Virtual/Pam.h writes one, and nothing
// compares against it.
//
// **It lives in `Virtual` because reading a composited frame is what a virtual output's consumer
// does.** Docs/Structure.md gives this module the output whose consumer is a file, an encoder, or a
// test, and all three of those decode pixels. Putting it in `Core` beside Texture.h would widen the
// portable tier for a type only this module's consumers and its tests construct, which is the same
// test Virtual/Allocator.h applies to itself one file over.
//
// **Every value is sixteen bits per channel and that is not future-proofing.** Seam/RenderTarget.h
// already names `XR30` and `AR30`, and decision 47 puts HDR content beside SDR content in one
// composite; a predicate library that quietly decoded ten-bit content into eight would report a
// banding bug as a pass. Sixteen bits holds both source depths exactly — bit replication, not a
// divide — so an eight-bit target compared against an eight-bit expectation is still an exact
// comparison, and a tolerance is something a test asks for rather than something the decode imposes.

// One decoded pixel, in the common depth. Channels are named rather than indexed because the whole
// class of bug this file exists to catch is a channel order — `XR24` is `B, G, R, X` in memory, and
// every test that ever spelled that inline eventually spelled it backwards.
struct Rgba16
{
	std::uint16_t Red = 0;
	std::uint16_t Green = 0;
	std::uint16_t Blue = 0;
	std::uint16_t Alpha = 0;

	// Whether every channel is within `tolerance` of another pixel's. Zero is exact, which is the
	// right default for a solid fill and the wrong one for anything that was filtered.
	[[nodiscard]] constexpr bool Within(Rgba16 other, std::uint16_t tolerance) const noexcept
	{
		return Near(Red, other.Red, tolerance) && Near(Green, other.Green, tolerance) &&
		       Near(Blue, other.Blue, tolerance) && Near(Alpha, other.Alpha, tolerance);
	}

	friend constexpr bool operator==(Rgba16, Rgba16) noexcept = default;

private:
	[[nodiscard]] static constexpr bool Near(std::uint16_t left, std::uint16_t right, std::uint16_t tolerance) noexcept
	{
		return (left > right ? left - right : right - left) <= tolerance;
	}
};

// Widening by bit replication rather than by a multiply, because replication is exactly right at
// both ends of the range and a scale is only right at one: `v * 65535 / 1023` needs a divide to land
// 1023 on 65535, where `(v << 6) | (v >> 4)` lands it there by construction and lands 0 on 0.
[[nodiscard]] constexpr std::uint16_t FromEightBit(std::uint8_t value) noexcept
{
	return static_cast<std::uint16_t>((value << 8) | value);
}

[[nodiscard]] constexpr std::uint16_t FromTenBit(std::uint16_t value) noexcept
{
	const std::uint16_t clamped = value & 0x03FFU;

	return static_cast<std::uint16_t>((clamped << 6) | (clamped >> 4));
}

// Two bits of alpha, which is all `AR30` carries. Replicated eight times over.
[[nodiscard]] constexpr std::uint16_t FromTwoBit(std::uint8_t value) noexcept
{
	return static_cast<std::uint16_t>((value & 0x3U) * 0x5555U);
}

// The inverse, rounded to nearest. Exact on anything that came from the widening above, which is the
// property `Pixels.Test.cpp` asserts across the whole eight-bit and ten-bit ranges — a writer and a
// reader that disagree by one would make every `MutableImageView::Fill` a test of the rounding.
[[nodiscard]] constexpr std::uint8_t ToEightBit(std::uint16_t value) noexcept
{
	return static_cast<std::uint8_t>((static_cast<std::uint32_t>(value) * 255U + 32767U) / 65535U);
}

[[nodiscard]] constexpr std::uint16_t ToTenBit(std::uint16_t value) noexcept
{
	return static_cast<std::uint16_t>((static_cast<std::uint32_t>(value) * 1023U + 32767U) / 65535U);
}

[[nodiscard]] constexpr std::uint8_t ToTwoBit(std::uint16_t value) noexcept
{
	return static_cast<std::uint8_t>((static_cast<std::uint32_t>(value) * 3U + 32767U) / 65535U);
}

// What a test means when it writes a colour. Spelled in the depth the author is thinking in, widened
// once, here — so that `Rgb8(255, 0, 0)` compares equal to what an `XR24` target holds and to what an
// `XR30` target holds, and a test that changes format changes nothing else.
[[nodiscard]] constexpr Rgba16 Rgba8(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha)
{
	return { FromEightBit(red), FromEightBit(green), FromEightBit(blue), FromEightBit(alpha) };
}

[[nodiscard]] constexpr Rgba16 Rgb8(std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
	return Rgba8(red, green, blue, 255);
}

// What the renderer composites an empty scene to, named once. Every test that checks a cleared
// rectangle wants this value and none of them should be spelling it.
inline constexpr Rgba16 OpaqueBlack = Rgba8(0, 0, 0, 255);

// How many bytes one pixel of a format occupies, and zero for a format this file cannot decode.
//
// `NV12` is the interesting zero. Seam/RenderTarget.h carries it because a client may commit one and
// an encoder may want one, and Docs/Open.md's *colour format for virtual outputs* is the entry that
// will eventually put one on an output. It is planar and chroma-subsampled, so a single view over a
// single stride cannot describe it — refusing by name is what keeps a future encoder target from
// being decoded as garbage RGB rather than reported as unsupported.
[[nodiscard]] constexpr std::uint32_t DecodableBytesPerPixel(std::uint32_t code) noexcept
{
	switch (code)
	{
		case FormatXrgb8888:
		case FormatArgb8888:
		case FormatXrgb2101010:
		case FormatArgb2101010:
			return 4;
		default:
			return 0;
	}
}

// Whether a format's alpha channel carries information. `XR24` and `XR30` have the bits and the bits
// mean nothing, so a decode reports opaque rather than whatever the renderer happened to leave — a
// test asserting `Alpha == 65535` over an `XR24` target is asserting the format, not the composite.
[[nodiscard]] constexpr bool HasAlpha(std::uint32_t code) noexcept
{
	return code == FormatArgb8888 || code == FormatArgb2101010;
}

// The source depth, which Virtual/Pam.h needs in order to write a file that is neither lossy nor
// gratuitously wide.
[[nodiscard]] constexpr std::uint32_t BitsPerChannel(std::uint32_t code) noexcept
{
	switch (code)
	{
		case FormatXrgb8888:
		case FormatArgb8888:
			return 8;
		case FormatXrgb2101010:
		case FormatArgb2101010:
			return 10;
		default:
			return 0;
	}
}

// A rectangle of pixels somebody else owns, and the questions worth asking about it.
//
// **Construction is fallible and that is the whole safety story.** A view is a pointer, a stride, and
// an extent, which is three chances to describe memory that is not there; `Over` checks that the
// stride spans a row, that the span spans every row, and that the format is one this file can
// decode. After that every accessor is total, which is what lets `At` be an expression rather than a
// statement with an error check in front of it.
class ImageView
{
public:
	ImageView() = default;

	[[nodiscard]] static Result<ImageView>
	Over(std::span<const std::byte> bytes, PixelSize<DeviceSpace> size, std::uint32_t stride, PixelFormat format);

	[[nodiscard]] bool IsValid() const noexcept { return !m_Bytes.empty(); }

	[[nodiscard]] PixelSize<DeviceSpace> Size() const noexcept { return m_Size; }

	[[nodiscard]] std::uint32_t Stride() const noexcept { return m_Stride; }

	[[nodiscard]] PixelFormat Format() const noexcept { return m_Format; }

	[[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return m_Bytes; }

	// The whole image as a rectangle, which is what most predicates are asked about.
	[[nodiscard]] PixelRect<DeviceSpace> Extent() const noexcept { return { {}, m_Size }; }

	[[nodiscard]] bool Contains(std::int32_t x, std::int32_t y) const noexcept
	{
		return x >= 0 && y >= 0 && x < m_Size.Width && y < m_Size.Height;
	}

	[[nodiscard]] bool Contains(PixelRect<DeviceSpace> rect) const noexcept;

	// One pixel, decoded.
	//
	// **Total, and outside the image it answers transparent black.** The alternative — a precondition,
	// or an optional every call site unwraps — buys nothing here, because a coordinate outside the
	// image is a bug in the test rather than a condition to handle, and the predicates below are
	// where that bug is made loud: each of them *refuses* a rectangle the view does not contain
	// rather than clipping it, so a misindexed query reports false rather than a smaller true.
	// Transparent black is also a value no `XR24` or `XR30` target can hold, so the common case is
	// unambiguous on inspection.
	[[nodiscard]] Rgba16 At(std::int32_t x, std::int32_t y) const noexcept;

	// Whether every pixel in `rect` is `colour`, within `tolerance`. False where the view does not
	// contain the rectangle, and false for an empty one — an empty rectangle is vacuously uniform and
	// a test that computed one by accident wants to hear about it.
	[[nodiscard]] bool
	IsUniform(PixelRect<DeviceSpace> rect, Rgba16 colour, std::uint16_t tolerance = 0) const noexcept;

	// How many pixels in `rect` match. Zero where the view does not contain the rectangle.
	[[nodiscard]] std::size_t
	CountMatching(PixelRect<DeviceSpace> rect, Rgba16 colour, std::uint16_t tolerance = 0) const noexcept;

	// The tightest rectangle enclosing every pixel that is *not* `background`, or nothing where every
	// pixel is.
	//
	// This is the predicate that replaces a golden image for a placement test: a solid drawn at a
	// projected quad has a bound, the bound is arithmetic the test can state, and the answer does not
	// depend on how the driver filtered the inside of it.
	[[nodiscard]] std::optional<PixelRect<DeviceSpace>>
	BoundsOfDiffering(Rgba16 background, std::uint16_t tolerance = 0) const noexcept;

private:
	std::span<const std::byte> m_Bytes;
	PixelSize<DeviceSpace> m_Size{};
	std::uint32_t m_Stride = 0;
	PixelFormat m_Format{};
};

// The same view, writable.
//
// **Two callers, and the second is why it is here rather than in a test.** A test prefills a target
// so that *was not drawn into* is a claim the bytes can answer, which is what
// Integration/RenderImport.Test.cpp does today by filling raw bytes with a repeated value — a
// technique that works only for colours whose four bytes happen to be equal. The other caller is the
// CPU renderer decision 79 puts in front of the boot console: a `Blit` compositing into a
// `MappedImage` is encoding pixels into exactly these formats through exactly this arithmetic, and
// having one encoder rather than two is what stops a blitter and a test disagreeing about what red
// is.
class MutableImageView
{
public:
	MutableImageView() = default;

	[[nodiscard]] static Result<MutableImageView>
	Over(std::span<std::byte> bytes, PixelSize<DeviceSpace> size, std::uint32_t stride, PixelFormat format);

	[[nodiscard]] bool IsValid() const noexcept { return m_Read.IsValid(); }

	// The read-only view of the same memory, for asking a question about what was just written.
	[[nodiscard]] const ImageView& Read() const noexcept { return m_Read; }

	// One pixel. Ignored outside the image, for `At`'s reason above.
	//
	// **Const, because a view is not its pixels.** This owns nothing — it is a pointer, a stride, and
	// an extent over memory a presenter allocated — so `const` here would mean *the view cannot be
	// repointed*, which is the same reading `std::span` and `DmabufBuffer::Pixels` already take. A
	// caller holding one by value in a `const Result<MutableImageView>` is holding a handle, not a
	// promise about the image.
	void Set(std::int32_t x, std::int32_t y, Rgba16 colour) const noexcept;

	// Every pixel in `rect`, clipped to the image. Returns how many were written, so that a caller
	// that expected to fill the whole thing can say so.
	std::size_t Fill(PixelRect<DeviceSpace> rect, Rgba16 colour) const noexcept;

private:
	std::span<std::byte> m_Bytes;
	ImageView m_Read;
};

// A mapped dmabuf, viewed, with the kernel's cache maintenance held open across the look.
//
// **It exists so that reading a frame without the sync is not something a caller can express.**
// `DmabufBuffer::CpuRead` already brackets `DMA_BUF_IOCTL_SYNC` and Virtual/Buffer.h says why the
// bracket is an object rather than two calls — the end is the half that gets forgotten. Handing out
// a bare `ImageView` over `buffer.Pixels()` would reintroduce exactly that: a view outlives the
// expression it was built in, and the sync would end while somebody still held it. Owning both puts
// the lifetime of the look and the lifetime of the bracket in one object.
//
// Neither copied nor moved, because `CpuRead` is neither.
class BufferReader
{
public:
	explicit BufferReader(const DmabufBuffer& buffer);

	BufferReader(const BufferReader&) = delete;
	BufferReader& operator=(const BufferReader&) = delete;
	BufferReader(BufferReader&&) = delete;
	BufferReader& operator=(BufferReader&&) = delete;

	[[nodiscard]] bool IsValid() const noexcept { return m_Status.has_value(); }

	// `ENODEV` where the buffer carries no mapping — an allocator that cannot map is a real case
	// Virtual/Buffer.h names — and `Over`'s own errors otherwise.
	[[nodiscard]] const Result<void>& Status() const noexcept { return m_Status; }

	[[nodiscard]] const ImageView& Image() const noexcept { return m_View; }

private:
	DmabufBuffer::CpuRead m_Read;
	ImageView m_View{};
	Result<void> m_Status{};
};

// Prints as rgba(65535, 0, 0, 65535), which is what a failed comparison should say. Not hexadecimal
// and not eight-bit: a value that was widened from ten bits has no eight-bit spelling, and printing
// one would make the two depths look equal when they are not.
template<>
struct std::formatter<Rgba16>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(Rgba16 pixel, Context& context) const
	{
		return std::format_to(context.out(), "rgba({}, {}, {}, {})", pixel.Red, pixel.Green, pixel.Blue, pixel.Alpha);
	}
};

// The widening and the narrowing are inverses over every value either depth can hold. Asserted at
// the ends here and swept across the full range in Pixels.Test.cpp, because a writer and a reader
// that disagree by one turns every fill-then-check into a test of the rounding.
static_assert(ToEightBit(FromEightBit(0)) == 0 && ToEightBit(FromEightBit(255)) == 255);
static_assert(ToEightBit(FromEightBit(127)) == 127 && ToEightBit(FromEightBit(128)) == 128);
static_assert(ToTenBit(FromTenBit(0)) == 0 && ToTenBit(FromTenBit(1023)) == 1023);
static_assert(ToTwoBit(FromTwoBit(0)) == 0 && ToTwoBit(FromTwoBit(3)) == 3);

// Full-range white is full-range white whichever depth it arrived in, which is the property that
// makes one expectation serve two formats.
static_assert(FromEightBit(255) == FromTenBit(1023));
static_assert(Rgb8(255, 255, 255) == Rgba16{ 65535, 65535, 65535, 65535 });

// An opaque black pixel is not a transparent one, which is what `At` answers outside the image.
static_assert(OpaqueBlack != Rgba16{});

static_assert(std::is_trivially_copyable_v<Rgba16> && std::formattable<Rgba16, char>);

// The formats this file decodes are exactly the single-plane RGB ones the seam names, and `NV12` is
// refused rather than mis-decoded.
static_assert(DecodableBytesPerPixel(FormatXrgb8888) == 4 && DecodableBytesPerPixel(FormatArgb2101010) == 4);
static_assert(DecodableBytesPerPixel(FormatNv12) == 0);
static_assert(!HasAlpha(FormatXrgb8888) && HasAlpha(FormatArgb8888));
static_assert(BitsPerChannel(FormatXrgb8888) == 8 && BitsPerChannel(FormatXrgb2101010) == 10);
