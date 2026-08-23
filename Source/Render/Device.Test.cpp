#include "Render/Device.h"

#include <cstring>
#include <optional>
#include <print>
#include <string_view>
#include <utility>

#include "Core/Result.h"
#include "Render/Vulkan.h"
#include "Seam/RenderTarget.h"
#include "Testing/Test.h"

// The device, against whatever Vulkan this machine has.
//
// **These are gated the way Virtual/Udmabuf.Test.cpp's are and for a stronger reason.** That file
// can lose access to a device node; this one can be on a machine with no ICD installed at all, which
// is the ordinary state of a minimal container. A skip is printed by name rather than passing
// quietly, because a green run that ran nothing is the rot decision 36's build-time checks exist to
// prevent. Docs/Open.md carries what CI should do beyond printing the line.
//
// **The class asked for is `Software` throughout, and that is not a shortcut.** The machine this was
// written on has an Intel GPU beside lavapipe, so a test that took whatever device came first would
// exercise a different driver here than in CI — and it is the *floor* tier whose failures matter,
// since decision 40 makes it the permanently occupied one. Asking for it by name is what makes the
// two runs the same run.

namespace
{
// `nullopt` is a skip rather than a failure. Two conditions collapse into it and the caller's
// next step is the same for both: no loader at all, and a loader with no device behind it that
// meets decision 40's list. `Tools/VulkanProbe.cpp` is what tells them apart.
[[nodiscard]] std::optional<VulkanDevice> Available(std::string_view test, DeviceClass wanted = DeviceClass::Software)
{
	Result<VulkanDevice> device = VulkanDevice::Open({ .Class = wanted });

	if (!device)
	{
		std::println(
			"  skipped Device.{}: {} ({})", test, device.error().Context(), std::strerror(device.error().Code())
		);

		return std::nullopt;
	}

	return std::optional<VulkanDevice>{ std::move(*device) };
}

constexpr PixelFormat Linear{ FormatXrgb8888, 0, ModifierLinear };
} // namespace

// A machine with no Vulkan is a machine this reports on rather than one it fails on. The assertion
// is not that Vulkan is present — it is that asking is safe, which is what the composition root
// needs before it can decide between this renderer and decision 79's console.
GYRO_TEST(Device, LoaderProbeIsSafeWithoutVulkan)
{
	const bool present = IsVulkanLoaderPresent();

	if (!present)
	{
		std::println("  no Vulkan loader on this machine; the renderer would fall back to the console");
	}

	// Idempotent, which is what lets a test gate and a startup probe both call it.
	GYRO_CHECK_EQ(IsVulkanLoaderPresent(), present);
}

// Decision 40's claim, as a test rather than as a paragraph: lavapipe is a device selection.
GYRO_TEST(Device, SoftwareDeviceOpens)
{
	std::optional<VulkanDevice> device = Available("SoftwareDeviceOpens");

	if (!device)
	{
		return;
	}

	GYRO_REQUIRE(device->IsValid());
	GYRO_CHECK(device->Description().IsSoftware());
	GYRO_CHECK(!device->Description().DeviceName().empty());
	GYRO_CHECK(device->Handle() != VK_NULL_HANDLE && device->Queue() != VK_NULL_HANDLE);

	std::println("  {}", device->Description());
}

// The pairing decision 102 rests on, checked against the driver rather than against the log. udmabuf
// produces linear and nothing else; if the floor tier stopped accepting linear, every virtual output
// on a machine with no GPU would stop binding, and this is where that would first be visible.
GYRO_TEST(Device, SoftwareDeviceRendersIntoLinear)
{
	std::optional<VulkanDevice> device = Available("SoftwareDeviceRendersIntoLinear");

	if (!device)
	{
		return;
	}

	GYRO_CHECK(device->Supports(Linear));
	GYRO_CHECK(device->Supports({ FormatArgb8888, 0, ModifierLinear }));
}

// A format the device cannot render into is refused before a target set is built, which is the whole
// point of asking. `ModifierInvalid` is the interesting one: it means *unknown* rather than *none*,
// and importing an image whose tiling nobody stated is how a composite comes out sheared.
GYRO_TEST(Device, UnsupportedFormatsAreRefused)
{
	std::optional<VulkanDevice> device = Available("UnsupportedFormatsAreRefused");

	if (!device)
	{
		return;
	}

	GYRO_CHECK(!device->Supports({}));
	GYRO_CHECK(!device->Supports({ FormatXrgb8888, 0, ModifierInvalid }));
	GYRO_CHECK(!device->Supports({ FormatNv12, 0, ModifierLinear }));
	GYRO_CHECK(!device->Supports({ FourCc('Z', 'Z', 'Z', 'Z'), 0, ModifierLinear }));
}

