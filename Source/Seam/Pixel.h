#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <type_traits>

#include "Seam/RenderTarget.h"

// What a `PixelFormat`'s four characters mean in memory, and the one depth everything above the
// bytes is spelled in.
//
// **It is here because the format codes are here.** Seam/RenderTarget.h reproduces DRM's fourccs so
// that a target can describe memory the portable tier is allowed to name; a fourcc that nothing can
// decode describes nothing, so the layout belongs beside the code rather than in whichever module
// needed it first. *(That module was `Virtual`, and Virtual/Pixels.h said as much: it argued for
// living there because only a virtual output's consumer decoded a frame, and named the CPU renderer
// as the caller that would eventually make that false. It did.)*
//
// **Two callers, and having one encoder rather than two is the whole point.** `Blit` composites into
// a `MappedImage` by writing these words, and a test asserts what it wrote by reading them. Two
// spellings of `XR24` that disagree by a least-significant bit turn every fill-then-check into a
// test of somebody's rounding; one spelling makes a channel-order bug fail loudly in both
// directions.
//
// **Header-only and constexpr, which is a decision rather than a habit.** The encode is the inner
// loop of a CPU composite — one call per pixel of the damage region, on the machine where there is
// no GPU to fall back to — so it has to inline. It is also the only thing in this module with an
// answer worth checking at compile time, and the assertions at the bottom are that check.

// One decoded pixel, in the common depth. Channels are named rather than indexed because the whole
// class of bug this file exists to catch is a channel order — `XR24` is `B, G, R, X` in memory, and
// every test that ever spelled that inline eventually spelled it backwards.
//
// **Every value is sixteen bits per channel and that is not future-proofing.** `XR30` and `AR30` are
// already named one file over, and decision 47 puts HDR content beside SDR content in one composite;
// a codec that quietly narrowed ten-bit content to eight would report a banding bug as a pass.
// Sixteen bits holds both source depths exactly — bit replication, not a divide — so an eight-bit
// target compared against an eight-bit expectation is still an exact comparison, and a tolerance is
// something a caller asks for rather than something the decode imposes.
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
// property Pixel.Test.cpp asserts across the whole eight-bit and ten-bit ranges — a writer and a
// reader that disagree by one would make every fill-then-check a test of the rounding.
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

// What a caller means when it writes a colour. Spelled in the depth the author is thinking in,
// widened once, here — so that `Rgb8(255, 0, 0)` compares equal to what an `XR24` target holds and
// to what an `XR30` target holds, and a caller that changes format changes nothing else.
[[nodiscard]] constexpr Rgba16 Rgba8(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha)
{
	return { FromEightBit(red), FromEightBit(green), FromEightBit(blue), FromEightBit(alpha) };
}

[[nodiscard]] constexpr Rgba16 Rgb8(std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
	return Rgba8(red, green, blue, 255);
}

// What a composite with nothing in it comes to, named once. Every caller that checks a cleared
// rectangle wants this value and none of them should be spelling it.
inline constexpr Rgba16 OpaqueBlack = Rgba8(0, 0, 0, 255);

// How many bytes one pixel of a format occupies, and zero for a format this file cannot code.
//
// `NV12` is the interesting zero. Seam/RenderTarget.h carries it because a client may commit one and
// an encoder may want one, and it is planar and chroma-subsampled, so a single word at a single
// stride cannot describe it — refusing by name is what keeps a future encoder target from being
// decoded as garbage RGB rather than reported as unsupported.
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
// mean nothing, so a decode reports opaque rather than whatever a renderer happened to leave — a
// caller asserting `Alpha == 65535` over an `XR24` target is asserting the format, not the
// composite.
[[nodiscard]] constexpr bool HasAlpha(std::uint32_t code) noexcept
{
	return code == FormatArgb8888 || code == FormatArgb2101010;
}

// The source depth, which a file writer needs in order to produce something that is neither lossy
// nor gratuitously wide.
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

