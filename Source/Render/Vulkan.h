#pragma once

#include <volk.h>

#include <cerrno>
#include <cstdint>
#include <string_view>

#include "Core/Result.h"
#include "Seam/RenderTarget.h"

// The one place `volk.h` is included, and the two translations every other file here would otherwise
// each write for itself: a `VkResult` into the seam's errno vocabulary, and a DRM four-character code
// into a `VkFormat`.
//
// **volk rather than the loader's own prototypes, and the reason is what gyro is.** Decision 9 took
// volk for dispatch overhead and decision 107 found the better reason underneath it: volk resolves
// Vulkan with `dlopen("libvulkan.so.1")`, so gyro links nothing and a machine with a broken Vulkan
// install answers `volkInitialize()` with a failure. A boot service that refuses to exec because a
// shared object moved is a black screen with no console behind it — decision 37 removed the VT that
// would have been the way out — where a boot service that starts, reports, and falls back to
// decision 79's console renderer is a machine somebody can fix.

// `VkResult` in the vocabulary Seam/Renderer.h answers with: `EBUSY` transient, `ENODEV` or `EACCES`
// where the device is gone, `EINVAL` where the caller asked for something inexpressible, `ENOMEM`
// where the allocation failed.
//
// **The mapping is where the interesting judgement is, and only three entries carry any.**
// `VK_ERROR_DEVICE_LOST` is `ENODEV` because Seam/Renderer.h says device loss is the composition
// root's problem rather than the loop's, and `ENODEV` is what the loop already forwards rather than
// retries. `VK_ERROR_INVALID_EXTERNAL_HANDLE` is `EINVAL` rather than `ENODEV` because it is what a
// dmabuf the device cannot import answers with, and that is a miswiring somebody has to fix rather
// than hardware that went away. Everything unrecognised is `EIO`, which is the honest report for a
// driver saying something this file has not been taught.
[[nodiscard]] constexpr int ErrnoFor(VkResult result) noexcept
{
	switch (result)
	{
		case VK_SUCCESS:
			return 0;

		case VK_ERROR_OUT_OF_HOST_MEMORY:
		case VK_ERROR_OUT_OF_DEVICE_MEMORY:
		case VK_ERROR_OUT_OF_POOL_MEMORY:
		case VK_ERROR_FRAGMENTED_POOL:
			return ENOMEM;

		case VK_ERROR_DEVICE_LOST:
		case VK_ERROR_INITIALIZATION_FAILED:
		case VK_ERROR_INCOMPATIBLE_DRIVER:
			return ENODEV;

		case VK_ERROR_EXTENSION_NOT_PRESENT:
		case VK_ERROR_FEATURE_NOT_PRESENT:
		case VK_ERROR_FORMAT_NOT_SUPPORTED:
		case VK_ERROR_INVALID_EXTERNAL_HANDLE:
		case VK_ERROR_TOO_MANY_OBJECTS:
			return EINVAL;

		default:
			return EIO;
	}
}

// A failed call turned into the seam's answer, with the driver's own enumerator kept in the message.
// The number is what a Mesa bug report is filed with, so losing it to a generic errno is losing the
// only part a driver author can act on.
[[nodiscard]] inline Result<void> Check(VkResult result, const char* context) noexcept
{
	if (result == VK_SUCCESS)
	{
		return {};
	}

	// The context is a literal at every call site and the code is appended by the reader that prints
	// it; `Failure` takes a view, so there is nothing here to own and nothing to allocate.
	return Failure(ErrnoFor(result), context);
}

