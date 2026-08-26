#include "Drm/Dumb.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <print>
#include <string>

#include "Core/Fd.h"
#include "Seam/RenderTarget.h"
#include "Testing/Test.h"

// The rung decision 151 reaches when the render device will not export.
//
// **Two halves with different gates, and the split is the point.** What this allocator refuses is
// decidable with no device at all, so those checks run everywhere and are where a widened `Supports`
// would be caught. What it produces needs a card node, which CI does not have — so that half prints a
// named skip rather than passing quietly, for the reason Render/Device.Test.cpp states at length: a
// green run that ran nothing is the rot decision 36's build-time checks exist to prevent.

namespace
{
// The first card node that opens. Not the catalog's *first with something connected* — this test does
// not scan out, and a card with no monitor on it allocates dumb buffers exactly as well as one that
// has. `CREATE_DUMB` needs the node authenticated, which a primary node grants on first open where
// nothing else holds master, and refuses otherwise; both land as a skip.
[[nodiscard]] Fd OpenAnyCard()
{
	for (int minor = 0; minor < 4; ++minor)
	{
		const std::string path = "/dev/dri/card" + std::to_string(minor);

		if (Fd card{ ::open(path.c_str(), O_RDWR | O_CLOEXEC) }; card.IsValid())
		{
			return card;
		}
	}

	return {};
}

constexpr PixelFormat Linear{ FormatXrgb8888, 0, ModifierLinear };
} // namespace

// A descriptor that never opened supports nothing. The card is checked before the format because a
// provider offered to the chain on a device that failed to open would be selected and then fail every
// allocation, which is a rung that lies rather than declines.
GYRO_TEST(Dumb, AnAllocatorWithNoCardSupportsNothing)
{
	const Drm::DumbAllocator allocator{ RawFd{} };

	GYRO_CHECK(!allocator.Supports(Linear));
}

// The pairing decision 151 rests on, as a test rather than as a paragraph: this rung offers linear and
// only linear, because a dumb buffer has no tiling to state. If it ever claimed a real modifier, a
// framebuffer would be built describing a layout the kernel did not lay out.
GYRO_TEST(Dumb, OnlyLinearIsOffered)
{
	const Fd card = OpenAnyCard();
	const Drm::DumbAllocator allocator{ card.IsValid() ? card.Borrow() : RawFd{ 1 } };

	GYRO_CHECK(allocator.Supports(Linear));
	GYRO_CHECK(!allocator.Supports(PixelFormat{ FormatXrgb8888, 0, ModifierInvalid }));
	GYRO_CHECK(!allocator.Supports(PixelFormat{ FormatXrgb8888, 0, 0x0100000000000001ULL }));

	// A planar format has no single bit depth for `CREATE_DUMB` to take, so it is refused rather than
	// allocated at a depth that would describe the luma plane alone.
	GYRO_CHECK(!allocator.Supports(PixelFormat{ FormatNv12, 0, ModifierLinear }));
}

// What the kernel actually returns, which is the half that cannot be reasoned out: the pitch is the
// driver's and may be wider than the width times the depth, and a buffer described at the computed
// stride would be read a row short of the truth on any part that aligns its scanout fetch.
GYRO_TEST(Dumb, TheKernelsOwnPitchIsWhatTheBufferReports)
{
	const Fd card = OpenAnyCard();

	if (!card.IsValid())
	{
		std::println("  skipped Dumb.TheKernelsOwnPitchIsWhatTheBufferReports: no card node opened");

		return;
	}

	Drm::DumbAllocator allocator{ card.Borrow() };
	const std::array<std::uint64_t, 1> linear{ ModifierLinear };

	Result<DmabufBuffer> buffer = allocator.Allocate({ 640, 480 }, FormatXrgb8888, linear);

	if (!buffer)
	{
		std::println(
			"  skipped Dumb.TheKernelsOwnPitchIsWhatTheBufferReports: {} ({})",
			buffer.error().Context(),
			std::strerror(buffer.error().Code())
		);

		return;
	}

	GYRO_REQUIRE(buffer->IsValid());
	GYRO_CHECK_EQ(buffer->Format(), Linear);
	GYRO_CHECK(buffer->Descriptor().IsValid());
	GYRO_CHECK(buffer->Stride() >= 640U * 4U);
	GYRO_CHECK_EQ(allocator.Allocations, std::uint64_t{ 1 });

	// One plane, and its descriptor is the buffer's. A second allocation is a second object, which is
	// what a target set of three depends on and what a handle reused across buffers would break.
	const RenderTarget described = buffer->Describe();
	const DmabufImage* const image = described.AsDmabuf();

	GYRO_REQUIRE(image != nullptr);
	GYRO_CHECK_EQ(image->PlaneCount, 1U);
	GYRO_CHECK_EQ(image->Planes[0].Offset, 0U);

	Result<DmabufBuffer> second = allocator.Allocate({ 640, 480 }, FormatXrgb8888, linear);

	GYRO_REQUIRE(second.has_value());
	GYRO_CHECK(second->Descriptor().Value != buffer->Descriptor().Value);
	GYRO_CHECK_EQ(allocator.Allocations, std::uint64_t{ 2 });
}

// A size no display would ask for, refused where it is decidable rather than after an ioctl.
GYRO_TEST(Dumb, AnEmptyExtentIsRefused)
{
	const Fd card = OpenAnyCard();
	Drm::DumbAllocator allocator{ card.IsValid() ? card.Borrow() : RawFd{ 1 } };
	const std::array<std::uint64_t, 1> linear{ ModifierLinear };

	GYRO_CHECK(!allocator.Allocate({ 0, 480 }, FormatXrgb8888, linear).has_value());
	GYRO_CHECK_EQ(allocator.Allocations, std::uint64_t{ 0 });
}

// An empty candidate list is a negotiation that produced nothing, and it is refused with no ioctl —
// the same sentence every other provider answers, so that a presenter walking a plane's table gets one
// vocabulary back.
GYRO_TEST(Dumb, AnEmptyCandidateListIsRefused)
{
	const Fd card = OpenAnyCard();
	Drm::DumbAllocator allocator{ card.IsValid() ? card.Borrow() : RawFd{ 1 } };

	GYRO_CHECK(!allocator.Allocate({ 640, 480 }, FormatXrgb8888, {}).has_value());
	GYRO_CHECK_EQ(allocator.Allocations, std::uint64_t{ 0 });
}
