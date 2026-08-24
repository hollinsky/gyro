#include "Render/Device.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>
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
constexpr std::array<const char*, 4> RequiredExtensions{
	VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
	VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
	VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
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

// Whether this device will write host memory into a tiled image with no queue involved.
//
// **Both halves are asked, and the extension alone is not one of them.** `VK_EXT_host_image_copy`
// can be present with `hostImageCopy` false — the extension says the entry points exist, the feature
// says the driver implements them — and enabling an extension whose feature is false is a device
// creation that fails, which on this path is a machine that comes up with no renderer at all. So it
// is asked the way decision 108 taught: the capability query, not the extension list.
[[nodiscard]] bool QueryHostImageCopy(VkPhysicalDevice device) noexcept
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

	const bool listed = std::ranges::any_of(available, [](const VkExtensionProperties& entry) noexcept {
		return std::string_view{ entry.extensionName } == VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME;
	});

	if (!listed)
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
	into.TimestampPeriod = properties.properties.limits.timestampPeriod;
	into.TimestampValidBits = QueryTimestampBits(device, family);
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
	int best = 0;

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

		const int rank = Rank(properties.deviceType);

		if (chosen == VK_NULL_HANDLE || rank < best)
		{
			chosen = candidates[index];
			best = rank;
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
	std::array<const char*, RequiredExtensions.size() + 2> extensions{};
	std::ranges::copy(RequiredExtensions, extensions.begin());
	std::uint32_t extensionCount = RequiredExtensions.size();

	if (device.m_Description.ExportsTimeline)
	{
		extensions[extensionCount] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
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

	const VkDeviceCreateInfo deviceInfo{ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		                                 .pNext = &features12,
		                                 .flags = 0,
		                                 .queueCreateInfoCount = 1,
		                                 .pQueueCreateInfos = &queueInfo,
		                                 .enabledLayerCount = 0,
		                                 .ppEnabledLayerNames = nullptr,
		                                 .enabledExtensionCount = extensionCount,
		                                 .ppEnabledExtensionNames = extensions.data(),
		                                 .pEnabledFeatures = nullptr };

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
[[nodiscard]] bool
ModifierEntry(VkPhysicalDevice physical, PixelFormat format, VkDrmFormatModifierPropertiesEXT& into) noexcept
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
[[nodiscard]] bool Renderable(VkPhysicalDevice physical, PixelFormat format, VkFormatFeatureFlags wanted) noexcept
{
	VkDrmFormatModifierPropertiesEXT entry{};

	if (!ModifierEntry(physical, format, entry) || entry.drmFormatModifierPlaneCount != 1)
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

	return Renderable(m_Physical, format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
}

bool VulkanDevice::Supports(PixelFormat format) const noexcept
{
	if (!IsValid() || !format.IsValid())
	{
		return false;
	}

	// Drawn into, which is the usage that matters — a target this device can sample but not render to
	// is one `BindTargets` must refuse rather than discover at the first frame.
	return Renderable(m_Physical, format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
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
[[nodiscard]] VkImageUsageFlags ExportUsage(VkFormatFeatureFlags features) noexcept
{
	const bool samplable = (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;

	return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	       (samplable ? VkImageUsageFlags{ VK_IMAGE_USAGE_SAMPLED_BIT } : VkImageUsageFlags{ 0 });
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
	const VkPhysicalDeviceExternalImageFormatInfo externalInfo{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
		.pNext = &modifierInfo,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
	};
	const VkPhysicalDeviceImageFormatInfo2 info{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		                                         .pNext = &externalInfo,
		                                         .format = vulkan,
		                                         .type = VK_IMAGE_TYPE_2D,
		                                         .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
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
	const VkImageUsageFlags usage = ExportUsage(Wanted);

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
		if (!ModifierEntry(m_Physical, PixelFormat{ code, 0, modifier }, entry))
		{
			continue;
		}

		// `Renderable` is the whole filter for the tiling itself: one plane, and every feature the usage
		// asks for. See its header for why the export path is held to the importer's limit rather than
		// to the wider one a description could carry.
		if (!Renderable(m_Physical, PixelFormat{ code, 0, modifier }, Wanted))
		{
			continue;
		}

		if (!CanExport(m_Physical, vulkan, size, usage, modifier))
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
	const VkExternalMemoryImageCreateInfo externalInfo{ .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		                                                .pNext = &modifierInfo,
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
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
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
	VkImageDrmFormatModifierPropertiesEXT actual{ .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
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

	// Still checked, and against the set rather than against one entry. A driver that answered with a
	// modifier nobody offered has laid the image out a way the parent will not import, and the failure
	// that produces is a window of stripes three seconds later rather than an error here.
	if (std::ranges::find(accepted.begin(), accepted.begin() + count, actual.drmFormatModifier) ==
	    accepted.begin() + count)
	{
		return Failure(EIO, "the driver laid the image out under a modifier it was not offered");
	}

	Candidate chosen{ .Modifier = actual.drmFormatModifier, .PlaneCount = 0, .Usage = usage };
	VkDrmFormatModifierPropertiesEXT described{};

	// The plane count belongs to the modifier the driver settled on, so it is looked up here rather
	// than carried down from the candidate loop — where, now that the loop keeps several, there is no
	// single one to carry.
	if (!ModifierEntry(m_Physical, PixelFormat{ code, 0, chosen.Modifier }, described))
	{
		return Failure(EIO, "the driver laid the image out under a modifier it does not describe");
	}

	chosen.PlaneCount = described.drmFormatModifierPlaneCount;

	if (chosen.PlaneCount == 0 || chosen.PlaneCount > MaxImagePlanes)
	{
		return Failure(EIO, "the chosen modifier has a plane count a description cannot carry");
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
		const VkImageSubresource subresource{ .aspectMask = MemoryPlanes[plane], .mipLevel = 0, .arrayLayer = 0 };
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
