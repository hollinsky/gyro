#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <type_traits>

#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Seam/Pixel.h"
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
// **What the bytes mean is not here, and used to be.** *(Moved 2026-08-22.)* `Rgba16`, the depth
// conversions, and the per-word codec are in Seam/Pixel.h, beside the format codes that give them
// their meaning. This file argued for holding them on the grounds that only a virtual output's
// consumer decoded a frame, and named `Blit` as the caller that would make that false; `Blit` is the
// second caller and a renderer may not depend on a presenter's module, so the shared half went down
// rather than the dependency going sideways. What is left here is the vocabulary for asking
// questions about an image, which has exactly the one caller this file always claimed.

// How two images differ: how far apart they are at their worst, where that was, and how much of the
// rectangle is further apart than the caller was willing to accept.
//
// **A record rather than a boolean, and the boolean is the cheap half.** `ImageView::Compare` is
// written for decision 62's oracle, which draws one scene through two executions of the same chain;
// when those disagree the question is immediately *by how much, and where* — a fringe along one
// corner arc is a coverage bug and a uniform shift across a whole fill is a conversion bug, and the
// two look identical to a predicate that answers false. Handing back the distance and the pixel is
// the difference between a failure somebody bisects in an hour and one they spend a day
// reconstructing this type in order to see.
//
// **`Worst` is in `Rgba16`'s depth and the formatter prints it in eight-bit code points**, which is
// the unit the threshold is stated in: `FromEightBit(1)` is 257, one code point of an ordinary sRGB
// output, and that is about where a difference stops being something a person could see in a
// gradient. A number in sixteen-bit units tells a reader nothing without that division, so the
// division happens where the number is printed rather than in everybody's head.
struct ImageDifference
{
	// False where the comparison could not be made: a rectangle one of the two images does not
	// contain, or an empty one. **Not the same as a disagreement** — a caller that read it as one
	// would be reporting a difference it never measured, which is exactly the failure a test
	// comparing a mistyped rectangle would otherwise pass off as a real one. `Agrees` is false in
	// both cases, deliberately, because a comparison that did not happen has not agreed either.
	bool Comparable = false;

	// How many pixels were looked at, how many of those were not identical, and how many were
	// further apart than `tolerance`.
	//
	// **`Differing` is the margin and `Beyond` is the verdict**, which is why both are here. A
	// threshold comparison reports its worst pixel at the threshold as soon as *any* pixel lands
	// near a rounding boundary, so the worst figure saturates and stops moving long before the
	// difference underneath it does. What keeps moving is the count: two paths whose values drift by
	// a fifth of a code point land on a different code at about a fifth of their pixels, so
	// `Differing / Compared` estimates the drift directly and is what shows a margin eroding while
	// the assertion still passes.
	std::size_t Compared = 0;
	std::size_t Differing = 0;
	std::size_t Beyond = 0;

	// The largest single-channel distance anywhere in the rectangle, whether or not it was within
	// tolerance, and the pixel it was found at with what each image held there.
	//
	// **Reported even when everything agreed**, which is what makes this worth printing on a passing
	// run: the margin between the worst pixel and the threshold is the only evidence that an
	// assertion is still measuring something, and a suite that prints it is one where a driver
	// upgrade eroding that margin is visible before it crosses.
	std::uint16_t Worst = 0;
	PixelPoint<DeviceSpace> Where{};
	Rgba16 Here{};
	Rgba16 There{};

	[[nodiscard]] constexpr bool Agrees() const noexcept { return Comparable && Beyond == 0; }

	friend constexpr bool operator==(const ImageDifference&, const ImageDifference&) noexcept = default;
};

// Prints as `3 of 2048 pixels beyond tolerance, 431 not identical; worst 0.74 code points at
// (24, 9), rgba(...) against rgba(...)`, which is a line somebody can act on without opening this
// file.
template<>
struct std::formatter<ImageDifference>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const ImageDifference& difference, Context& context) const
	{
		if (!difference.Comparable)
		{
			return std::format_to(context.out(), "not comparable: the rectangle is not inside both images");
		}

		return std::format_to(
			context.out(),
			"{} of {} pixels beyond tolerance, {} not identical; worst {:.2f} code points at ({}, {}), {} "
			"against {}",
			difference.Beyond,
			difference.Compared,
			difference.Differing,
			static_cast<double>(difference.Worst) / static_cast<double>(FromEightBit(1)),
			difference.Where.X,
			difference.Where.Y,
			difference.Here,
			difference.There
		);
	}
};

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

	// How this image and another differ over `rect`, and by how much, and where.
	//
	// **This is decision 62's oracle in one call, and the report is most of what it is for.** That
	// decision draws one scene twice — the fused chain and the separate passes — and asserts the two
	// agree below the perceptual threshold rather than to the bit. A predicate that answered *they
	// differ* would leave whoever is holding a failed run to write this function anyway, at the worst
	// possible moment, so it is written once here: the worst pixel, where it is, what each side held
	// there, and how many pixels were further apart than the caller allowed.
	//
	// **Neither image is the expected one**, which is the difference from every other predicate in
	// this file. `IsUniform` and `BoundsOfDiffering` compare an image against a claim a test wrote
	// down; this compares two images against each other, and the reason decision 62 wants that is
	// that neither of the two is a golden frame — the reference is the other execution of the same
	// scene, available at runtime and unable to go stale.
	//
	// **The two formats need not match.** Both sides decode through `At`, so an `XR24` composite and
	// an `XR30` one are comparable, and `Rgba16`'s depth is what the distance is measured in.
	// `Comparable` is false where either image does not contain the rectangle.
	[[nodiscard]] ImageDifference
	Compare(const ImageView& other, PixelRect<DeviceSpace> rect, std::uint16_t tolerance = 0) const noexcept;

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

// The formatter, the codec, and the depth conversions all assert themselves in Seam/Pixel.h. What
// is worth asserting here is that a view is a handle rather than an image: two of them over the same
// memory are the same view, and a default-constructed one is not usable.
static_assert(std::is_trivially_copyable_v<ImageView> && std::is_trivially_copyable_v<MutableImageView>);
static_assert(std::is_trivially_copyable_v<ImageDifference> && std::formattable<ImageDifference, char>);

// A comparison that did not happen has not agreed, which is the reading `Comparable` exists to make
// unmissable: a default-constructed report is the one a caller gets when the rectangle was wrong.
static_assert(!ImageDifference{}.Agrees());
static_assert(!std::is_copy_constructible_v<BufferReader>, "The sync bracket is not something two objects may hold");
