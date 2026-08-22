#include "Render/Device.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
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
// capability query rather than the extension list. See decision 104.
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

// Whether a timeline semaphore on this device can be exported as a descriptor. Decision 104's
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

void Describe(VkPhysicalDevice device, DeviceDescription& into) noexcept
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
	Describe(chosen, device.m_Description);

	if (!FindGraphicsQueue(chosen, device.m_QueueFamily))
	{
		return Failure(ENODEV, "the chosen Vulkan device has no graphics queue");
	}

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
	// — decision 104's whole subject, and lavapipe's actual behaviour.
	std::array<const char*, RequiredExtensions.size() + 1> extensions{};
	std::ranges::copy(RequiredExtensions, extensions.begin());
	std::uint32_t extensionCount = RequiredExtensions.size();

	if (device.m_Description.ExportsTimeline)
	{
		extensions[extensionCount] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
		++extensionCount;
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

bool VulkanDevice::Supports(PixelFormat format) const noexcept
{
	if (!IsValid() || !format.IsValid())
	{
		return false;
	}

	// **`ModifierInvalid` is refused, and Seam/RenderTarget.h is why.** Invalid means the modifier is
	// *unknown*, not that there is none — an implicitly-tiled legacy allocation carries it. An image
	// imported under a layout nobody stated is an image whose tiling the driver and the allocator
	// have each guessed at separately, and the two guesses agreeing is what makes it work on the
	// machine it was written on. A target whose modifier is unknown is one gyro declines to draw
	// into rather than one it draws into wrong.
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
	vkGetPhysicalDeviceFormatProperties2(m_Physical, vulkan, &properties);

	const std::uint32_t written = std::min(list.drmFormatModifierCount, MaxModifiers);

	for (std::uint32_t index = 0; index < written; ++index)
	{
		if (entries[index].drmFormatModifier != format.Modifier)
		{
			continue;
		}

		// Single-plane only, and it is a real limit rather than a placeholder: every format a
		// composite is recorded into is one plane, which Virtual/Buffer.h states from the allocating
		// side. A multi-plane tiling of an RGB format is an auxiliary compression plane, and reading
		// one back on the CPU is not something a target this module hands out supports yet.
		if (entries[index].drmFormatModifierPlaneCount != 1)
		{
			continue;
		}

		// Drawn into, which is the usage that matters — a target this device can sample but not
		// render to is one `BindTargets` must refuse rather than discover at the first frame.
		return (entries[index].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
	}

	return false;
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
