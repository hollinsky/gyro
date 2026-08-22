// What Vulkan this machine actually has, asked of the drivers rather than read off a document.
//
// Standalone, and the same brief as UringProbe.cpp: it runs on a target machine before gyro does,
// and on a machine gyro would not start on at all. It links the Vulkan *headers* and nothing else —
// the loader is `dlopen`ed exactly as volk does it — so it builds where there is no `libvulkan.so`
// symlink and no `vulkan.pc`, and it runs where there is no ICD and says so.
//
// **It exists because Docs/Decisions.md decision 40 rested for a year on an unverified claim.** That
// entry names four extensions lavapipe advertises and calls them *the complete set gyro asks of a
// device*. The claim held. What did not hold is the sentence one file over — Seam/SyncPoint.h's *DRM
// syncobj timelines throughout, exported from Vulkan timeline semaphores* — because lavapipe cannot
// create an exportable semaphore of any kind, which is decision 104. Both of those were found by
// running this, and the triage rule the log already carries is the reason: an entry that names the
// source its argument rests on can be retired by an afternoon of reading.
//
// Build and run:
//
//     ninja -C build VulkanProbe && ./build/VulkanProbe
//
// `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json` restricts it to lavapipe, which is
// the thing to do on a workstation that also has a GPU: the floor tier is the one whose answers gyro
// depends on, and it is the one a developer's machine will not exercise by default.

#include <dlfcn.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
void* g_Library = nullptr;
PFN_vkGetInstanceProcAddr g_GetInstanceProcAddr = nullptr;

template<typename Function>
[[nodiscard]] Function Load(VkInstance instance, const char* name)
{
	return reinterpret_cast<Function>(g_GetInstanceProcAddr(instance, name));
}

// The four decision 40 names, plus the ones the renderer turned out to want. Each line is a claim
// somewhere in the docs, printed so that a machine can disagree with it out loud.
constexpr const char* Interesting[] = {
	"VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier", "VK_KHR_external_memory_fd",
	"VK_KHR_timeline_semaphore",      "VK_KHR_external_semaphore_fd",     "VK_EXT_queue_family_foreign",
	"VK_KHR_dynamic_rendering",       "VK_EXT_host_image_copy",           "VK_KHR_calibrated_timestamps",
};

[[nodiscard]] const char* TypeName(VkPhysicalDeviceType type)
{
	switch (type)
	{
		case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
			return "discrete";
		case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
			return "integrated";
		case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
			return "virtual";
		case VK_PHYSICAL_DEVICE_TYPE_CPU:
			return "software";
		default:
			return "other";
	}
}

