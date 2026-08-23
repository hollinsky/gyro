#include "Virtual/Pixels.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Geometry/Space.h"
#include "Seam/RenderTarget.h"
#include "Testing/Test.h"

// The predicates a pixel test is written in, tested against bytes this file lays out by hand.
//
// Everything here is arithmetic over a `std::vector`, so it runs on every machine — no udmabuf, no
// device, no gate. That is deliberate: the thing an integration test leans on hardest is the
// decoder, and a decoder whose own tests only ran where there was a GPU would be the one part of the
// pixel path nobody could check when it broke.

namespace
{
constexpr PixelFormat Xrgb8{ FormatXrgb8888, 0, ModifierLinear };
constexpr PixelFormat Argb8{ FormatArgb8888, 0, ModifierLinear };
constexpr PixelFormat Xrgb10{ FormatXrgb2101010, 0, ModifierLinear };
constexpr PixelFormat Argb10{ FormatArgb2101010, 0, ModifierLinear };

constexpr PixelSize<DeviceSpace> Small{ 4, 3 };

// A stride wider than a row, because a real allocator pads and a decoder that multiplied by width
// would pass every test written over a tight one and then read the padding on hardware.
constexpr std::uint32_t PaddedStride = 4 * 4 + 8;

// Storage for `Small` at `PaddedStride`, filled with a value neither format decodes to anything
// meaningful, so an untouched pixel is distinguishable from a written one.
[[nodiscard]] std::vector<std::byte> Canvas()
{
	return std::vector<std::byte>(static_cast<std::size_t>(PaddedStride) * Small.Height, std::byte{ 0x5A });
}
} // namespace

// The channel order, which is the bug this whole file exists to prevent. `XR24` is
// `DRM_FORMAT_XRGB8888` — the letters are the bits of a little-endian word, so red sits in byte two.
GYRO_TEST(Pixels, ChannelOrderFollowsTheLittleEndianWord)
{
	std::vector<std::byte> bytes = Canvas();

	// Opaque red, spelled as the four bytes a driver would actually leave in memory.
	bytes[0] = std::byte{ 0x00 }; // Blue
	bytes[1] = std::byte{ 0x00 }; // Green
	bytes[2] = std::byte{ 0xFF }; // Red
	bytes[3] = std::byte{ 0x00 }; // Ignored

	const Result<ImageView> view = ImageView::Over(bytes, Small, PaddedStride, Xrgb8);
	GYRO_REQUIRE_EQ(view.has_value(), true);

	// Opaque despite the X byte being zero, because `XR24` has no alpha and reporting the ignored
	// bits would make every test of a solid an assertion about the format.
	GYRO_CHECK_EQ(view->At(0, 0), Rgb8(255, 0, 0));

	// The same bytes read as `AR24` are transparent red, which is the whole difference between the
	// two formats and the reason they are not interchangeable.
	const Result<ImageView> alpha = ImageView::Over(bytes, Small, PaddedStride, Argb8);
	GYRO_REQUIRE_EQ(alpha.has_value(), true);
	GYRO_CHECK_EQ(alpha->At(0, 0), Rgba8(255, 0, 0, 0));
}

// Ten bits decode to the same value eight bits do at the ends of the range, which is what lets one
// expectation serve both formats.
GYRO_TEST(Pixels, TenBitDecodesToTheSameFullRangeAsEight)
{
	std::vector<std::byte> bytes = Canvas();

	const Result<MutableImageView> canvas = MutableImageView::Over(bytes, Small, PaddedStride, Xrgb10);
	GYRO_REQUIRE_EQ(canvas.has_value(), true);

	canvas->Set(1, 1, Rgb8(255, 0, 0));
	canvas->Set(2, 1, Rgb8(0, 255, 0));
	canvas->Set(3, 1, Rgb8(0, 0, 255));

	GYRO_CHECK_EQ(canvas->Read().At(1, 1), Rgb8(255, 0, 0));
	GYRO_CHECK_EQ(canvas->Read().At(2, 1), Rgb8(0, 255, 0));
	GYRO_CHECK_EQ(canvas->Read().At(3, 1), Rgb8(0, 0, 255));

	// And the value that only ten bits can hold survives the round trip, which eight bits would have
	// quantized away — the point of decoding into sixteen rather than into eight.
	const Rgba16 fine{ FromTenBit(513), FromTenBit(514), FromTenBit(515), 65535 };
	canvas->Set(0, 2, fine);
	GYRO_CHECK_EQ(canvas->Read().At(0, 2), fine);
	GYRO_CHECK(canvas->Read().At(0, 2) != Rgba16{ FromTenBit(512), FromTenBit(514), FromTenBit(515), 65535 });
}

