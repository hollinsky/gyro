#include "Render/Device.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <print>
#include <string_view>
#include <utility>

#include "Core/Result.h"
#include "Geometry/Space.h"
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

// **The export's refusals, which are the half of it a machine with no dmabuf support can still
// answer.** Each of these is a way for a caller to ask for a target that cannot be described, and
// each of them succeeding quietly would put a half-built buffer into a presenter's target set rather
// than an error into its log.
GYRO_TEST(Device, AnExportRefusesWhatItCannotDescribe)
{
	std::optional<VulkanDevice> device = Available("AnExportRefusesWhatItCannotDescribe");

	if (!device)
	{
		return;
	}

	constexpr PixelSize<DeviceSpace> Resolution{ 64, 32 };
	constexpr std::array<std::uint64_t, 1> Linear{ ModifierLinear };

	// No candidates at all, which is a modifier negotiation that found nothing in common. It is the
	// case most easily written as an assertion by a caller who assumed the list was non-empty.
	const Result<ExportedImage> nothing = device->Export(Resolution, FormatXrgb8888, {});
	GYRO_REQUIRE_EQ(nothing.has_value(), false);
	GYRO_CHECK_EQ(nothing.error().Code(), EINVAL);

	// `ModifierInvalid`, which is *unknown* rather than *none* — the same refusal `Supports` makes
	// from the import side, for the same reason: an image laid out under a tiling nobody stated works
	// by luck on the machine it was written on.
	constexpr std::array<std::uint64_t, 1> Unknown{ ModifierInvalid };
	const Result<ExportedImage> unknown = device->Export(Resolution, FormatXrgb8888, Unknown);
	GYRO_REQUIRE_EQ(unknown.has_value(), false);
	GYRO_CHECK_EQ(unknown.error().Code(), EINVAL);

	// A modifier no driver has ever advertised. This is what a parent compositor sending a vendor's
	// tiling to a machine with a different vendor's GPU looks like, and it must not be picked.
	constexpr std::array<std::uint64_t, 1> Foreign{ 0x00ff'0000'0000'0001ULL };
	const Result<ExportedImage> foreign = device->Export(Resolution, FormatXrgb8888, Foreign);
	GYRO_REQUIRE_EQ(foreign.has_value(), false);
	GYRO_CHECK_EQ(foreign.error().Code(), EINVAL);

	// A fourcc Render/Vulkan.h maps to no `VkFormat`. `NV12` is the honest example: a client commits
	// one and an encoder wants one, and neither is a target a composite is recorded into.
	const Result<ExportedImage> planar = device->Export(Resolution, FormatNv12, Linear);
	GYRO_REQUIRE_EQ(planar.has_value(), false);
	GYRO_CHECK_EQ(planar.error().Code(), EINVAL);

	// No extent. A presenter that had not yet been configured is what produces this, and a zero-sized
	// image is one `vkCreateImage` would refuse anyway — reported here, where the caller's mistake is.
	const Result<ExportedImage> empty = device->Export({}, FormatXrgb8888, Linear);
	GYRO_REQUIRE_EQ(empty.has_value(), false);
	GYRO_CHECK_EQ(empty.error().Code(), EINVAL);
}

// **Decision 108 from the allocating side: the implication rather than the answer.** Whether this
// machine's device can hand out a syncobj descriptor is the machine's business — lavapipe cannot and
// anv can — so what is asserted is that the capability query and the attempt agree. A device that
// claimed one and then failed, or refused one it could have made, is the mismatch a nested output
// would discover as a commit with no acquire point on it.
//
// The class is a parameter because both answers are worth having and no single device gives both.
namespace
{
void CheckTheTimelineMatchesTheClaim(std::string_view test, DeviceClass wanted)
{
	std::optional<VulkanDevice> device = Available(test, wanted);

	if (!device)
	{
		return;
	}

	std::println("  {}", device->Description());

	const Result<ExportedTimeline> timeline = device->ExportTimeline();
	GYRO_CHECK_EQ(timeline.has_value(), device->Description().ExportsTimeline);

	if (!timeline)
	{
		// Reported rather than asserted, which is the whole of what decision 108 asks of this path.
		GYRO_CHECK_EQ(timeline.error().Code(), ENOTSUP);

		return;
	}

	GYRO_REQUIRE(timeline->IsValid());
	GYRO_CHECK(timeline->Handle() != VK_NULL_HANDLE);

	// The descriptor is what `wp_linux_drm_syncobj_v1` is handed and what a KMS commit programs as
	// `IN_FENCE_FD`. On anv it is an `anon_inode:syncobj_file`, which is Seam/SyncPoint.h's premise.
	GYRO_REQUIRE(timeline->Descriptor().IsValid());

	// Nothing has been submitted against it, so it is where it started. A timeline that came back
	// already signalled would tell a presenter every buffer was free before the first frame.
	const Result<std::uint64_t> counter = timeline->Counter();
	GYRO_REQUIRE_EQ(counter.has_value(), true);
	GYRO_CHECK_EQ(*counter, std::uint64_t{ 0 });

	// Two are two, which is what a nested output needs: an acquire timeline and a release timeline
	// are separate objects and a shared one would have the parent's progress and gyro's on one counter.
	const Result<ExportedTimeline> second = device->ExportTimeline();
	GYRO_REQUIRE_EQ(second.has_value(), true);
	GYRO_CHECK(second->Handle() != timeline->Handle());
	GYRO_CHECK(second->Descriptor() != timeline->Descriptor());
}
} // namespace

GYRO_TEST(Device, AnExportedTimelineMatchesWhatTheSoftwareDeviceClaims)
{
	CheckTheTimelineMatchesTheClaim("AnExportedTimelineMatchesWhatTheSoftwareDeviceClaims", DeviceClass::Software);
}

// The other answer, on a machine that has one. Skipped where there is no GPU, for the reason every
// hardware-gated test here gives: lavapipe can never take this branch, so a suite that only ran the
// floor tier would leave the descriptor-producing path with no coverage anywhere.
GYRO_TEST(Device, AnExportedTimelineMatchesWhatTheHardwareDeviceClaims)
{
	CheckTheTimelineMatchesTheClaim("AnExportedTimelineMatchesWhatTheHardwareDeviceClaims", DeviceClass::Hardware);
}

// The heap selection, which is the whole of what turns `VK_EXT_memory_budget` into two counters and
// is the only part of it a machine with no GPU can exercise. The structures are filled by hand
// because that is the honest boundary: everything below this is the driver's answer, and everything
// above it is arithmetic that must not depend on which driver gave it.
namespace
{
struct Heaps
{
	VkPhysicalDeviceMemoryProperties Properties{};
	VkPhysicalDeviceMemoryBudgetPropertiesEXT Budget{};

	void Add(VkDeviceSize size, VkMemoryHeapFlags flags, VkDeviceSize usage, VkDeviceSize budget) noexcept
	{
		const std::uint32_t index = Properties.memoryHeapCount;
		Properties.memoryHeaps[index] = VkMemoryHeap{ .size = size, .flags = flags };
		Budget.heapUsage[index] = usage;
		Budget.heapBudget[index] = budget;
		++Properties.memoryHeapCount;
	}
};

constexpr VkDeviceSize Mib = VkDeviceSize{ 1 } << 20;
} // namespace

// The ordinary integrated shape: one device-local heap that is system memory, and a host-visible
// window beside it that no composite is ever evicted from.
GYRO_TEST(Device, TheMemoryReadingIsTheLargestDeviceLocalHeap)
{
	Heaps heaps;
	heaps.Add(256 * Mib, 0, 200 * Mib, 250 * Mib);
	heaps.Add(8192 * Mib, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT, 1400 * Mib, 1490 * Mib);
	heaps.Add(512 * Mib, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT, 4 * Mib, 500 * Mib);

	const GpuMemory reading = DeviceLocalMemory(heaps.Properties, heaps.Budget);

	GYRO_REQUIRE(reading.IsValid());
	GYRO_CHECK_EQ(reading.UsageBytes, 1400 * Mib);
	GYRO_CHECK_EQ(reading.BudgetBytes, 1490 * Mib);

	// The figure the counters actually carry: what is left before the driver starts taking memory
	// back, which is the reading a person hunting a stutter is after.
	GYRO_CHECK_EQ(Mebibytes(reading.HeadroomBytes()), std::int64_t{ 90 });
	GYRO_CHECK_EQ(Mebibytes(static_cast<std::int64_t>(reading.UsageBytes)), std::int64_t{ 1400 });
}

// A process over its budget is the one frame this pair exists to explain, and an unsigned difference
// would draw it as sixteen exabytes of headroom instead of the eviction it is.
GYRO_TEST(Device, MemoryHeadroomGoesNegativeOverBudget)
{
	Heaps heaps;
	heaps.Add(8192 * Mib, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT, 1600 * Mib, 1490 * Mib);

	const GpuMemory reading = DeviceLocalMemory(heaps.Properties, heaps.Budget);

	GYRO_REQUIRE(reading.IsValid());
	GYRO_CHECK_EQ(Mebibytes(reading.HeadroomBytes()), std::int64_t{ -110 });
}

// A device that reports no device-local heap at all — a software one, which cannot be evicted from —
// loses the counters rather than falling back to a host heap and answering a question nobody asked.
GYRO_TEST(Device, AHostOnlyDeviceReportsNoMemoryReading)
{
	Heaps heaps;
	heaps.Add(8192 * Mib, 0, 1400 * Mib, 1490 * Mib);

	GYRO_CHECK(!DeviceLocalMemory(heaps.Properties, heaps.Budget).IsValid());
}

// The other absence, and the one the extension check produces: a driver that lists
// `VK_EXT_memory_budget` and then answers with a zero budget is treated as not answering, because a
// heap the driver will lend nothing of is not a reading a reader could act on.
GYRO_TEST(Device, AnUnansweredBudgetIsNoMemoryReading)
{
	Heaps heaps;
	heaps.Add(8192 * Mib, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT, 0, 0);

	GYRO_CHECK(!DeviceLocalMemory(heaps.Properties, heaps.Budget).IsValid());
}

// And the branch above it: a device that never reported the extension answers nothing at all, with no
// query made. An invalid device is the same path — `ReadMemory` is called from the frame thread and
// must be safe on a renderer that failed to open.
GYRO_TEST(Device, AnUnsupportedDeviceReadsNoMemory)
{
	const VulkanDevice absent;

	GYRO_CHECK(!absent.Description().ReportsMemoryBudget);
	GYRO_CHECK(!absent.ReadMemory().IsValid());
}

// What the machine this runs on actually says, asserted only against what it claimed. A device that
// reports the extension has to give a budget the usage fits inside; one that does not must give
// nothing rather than a pair of zeros the trace would draw as a flat line.
//
// **Both classes, for `CheckTheTimelineMatchesTheClaim`'s reason and a sharper one.** What the two
// drivers put in `heapUsage` is not the same quantity — lavapipe reports the machine's memory in use
// and anv reports what this process has allocated — and the counters are only worth reading on the
// part that can actually evict a texture, so the hardware run is the one that matters and the
// software run is what proves the path is safe where it does not.
namespace
{
void CheckTheMemoryReadingMatchesTheClaim(std::string_view test, DeviceClass wanted)
{
	std::optional<VulkanDevice> device = Available(test, wanted);

	if (!device)
	{
		return;
	}

	const GpuMemory reading = device->ReadMemory();

	std::println(
		"  {}: memory budget reported {}, usage {} MiB, headroom {} MiB",
		device->Description().DeviceName(),
		device->Description().ReportsMemoryBudget,
		Mebibytes(static_cast<std::int64_t>(reading.UsageBytes)),
		Mebibytes(reading.HeadroomBytes())
	);

	if (!device->Description().ReportsMemoryBudget)
	{
		GYRO_CHECK(!reading.IsValid());

		return;
	}

	// A listed extension is still allowed to answer nothing, which is the reading rather than a
	// failure — see `DeviceDescription::ReportsMemoryBudget`. What is not allowed is a budget with no
	// heap behind it.
	if (reading.IsValid())
	{
		GYRO_CHECK(reading.BudgetBytes > 0);
	}
}
} // namespace

GYRO_TEST(Device, TheMemoryReadingMatchesWhatTheSoftwareDeviceClaims)
{
	CheckTheMemoryReadingMatchesTheClaim("TheMemoryReadingMatchesWhatTheSoftwareDeviceClaims", DeviceClass::Software);
}

GYRO_TEST(Device, TheMemoryReadingMatchesWhatTheHardwareDeviceClaims)
{
	CheckTheMemoryReadingMatchesTheClaim("TheMemoryReadingMatchesWhatTheHardwareDeviceClaims", DeviceClass::Hardware);
}
