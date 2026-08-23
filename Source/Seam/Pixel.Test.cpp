#include "Seam/Pixel.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "Seam/RenderTarget.h"
#include "Testing/Test.h"

// What a fourcc means in memory, checked against bytes this file lays out by hand.
//
// **The channel order is the bug all of this exists to prevent**, and it is invisible to any test
// that writes through the same encoder it reads through: a codec with red and blue transposed round
// trips perfectly and puts a blue logo on the screen. So the tests below spell the bytes a driver
// would actually leave in memory, in the order a hexdump shows them, and decode *those*.
//
// Everything here is arithmetic over an array, so it runs on every machine. That matters more here
// than it looks: `Blit` is what draws when there is no GPU at all, and a codec whose tests needed
// one would be the part of the pixel path nobody could check on the machine it was written for.

namespace
{
// Opaque red as four bytes, which is `B, G, R, X` and not `R, G, B, A`.
constexpr std::array<std::byte, 4> RedBytes{ std::byte{ 0x00 },
	                                         std::byte{ 0x00 },
	                                         std::byte{ 0xFF },
	                                         std::byte{ 0x00 } };
} // namespace

// `XR24` is `DRM_FORMAT_XRGB8888` — the letters are the bits of a little-endian word, so red sits in
// byte two and the ignored channel in byte three.
GYRO_TEST(Pixel, ChannelOrderFollowsTheLittleEndianWord)
{
	const std::uint32_t word = LoadWord(RedBytes.data());

	// Opaque despite the X byte being zero, because `XR24` has no alpha and reporting the ignored
	// bits would make every assertion about a solid an assertion about the format instead.
	GYRO_CHECK_EQ(DecodePixel(word, FormatXrgb8888), Rgb8(255, 0, 0));

	// The same four bytes read as `AR24` are *transparent* red, which is the whole difference between
	// the two formats and the reason they are not interchangeable.
	GYRO_CHECK_EQ(DecodePixel(word, FormatArgb8888), Rgba8(255, 0, 0, 0));
}

// The bytes an encode leaves, checked as bytes. The assertion that fails if red and blue are ever
// transposed, which a round trip through this file's own encoder cannot see.
GYRO_TEST(Pixel, AnEncodeLeavesTheBytesADriverExpects)
{
	std::array<std::byte, 4> bytes{};
	StoreWord(bytes.data(), EncodePixel(Rgb8(255, 0, 0), FormatXrgb8888));

	GYRO_CHECK_EQ(bytes[0], std::byte{ 0x00 });
	GYRO_CHECK_EQ(bytes[1], std::byte{ 0x00 });
	GYRO_CHECK_EQ(bytes[2], std::byte{ 0xFF });

	// The ignored channel goes out as all ones rather than as zero: the bits mean nothing by
	// definition, and a scanout path that a driver decided to read anyway should find opaque.
	GYRO_CHECK_EQ(bytes[3], std::byte{ 0xFF });

	StoreWord(bytes.data(), EncodePixel(Rgb8(0, 0, 255), FormatXrgb8888));
	GYRO_CHECK_EQ(bytes[0], std::byte{ 0xFF });
	GYRO_CHECK_EQ(bytes[2], std::byte{ 0x00 });
}

// Ten bits decode to the same value eight bits do at the ends of the range, which is what lets one
// expectation serve both formats — and what makes an `XR30` boot target cost a test nothing.
GYRO_TEST(Pixel, TenBitDecodesToTheSameFullRangeAsEight)
{
	GYRO_CHECK_EQ(DecodePixel(EncodePixel(Rgb8(255, 0, 0), FormatXrgb2101010), FormatXrgb2101010), Rgb8(255, 0, 0));
	GYRO_CHECK_EQ(DecodePixel(EncodePixel(Rgb8(0, 255, 0), FormatXrgb2101010), FormatXrgb2101010), Rgb8(0, 255, 0));
	GYRO_CHECK_EQ(DecodePixel(EncodePixel(Rgb8(0, 0, 255), FormatXrgb2101010), FormatXrgb2101010), Rgb8(0, 0, 255));

	// And a value only ten bits can hold survives, which eight would have quantized away — the point
	// of coding through sixteen rather than through eight.
	const Rgba16 fine{ FromTenBit(513), FromTenBit(514), FromTenBit(515), 65535 };
	GYRO_CHECK_EQ(DecodePixel(EncodePixel(fine, FormatXrgb2101010), FormatXrgb2101010), fine);
	GYRO_CHECK(fine != Rgba16{ FromTenBit(512), FromTenBit(514), FromTenBit(515), 65535 });
}

// Two bits of alpha is all `AR30` has, and a writer that assumed eight would put opaque where the
// caller asked for half.
GYRO_TEST(Pixel, TenBitAlphaIsTwoBitsAndSaysSo)
{
	const auto trip = [](Rgba16 pixel) {
		return DecodePixel(EncodePixel(pixel, FormatArgb2101010), FormatArgb2101010);
	};

	GYRO_CHECK_EQ(trip(Rgba8(255, 255, 255, 0)).Alpha, std::uint16_t{ 0 });
	GYRO_CHECK_EQ(trip(Rgba8(255, 255, 255, 255)).Alpha, std::uint16_t{ 65535 });

	// One of the two values in between, which is what a caller asking for half actually gets.
	GYRO_CHECK_EQ(trip(Rgba16{ 65535, 65535, 65535, FromTwoBit(1) }).Alpha, FromTwoBit(1));
}

// The widening and the narrowing are inverses across the whole of both ranges. A single off-by-one
// here would make every fill-then-check a test of the rounding instead of the composite.
GYRO_TEST(Pixel, DepthConversionRoundTripsExactly)
{
	for (std::uint32_t value = 0; value <= 255; ++value)
	{
		GYRO_REQUIRE_EQ(ToEightBit(FromEightBit(static_cast<std::uint8_t>(value))), static_cast<std::uint8_t>(value));
	}

	for (std::uint32_t value = 0; value <= 1023; ++value)
	{
		GYRO_REQUIRE_EQ(ToTenBit(FromTenBit(static_cast<std::uint16_t>(value))), static_cast<std::uint16_t>(value));
	}
}

// A format this file cannot code is refused rather than mis-coded, which is what keeps a planar
// target from being drawn into as though it were packed RGB.
GYRO_TEST(Pixel, APlanarFormatIsRefusedRatherThanMisread)
{
	GYRO_CHECK_EQ(DecodableBytesPerPixel(FormatNv12), 0U);
	GYRO_CHECK_EQ(BitsPerChannel(FormatNv12), 0U);
	GYRO_CHECK_EQ(DecodePixel(0xFFFFFFFFU, FormatNv12), Rgba16{});
	GYRO_CHECK_EQ(EncodePixel(Rgb8(255, 0, 0), FormatNv12), 0U);
}
