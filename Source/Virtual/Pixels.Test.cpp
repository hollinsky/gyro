#include "Virtual/Pixels.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Geometry/Space.h"
#include "Seam/Pixel.h"
#include "Seam/RenderTarget.h"
#include "Testing/Test.h"

// The predicates a pixel test is written in, tested against bytes this file lays out by hand.
//
// Everything here is arithmetic over a `std::vector`, so it runs on every machine — no udmabuf, no
// device, no gate. That is deliberate: the thing an integration test leans on hardest is the
// decoder, and a decoder whose own tests only ran where there was a GPU would be the one part of the
// pixel path nobody could check when it broke.
//
// What the bytes *mean* is Seam/Pixel.Test.cpp's, one module down. What is here is the shape: a
// stride, an extent, a rectangle the image does not contain, and the padding a fill must not touch.

namespace
{
constexpr PixelFormat Xrgb8{ FormatXrgb8888, 0, ModifierLinear };
constexpr PixelFormat Argb8{ FormatArgb8888, 0, ModifierLinear };

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

// The views carry the codec through unchanged, which is the one thing left to check about it here:
// a `Set` at a padded stride and an `At` over the same memory agree, and both agree with what
// Seam/Pixel.Test.cpp says the bytes mean. The channel order, the depth conversions, and the refusal
// of a planar format are that file's, beside the fourccs they decode.
GYRO_TEST(Pixels, AViewCarriesTheCodecThroughUntouched)
{
	std::vector<std::byte> bytes = Canvas();

	const Result<MutableImageView> canvas = MutableImageView::Over(bytes, Small, PaddedStride, Xrgb8);
	GYRO_REQUIRE_EQ(canvas.has_value(), true);

	canvas->Set(1, 1, Rgb8(255, 0, 0));

	// Through the view, and through the bytes underneath it at the offset the stride puts them at —
	// which is what catches a view that indexed rows by width rather than by stride.
	GYRO_CHECK_EQ(canvas->Read().At(1, 1), Rgb8(255, 0, 0));
	GYRO_CHECK_EQ(LoadWord(bytes.data() + PaddedStride + 4), EncodePixel(Rgb8(255, 0, 0), FormatXrgb8888));

	// The same memory read as `AR24` is *opaque* red, because the write went out through `XR24` and
	// that format sends its ignored channel as all ones. The view is carrying the format it was
	// handed rather than normalising one, which is the half of Seam/Pixel.h's contract that only
	// shows up when two views disagree about the same bytes.
	const Result<ImageView> alpha = ImageView::Over(bytes, Small, PaddedStride, Argb8);
	GYRO_REQUIRE_EQ(alpha.has_value(), true);
	GYRO_CHECK_EQ(alpha->At(1, 1), Rgba8(255, 0, 0, 255));
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
