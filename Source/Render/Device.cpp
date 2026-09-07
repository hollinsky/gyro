#include "Render/Device.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/Result.h"
#include "Render/Vulkan.h"

namespace
{
// SPEC: how many physical devices and modifiers one query will look at. Both are far past any
// machine — four ICDs on a workstation is unusual and a driver advertising more than sixty-four
// tilings for one format does not exist — and both are fixed because these run with no allocation
// permitted in the `noexcept` accessors below.
constexpr std::uint32_t MaxPhysicalDevices = 16;
constexpr std::uint32_t MaxModifiers = 64;

// What decision 40 asks of a device, and the reason each one is here.
//
// The first three are the import path: a descriptor becomes memory, the memory is known to be a
// dmabuf, and the image over it is laid out by a modifier rather than by the driver's own
// tiling. The fourth is the handoff — a composite is read by something that is not a Vulkan
// queue, and `VK_QUEUE_FAMILY_FOREIGN_EXT` is how that is said in a barrier.
//
// `VK_KHR_timeline_semaphore` is not here because it is core in 1.2 and the feature is enabled
// below; `VK_KHR_external_semaphore_fd` is not here because it is optional — lavapipe advertises
// it and then refuses to create an exportable semaphore, so what settles the question is the
// capability query rather than the extension list. See decision 108.
//
// **`VK_EXT_image_drm_format_modifier` was the fourth and is now optional**, which is decision 40 as
// revised. It is what lets a tiling be named, and a device that cannot name one can still allocate,
// import and composite — linear, the way everybody did before modifiers existed. Requiring it cost
// the whole picture on the one device every machine has: lavapipe does not implement it, so a laptop
// whose GPU gyro could not bring up fell through to nothing rather than to a slow composite.
// `DeviceDescription::StatesModifiers` is where its absence is carried and every branch on it says
// linear.
constexpr std::array<const char*, 3> RequiredExtensions{
	VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
	VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
	VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
};

// Lower is better. Not `VkPhysicalDeviceType`'s own order, which puts `OTHER` first and `CPU`
// last with `VIRTUAL_GPU` in between — this is the order gyro would choose between two devices
// on one machine, and the only rung that matters today is that software comes last.
[[nodiscard]] int Rank(VkPhysicalDeviceType type) noexcept
{
	switch (type)
	{
		case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
			return 0;
		case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
			return 1;
		case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
			return 2;
		case VK_PHYSICAL_DEVICE_TYPE_CPU:
			return 3;
		default:
			return 4;
	}
}

[[nodiscard]] bool Admits(DeviceClass wanted, VkPhysicalDeviceType type) noexcept
{
	switch (wanted)
	{
		case DeviceClass::Hardware:
			return type != VK_PHYSICAL_DEVICE_TYPE_CPU;
		case DeviceClass::Software:
			return type == VK_PHYSICAL_DEVICE_TYPE_CPU;
		case DeviceClass::Any:
			return true;
	}

	return false;
}

[[nodiscard]] bool HasEveryExtension(VkPhysicalDevice device) noexcept
{
	std::uint32_t count = 0;
	if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS)
	{
		return false;
	}

	// A driver's extension list is a few hundred entries and this runs once per candidate at
	// startup, so the allocation is the cheap part of a call that loaded a driver to answer.
	std::vector<VkExtensionProperties> available(count);
	if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data()) != VK_SUCCESS)
	{
		return false;
	}

	return std::ranges::all_of(RequiredExtensions, [&available](const char* wanted) {
		return std::ranges::any_of(available, [wanted](const VkExtensionProperties& entry) {
			return std::strcmp(entry.extensionName, wanted) == 0;
		});
	});
}

// The first queue family that can draw. One queue is what gyro asks for and one is what it uses:
// decision 29 schedules outputs onto one frame thread, so a second queue would be a second
// submission order for work that is already ordered by the loop that produced it.
[[nodiscard]] bool FindGraphicsQueue(VkPhysicalDevice device, std::uint32_t& family) noexcept
{
	std::uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);

	std::vector<VkQueueFamilyProperties> families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

	for (std::uint32_t index = 0; index < count; ++index)
	{
		if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
		{
			family = index;

			return true;
		}
	}

	return false;
}

// Whether this device's extension list carries one name.
//
// **A second enumeration per question, and that is the right trade here.** Four capabilities are
// asked about at startup and each one re-reads a list of a few hundred entries, which costs a
// fraction of a millisecond once — against a cached list that would have to be built before the
// physical device is chosen and carried through selection, where three of the four callers do not
// want it. What this replaces is four copies of the same fifteen lines, one of which is the place a
// missed `!= VK_SUCCESS` would go unnoticed.
//
// **It is never the whole answer for a capability.** Decision 108's lesson is that an extension can
// be listed and still refuse the thing it names, so every caller below pairs this with the query
// that settles it. The one exception is the modifier extension, where being listed *is* the
// capability: what it provides is the ability to state a tiling, and the per-format vetoes that
// follow are asked of each format anyway.
[[nodiscard]] bool Lists(VkPhysicalDevice device, std::string_view extension) noexcept
{
	std::uint32_t count = 0;

	if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS)
	{
		return false;
	}

	std::vector<VkExtensionProperties> available(count);

	if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data()) != VK_SUCCESS)
	{
		return false;
	}

	return std::ranges::any_of(available, [extension](const VkExtensionProperties& entry) noexcept {
		return std::string_view{ entry.extensionName } == extension;
	});
}

// Whether this device will write host memory into a tiled image with no queue involved.
//
// **Both halves are asked, and the extension alone is not one of them.** `VK_EXT_host_image_copy`
// can be present with `hostImageCopy` false — the extension says the entry points exist, the feature
// says the driver implements them — and enabling an extension whose feature is false is a device
// creation that fails, which on this path is a machine that comes up with no renderer at all. So it
// is asked the way decision 108 taught: the capability query, not the extension list.
[[nodiscard]] bool QueryHostImageCopy(VkPhysicalDevice device) noexcept
{
	if (!Lists(device, VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME))
	{
		return false;
	}

	VkPhysicalDeviceHostImageCopyFeatures host{};
	host.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES;

	VkPhysicalDeviceFeatures2 features{};
	features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features.pNext = &host;

	vkGetPhysicalDeviceFeatures2(device, &features);

	return host.hostImageCopy == VK_TRUE;
}

// Whether this device will read its own clock and `CLOCK_MONOTONIC` together, which is two questions
// rather than one: the extension has to be there, and `CLOCK_MONOTONIC` has to be among the domains it
// offers. A driver that calibrates only against `CLOCK_MONOTONIC_RAW` is one gyro cannot use, because
// Core/Time.h's timebase is the adjusted one and the two diverge under NTP.
[[nodiscard]] bool QueryCalibration(VkPhysicalDevice device) noexcept
{
	if (!Lists(device, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME))
	{
		return false;
	}

	std::uint32_t domains = 0;

	if (vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(device, &domains, nullptr) != VK_SUCCESS)
	{
		return false;
	}

	std::vector<VkTimeDomainEXT> offered(domains);

	if (vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(device, &domains, offered.data()) != VK_SUCCESS)
	{
		return false;
	}

	return std::ranges::find(offered, VK_TIME_DOMAIN_DEVICE_EXT) != offered.end() &&
	       std::ranges::find(offered, VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT) != offered.end();
}

[[nodiscard]] bool QueryPipelineStatistics(VkPhysicalDevice device) noexcept
{
	VkPhysicalDeviceFeatures features{};
	vkGetPhysicalDeviceFeatures(device, &features);

	return features.pipelineStatisticsQuery == VK_TRUE;
}