// Two bits of alpha is all `AR30` has, and a writer that assumed eight would put opaque where the
// caller asked for half.
GYRO_TEST(Pixels, TenBitAlphaIsTwoBitsAndSaysSo)
{
	std::vector<std::byte> bytes = Canvas();

	const Result<MutableImageView> canvas = MutableImageView::Over(bytes, Small, PaddedStride, Argb10);
	GYRO_REQUIRE_EQ(canvas.has_value(), true);

	canvas->Set(0, 0, Rgba8(255, 255, 255, 0));
	GYRO_CHECK_EQ(canvas->Read().At(0, 0).Alpha, std::uint16_t{ 0 });

	canvas->Set(1, 0, Rgba8(255, 255, 255, 255));
	GYRO_CHECK_EQ(canvas->Read().At(1, 0).Alpha, std::uint16_t{ 65535 });

	// One of the two values in between, which is what a caller asking for half actually gets.
	canvas->Set(2, 0, Rgba16{ 65535, 65535, 65535, FromTwoBit(1) });
	GYRO_CHECK_EQ(canvas->Read().At(2, 0).Alpha, FromTwoBit(1));
}

// The widening and the narrowing are inverses across the whole of both ranges. A single off-by-one
// here would make every fill-then-check test a test of the rounding instead of the composite.
GYRO_TEST(Pixels, DepthConversionRoundTripsExactly)
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

// A view that would read past its memory is refused rather than constructed, which is the whole of
// why `Over` returns a `Result`.
GYRO_TEST(Pixels, AViewThatWouldOverrunIsRefused)
{
	std::vector<std::byte> bytes = Canvas();

	GYRO_CHECK_EQ(ImageView::Over(bytes, Small, 4 * 4 - 1, Xrgb8).has_value(), false);
	GYRO_CHECK_EQ(ImageView::Over(std::span{ bytes }.first(8), Small, PaddedStride, Xrgb8).has_value(), false);
	GYRO_CHECK_EQ(ImageView::Over(bytes, PixelSize<DeviceSpace>{ 0, 3 }, PaddedStride, Xrgb8).has_value(), false);

	// Planar, so one stride cannot describe it. Refused by name rather than decoded as garbage RGB,
	// which is what an encoder target would otherwise silently become.
	GYRO_CHECK_EQ(
		ImageView::Over(bytes, Small, PaddedStride, PixelFormat{ FormatNv12, 0, ModifierLinear }).has_value(), false
	);

	// The tight case is legal: a buffer holds `stride * (height - 1) + row`, not `stride * height`,
	// because the last row's padding need not be mapped.
	const std::size_t tight = static_cast<std::size_t>(PaddedStride) * (Small.Height - 1) + 4 * 4;
	GYRO_CHECK_EQ(ImageView::Over(std::span{ bytes }.first(tight), Small, PaddedStride, Xrgb8).has_value(), true);
}