// **Little-endian words rather than byte offsets, because that is what DRM's names mean.** `XR24` is
// `DRM_FORMAT_XRGB8888`, and the `XRGB` is the order of the *bits in a 32-bit little-endian word* —
// so the bytes in memory run `B, G, R, X`. Spelling it as a word and shifting is the reading that
// matches the kernel's own definition; spelling it as `bytes[2], bytes[1], bytes[0]` is the same
// answer arrived at by a coincidence that stops holding for the ten-bit formats, where no channel is
// byte-aligned at all.
[[nodiscard]] inline std::uint32_t LoadWord(const std::byte* at) noexcept
{
	std::uint32_t word = 0;
	std::memcpy(&word, at, sizeof word);

	return word;
}

inline void StoreWord(std::byte* at, std::uint32_t word) noexcept
{
	std::memcpy(at, &word, sizeof word);
}

[[nodiscard]] constexpr Rgba16 DecodePixel(std::uint32_t word, std::uint32_t code) noexcept
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
[[nodiscard]] constexpr std::uint32_t EncodePixel(Rgba16 pixel, std::uint32_t code) noexcept
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
// the ends here and swept across the full range in Pixel.Test.cpp, because a writer and a reader
// that disagree by one turns every fill-then-check into a test of the rounding.
static_assert(ToEightBit(FromEightBit(0)) == 0 && ToEightBit(FromEightBit(255)) == 255);
static_assert(ToEightBit(FromEightBit(127)) == 127 && ToEightBit(FromEightBit(128)) == 128);
static_assert(ToTenBit(FromTenBit(0)) == 0 && ToTenBit(FromTenBit(1023)) == 1023);
static_assert(ToTwoBit(FromTwoBit(0)) == 0 && ToTwoBit(FromTwoBit(3)) == 3);

// Full-range white is full-range white whichever depth it arrived in, which is the property that
// makes one expectation serve two formats.
static_assert(FromEightBit(255) == FromTenBit(1023));
static_assert(Rgb8(255, 255, 255) == Rgba16{ 65535, 65535, 65535, 65535 });

// An opaque black pixel is not a transparent one, which is what a decode outside an image answers.
static_assert(OpaqueBlack != Rgba16{});

static_assert(std::is_trivially_copyable_v<Rgba16> && std::formattable<Rgba16, char>);

// The formats this file codes are exactly the single-plane RGB ones the seam names, and `NV12` is
// refused rather than mis-coded.
static_assert(DecodableBytesPerPixel(FormatXrgb8888) == 4 && DecodableBytesPerPixel(FormatArgb2101010) == 4);
static_assert(DecodableBytesPerPixel(FormatNv12) == 0);
static_assert(!HasAlpha(FormatXrgb8888) && HasAlpha(FormatArgb8888));
static_assert(BitsPerChannel(FormatXrgb8888) == 8 && BitsPerChannel(FormatXrgb2101010) == 10);

// The channel order, at compile time and in both directions — which is the bug this file is named
// for. Red in the second byte of an `XR24` word, and an ignored alpha coming back opaque.
static_assert(EncodePixel(Rgb8(255, 0, 0), FormatXrgb8888) == 0xFFFF0000U);
static_assert(EncodePixel(Rgb8(0, 0, 255), FormatXrgb8888) == 0xFF0000FFU);
static_assert(DecodePixel(0xFFFF0000U, FormatXrgb8888) == Rgb8(255, 0, 0));
static_assert(DecodePixel(0x00FF0000U, FormatXrgb8888) == Rgb8(255, 0, 0), "X is ignored, so it decodes opaque");
static_assert(DecodePixel(0x00FF0000U, FormatArgb8888) == Rgba8(255, 0, 0, 0), "A is not");

// A round trip through the widest format this file codes, which is the one where no channel is
// byte-aligned and an off-by-a-shift is otherwise invisible.
static_assert(DecodePixel(EncodePixel(Rgb8(255, 0, 0), FormatXrgb2101010), FormatXrgb2101010) == Rgb8(255, 0, 0));