// Whether a timeline semaphore on this device can be exported as a descriptor. Decision 108's
// whole question, asked of the driver rather than inferred from its extension list.
[[nodiscard]] bool QueryTimelineExport(VkPhysicalDevice device) noexcept
{
	const VkSemaphoreTypeCreateInfo type{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		                                  .pNext = nullptr,
		                                  .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
		                                  .initialValue = 0 };
	const VkPhysicalDeviceExternalSemaphoreInfo info{ .sType =
		                                                  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
		                                              .pNext = &type,
		                                              .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };

	VkExternalSemaphoreProperties properties{ .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
		                                      .pNext = nullptr,
		                                      .exportFromImportedHandleTypes = 0,
		                                      .compatibleHandleTypes = 0,
		                                      .externalSemaphoreFeatures = 0 };
	vkGetPhysicalDeviceExternalSemaphoreProperties(device, &info, &properties);

	return (properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0;
}

// How many bits of a timestamp query taken on this family carry a value. Zero where the family
// cannot timestamp at all, which is a real answer rather than a failure: llvmpipe reports sixty-four
// and a graphics family with none is a device whose GPU costs are simply not measurable.
[[nodiscard]] std::uint32_t QueryTimestampBits(VkPhysicalDevice device, std::uint32_t family) noexcept
{
	std::uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);

	if (family >= count)
	{
		return 0;
	}

	std::vector<VkQueueFamilyProperties> families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

	return families[family].timestampValidBits;
}

// This physical device's DRM node minors, each -1 where the driver does not carry it.
//
// **`VK_EXT_physical_device_drm` is a physical-device property query, not a device feature**, so it is
// read here without enabling anything at device creation — gyro uses none of its commands, only the
// minors it reports. Guarded on the extension being listed because the property struct is only
// populated where the implementation supports it; a driver without it — or one that has no node of the
// kind asked for — leaves decision 142's clock reader with nothing to open and its deadline with no
// node to state itself on, which both report rather than guess.
//
// **Both minors, because they are two different jobs on two different nodes.** The clock reader wants
// the primary node's sysfs kobject, which is where the driver hangs its frequency attributes; the
// deadline wants a render node, because a syncobj ioctl is `DRM_RENDER_ALLOW` and opening the primary
// node a second time is a thing gyro holds DRM master by.
struct DrmMinors
{
	std::int64_t Primary = -1;
	std::int64_t Render = -1;
};

[[nodiscard]] DrmMinors QueryDrmMinors(VkPhysicalDevice device) noexcept
{
	if (!Lists(device, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME))
	{
		return {};
	}

	VkPhysicalDeviceDrmPropertiesEXT drm{};
	drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;

	VkPhysicalDeviceProperties2 properties{};
	properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	properties.pNext = &drm;
	vkGetPhysicalDeviceProperties2(device, &properties);

	return { .Primary = drm.hasPrimary == VK_TRUE ? static_cast<std::int64_t>(drm.primaryMinor) : -1,
		     .Render = drm.hasRender == VK_TRUE ? static_cast<std::int64_t>(drm.renderMinor) : -1 };
}

