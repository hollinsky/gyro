#include "Virtual/Udmabuf.h"

#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <optional>
#include <print>
#include <span>
#include <string_view>
#include <utility>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Seam/RenderTarget.h"
#include "Testing/Test.h"
#include "Virtual/Buffer.h"

// The provider, against the kernel it is a provider for.
//
// **These need `/dev/udmabuf` and will not always have it.** Docs/Decisions.md decision 102 records
// why: the node is `0600 root:kvm` and a workstation reaches it only through logind's `uaccess` ACL,
// so a container or an SSH session with no seat has the kernel support and no access to it. A test
// that cannot run says so by name rather than passing quietly — a green run that skipped everything
// is exactly the rot decision 36's build-time checks exist to prevent, and Docs/Open.md carries the
// question of what CI should do beyond printing this line.

namespace
{
// Opened *and probed* once per test rather than shared, because each test wants to own the
// failure. Probed rather than merely opened for the reason `OpenAndProbeUdmabuf` gives: a device
// node with no driver behind it opens perfectly and then refuses the only ioctl that matters, so
// a gate that stopped at `open` would report a hard failure where the honest answer is a skip.
//
// **`nullopt` is a skip and not a failure, so the caller returns rather than asserting.** A
// `GYRO_REQUIRE` here would be the strictest gating option — udmabuf as a hard test prerequisite —
// and that is not the one taken: the environment that lacks it is a container or a seatless SSH
// session, and failing there would make the suite unrunnable in exactly the place decision 6 says
// it must run. What is owed instead is that the skip be *loud*, which is the printed line naming
// the operation and its errno, and that CI check the tests did not all skip. Docs/Open.md carries
// the second half; this is the first.
[[nodiscard]] std::optional<UdmabufAllocator> Available(std::string_view test)
{
	Result<Fd> device = OpenAndProbeUdmabuf();

	if (!device)
	{
		std::println(
			"  skipped Udmabuf.{}: {} ({})", test, device.error().Context(), std::strerror(device.error().Code())
		);

		return std::nullopt;
	}

	return std::optional<UdmabufAllocator>{ std::in_place, std::move(*device) };
}

constexpr PixelFormat Linear{ FormatXrgb8888, 0, ModifierLinear };
} // namespace

GYRO_TEST(Udmabuf, BytesPerPixelKnowsTheFormatsItAllocates)
{
	GYRO_CHECK_EQ(BytesPerPixel(FormatXrgb8888), 4U);
	GYRO_CHECK_EQ(BytesPerPixel(FormatArgb8888), 4U);
	GYRO_CHECK_EQ(BytesPerPixel(FormatXrgb2101010), 4U);

	// Planar, and this provider does not allocate one. Zero rather than a guess is what makes
	// `Supports` refuse it instead of computing a stride from nothing.
	GYRO_CHECK_EQ(BytesPerPixel(FormatNv12), 0U);
	GYRO_CHECK_EQ(BytesPerPixel(0), 0U);
}

GYRO_TEST(Udmabuf, ImageBytesRoundUpToWholePages)
{
	// `UDMABUF_CREATE` is `EINVAL` on a size that is not a whole number of pages — confirmed by
	// running it, per decision 102 — so the rounding is a requirement rather than an optimisation.
	const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));

	// One row of one pixel: far under a page, and it must still round up to one.
	GYRO_CHECK_EQ(UdmabufImageBytes(PixelSize<DeviceSpace>{ 1, 1 }, 4), page);

	// A 1920x1080 XR24 image is 8'294'400 bytes, which is a whole number of 4 KiB pages already.
	const std::size_t full = UdmabufImageBytes(PixelSize<DeviceSpace>{ 1920, 1080 }, 7680);
	GYRO_CHECK(full >= 1920U * 1080U * 4U);
	GYRO_CHECK_EQ(full % page, 0U);

	// An empty extent has no bytes rather than one page, so that a caller cannot allocate an image
	// for an output that is not on.
	GYRO_CHECK_EQ(UdmabufImageBytes(PixelSize<DeviceSpace>{ 0, 1080 }, 7680), 0U);
	GYRO_CHECK_EQ(UdmabufImageBytes(PixelSize<DeviceSpace>{ 1920, 1080 }, 0), 0U);
}

GYRO_TEST(Udmabuf, SupportsLinearAndNothingTiled)
{
	const UdmabufAllocator allocator{ Fd{} };

	GYRO_CHECK(allocator.Supports(Linear));

	// Unknown is not linear, per Seam/RenderTarget.h, and it is accepted here because a caller that
	// did not state a modifier gets told what it actually got.
	GYRO_CHECK(allocator.Supports(PixelFormat{ FormatXrgb8888, 0, ModifierInvalid }));

	// A tiled modifier is a real device's, and this provider has no device.
	GYRO_CHECK(!allocator.Supports(PixelFormat{ FormatXrgb8888, 0, 0x0100000000000001ULL }));
	GYRO_CHECK(!allocator.Supports(PixelFormat{ FormatNv12, 0, ModifierLinear }));
	GYRO_CHECK(!allocator.Supports(PixelFormat{}));
}