// Decision 108, as the measurement it came from. This does not assert *which* answer the device
// gives — that is the machine's business — it asserts that the renderer's behaviour is derived from
// the answer rather than assumed, which is the thing that was wrong before it was measured.
GYRO_TEST(Device, TimelineExportIsAskedRatherThanAssumed)
{
	std::optional<VulkanDevice> software = Available("TimelineExportIsAskedRatherThanAssumed");

	if (!software)
	{
		return;
	}

	std::println(
		"  {} timeline export: {}",
		software->Description().DriverName(),
		software->Description().ExportsTimeline ? "yes" : "no"
	);

	// Stable across two queries of the same device: it is a device property, not a per-call answer.
	Result<VulkanDevice> again = VulkanDevice::Open({ .Class = DeviceClass::Software });
	GYRO_REQUIRE(again.has_value());
	GYRO_CHECK_EQ(again->Description().ExportsTimeline, software->Description().ExportsTimeline);
}

// The move that device migration is. Decision 41 has the composition root replacing the device on
// every boot, and Docs/Structure.md's rule is that no Vulkan handle outlives the device that made
// it — so the handle has to travel with the object rather than be duplicated by it.
GYRO_TEST(Device, MovingCarriesTheHandleAndLeavesNothingBehind)
{
	std::optional<VulkanDevice> device = Available("MovingCarriesTheHandleAndLeavesNothingBehind");

	if (!device)
	{
		return;
	}

	const VkDevice handle = device->Handle();

	VulkanDevice moved = std::move(*device);
	GYRO_CHECK_EQ(moved.Handle(), handle);
	GYRO_CHECK(!device->IsValid());

	VulkanDevice assigned;
	assigned = std::move(moved);
	GYRO_CHECK_EQ(assigned.Handle(), handle);
	GYRO_CHECK(!moved.IsValid());
}

// A default device answers everything without touching a driver, which is what keeps a failed `Open`
// from being usable by a caller that did not check.
GYRO_TEST(Device, DefaultDeviceIsInert)
{
	const VulkanDevice device;

	GYRO_CHECK(!device.IsValid());
	GYRO_CHECK(!device.Supports(Linear));
	GYRO_CHECK_EQ(device.ImportableMemoryTypes(RawFd{ 0 }), 0U);
	GYRO_CHECK_EQ(device.Handle(), VK_NULL_HANDLE);
}

// Asking for hardware on a machine that has none is a refusal rather than a software device handed
// over quietly, which is the mistake that would make a benchmark measure llvmpipe.
GYRO_TEST(Device, HardwareIsNotSatisfiedBySoftware)
{
	Result<VulkanDevice> hardware = VulkanDevice::Open({ .Class = DeviceClass::Hardware });

	if (!hardware)
	{
		std::println("  no hardware Vulkan device here: {}", hardware.error().Context());

		return;
	}

	GYRO_CHECK(!hardware->Description().IsSoftware());
}

// The errno vocabulary, which is what a caller branches on. Checked here rather than only in the
// header's assertions because these are the two a composition root has to tell apart: a device that
// went away is a rebuild, and a request it refused is a bug.
GYRO_TEST(Device, ResultsSpeakTheSeamsVocabulary)
{
	GYRO_CHECK_EQ(ErrnoFor(VK_SUCCESS), 0);
	GYRO_CHECK_EQ(ErrnoFor(VK_ERROR_DEVICE_LOST), ENODEV);
	GYRO_CHECK_EQ(ErrnoFor(VK_ERROR_FORMAT_NOT_SUPPORTED), EINVAL);
	GYRO_CHECK_EQ(ErrnoFor(VK_ERROR_OUT_OF_HOST_MEMORY), ENOMEM);

	// Anything the mapping has not been taught reports as I/O rather than as one of the four a caller
	// acts on, so a new driver error cannot be mistaken for a condition to retry.
	GYRO_CHECK_EQ(ErrnoFor(VK_ERROR_SURFACE_LOST_KHR), EIO);

	const Result<void> failed = Check(VK_ERROR_DEVICE_LOST, "vkQueueSubmit");
	GYRO_REQUIRE(!failed.has_value());
	GYRO_CHECK_EQ(failed.error().Code(), ENODEV);
	GYRO_CHECK_EQ(failed.error().Context(), std::string_view{ "vkQueueSubmit" });
	GYRO_CHECK(Check(VK_SUCCESS, "vkQueueSubmit").has_value());
}