// What a composite is written into, from what the presenter allocated it as.
//
// **`X` and `A` map to the same `VkFormat`, and that is correct rather than sloppy.** DRM's `XR24`
// and `AR24` are one memory layout; the difference is whether the fourth channel means anything,
// which is a question about *blending* that Seam/Renderer.h already answers elsewhere — a draw item
// carries its own alpha mode in `ColorState`, and the target's own alpha is the presenter's business
// when it programs a plane. Vulkan has no `B8G8R8X8`, so inventing a distinction here would be
// inventing one the API cannot carry.
//
// **Little-endian, which is what makes this look backwards.** `DRM_FORMAT_XRGB8888` is a 32-bit
// word, so the bytes in memory run B, G, R, X — and Vulkan names formats by byte order. `XR24` is
// therefore `B8G8R8A8`, and a reader who expects `R8G8B8A8` is reading the DRM name as byte order.
//
// `VK_FORMAT_UNDEFINED` for anything else, including `NV12`: a client may commit one and an encoder
// may want one, but neither is a target a composite is recorded into.
[[nodiscard]] constexpr VkFormat VulkanFormat(std::uint32_t code) noexcept
{
	if (code == FormatXrgb8888 || code == FormatArgb8888)
	{
		return VK_FORMAT_B8G8R8A8_UNORM;
	}

	if (code == FormatXrgb2101010 || code == FormatArgb2101010)
	{
		return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
	}

	return VK_FORMAT_UNDEFINED;
}

// For a log line and a test failure. Only the ones this module can actually produce are named; the
// rest print as their number, which is what a driver's own documentation is indexed by anyway.
[[nodiscard]] constexpr std::string_view Name(VkResult result) noexcept
{
	switch (result)
	{
		case VK_SUCCESS:
			return "VK_SUCCESS";
		case VK_TIMEOUT:
			return "VK_TIMEOUT";
		case VK_NOT_READY:
			return "VK_NOT_READY";
		case VK_ERROR_OUT_OF_HOST_MEMORY:
			return "VK_ERROR_OUT_OF_HOST_MEMORY";
		case VK_ERROR_OUT_OF_DEVICE_MEMORY:
			return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
		case VK_ERROR_INITIALIZATION_FAILED:
			return "VK_ERROR_INITIALIZATION_FAILED";
		case VK_ERROR_DEVICE_LOST:
			return "VK_ERROR_DEVICE_LOST";
		case VK_ERROR_EXTENSION_NOT_PRESENT:
			return "VK_ERROR_EXTENSION_NOT_PRESENT";
		case VK_ERROR_FEATURE_NOT_PRESENT:
			return "VK_ERROR_FEATURE_NOT_PRESENT";
		case VK_ERROR_INCOMPATIBLE_DRIVER:
			return "VK_ERROR_INCOMPATIBLE_DRIVER";
		case VK_ERROR_FORMAT_NOT_SUPPORTED:
			return "VK_ERROR_FORMAT_NOT_SUPPORTED";
		case VK_ERROR_INVALID_EXTERNAL_HANDLE:
			return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
		default:
			return "VkResult";
	}
}

// The contract the seam rests on. A format this module cannot name is one `BindTargets` refuses, and
// the assertions are here rather than in the test because they are the whole of the function.
static_assert(VulkanFormat(FormatXrgb8888) == VK_FORMAT_B8G8R8A8_UNORM);
static_assert(VulkanFormat(FormatArgb8888) == VulkanFormat(FormatXrgb8888), "One layout, two names");
static_assert(VulkanFormat(FormatXrgb2101010) == VK_FORMAT_A2R10G10B10_UNORM_PACK32);
static_assert(VulkanFormat(FormatNv12) == VK_FORMAT_UNDEFINED, "A planar client buffer is not a composite target");
static_assert(VulkanFormat(0) == VK_FORMAT_UNDEFINED);

static_assert(ErrnoFor(VK_SUCCESS) == 0);
static_assert(ErrnoFor(VK_ERROR_DEVICE_LOST) == ENODEV, "Device loss is the root's problem, not the loop's");
static_assert(ErrnoFor(VK_ERROR_INVALID_EXTERNAL_HANDLE) == EINVAL, "A dmabuf the device refuses is a miswiring");
static_assert(ErrnoFor(VK_ERROR_OUT_OF_DEVICE_MEMORY) == ENOMEM);