GYRO_TEST(Udmabuf, AllocatingWithNoDeviceIsEnodevRatherThanACrash)
{
	UdmabufAllocator allocator{ Fd{} };

	GYRO_CHECK(!allocator.IsValid());

	const Result<DmabufBuffer> buffer = allocator.Allocate(PixelSize<DeviceSpace>{ 64, 32 }, Linear);

	GYRO_REQUIRE(!buffer.has_value());
	GYRO_CHECK_EQ(buffer.error().Code(), ENODEV);
}

GYRO_TEST(Udmabuf, AllocatesARealDmabuf)
{
	std::optional<UdmabufAllocator> allocator = Available("AllocatesARealDmabuf");

	if (!allocator)
	{
		return;
	}

	Result<DmabufBuffer> buffer = allocator->Allocate(PixelSize<DeviceSpace>{ 64, 32 }, Linear);
	GYRO_REQUIRE(buffer.has_value());

	GYRO_CHECK(buffer->IsValid());
	GYRO_CHECK_EQ(buffer->Size(), (PixelSize<DeviceSpace>{ 64, 32 }));
	GYRO_CHECK_EQ(buffer->Stride(), 256U);

	// Described as linear even though the request could have said unknown: the target description is
	// what a renderer imports against, and it must say what the buffer is.
	GYRO_CHECK_EQ(buffer->Format(), Linear);

	// The whole point of the provider: this is a descriptor a Vulkan device can import.
	GYRO_CHECK(buffer->Descriptor().IsValid());
	GYRO_CHECK(buffer->Describe().IsValid());
}

GYRO_TEST(Udmabuf, TheMappingIsTheBufferAndSurvivesTheMemfd)
{
	std::optional<UdmabufAllocator> allocator = Available("TheMappingIsTheBufferAndSurvivesTheMemfd");

	if (!allocator)
	{
		return;
	}

	Result<DmabufBuffer> buffer = allocator->Allocate(PixelSize<DeviceSpace>{ 16, 4 }, Linear);
	GYRO_REQUIRE(buffer.has_value());

	// The allocator closes the memfd as soon as the dmabuf exists, because the driver pins the pages.
	// If that were wrong, the mapping below would be reading freed memory rather than the image, so
	// this is the test that holds decision 102's one-descriptor-per-target claim up.
	GYRO_REQUIRE(buffer->IsMapped());

	const std::span<std::byte> pixels = buffer->Pixels();
	GYRO_REQUIRE(!pixels.empty());

	// Written through the mapping and read back through the bracket a consumer would use, which is
	// the round trip an inspection test depends on — no readback, no staging buffer.
	pixels[0] = std::byte{ 0xAB };
	pixels[pixels.size() - 1] = std::byte{ 0xCD };

	const DmabufRead read{ *buffer };
	GYRO_REQUIRE_EQ(read.Bytes().size(), pixels.size());
	GYRO_CHECK_EQ(read.Bytes()[0], std::byte{ 0xAB });
	GYRO_CHECK_EQ(read.Bytes()[read.Bytes().size() - 1], std::byte{ 0xCD });
}

GYRO_TEST(Udmabuf, RefusesWhatItCannotProduce)
{
	std::optional<UdmabufAllocator> allocator = Available("RefusesWhatItCannotProduce");

	if (!allocator)
	{
		return;
	}

	const Result<DmabufBuffer> empty = allocator->Allocate(PixelSize<DeviceSpace>{ 0, 32 }, Linear);
	GYRO_REQUIRE(!empty.has_value());
	GYRO_CHECK_EQ(empty.error().Code(), EINVAL);

	const Result<DmabufBuffer> planar =
		allocator->Allocate(PixelSize<DeviceSpace>{ 64, 32 }, PixelFormat{ FormatNv12, 0, ModifierLinear });
	GYRO_REQUIRE(!planar.has_value());
	GYRO_CHECK_EQ(planar.error().Code(), EINVAL);
}

GYRO_TEST(Udmabuf, TwoBuffersAreTwoImages)
{
	std::optional<UdmabufAllocator> allocator = Available("TwoBuffersAreTwoImages");

	if (!allocator)
	{
		return;
	}

	Result<DmabufBuffer> first = allocator->Allocate(PixelSize<DeviceSpace>{ 16, 4 }, Linear);
	Result<DmabufBuffer> second = allocator->Allocate(PixelSize<DeviceSpace>{ 16, 4 }, Linear);
	GYRO_REQUIRE(first.has_value() && second.has_value());
	GYRO_REQUIRE(first->IsMapped() && second->IsMapped());

	GYRO_CHECK(first->Descriptor() != second->Descriptor());

	// A ring whose images aliased would composite the next frame over the one being read, which is
	// the tearing a consumer would report and nobody would attribute to the allocator.
	first->Pixels()[0] = std::byte{ 0x11 };
	second->Pixels()[0] = std::byte{ 0x22 };

	GYRO_CHECK_EQ(first->Pixels()[0], std::byte{ 0x11 });
	GYRO_CHECK_EQ(second->Pixels()[0], std::byte{ 0x22 });
}