void Describe(VkPhysicalDevice device, std::uint32_t family, DeviceDescription& into) noexcept
{
	VkPhysicalDeviceDriverProperties driver{};
	driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

	VkPhysicalDeviceProperties2 properties{};
	properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	properties.pNext = &driver;
	vkGetPhysicalDeviceProperties2(device, &properties);

	std::ranges::copy(properties.properties.deviceName, into.Name.begin());
	std::ranges::copy(driver.driverName, into.Driver.begin());
	into.Name.back() = '\0';
	into.Driver.back() = '\0';
	into.Type = properties.properties.deviceType;
	into.ApiVersion = properties.properties.apiVersion;
	into.ExportsTimeline = QueryTimelineExport(device);
	into.CopiesFromHost = QueryHostImageCopy(device);
	into.StatesModifiers = Lists(device, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
	into.TimestampPeriod = properties.properties.limits.timestampPeriod;
	into.TimestampValidBits = QueryTimestampBits(device, family);
	into.CalibratesTimestamps = QueryCalibration(device);
	into.CountsPipelineStatistics = QueryPipelineStatistics(device);

	// Listed is the whole of the capability here — the extension carries no feature struct and nothing
	// about enabling it can fail — so this is `StatesModifiers`' shape rather than `ExportsTimeline`'s.
	// A driver that lists it and then answers with a zero budget is caught by the reading instead; see
	// `DeviceDescription::ReportsMemoryBudget`.
	into.ReportsMemoryBudget = Lists(device, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
	const DrmMinors minors = QueryDrmMinors(device);
	into.PrimaryMinor = minors.Primary;
	into.RenderMinor = minors.Render;
}
} // namespace

bool IsVulkanLoaderPresent() noexcept
{
	// `volkInitialize` is the `dlopen`, and it is idempotent — calling it twice reuses the handle
	// rather than opening a second one — so this needs no once-flag of its own and stays callable
	// from a test gate that runs before anything else.
	return volkInitialize() == VK_SUCCESS;
}

Result<VulkanDevice> VulkanDevice::Open(VulkanDevicePolicy policy)
{
	if (!IsVulkanLoaderPresent())
	{
		return Failure(ENODEV, "no Vulkan loader: dlopen(libvulkan.so.1) failed");
	}

	// Built in place so that every early return below runs the destructor. An instance created and
	// then abandoned on a device that turned out to lack an extension is the leak this shape removes,
	// and it is the one a `goto`-free early-return function otherwise grows one of per failure.
	VulkanDevice device;

	const VkApplicationInfo application{ .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		                                 .pNext = nullptr,
		                                 .pApplicationName = policy.Application.data(),
		                                 .applicationVersion = 0,
		                                 .pEngineName = "gyro",
		                                 .engineVersion = 0,
		                                 // 1.3 for dynamic rendering, which is what a composite is
		                                 // recorded inside and what removes the render-pass and
		                                 // framebuffer objects a target set would otherwise carry two
		                                 // of per image.
		                                 .apiVersion = VK_API_VERSION_1_3 };
	const VkInstanceCreateInfo instanceInfo{ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		                                     .pNext = nullptr,
		                                     .flags = 0,
		                                     .pApplicationInfo = &application,
		                                     .enabledLayerCount = 0,
		                                     .ppEnabledLayerNames = nullptr,
		                                     // None. Everything the import path needs is core in 1.1,
		                                     // and an instance extension gyro does not use is a
		                                     // surface for a layer to attach to on a boot service.
		                                     .enabledExtensionCount = 0,
		                                     .ppEnabledExtensionNames = nullptr };

	if (Result<void> created = Check(vkCreateInstance(&instanceInfo, nullptr, &device.m_Instance), "vkCreateInstance");
	    !created)
	{
		return std::unexpected{ created.error() };
	}

	volkLoadInstanceOnly(device.m_Instance);

	std::uint32_t count = MaxPhysicalDevices;
	std::array<VkPhysicalDevice, MaxPhysicalDevices> candidates{};
	const VkResult enumerated = vkEnumeratePhysicalDevices(device.m_Instance, &count, candidates.data());

	// `VK_INCOMPLETE` means the machine has more devices than the cap above, which is not a failure:
	// the ones that were written are still ranked, and a seventeenth ICD is not the one gyro wanted.
	if (enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE)
	{
		return std::unexpected{ Check(enumerated, "vkEnumeratePhysicalDevices").error() };
	}

	VkPhysicalDevice chosen = VK_NULL_HANDLE;
	std::pair<int, int> best{};

	for (std::uint32_t index = 0; index < count; ++index)
	{
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(candidates[index], &properties);

		if (!Admits(policy.Class, properties.deviceType) || properties.apiVersion < VK_API_VERSION_1_3)
		{
			continue;
		}

		if (!HasEveryExtension(candidates[index]))
		{
			continue;
		}

		// Scanout first, rank second, and the pair is what says they are not comparable quantities: no
		// amount of being a faster part makes a device the one the display engine can read from. The
		// minors are queried per candidate rather than taken off `Describe`, because `Describe` runs
		// once on the device that has already been chosen and this is the choosing.
		const bool scansOut =
			policy.ScanoutMinor >= 0 && QueryDrmMinors(candidates[index]).Primary == policy.ScanoutMinor;
		const std::pair<int, int> score{ scansOut ? 0 : 1, Rank(properties.deviceType) };

		if (chosen == VK_NULL_HANDLE || score < best)
		{
			chosen = candidates[index];
			best = score;
		}
	}

	if (chosen == VK_NULL_HANDLE)
	{
		// One message for two conditions on purpose, because the reader's next step is the same for
		// both: run `VulkanProbe` and look at what the machine actually has. Splitting them would
		// mean claiming which of "no device" and "no device good enough" applies, and a machine with
		// one ICD that is missing one extension is honestly both.
		return Failure(ENODEV, "no Vulkan 1.3 device of the requested class with the required extensions");
	}

	device.m_Physical = chosen;

	if (!FindGraphicsQueue(chosen, device.m_QueueFamily))
	{
		return Failure(ENODEV, "the chosen Vulkan device has no graphics queue");
	}

	// After the family is known, not before: `timestampValidBits` is a property of the queue family
	// rather than of the device, and it is the half of the timestamp pair that decides whether this
	// renderer reports a GPU cost at all.
	Describe(chosen, device.m_QueueFamily, device.m_Description);

	// What the capability queries answered, printed once at open so a machine that cannot measure
	// or calibrate its GPU is explained without reading the driver.
	spdlog::info(
		"device: {} ({}), {} timestamp bits at {} ns/tick, calibrates {}, counts fragments {}, exports "
		"timeline {}, copies from host {}, reports memory budget {}, DRM primary {} render {}",
		device.m_Description.Name.data(),
		device.m_Description.Driver.data(),
		device.m_Description.TimestampValidBits,
		device.m_Description.TimestampPeriod,
		device.m_Description.CalibratesTimestamps,
		device.m_Description.CountsPipelineStatistics,
		device.m_Description.ExportsTimeline,
		device.m_Description.CopiesFromHost,
		device.m_Description.ReportsMemoryBudget,
		device.m_Description.PrimaryMinor,
		device.m_Description.RenderMinor
	);

	const float priority = 1.0F;
	const VkDeviceQueueCreateInfo queueInfo{ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		                                     .pNext = nullptr,
		                                     .flags = 0,
		                                     .queueFamilyIndex = device.m_QueueFamily,
		                                     .queueCount = 1,
		                                     .pQueuePriorities = &priority };

	// Only what is used. A feature enabled and not used costs nothing on most drivers and is a
	// different pipeline cache key on some, which is the kind of difference that shows up as a first
	// frame that missed on one machine and not another.
	// Value-initialised and then assigned, rather than a designated-initializer list. These two
	// structures carry sixty-odd fields between them and the build's `-Wmissing-field-initializers`
	// would require every one of them named — which is the right rule for gyro's own aggregates,
	// where a forgotten field is a defect, and the wrong shape for a Vulkan feature struct whose
	// whole contract is that everything unmentioned is `VK_FALSE`.
	VkPhysicalDeviceVulkan13Features features13{};
	features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	features13.dynamicRendering = VK_TRUE;

	VkPhysicalDeviceVulkan12Features features12{};
	features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	features12.pNext = &features13;
	features12.timelineSemaphore = VK_TRUE;

	// The optional one. Asked for only where the capability query said the answer is yes, so that a
	// driver which advertises the extension and refuses the semaphore does not fail device creation
	// — decision 108's whole subject, and lavapipe's actual behaviour.
	std::array<const char*, RequiredExtensions.size() + 5> extensions{};
	std::ranges::copy(RequiredExtensions, extensions.begin());
	std::uint32_t extensionCount = RequiredExtensions.size();

	if (device.m_Description.ExportsTimeline)
	{
		extensions[extensionCount] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
		++extensionCount;
	}

	// The fourth optional one, and the one that used to be required. It carries no feature struct —
	// what it adds is a tiling an image can be created with and two queries about it — so being
	// listed is the whole of the capability, and `Describe` reads it that way. Where it is absent
	// every path takes linear; see `DeviceDescription::StatesModifiers`.
	if (device.m_Description.StatesModifiers)
	{
		extensions[extensionCount] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
		++extensionCount;
	}

	// The second optional one, and it is optional for a different reason than the first. Decision
	// 108's is optional because a driver advertises it and then refuses; this one is optional because
	// it is genuinely newer than the floor gyro runs on, and Render/Textures.h has a path that does
	// not need it. Both are enabled only where the query said yes, which is what keeps a device
	// creation from failing over a capability nothing has to have.
	VkPhysicalDeviceHostImageCopyFeatures hostCopy{};
	hostCopy.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES;

	if (device.m_Description.CopiesFromHost)
	{
		extensions[extensionCount] = VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME;
		++extensionCount;

		hostCopy.hostImageCopy = VK_TRUE;
		hostCopy.pNext = &features13;
		features12.pNext = &hostCopy;
	}

	// The third optional one, and the only one enabled for an instrument rather than for a picture.
	// Decision 139's tracing is what wants it and nothing else does, so a driver without it loses the
	// GPU track and keeps every frame.
	if (device.m_Description.CalibratesTimestamps)
	{
		extensions[extensionCount] = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
		++extensionCount;
	}

	// The fifth, and the second one enabled for an instrument rather than for a picture. It has to be
	// enabled at creation even though what it adds is a physical-device query: the budget structure is
	// only promised to be filled for a device that asked for the extension. A driver without it loses
	// the two memory counters on the GPU row and keeps everything else.
	if (device.m_Description.ReportsMemoryBudget)
	{
		extensions[extensionCount] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
		++extensionCount;
	}

	// Not an extension at all — a core feature, off by default, and the counter that says whether a
	// composite was expensive because of the device or because of how many times gyro drew over the
	// same pixel. Asked for only where the query said yes, for `RequiredExtensions`' reason: a device
	// creation that fails over an instrument is a machine with no renderer.
	VkPhysicalDeviceFeatures baseFeatures{};
	baseFeatures.pipelineStatisticsQuery = device.m_Description.CountsPipelineStatistics ? VK_TRUE : VK_FALSE;

	const VkDeviceCreateInfo deviceInfo{ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		                                 .pNext = &features12,
		                                 .flags = 0,
		                                 .queueCreateInfoCount = 1,
		                                 .pQueueCreateInfos = &queueInfo,
		                                 .enabledLayerCount = 0,
		                                 .ppEnabledLayerNames = nullptr,
		                                 .enabledExtensionCount = extensionCount,
		                                 .ppEnabledExtensionNames = extensions.data(),
		                                 .pEnabledFeatures = &baseFeatures };

	if (Result<void> created = Check(vkCreateDevice(chosen, &deviceInfo, nullptr, &device.m_Device), "vkCreateDevice");
	    !created)
	{
		return std::unexpected{ created.error() };
	}

	// The device's own entry points, replacing the loader's trampolines. Global rather than a
	// per-device table because gyro runs one device at a time by construction — device migration
	// destroys before it constructs — so a table would be a second copy of a fact the process
	// already has one of.
	volkLoadDevice(device.m_Device);

	vkGetDeviceQueue(device.m_Device, device.m_QueueFamily, 0, &device.m_Queue);

	// Carried out of the policy because every party that decides a target's usage — the modifier query
	// and both allocation arms — runs long after this call has returned.
	device.m_Readable = policy.Readable;

	// Decision 142's clock reader, resolved from the DRM node the device reported. It warns and comes up
	// invalid on a driver it does not cover rather than failing device creation — a machine whose GPU
	// clock gyro cannot read is one that draws every frame and files a zero operating point, not one
	// that comes up with no renderer.
	device.m_GpuClock = GpuClock::Open(device.m_Description.PrimaryMinor);

	return device;
}

void VulkanDevice::Reset() noexcept
{
	if (m_Device != VK_NULL_HANDLE)
	{
		vkDestroyDevice(m_Device, nullptr);
		m_Device = VK_NULL_HANDLE;
	}

	if (m_Instance != VK_NULL_HANDLE)
	{
		vkDestroyInstance(m_Instance, nullptr);
		m_Instance = VK_NULL_HANDLE;
	}

	m_Physical = VK_NULL_HANDLE;
	m_Queue = VK_NULL_HANDLE;
}

namespace
{
// The driver's own entry for one format under one modifier, or nothing where it lists neither.
//
// **`ModifierInvalid` is refused here rather than at each caller, and Seam/RenderTarget.h is why.**
// Invalid means the modifier is *unknown*, not that there is none — an implicitly-tiled legacy
// allocation carries it. An image laid out under a tiling nobody stated is one the driver and the
// allocator have each guessed at separately, and the two guesses agreeing is what makes it work on
// the machine it was written on. So gyro neither imports one nor allocates one.
//
// **One query, three callers**: whether a target can be drawn into, whether it can also be sampled,
// and which modifier an export should pick. They differ in the feature bit they read and in the plane
// count they will accept; none of them differs in how the list is fetched, and three copies of a
// sixty-four-entry query is three places for the cap to drift.
[[nodiscard]] bool ModifierEntry(
	VkPhysicalDevice physical,
	bool statesModifiers,
	PixelFormat format,
	VkDrmFormatModifierPropertiesEXT& into
) noexcept
{
	if (format.Modifier == ModifierInvalid)
	{
		return false;
	}

	const VkFormat vulkan = VulkanFormat(format.Code);

	if (vulkan == VK_FORMAT_UNDEFINED)
	{
		return false;
	}

	// **The device that cannot state a tiling knows exactly one, and it is answered from the format's
	// linear features rather than from the modifier list.** The list has to be *fabricated* here
	// rather than queried, and the reason is sharper than "the extension is absent": lavapipe fills a
	// `VkDrmFormatModifierPropertiesListEXT` chained onto a format query it does not implement —
	// `VulkanProbe` reported a linear entry off a driver whose extension list has no such name. So
	// the query would appear to work and would be a driver answering about a structure it never
	// agreed to. `linearTilingFeatures` is the same fact asked in a form 1.0 defines, and linear is
	// one plane by construction.
	if (!statesModifiers)
	{
		if (format.Modifier != ModifierLinear)
		{
			return false;
		}

		VkFormatProperties properties{};
		vkGetPhysicalDeviceFormatProperties(physical, vulkan, &properties);

		into = VkDrmFormatModifierPropertiesEXT{ .drmFormatModifier = ModifierLinear,
			                                     .drmFormatModifierPlaneCount = 1,
			                                     .drmFormatModifierTilingFeatures = properties.linearTilingFeatures };

		return true;
	}

	std::array<VkDrmFormatModifierPropertiesEXT, MaxModifiers> entries{};
	VkDrmFormatModifierPropertiesListEXT list{ .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
		                                       .pNext = nullptr,
		                                       .drmFormatModifierCount = MaxModifiers,
		                                       .pDrmFormatModifierProperties = entries.data() };
	VkFormatProperties2 properties{ .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		                            .pNext = &list,
		                            .formatProperties = {} };
	vkGetPhysicalDeviceFormatProperties2(physical, vulkan, &properties);

	const std::uint32_t written = std::min(list.drmFormatModifierCount, MaxModifiers);

	for (std::uint32_t index = 0; index < written; ++index)
	{
		if (entries[index].drmFormatModifier == format.Modifier)
		{
			into = entries[index];

			return true;
		}
	}

	return false;
}

// Single-plane only, and it is a real limit rather than a placeholder: every format a composite is
// recorded into is one plane, which Virtual/Buffer.h states from the allocating side. A multi-plane
// tiling of an RGB format is an auxiliary compression plane, and reading one back on the CPU is not
// something a target this module hands out supports yet.
//
// **The export path below holds to the same limit, which it did not always.** It used to accept
// anything a description could *say*, on the argument that refusing a modifier before the parent
// compositor had been asked about it was answering an unasked question. That was safe only while the
// caller chose — it walked the host's order and linear is one plane, so the wider door was never gone
// through. Decision 138 moved the choice to the driver, which naturally picks the best layout it has,
// and on Tiger Lake that is a Y-tiled CCS pair. The result was an output that allocated a ring
// nothing could draw into and reported *the targets were never bound* twenty seconds later. A target
// this device exports is a target this device is about to render into, so the two paths take the same
// answer from the same function.
[[nodiscard]] bool
Renderable(VkPhysicalDevice physical, bool statesModifiers, PixelFormat format, VkFormatFeatureFlags wanted) noexcept
{
	VkDrmFormatModifierPropertiesEXT entry{};

	if (!ModifierEntry(physical, statesModifiers, format, entry) || entry.drmFormatModifierPlaneCount != 1)
	{
		return false;
	}

	return (entry.drmFormatModifierTilingFeatures & wanted) == wanted;
}
} // namespace

bool VulkanDevice::SupportsSampling(PixelFormat format) const noexcept
{
	if (!IsValid() || !format.IsValid())
	{
		return false;
	}

	return Renderable(m_Physical, m_Description.StatesModifiers, format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
}

std::size_t VulkanDevice::SamplingModifiers(std::uint32_t code, std::span<std::uint64_t> into) const noexcept
{
	if (!IsValid() || code == 0 || into.empty())
	{
		return 0;
	}

	const VkFormat vulkan = VulkanFormat(code);

	if (vulkan == VK_FORMAT_UNDEFINED)
	{
		return 0;
	}

	// The device that cannot state a tiling knows exactly one, per `ModifierEntry` above — so the
	// answer is at most linear, asked through the same predicate rather than through a second reading
	// of the same properties.
	if (!m_Description.StatesModifiers)
	{
		if (!SupportsSampling(PixelFormat{ .Code = code, .Modifier = ModifierLinear }))
		{
			return 0;
		}

		into[0] = ModifierLinear;

		return 1;
	}

	std::array<VkDrmFormatModifierPropertiesEXT, MaxModifiers> entries{};
	VkDrmFormatModifierPropertiesListEXT list{ .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
		                                       .pNext = nullptr,
		                                       .drmFormatModifierCount = MaxModifiers,
		                                       .pDrmFormatModifierProperties = entries.data() };
	VkFormatProperties2 properties{ .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		                            .pNext = &list,
		                            .formatProperties = {} };
	vkGetPhysicalDeviceFormatProperties2(m_Physical, vulkan, &properties);

	const std::uint32_t written = std::min(list.drmFormatModifierCount, MaxModifiers);

	std::size_t taken = 0;

	for (std::uint32_t index = 0; index < written && taken < into.size(); ++index)
	{
		const VkDrmFormatModifierPropertiesEXT& entry = entries[index];

		// One plane, which is what `ImportImage` can describe and therefore the whole of what an import
		// through this device can honour. Offering a pair gyro would refuse at `attach` is worse than
		// not offering it: the client has already allocated by then.
		if (entry.drmFormatModifierPlaneCount != 1 || entry.drmFormatModifier == ModifierInvalid)
		{
			continue;
		}

		if ((entry.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0)
		{
			continue;
		}

		into[taken] = entry.drmFormatModifier;

		++taken;
	}

	return taken;
}

bool VulkanDevice::Supports(PixelFormat format) const noexcept
{
	if (!IsValid() || !format.IsValid())
	{
		return false;
	}

	// Drawn into, which is the usage that matters — a target this device can sample but not render to
	// is one `BindTargets` must refuse rather than discover at the first frame.
	return Renderable(m_Physical, m_Description.StatesModifiers, format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
}

bool VulkanDevice::SupportsHostTexture(PixelFormat format) const noexcept
{
	if (!IsValid() || !format.IsValid() || !m_Description.CopiesFromHost)
	{
		return false;
	}

	const VkFormat vulkan = VulkanFormat(format.Code);

	if (vulkan == VK_FORMAT_UNDEFINED)
	{
		return false;
	}

	// The `2` form rather than `vkGetPhysicalDeviceFormatProperties`, because the bit this is really
	// asking about lives past the thirty-two the older structure has room for —
	// `VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT` is bit 46. The sampling bit is readable either
	// way; asking both through one call is what keeps the two answers from being taken under
	// different queries.
	VkFormatProperties3 extended{ .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
		                          .pNext = nullptr,
		                          .linearTilingFeatures = 0,
		                          .optimalTilingFeatures = 0,
		                          .bufferFeatures = 0 };
	VkFormatProperties2 properties{ .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		                            .pNext = &extended,
		                            .formatProperties = {} };

	vkGetPhysicalDeviceFormatProperties2(m_Physical, vulkan, &properties);

	constexpr VkFormatFeatureFlags2 Wanted =
		VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT;

	return (extended.optimalTilingFeatures & Wanted) == Wanted;
}

std::uint32_t VulkanDevice::MemoryType(std::uint32_t allowed, VkMemoryPropertyFlags properties) const noexcept
{
	if (!IsValid())
	{
		return MemoryTypeNone;
	}

	VkPhysicalDeviceMemoryProperties memory{};
	vkGetPhysicalDeviceMemoryProperties(m_Physical, &memory);

	// First match wins, which is the ordinary answer and the one every driver orders usefully: the
	// spec requires the types to be sorted so that a more specific one never precedes a subset of
	// itself, so the first hit is the least surprising type carrying what was asked for.
	for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index)
	{
		if ((allowed & (1U << index)) == 0)
		{
			continue;
		}

		if ((memory.memoryTypes[index].propertyFlags & properties) == properties)
		{
			return index;
		}
	}

	return MemoryTypeNone;
}

std::uint32_t VulkanDevice::ImportableMemoryTypes(RawFd descriptor) const noexcept
{
	if (!IsValid() || !descriptor.IsValid())
	{
		return 0;
	}

	VkMemoryFdPropertiesKHR properties{ .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
		                                .pNext = nullptr,
		                                .memoryTypeBits = 0 };

	if (vkGetMemoryFdPropertiesKHR(
			m_Device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, descriptor.Value, &properties
		) != VK_SUCCESS)
	{
		return 0;
	}

	return properties.memoryTypeBits;
}

namespace
{
// What an exported target is created with, and the one conditional bit is the same conditional the
// import path already applies.
//
// `COLOR_ATTACHMENT` because this is a render target and nothing else. `TRANSFER_DST` and `SAMPLED`
// because the renderer that imports the descriptor back asks for exactly those — Render/Backdrop.h
// reads the composite it is drawing into — and an image allocated under a narrower usage than the one
// it will be imported under is a difference the driver is entitled to notice. `SAMPLED` only where
// the tiling lists it, for the reason the import states: demanding it unconditionally would refuse a
// compressed modifier that renders perfectly well, and what such a modifier costs is a blur rather
// than a screen.
[[nodiscard]] VkImageUsageFlags ExportUsage(VkFormatFeatureFlags features, bool readable) noexcept
{
	const bool samplable = (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;

	return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	       (samplable ? VkImageUsageFlags{ VK_IMAGE_USAGE_SAMPLED_BIT } : VkImageUsageFlags{ 0 }) |
	       (readable ? VkImageUsageFlags{ VK_IMAGE_USAGE_TRANSFER_SRC_BIT } : VkImageUsageFlags{ 0 });
}

// Whether the driver will create an *exportable* image with this modifier, usage and extent.
//
// **A separate question from the tiling's feature bits, and not implied by them.** The modifier list
// says what a tiling can do once an image exists; this says whether one can be created at all at this
// size and whether a descriptor can be got out of it afterwards. A driver that renders into a
// modifier and refuses to export it would otherwise be found at `vkGetMemoryFdKHR`, after the
// allocation, with a half-built target to unwind.
[[nodiscard]] bool CanExport(
	VkPhysicalDevice physical,
	bool statesModifiers,
	VkFormat vulkan,
	PixelSize<DeviceSpace> size,
	VkImageUsageFlags usage,
	std::uint64_t modifier
) noexcept
{
	const VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierInfo{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
		.pNext = nullptr,
		.drmFormatModifier = modifier,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = nullptr
	};

	// The modifier half of the question drops out where there are no modifiers, and what is left is
	// still the question worth asking: whether a descriptor can be got out of an image of this size
	// at all. `ModifierEntry` has already refused anything but linear on that device, so the tiling
	// below matches the one the allocation will use.
	const VkPhysicalDeviceExternalImageFormatInfo externalInfo{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
		.pNext = statesModifiers ? &modifierInfo : nullptr,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
	};
	const VkPhysicalDeviceImageFormatInfo2 info{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		                                         .pNext = &externalInfo,
		                                         .format = vulkan,
		                                         .type = VK_IMAGE_TYPE_2D,
		                                         .tiling = statesModifiers ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT :
		                                                                     VK_IMAGE_TILING_LINEAR,
		                                         .usage = usage,
		                                         .flags = 0 };

	VkExternalImageFormatProperties external{ .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
		                                      .pNext = nullptr,
		                                      .externalMemoryProperties = {} };
	VkImageFormatProperties2 properties{ .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
		                                 .pNext = &external,
		                                 .imageFormatProperties = {} };

	if (vkGetPhysicalDeviceImageFormatProperties2(physical, &info, &properties) != VK_SUCCESS)
	{
		return false;
	}

	if ((external.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0)
	{
		return false;
	}

	return static_cast<std::uint32_t>(size.Width) <= properties.imageFormatProperties.maxExtent.width &&
	       static_cast<std::uint32_t>(size.Height) <= properties.imageFormatProperties.maxExtent.height;
}

// The memory plane aspects, in order. A modifier's planes are named by these rather than by
// `COLOR_BIT`, which is what `vkGetImageSubresourceLayout` requires of a modifier-tiled image and the
// one thing about reading a layout back that is easy to get wrong — `COLOR_BIT` is accepted by some
// drivers and answers about the *format's* planes rather than the *memory's*.
constexpr std::array<VkImageAspectFlags, MaxImagePlanes> MemoryPlanes{
	VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
	VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
	VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
	VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
};

// The modifier that survived every veto, and the two things about it the allocation then needs.
struct Candidate
{
	std::uint64_t Modifier = ModifierInvalid;
	std::uint32_t PlaneCount = 0;
	VkImageUsageFlags Usage = 0;
};
} // namespace

bool VulkanDevice::Exports(PixelFormat format) const noexcept
{
	if (!IsValid() || !format.IsValid())
	{
		return false;
	}

	const VkFormat vulkan = VulkanFormat(format.Code);

	if (vulkan == VK_FORMAT_UNDEFINED)
	{
		return false;
	}

	// The same usage an export is created with, because a driver is entitled to refuse a handle type
	// for one usage and allow it for another — and a capability answered under a narrower usage than
	// the allocation will ask for is the reassurance that fails at the allocation.
	constexpr VkFormatFeatureFlags Wanted =
		VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

	return CanExport(
		m_Physical,
		m_Description.StatesModifiers,
		vulkan,
		PixelSize<DeviceSpace>{ 1, 1 },
		ExportUsage(Wanted, m_Readable),
		format.Modifier
	);
}

void ExportedImage::Reset() noexcept
{
	if (m_Device != VK_NULL_HANDLE)
	{
		if (m_Image != VK_NULL_HANDLE)
		{
			vkDestroyImage(m_Device, m_Image, nullptr);
		}

		if (m_Memory != VK_NULL_HANDLE)
		{
			vkFreeMemory(m_Device, m_Memory, nullptr);
		}
	}

	m_Device = VK_NULL_HANDLE;
	m_Image = VK_NULL_HANDLE;
	m_Memory = VK_NULL_HANDLE;

	// Closed last and closed here, which is what makes this the owner the header says it is. The
	// buffer itself is not destroyed by any of this: a dmabuf is reference-counted by the kernel, so a
	// consumer that duplicated the descriptor still holds the pages after the `VkDeviceMemory` is
	// gone. What ends is gyro's own ability to draw into it.
	m_Descriptor.Reset();
	m_Target = RenderTarget{};
}

void ExportedTimeline::Reset() noexcept
{
	if (m_Device != VK_NULL_HANDLE && m_Semaphore != VK_NULL_HANDLE)
	{
		vkDestroySemaphore(m_Device, m_Semaphore, nullptr);
	}

	m_Device = VK_NULL_HANDLE;
	m_Semaphore = VK_NULL_HANDLE;
	m_Descriptor.Reset();
}

Result<std::uint64_t> ExportedTimeline::Counter() const noexcept
{
	if (m_Semaphore == VK_NULL_HANDLE)
	{
		return Failure(EINVAL, "reading a timeline that was never created");
	}

	std::uint64_t value = 0;

	if (Result<void> read =
	        Check(vkGetSemaphoreCounterValue(m_Device, m_Semaphore, &value), "vkGetSemaphoreCounterValue");
	    !read)
	{
		return std::unexpected{ read.error() };
	}

	return value;
}

Result<ExportedImage>
VulkanDevice::Export(PixelSize<DeviceSpace> size, std::uint32_t code, std::span<const std::uint64_t> modifiers) const
{
	if (!IsValid())
	{
		return Failure(ENODEV, "no Vulkan device to allocate an exported target from");
	}

	if (size.IsEmpty())
	{
		return Failure(EINVAL, "an exported target needs an extent");
	}

	const VkFormat vulkan = VulkanFormat(code);

	if (vulkan == VK_FORMAT_UNDEFINED)
	{
		return Failure(EINVAL, "that fourcc is not one a composite is recorded into");
	}

	// **Every candidate the device will take, not the first one.** Decision 138: the caller's order is
	// the parent compositor's import ranking and says nothing about what this GPU renders into
	// quickly, so keeping the whole survivor set is what lets the driver answer that question below.
	std::array<std::uint64_t, MaxModifiers> accepted{};
	std::uint32_t count = 0;

	// **The usage the whole set has to satisfy, and it is the maximum rather than the intersection.**
	// One image is created, so one usage is asked for, and a modifier that cannot carry it has to be
	// dropped rather than quietly lowering what the others are created with — a target that lost
	// `SAMPLED` because some entry further down the host's table could not sample is a backdrop that
	// stops being readable on a machine where every modifier gyro would actually pick was fine.
	constexpr VkFormatFeatureFlags Wanted =
		VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	const VkImageUsageFlags usage = ExportUsage(Wanted, m_Readable);

	for (const std::uint64_t modifier : modifiers)
	{
		if (count == MaxModifiers)
		{
			break;
		}

		VkDrmFormatModifierPropertiesEXT entry{};

		// `ModifierEntry` is where `ModifierInvalid` is refused, so a list carrying it loses that one
		// candidate rather than the whole call — which is what a parent compositor advertising a
		// legacy entry beside real ones deserves.
		if (!ModifierEntry(m_Physical, m_Description.StatesModifiers, PixelFormat{ code, 0, modifier }, entry))
		{
			continue;
		}

		// `Renderable` is the whole filter for the tiling itself: one plane, and every feature the usage
		// asks for. See its header for why the export path is held to the importer's limit rather than
		// to the wider one a description could carry.
		if (!Renderable(m_Physical, m_Description.StatesModifiers, PixelFormat{ code, 0, modifier }, Wanted))
		{
			continue;
		}

		if (!CanExport(m_Physical, m_Description.StatesModifiers, vulkan, size, usage, modifier))
		{
			continue;
		}

		accepted[count] = modifier;
		++count;
	}

	if (count == 0)
	{
		return Failure(EINVAL, "no offered modifier is one this device will allocate an exportable target under");
	}

	// **The whole survivor set rather than the explicit layout the import path states, and the
	// direction is the difference.** An import knows the offsets and strides the allocator already
	// committed to and has to say them; an allocation is the moment those are *decided*, so the driver
	// picks among these and lays the image out, and both answers are read back below.
	const VkImageDrmFormatModifierListCreateInfoEXT modifierInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
		.pNext = nullptr,
		.drmFormatModifierCount = count,
		.pDrmFormatModifiers = accepted.data()
	};

	// Nothing to offer where a tiling cannot be named: the survivor set is linear or it is empty, and
	// `VK_IMAGE_TILING_LINEAR` says the same thing in the vocabulary such a device has.
	const VkExternalMemoryImageCreateInfo externalInfo{ .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		                                                .pNext =
		                                                    m_Description.StatesModifiers ? &modifierInfo : nullptr,
		                                                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
	const VkImageCreateInfo imageInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &externalInfo,
		.flags = 0,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = vulkan,
		.extent = { static_cast<std::uint32_t>(size.Width), static_cast<std::uint32_t>(size.Height), 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = m_Description.StatesModifiers ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = nullptr,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
	};

	// Built in place for `Open`'s reason: every early return below runs the destructor, so a failure
	// after the image exists and before the descriptor does frees the image rather than stranding it.
	ExportedImage exported;
	exported.m_Device = m_Device;

	if (Result<void> created = Check(vkCreateImage(m_Device, &imageInfo, nullptr, &exported.m_Image), "vkCreateImage");
	    !created)
	{
		return std::unexpected{ created.error() };
	}

	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(m_Device, exported.m_Image, &requirements);

	VkPhysicalDeviceMemoryProperties properties{};
	vkGetPhysicalDeviceMemoryProperties(m_Physical, &properties);

	// Device-local where the image allows it, because a target is scanned out or sampled by hardware
	// and never read by this process. Falling back to the first allowed type rather than failing is
	// what keeps the floor tier working: lavapipe's memory is host memory, and a device-local
	// requirement there would refuse every allocation on the one device that is always present.
	std::uint32_t type = properties.memoryTypeCount;
	std::uint32_t fallback = properties.memoryTypeCount;

	for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((requirements.memoryTypeBits & (1U << index)) == 0)
		{
			continue;
		}

		if (fallback == properties.memoryTypeCount)
		{
			fallback = index;
		}

		if ((properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)
		{
			type = index;

			break;
		}
	}

	if (type == properties.memoryTypeCount)
	{
		type = fallback;
	}

	if (type == properties.memoryTypeCount)
	{
		return Failure(ENOMEM, "no memory type satisfies an exported target");
	}

	const VkExportMemoryAllocateInfo exportInfo{ .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
		                                         .pNext = nullptr,
		                                         .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };

	// Dedicated for the import path's reason read the other way round: the descriptor names the whole
	// allocation, so an allocation holding two images would hand out a buffer with somebody else's
	// pixels in it. A driver that was not told so may also simply refuse the export.
	const VkMemoryDedicatedAllocateInfo dedicatedInfo{ .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		                                               .pNext = &exportInfo,
		                                               .image = exported.m_Image,
		                                               .buffer = VK_NULL_HANDLE };
	const VkMemoryAllocateInfo allocateInfo{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		                                     .pNext = &dedicatedInfo,
		                                     .allocationSize = requirements.size,
		                                     .memoryTypeIndex = type };

	if (Result<void> allocated =
	        Check(vkAllocateMemory(m_Device, &allocateInfo, nullptr, &exported.m_Memory), "vkAllocateMemory");
	    !allocated)
	{
		return std::unexpected{ allocated.error() };
	}

	if (Result<void> bound =
	        Check(vkBindImageMemory(m_Device, exported.m_Image, exported.m_Memory, 0), "vkBindImageMemory");
	    !bound)
	{
		return std::unexpected{ bound.error() };
	}

	// **Which of them the driver chose, and this is now the answer rather than a formality.** It is
	// what goes into the description, so a nested output tells the parent what it has rather than what
	// it asked for — and it is the only thing that makes handing over a set safe, because the set is
	// exactly what a parent must not be left guessing among.
	// **Linear is asserted rather than read back, and it is the one place in this function that is
	// allowed to assert.** There is no query to ask a device that does not implement the extension,
	// and no ambiguity for it to resolve: the image was created `VK_IMAGE_TILING_LINEAR`, the
	// candidate loop admitted no other modifier, and a linear image is one plane. What the driver
	// still gets to decide is the *pitch*, and that is read below exactly as it is on the other arm.
	Candidate chosen{ .Modifier = ModifierLinear, .PlaneCount = 1, .Usage = usage };

	if (m_Description.StatesModifiers)
	{
		VkImageDrmFormatModifierPropertiesEXT actual{ .sType =
			                                              VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
			                                          .pNext = nullptr,
			                                          .drmFormatModifier = ModifierInvalid };

		if (Result<void> read = Check(
				vkGetImageDrmFormatModifierPropertiesEXT(m_Device, exported.m_Image, &actual),
				"vkGetImageDrmFormatModifierPropertiesEXT"
			);
		    !read)
		{
			return std::unexpected{ read.error() };
		}

		// Still checked, and against the set rather than against one entry. A driver that answered with
		// a modifier nobody offered has laid the image out a way the parent will not import, and the
		// failure that produces is a window of stripes three seconds later rather than an error here.
		if (std::ranges::find(accepted.begin(), accepted.begin() + count, actual.drmFormatModifier) ==
		    accepted.begin() + count)
		{
			return Failure(EIO, "the driver laid the image out under a modifier it was not offered");
		}

		chosen.Modifier = actual.drmFormatModifier;
		chosen.PlaneCount = 0;

		VkDrmFormatModifierPropertiesEXT described{};

		// The plane count belongs to the modifier the driver settled on, so it is looked up here rather
		// than carried down from the candidate loop — where, now that the loop keeps several, there is
		// no single one to carry.
		if (!ModifierEntry(m_Physical, true, PixelFormat{ code, 0, chosen.Modifier }, described))
		{
			return Failure(EIO, "the driver laid the image out under a modifier it does not describe");
		}

		chosen.PlaneCount = described.drmFormatModifierPlaneCount;

		if (chosen.PlaneCount == 0 || chosen.PlaneCount > MaxImagePlanes)
		{
			return Failure(EIO, "the chosen modifier has a plane count a description cannot carry");
		}
	}

	const VkMemoryGetFdInfoKHR getInfo{ .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
		                                .pNext = nullptr,
		                                .memory = exported.m_Memory,
		                                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
	int descriptor = InvalidFd;

	if (Result<void> got = Check(vkGetMemoryFdKHR(m_Device, &getInfo, &descriptor), "vkGetMemoryFdKHR"); !got)
	{
		return std::unexpected{ got.error() };
	}

	// Owned from here, so every return below frees it. `vkGetMemoryFdKHR` hands ownership to the
	// caller — the opposite of the import, where `vkAllocateMemory` takes it — which is the asymmetry
	// most easily got wrong in a pair of paths that otherwise mirror each other.
	exported.m_Descriptor = Fd{ descriptor };

	DmabufImage image{};
	image.PlaneCount = chosen.PlaneCount;

	for (std::uint32_t plane = 0; plane < chosen.PlaneCount; ++plane)
	{
		// `COLOR_BIT` where there is no modifier, and the substitution is required rather than
		// cosmetic: `VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT` is only a legal aspect for an image whose
		// tiling *is* a modifier, so asking a linear image about it is the invalid usage the header
		// above warns about, read from the other side.
		const VkImageSubresource subresource{ .aspectMask = m_Description.StatesModifiers ?
			                                                    MemoryPlanes[plane] :
			                                                    VkImageAspectFlags{ VK_IMAGE_ASPECT_COLOR_BIT },
			                                  .mipLevel = 0,
			                                  .arrayLayer = 0 };
		VkSubresourceLayout layout{};
		vkGetImageSubresourceLayout(m_Device, exported.m_Image, &subresource, &layout);

		if (layout.offset > std::numeric_limits<std::uint32_t>::max() ||
		    layout.rowPitch > std::numeric_limits<std::uint32_t>::max())
		{
			return Failure(EINVAL, "a plane's offset or stride is past what a description can carry");
		}

		// **Every plane names the same descriptor, and that is correct rather than a shortcut.** The
		// allocation is one `VkDeviceMemory` and therefore one dmabuf; a modifier's extra planes are
		// offsets into it, not separate buffers. Only a disjoint image would have several, and this
		// does not ask for one — a compression plane belongs beside the pixels it describes.
		image.Planes[plane] = { .Descriptor = exported.m_Descriptor.Borrow(),
			                    .Offset = static_cast<std::uint32_t>(layout.offset),
			                    .Stride = static_cast<std::uint32_t>(layout.rowPitch) };
	}

	exported.m_Target =
		RenderTarget{ .Size = size, .Format = PixelFormat{ code, 0, chosen.Modifier }, .Memory = image };

	return exported;
}

Result<VkImage>
VulkanDevice::ImportImage(VkExtent2D size, PixelFormat format, const DmabufPlane& plane, VkImageUsageFlags usage) const
{
	if (!IsValid())
	{
		return Failure(ENODEV, "no Vulkan device to import an image onto");
	}

	const VkFormat vulkan = VulkanFormat(format.Code);

	if (vulkan == VK_FORMAT_UNDEFINED)
	{
		return Failure(EINVAL, "that fourcc is not one this device names");
	}

	// **The offset has nowhere to go on the linear arm, so a non-zero one is refused rather than
	// dropped.** With a modifier it travels in `pPlaneLayouts`; without one the only place to put it
	// is `vkBindImageMemory`'s own offset, which the driver constrains by an alignment the caller
	// knows nothing about — and silently binding at zero is the whole buffer read one plane early.
	// Every allocation gyro makes for itself is dedicated and therefore at zero, so this is a door
	// that only a foreign allocator can arrive at.
	if (!m_Description.StatesModifiers && plane.Offset != 0)
	{
		return Failure(EINVAL, "this device imports a linear buffer only at the start of its allocation");
	}

	const VkSubresourceLayout stated{
		.offset = plane.Offset, .size = 0, .rowPitch = plane.Stride, .arrayPitch = 0, .depthPitch = 0
	};

	// **Explicit rather than a modifier list, and the difference matters.** A list asks the driver to
	// pick a tiling and lay the image out itself; explicit states the layout the *allocator* already
	// committed to, which is the only correct thing to do for memory this device did not allocate.
	// Getting this backwards produces an image that imports cleanly and reads at the wrong stride.
	const VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
		.pNext = nullptr,
		.drmFormatModifier = format.Modifier,
		.drmFormatModifierPlaneCount = 1,
		.pPlaneLayouts = &stated
	};
	const VkExternalMemoryImageCreateInfo externalInfo{ .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		                                                .pNext =
		                                                    m_Description.StatesModifiers ? &modifierInfo : nullptr,
		                                                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
	const VkImageCreateInfo imageInfo{ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		                               .pNext = &externalInfo,
		                               .flags = 0,
		                               .imageType = VK_IMAGE_TYPE_2D,
		                               .format = vulkan,
		                               .extent = { size.width, size.height, 1 },
		                               .mipLevels = 1,
		                               .arrayLayers = 1,
		                               .samples = VK_SAMPLE_COUNT_1_BIT,
		                               .tiling = m_Description.StatesModifiers ?
		                                             VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT :
		                                             VK_IMAGE_TILING_LINEAR,
		                               .usage = usage,
		                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		                               .queueFamilyIndexCount = 0,
		                               .pQueueFamilyIndices = nullptr,
		                               .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

	VkImage image = VK_NULL_HANDLE;

	if (Result<void> created = Check(vkCreateImage(m_Device, &imageInfo, nullptr, &image), "vkCreateImage"); !created)
	{
		return std::unexpected{ created.error() };
	}

	// **Read back and compared, because on the linear arm the layout was never stated.** The driver
	// laid this image out to suit itself, and the caller's stride is a fact about somebody else's
	// allocation; the two agreeing is the ordinary case and the case where they do not is a window
	// sheared across the screen with nothing in the log. Cheap to ask, and the answer is a named
	// refusal a person can act on.
	if (!m_Description.StatesModifiers)
	{
		const VkImageSubresource subresource{ .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .arrayLayer = 0 };
		VkSubresourceLayout actual{};
		vkGetImageSubresourceLayout(m_Device, image, &subresource, &actual);

		if (actual.offset != stated.offset || actual.rowPitch != stated.rowPitch)
		{
			vkDestroyImage(m_Device, image, nullptr);

			return Failure(EINVAL, "this device lays a linear image out at a stride the buffer was not written at");
		}
	}

	return image;
}

GpuMemory VulkanDevice::ReadMemory() const noexcept
{
	if (!IsValid() || !m_Description.ReportsMemoryBudget)
	{
		return {};
	}

	// Value-initialised and chained rather than brace-initialised whole, for the reason the feature
	// structs above are: the budget structure carries two thirty-two element arrays the build's
	// `-Wmissing-field-initializers` would have this name one by one.
	VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
	budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;

	VkPhysicalDeviceMemoryProperties2 properties{};
	properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
	properties.pNext = &budget;

	// The physical device rather than the logical one, which is why this is `const` and why it is cheap
	// enough to sit on the frame path at all: no queue, no submission, and on every driver measured a
	// read of numbers the kernel already keeps.
	vkGetPhysicalDeviceMemoryProperties2(m_Physical, &properties);

	return DeviceLocalMemory(properties.memoryProperties, budget);
}

Result<GpuCalibration> VulkanDevice::Calibrate() const
{
	if (!IsValid() || !m_Description.CalibratesTimestamps)
	{
		return Failure(ENOTSUP, "this device will not read its clock and the host's together");
	}

	// Device first, so that the pair comes back in the order the reader converts them in. The driver
	// samples both inside one window and reports its width as `maxDeviation`; nothing here reads that,
	// because the only thing to do with a deviation larger than expected is take the reading anyway —
	// a GPU track placed to within a driver's own sampling window is the best there is.
	const std::array<VkCalibratedTimestampInfoEXT, 2> wanted{
		VkCalibratedTimestampInfoEXT{ .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
		                              .pNext = nullptr,
		                              .timeDomain = VK_TIME_DOMAIN_DEVICE_EXT },
		VkCalibratedTimestampInfoEXT{ .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
		                              .pNext = nullptr,
		                              .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT }
	};

	std::array<std::uint64_t, 2> stamps{};
	std::uint64_t deviation = 0;

	if (Result<void> read = Check(
			vkGetCalibratedTimestampsEXT(m_Device, 2, wanted.data(), stamps.data(), &deviation),
			"vkGetCalibratedTimestampsEXT"
		);
	    !read)
	{
		return std::unexpected{ read.error() };
	}

	return GpuCalibration{ .Host = Monotonic::FromNanoseconds(static_cast<std::int64_t>(stamps[1])),
		                   .Device = stamps[0] };
}

Result<ExportedTimeline> VulkanDevice::ExportTimeline() const
{
	if (!IsValid())
	{
		return Failure(ENODEV, "no Vulkan device to create a timeline on");
	}

	// Reported rather than attempted. Where the capability query said no, `Open` did not enable
	// `VK_KHR_external_semaphore_fd` at all, so `vkGetSemaphoreFdKHR` below is an entry point this
	// device was never asked for — and volk would have it pointing at nothing.
	if (!m_Description.ExportsTimeline)
	{
		return Failure(ENOTSUP, "this device creates no exportable timeline semaphore");
	}

	ExportedTimeline timeline;
	timeline.m_Device = m_Device;

	const VkExportSemaphoreCreateInfo exportInfo{ .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
		                                          .pNext = nullptr,
		                                          .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
	const VkSemaphoreTypeCreateInfo typeInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		                                      .pNext = &exportInfo,
		                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
		                                      .initialValue = 0 };
	const VkSemaphoreCreateInfo semaphoreInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		                                       .pNext = &typeInfo,
		                                       .flags = 0 };

	if (Result<void> created =
	        Check(vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &timeline.m_Semaphore), "vkCreateSemaphore");
	    !created)
	{
		return std::unexpected{ created.error() };
	}

	const VkSemaphoreGetFdInfoKHR getInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
		                                   .pNext = nullptr,
		                                   .semaphore = timeline.m_Semaphore,
		                                   .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
	int descriptor = InvalidFd;

	if (Result<void> exported = Check(vkGetSemaphoreFdKHR(m_Device, &getInfo, &descriptor), "vkGetSemaphoreFdKHR");
	    !exported)
	{
		return std::unexpected{ exported.error() };
	}

	timeline.m_Descriptor = Fd{ descriptor };

	return timeline;
}