// The predicates refuse a rectangle the image does not contain rather than clipping it, which is
// what makes a misindexed test fail instead of quietly asserting less.
GYRO_TEST(Pixels, PredicatesRefuseARectangleTheImageDoesNotContain)
{
	std::vector<std::byte> bytes = Canvas();

	const Result<MutableImageView> canvas = MutableImageView::Over(bytes, Small, PaddedStride, Xrgb8);
	GYRO_REQUIRE_EQ(canvas.has_value(), true);

	GYRO_CHECK_EQ(canvas->Fill(canvas->Read().Extent(), Rgb8(9, 9, 9)), std::size_t{ 12 });

	GYRO_CHECK(canvas->Read().IsUniform(canvas->Read().Extent(), Rgb8(9, 9, 9)));
	GYRO_CHECK(!canvas->Read().IsUniform(PixelRect<DeviceSpace>{ { 0, 0 }, { 5, 3 } }, Rgb8(9, 9, 9)));
	GYRO_CHECK(!canvas->Read().IsUniform(PixelRect<DeviceSpace>{ { -1, 0 }, { 2, 2 } }, Rgb8(9, 9, 9)));

	// An empty rectangle is vacuously uniform and is reported as not uniform, because a test that
	// computed one by accident wants to hear about it.
	GYRO_CHECK(!canvas->Read().IsUniform(PixelRect<DeviceSpace>{ { 1, 1 }, { 0, 0 } }, Rgb8(9, 9, 9)));

	GYRO_CHECK_EQ(
		canvas->Read().CountMatching(PixelRect<DeviceSpace>{ { 0, 0 }, { 9, 9 } }, Rgb8(9, 9, 9)), std::size_t{ 0 }
	);

	// Outside the image, `At` is transparent black — a value no `XR24` target can hold.
	GYRO_CHECK_EQ(canvas->Read().At(4, 0), Rgba16{});
	GYRO_CHECK_EQ(canvas->Read().At(-1, 0), Rgba16{});
}

// The predicate that stands in for a golden image: where is the thing that was drawn?
GYRO_TEST(Pixels, TheBoundOfWhatDiffersIsTheThingThatWasDrawn)
{
	std::vector<std::byte> bytes = Canvas();

	const Result<MutableImageView> canvas = MutableImageView::Over(bytes, Small, PaddedStride, Xrgb8);
	GYRO_REQUIRE_EQ(canvas.has_value(), true);

	const Rgba16 background = Rgb8(0, 0, 0);
	canvas->Fill(canvas->Read().Extent(), background);

	GYRO_CHECK_EQ(canvas->Read().BoundsOfDiffering(background).has_value(), false);

	canvas->Fill(PixelRect<DeviceSpace>{ { 1, 1 }, { 2, 1 } }, Rgb8(255, 0, 0));

	const std::optional<PixelRect<DeviceSpace>> drawn = canvas->Read().BoundsOfDiffering(background);
	GYRO_REQUIRE_EQ(drawn.has_value(), true);
	GYRO_CHECK_EQ(*drawn, (PixelRect<DeviceSpace>{ { 1, 1 }, { 2, 1 } }));

	// And with a tolerance wide enough to swallow the difference, nothing was drawn at all — which is
	// what makes tolerance a parameter rather than a constant hidden in the comparison.
	GYRO_CHECK_EQ(canvas->Read().BoundsOfDiffering(background, 65535).has_value(), false);
}

// A fill lands inside the rectangle and nowhere else, including in the stride padding — the one
// place a row-at-a-time writer goes wrong and no visible pixel reports it.
GYRO_TEST(Pixels, AFillClipsAndLeavesThePaddingAlone)
{
	std::vector<std::byte> bytes = Canvas();

	const Result<MutableImageView> canvas = MutableImageView::Over(bytes, Small, PaddedStride, Xrgb8);
	GYRO_REQUIRE_EQ(canvas.has_value(), true);

	// Half off the right edge, so the clip is what decides how many pixels are written.
	GYRO_CHECK_EQ(canvas->Fill(PixelRect<DeviceSpace>{ { 2, 0 }, { 4, 2 } }, Rgb8(1, 2, 3)), std::size_t{ 4 });

	GYRO_CHECK(canvas->Read().IsUniform(PixelRect<DeviceSpace>{ { 2, 0 }, { 2, 2 } }, Rgb8(1, 2, 3)));

	for (std::size_t index = 4 * 4; index < PaddedStride; ++index)
	{
		GYRO_REQUIRE_EQ(bytes[index], std::byte{ 0x5A });
	}
}