// Whether a semaphore of this kind can be exported, asked two ways.
//
// **The capability query and the creation are both printed, because a driver can disagree with
// itself and one of them did.** lavapipe answers `exportable=no` to the query *and*
// `VK_ERROR_INVALID_EXTERNAL_HANDLE` to `vkCreateSemaphore`, which agree; a driver where those
// two differed would be one whose renderer took the wrong branch at startup. The descriptor's
// `/proc/self/fd` link is printed too, because that is what distinguishes an
// `anon_inode:syncobj_file` — which KMS and `wp_linux_drm_syncobj_v1` accept — from a sync_file,
// which is a different object with a different lifetime.
void ReportSemaphore(
	VkPhysicalDevice physical,
	VkDevice device,
	PFN_vkGetPhysicalDeviceExternalSemaphoreProperties query,
	PFN_vkCreateSemaphore create,
	PFN_vkGetSemaphoreFdKHR get,
	PFN_vkDestroySemaphore destroy,
	bool timeline,
	VkExternalSemaphoreHandleTypeFlagBits handle
)
{
	const char* kind = timeline ? "timeline" : "binary  ";
	const char* transport = handle == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT ? "opaque_fd" : "sync_fd  ";

	VkSemaphoreTypeCreateInfo type{};
	type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
	type.semaphoreType = timeline ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY;

	VkPhysicalDeviceExternalSemaphoreInfo info{};
	info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
	info.pNext = &type;
	info.handleType = handle;

	VkExternalSemaphoreProperties properties{};
	properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
	query(physical, &info, &properties);

	const bool claimed = (properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0;
	std::printf("    %s %s  query says %-3s", kind, transport, claimed ? "yes" : "no");

	if (!claimed || device == VK_NULL_HANDLE)
	{
		std::printf("\n");

		return;
	}

	// **`sync_fd` is reported and never attempted, and finding out why cost an afternoon.**
	// Exporting one requires the semaphore to have a pending signal operation — a submission
	// already queued against it — and asking before that is invalid usage that *hangs* rather
	// than returning an error, which is what lavapipe does. That is also the argument
	// Seam/SyncPoint.h makes for a timeline over a binary fence, measured: a point that can only
	// be named after the submission is a round trip inside the frame section.
	if (handle == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT)
	{
		std::printf("  (export needs a pending signal; not attempted)\n");

		return;
	}

	// Only attempted where the query said yes, and only for the transport gyro would use.
	VkExportSemaphoreCreateInfo exportInfo{};
	exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
	exportInfo.handleTypes = handle;
	type.pNext = &exportInfo;

	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	semaphoreInfo.pNext = &type;

	VkSemaphore semaphore = VK_NULL_HANDLE;
	const VkResult created = create(device, &semaphoreInfo, nullptr, &semaphore);

	if (created != VK_SUCCESS)
	{
		std::printf("  create %d\n", created);

		return;
	}

	VkSemaphoreGetFdInfoKHR getInfo{};
	getInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
	getInfo.semaphore = semaphore;
	getInfo.handleType = handle;

	int descriptor = -1;
	const VkResult exported = get != nullptr ? get(device, &getInfo, &descriptor) : VK_ERROR_EXTENSION_NOT_PRESENT;

	if (exported == VK_SUCCESS && descriptor >= 0)
	{
		char path[64] = {};
		char link[128] = {};
		std::snprintf(path, sizeof path, "/proc/self/fd/%d", descriptor);
		const ssize_t length = readlink(path, link, sizeof link - 1);
		std::printf("  exported -> %s\n", length > 0 ? link : "?");
		close(descriptor);
	}
	else
	{
		std::printf("  export %d\n", exported);
	}

	destroy(device, semaphore, nullptr);
}

void ReportModifiers(
	VkPhysicalDevice physical,
	PFN_vkGetPhysicalDeviceFormatProperties2 query,
	VkFormat format,
	const char* name
)
{
	VkDrmFormatModifierPropertiesListEXT list{};
	list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;

	VkFormatProperties2 properties{};
	properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
	properties.pNext = &list;
	query(physical, format, &properties);

	std::vector<VkDrmFormatModifierPropertiesEXT> entries(list.drmFormatModifierCount);
	list.pDrmFormatModifierProperties = entries.data();
	query(physical, format, &properties);

	std::printf("    %s: %u modifier(s)\n", name, list.drmFormatModifierCount);

	for (const VkDrmFormatModifierPropertiesEXT& entry : entries)
	{
		// The one bit that decides whether a target can be composited into at all. A modifier a
		// device can sample but not render to is one BindTargets must refuse.
		const bool attachable = (entry.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
		std::printf(
			"      0x%016lx  planes %u  %s\n",
			entry.drmFormatModifier,
			entry.drmFormatModifierPlaneCount,
			attachable ? "renderable" : "not renderable"
		);
	}
}
} // namespace

int main()
{
	g_Library = dlopen("libvulkan.so.1", RTLD_NOW);

	if (g_Library == nullptr)
	{
		// Not a failure of the probe. This is the state a boot service has to survive, and reporting
		// it is the whole reason gyro does not link the loader.
		std::printf("no Vulkan loader: %s\n", dlerror());
		std::printf("gyro would come up on the console renderer and say so.\n");

		return 1;
	}

	g_GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_Library, "vkGetInstanceProcAddr"));

	if (g_GetInstanceProcAddr == nullptr)
	{
		std::printf("libvulkan.so.1 has no vkGetInstanceProcAddr\n");

		return 1;
	}

	VkApplicationInfo application{};
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "VulkanProbe";
	application.pEngineName = "gyro";
	application.apiVersion = VK_API_VERSION_1_3;

	VkInstanceCreateInfo instanceInfo{};
	instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &application;

	VkInstance instance = VK_NULL_HANDLE;
	const VkResult created = Load<PFN_vkCreateInstance>(nullptr, "vkCreateInstance")(&instanceInfo, nullptr, &instance);

	if (created != VK_SUCCESS)
	{
		std::printf("vkCreateInstance failed with %d — a loader with no ICD behind it\n", created);

		return 1;
	}

	auto enumerate = Load<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
	auto properties2 = Load<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
	auto extensions = Load<PFN_vkEnumerateDeviceExtensionProperties>(instance, "vkEnumerateDeviceExtensionProperties");
	auto formats = Load<PFN_vkGetPhysicalDeviceFormatProperties2>(instance, "vkGetPhysicalDeviceFormatProperties2");
	auto features2 = Load<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2");
	auto semaphores = Load<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
		instance, "vkGetPhysicalDeviceExternalSemaphoreProperties"
	);
	auto createDevice = Load<PFN_vkCreateDevice>(instance, "vkCreateDevice");
	auto getDeviceProcAddr = Load<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");

	std::uint32_t count = 0;
	enumerate(instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	enumerate(instance, &count, devices.data());

	std::printf("%u physical device(s)\n", count);

	for (VkPhysicalDevice physical : devices)
	{
		VkPhysicalDeviceDriverProperties driver{};
		driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

		VkPhysicalDeviceProperties2 device{};
		device.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		device.pNext = &driver;
		properties2(physical, &device);

		std::printf(
			"\n=== %s [%s] %s api %u.%u.%u ===\n",
			device.properties.deviceName,
			driver.driverName,
			TypeName(device.properties.deviceType),
			VK_API_VERSION_MAJOR(device.properties.apiVersion),
			VK_API_VERSION_MINOR(device.properties.apiVersion),
			VK_API_VERSION_PATCH(device.properties.apiVersion)
		);

		std::uint32_t available = 0;
		extensions(physical, nullptr, &available, nullptr);
		std::vector<VkExtensionProperties> present(available);
		extensions(physical, nullptr, &available, present.data());

		std::printf("  extensions gyro asks about:\n");

		for (const char* wanted : Interesting)
		{
			bool found = false;

			for (const VkExtensionProperties& entry : present)
			{
				found = found || std::strcmp(entry.extensionName, wanted) == 0;
			}

			std::printf("    %-38s %s\n", wanted, found ? "yes" : "no");
		}

		VkPhysicalDeviceVulkan13Features features13{};
		features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

		VkPhysicalDeviceVulkan12Features features12{};
		features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		features12.pNext = &features13;

		VkPhysicalDeviceFeatures2 features{};
		features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features.pNext = &features12;
		features2(physical, &features);

		std::printf(
			"  features: timelineSemaphore %s, dynamicRendering %s\n",
			features12.timelineSemaphore != 0u ? "yes" : "no",
			features13.dynamicRendering != 0u ? "yes" : "no"
		);

		std::printf("  what a composite target can be:\n");
		ReportModifiers(physical, formats, VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM (XR24/AR24)");
		ReportModifiers(physical, formats, VK_FORMAT_A2R10G10B10_UNORM_PACK32, "A2R10G10B10 (XR30/AR30)");

		// A device is created only to attempt the exports, and only with the one extension that
		// makes them possible. A driver that refuses the device outright still gets its query
		// results printed above, which is the half that matters.
		const char* wanted[] = { VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME };
		const float priority = 1.0F;

		VkDeviceQueueCreateInfo queue{};
		queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queue.queueCount = 1;
		queue.pQueuePriorities = &priority;

		VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
		timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
		timeline.timelineSemaphore = VK_TRUE;

		VkDeviceCreateInfo deviceInfo{};
		deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceInfo.pNext = &timeline;
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queue;
		deviceInfo.enabledExtensionCount = 1;
		deviceInfo.ppEnabledExtensionNames = wanted;

		VkDevice logical = VK_NULL_HANDLE;
		const VkResult opened = createDevice(physical, &deviceInfo, nullptr, &logical);

		std::printf("  synchronization gyro can hand to a presenter:\n");

		if (opened != VK_SUCCESS)
		{
			std::printf("    (no device: vkCreateDevice returned %d)\n", opened);

			continue;
		}

		auto createSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(getDeviceProcAddr(logical, "vkCreateSemaphore"));
		auto destroySemaphore =
			reinterpret_cast<PFN_vkDestroySemaphore>(getDeviceProcAddr(logical, "vkDestroySemaphore"));
		auto getSemaphoreFd =
			reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(getDeviceProcAddr(logical, "vkGetSemaphoreFdKHR"));

		for (bool isTimeline : { true, false })
		{
			for (VkExternalSemaphoreHandleTypeFlagBits handle :
			     { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT })
			{
				ReportSemaphore(
					physical, logical, semaphores, createSemaphore, getSemaphoreFd, destroySemaphore, isTimeline, handle
				);
			}
		}

		// The line decision 104 turns on, stated rather than left to be read off the table above.
		VkSemaphoreTypeCreateInfo type{};
		type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

		VkPhysicalDeviceExternalSemaphoreInfo info{};
		info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
		info.pNext = &type;
		info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

		VkExternalSemaphoreProperties exportable{};
		exportable.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
		semaphores(physical, &info, &exportable);

		std::printf(
			"  => %s\n",
			(exportable.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0 ?
				"this device hands out waitable sync points; a presenter gets a real fence" :
				"this device exports no timeline; the renderer finishes the frame in Record (decision 104)"
		);

		auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(getDeviceProcAddr(logical, "vkDestroyDevice"));
		destroyDevice(logical, nullptr);
	}

	Load<PFN_vkDestroyInstance>(instance, "vkDestroyInstance")(instance, nullptr);

	return 0;
}
