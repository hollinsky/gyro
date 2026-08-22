#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string_view>
#include <utility>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Render/Vulkan.h"
#include "Seam/RenderTarget.h"

// The Vulkan device, and everything about a machine that is decided once.
//
// **It is separate from the renderer because it is what device migration replaces.**
// Docs/Architecture.md#device-migration runs on every boot — `simpledrm` binds the framebuffer
// before the real driver loads — so `Render` tears down and rebuilds whole while the frame loop keeps
// running and the presenter holds the last frame on glass. Docs/Structure.md states the consequence:
// no Vulkan handle outlives the device that made it, and the swap is the composition root's. Having
// the handles in one owned object with a destructor is what makes that a `std::move` at the top
// rather than an ordering somebody has to remember.
//
// **Selection is a policy and decision 40 is why.** Software rendering is *a physical device
// selection, not a parallel code path* — no second renderer, no `#ifdef`, no backend — so the floor
// tier is `DeviceClass::Software` and nothing else about the frame loop changes. It is a policy
// rather than an inference because two callers want opposite things from the same machine: gyro
// wants the best device present, and the test that proves the floor tier works wants lavapipe *even
// on a workstation that has a GPU*, which is the case this file was written on.

// Which physical device to take.
enum class DeviceClass : std::uint8_t
{
	// The best present, preferring discrete over integrated over software. What gyro asks for.
	Any,

	// Refuse a software device. Nothing in gyro asks for this; a benchmark that would silently
	// measure llvmpipe does.
	Hardware,

	// A software device only. Decision 34's floor tier, reached by asking for it — which is what
	// makes the floor a path that is exercised rather than one that is argued about.
	Software,
};

struct VulkanDevicePolicy
{
	DeviceClass Class = DeviceClass::Any;

	// What shows up in the driver's own logs and in a GPU vendor's crash reports. Not the version,
	// which travels separately.
	std::string_view Application = "gyro";
};

// What the device turned out to be, for a log line and for the two branches that read it.
struct DeviceDescription
{
	std::array<char, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE> Name{};
	std::array<char, VK_MAX_DRIVER_NAME_SIZE> Driver{};
	VkPhysicalDeviceType Type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
	std::uint32_t ApiVersion = 0;

	// Whether a timeline semaphore on this device can be exported as a descriptor.
	//
	// **This is the field decision 104 exists for, and it is a device property rather than a
	// per-frame branch.** lavapipe advertises `VK_KHR_external_semaphore_fd` and refuses to *create*
	// an exportable semaphore of either kind, so a renderer on the floor tier has no descriptor to
	// put in a `SyncPoint` and must finish the frame before `Record` returns. Queried once with
	// `vkGetPhysicalDeviceExternalSemaphoreProperties`, whose answer matched the creation failure
	// exactly on both drivers this was measured against.
	bool ExportsTimeline = false;

	[[nodiscard]] std::string_view DeviceName() const noexcept { return { Name.data() }; }

	[[nodiscard]] std::string_view DriverName() const noexcept { return { Driver.data() }; }

	// Whether this is decision 40's permanently-occupied floor tier.
	[[nodiscard]] constexpr bool IsSoftware() const noexcept { return Type == VK_PHYSICAL_DEVICE_TYPE_CPU; }
};

// Whether Vulkan can be reached at all on this machine.
//
// **It is `dlopen` and nothing more, and the weaker forms are wrong in the same way
// Virtual/Udmabuf.h's are.** A `stat` on the loader says nothing about whether an ICD is installed
// behind it, and gyro's own build says nothing at all, because volk resolves the loader at runtime
// rather than at link time. What this answers is the first of the two questions — is there a loader
// — and `VulkanDevice::Open` answers the second by opening one.
[[nodiscard]] bool IsVulkanLoaderPresent() noexcept;

// The instance, the physical device, the logical device and its queue, owned together.
//
// Move-only for `DmabufBuffer`'s reason: a copy would destroy the same `VkDevice` twice, and the
// second destruction lands on whatever the driver put at that address. Returned by value through a
// `Result` rather than constructed fallibly, so that there is no half-open state to check for.
class VulkanDevice
{
public:
	VulkanDevice() = default;

	~VulkanDevice() { Reset(); }

	VulkanDevice(const VulkanDevice&) = delete;
	VulkanDevice& operator=(const VulkanDevice&) = delete;

	VulkanDevice(VulkanDevice&& other) noexcept
		: m_Instance{ std::exchange(other.m_Instance, VK_NULL_HANDLE) },
		  m_Physical{ std::exchange(other.m_Physical, VK_NULL_HANDLE) },
		  m_Device{ std::exchange(other.m_Device, VK_NULL_HANDLE) },
		  m_Queue{ std::exchange(other.m_Queue, VK_NULL_HANDLE) },
		  m_QueueFamily{ std::exchange(other.m_QueueFamily, 0) }, m_Description{ other.m_Description }
	{}

	VulkanDevice& operator=(VulkanDevice&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			m_Instance = std::exchange(other.m_Instance, VK_NULL_HANDLE);
			m_Physical = std::exchange(other.m_Physical, VK_NULL_HANDLE);
			m_Device = std::exchange(other.m_Device, VK_NULL_HANDLE);
			m_Queue = std::exchange(other.m_Queue, VK_NULL_HANDLE);
			m_QueueFamily = std::exchange(other.m_QueueFamily, 0);
			m_Description = other.m_Description;
		}

		return *this;
	}

	// Bring up a device, or say why not.
	//
	// **Unbounded, allocating, and permitted to be both.** It loads a driver, compiles nothing, and
	// talks to the kernel; it runs at startup and at a device migration, never inside a frame.
	//
	// `ENODEV` where there is no loader, no ICD, or no device of the class asked for — which is the
	// answer a machine with no GPU gives to `DeviceClass::Hardware` and the one a container with no
	// Mesa gives to anything. `EINVAL` where a device exists but lacks what
	// Docs/Decisions.md decision 40 requires of one.
	[[nodiscard]] static Result<VulkanDevice> Open(VulkanDevicePolicy policy = {});

	[[nodiscard]] bool IsValid() const noexcept { return m_Device != VK_NULL_HANDLE; }

	[[nodiscard]] VkDevice Handle() const noexcept { return m_Device; }

	[[nodiscard]] VkPhysicalDevice Physical() const noexcept { return m_Physical; }

	[[nodiscard]] VkQueue Queue() const noexcept { return m_Queue; }

	[[nodiscard]] std::uint32_t QueueFamily() const noexcept { return m_QueueFamily; }

	[[nodiscard]] const DeviceDescription& Description() const noexcept { return m_Description; }

	// Whether an image of this format and modifier can be imported from a dmabuf and drawn into.
	//
	// Asked before a target set is torn down for a reconfiguration that cannot be honoured, which is
	// `IDmabufAllocator::Supports` one seam over and for the same reason. It queries the driver
	// rather than consulting a table: lavapipe accepts linear and nothing else, a real driver accepts
	// several tilings, and a table here would be a third opinion that can disagree with both.
	[[nodiscard]] bool Supports(PixelFormat format) const noexcept;

	// The memory type an imported dmabuf must be allocated against, as a bitmask. Zero where the
	// descriptor is not importable at all, which is the honest answer for a buffer from a device this
	// one cannot share with.
	[[nodiscard]] std::uint32_t ImportableMemoryTypes(RawFd descriptor) const noexcept;

private:
	void Reset() noexcept;

	VkInstance m_Instance = VK_NULL_HANDLE;

	// Not owned; a physical device belongs to the instance and is invalidated with it.
	VkPhysicalDevice m_Physical = VK_NULL_HANDLE;

	VkDevice m_Device = VK_NULL_HANDLE;

	// Not owned either; a queue is retrieved from the device rather than created.
	VkQueue m_Queue = VK_NULL_HANDLE;

	std::uint32_t m_QueueFamily = 0;
	DeviceDescription m_Description{};
};

// Prints as llvmpipe [llvmpipe] api 1.4.354 software no-export, which is the whole of what a startup
// line needs to say: two callers have asked which device came up, and the two things that change
// behaviour downstream are whether it is the floor tier and whether it can hand out a descriptor.
template<>
struct std::formatter<DeviceDescription>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const DeviceDescription& description, Context& context) const
	{
		return std::format_to(
			context.out(),
			"{} [{}] api {}.{}.{} {} {}",
			description.DeviceName(),
			description.DriverName(),
			VK_API_VERSION_MAJOR(description.ApiVersion),
			VK_API_VERSION_MINOR(description.ApiVersion),
			VK_API_VERSION_PATCH(description.ApiVersion),
			description.IsSoftware() ? "software" : "hardware",
			description.ExportsTimeline ? "exports" : "no-export"
		);
	}
};

// The contract everything downstream assumes. A copied device would destroy one `VkDevice` twice,
// and the second call lands on whatever the driver has since put at that address.
static_assert(!std::is_copy_constructible_v<VulkanDevice> && !std::is_copy_assignable_v<VulkanDevice>);
static_assert(
	std::is_nothrow_move_constructible_v<VulkanDevice>,
	"Device migration is a move at the composition root, and a throwing one leaves two owners"
);
static_assert(std::formattable<DeviceDescription, char>);

// A default description names no device and claims nothing, which is what keeps a failed `Open` from
// reading as a working software device.
static_assert(!DeviceDescription{}.IsSoftware(), "Unknown is not software");
static_assert(!DeviceDescription{}.ExportsTimeline);
