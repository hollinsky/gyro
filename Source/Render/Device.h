#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string_view>
#include <utility>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Geometry/Space.h"
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
	// **This is the field decision 108 exists for, and it is a device property rather than a
	// per-frame branch.** lavapipe advertises `VK_KHR_external_semaphore_fd` and refuses to *create*
	// an exportable semaphore of either kind, so a renderer on the floor tier has no descriptor to
	// put in a `SyncPoint` and must finish the frame before `Record` returns. Queried once with
	// `vkGetPhysicalDeviceExternalSemaphoreProperties`, whose answer matched the creation failure
	// exactly on both drivers this was measured against.
	bool ExportsTimeline = false;

	// Whether this device can write host memory straight into an optimally-tiled image, with no
	// queue, no command buffer and no staging buffer between the two.
	//
	// **It is what makes a `wl_shm` import a dispatch-thread operation at all**, which is the whole
	// reason it is queried. Seam/Importer.h puts `Adopt` on the dispatch thread while the frame
	// thread is compositing, and the ordinary way to fill a device image — a staging buffer and
	// `vkCmdCopyBufferToImage` — needs a queue submission, on a queue the `SCHED_FIFO` frame thread
	// is already submitting to. Sharing it means a lock the frame thread can block on behind a
	// lower-priority thread, which is a missed frame every time a client posts a software buffer;
	// a second queue means a second submission order, which is what
	// Render/Device.cpp's queue selection declines for a reason that still stands. `VK_EXT_host_image_copy`
	// makes the question go away rather than answering it: the copy is `memcpy` into a tiled image
	// performed by the driver on the calling thread.
	//
	// **A feature and not just an extension.** Promoted to Vulkan 1.4 core, and gyro requires 1.3, so
	// it is asked for by name — and it is asked for optionally, the way `ExportsTimeline` is, because
	// a driver without it is not a machine gyro refuses to run on — it is a device that imports
	// dmabufs and refuses mapped buffers, which Render/Textures.h states as a named refusal rather
	// than working around. Every current Mesa driver and both proprietary ones have it.
	bool CopiesFromHost = false;

	// The GPU's own clock, as the two numbers a timestamp query has to be read through: how many
	// nanoseconds one tick is, and how many of a query's sixty-four bits actually carry a value.
	//
	// **Both are needed and neither is a formality.** The period is a `float` and is not an integer
	// on real hardware — Tiger Lake's is 52.083 ns, a 19.2 MHz counter — so a tick count is not a
	// nanosecond count and the conversion cannot be elided. The valid bits are the half that is easy
	// to skip and expensive to skip: the same device reports thirty-six, which is a counter that
	// **wraps every fifty-nine minutes**. A plain subtraction across that wrap yields an hour-long
	// frame, and Frame/Budget.h's mark is a maximum over a window, so one such sample holds an output
	// at decision 35's floor tier for the next two hundred and fifty-six frames. `TimestampSpan`
	// below is where both are applied, so that no caller has to remember either.
	//
	// **Zero valid bits means this device cannot measure GPU time at all**, which is a report rather
	// than a defect: `IRenderer::CollectCosts` then returns nothing forever, exactly as `Blit` does,
	// and `Budget` schedules on the CPU half of `C` alone.
	float TimestampPeriod = 0.0F;
	std::uint32_t TimestampValidBits = 0;

	// Whether a timestamp taken on this device's queue means anything.
	[[nodiscard]] constexpr bool MeasuresGpuTime() const noexcept
	{
		return TimestampValidBits != 0 && TimestampPeriod > 0.0F;
	}

	// The elapsed time between two raw timestamp query results, wrap included.
	//
	// **The subtraction is modular and that is the whole point.** Masking each value to the valid
	// bits and subtracting in that width is correct across the counter's wrap for any span shorter
	// than the counter's own period — which a composite is by five orders of magnitude — so the
	// hour-long frame described above cannot be produced rather than being filtered out afterwards.
	[[nodiscard]] constexpr Duration TimestampSpan(std::uint64_t begin, std::uint64_t end) const noexcept
	{
		if (!MeasuresGpuTime())
		{
			return Duration::zero();
		}

		const std::uint64_t mask =
			TimestampValidBits >= 64 ? ~std::uint64_t{ 0 } : ((std::uint64_t{ 1 } << TimestampValidBits) - 1);
		const std::uint64_t ticks = (end - begin) & mask;

		return Duration{ static_cast<std::int64_t>(static_cast<double>(ticks) * static_cast<double>(TimestampPeriod)) };
	}

	[[nodiscard]] std::string_view DeviceName() const noexcept { return { Name.data() }; }

	[[nodiscard]] std::string_view DriverName() const noexcept { return { Driver.data() }; }

	// Whether this is decision 40's permanently-occupied floor tier.
	[[nodiscard]] constexpr bool IsSoftware() const noexcept { return Type == VK_PHYSICAL_DEVICE_TYPE_CPU; }
};

// No memory type carries what was asked for. Distinct from every valid index because
// `VK_MAX_MEMORY_TYPES` is 32 and an index is one of those — so this is the first value that cannot
// be one, rather than a sentinel picked to be large.
inline constexpr std::uint32_t MemoryTypeNone = VK_MAX_MEMORY_TYPES;

// Whether Vulkan can be reached at all on this machine.
//
// **It is `dlopen` and nothing more, and the weaker forms are wrong in the same way
// Virtual/Udmabuf.h's are.** A `stat` on the loader says nothing about whether an ICD is installed
// behind it, and gyro's own build says nothing at all, because volk resolves the loader at runtime
// rather than at link time. What this answers is the first of the two questions — is there a loader
// — and `VulkanDevice::Open` answers the second by opening one.
[[nodiscard]] bool IsVulkanLoaderPresent() noexcept;

// An image this device allocated, and the dmabuf it was handed out as.
//
// **The other direction, and it exists because a nested output has nothing else to allocate with.**
// Seam/RenderTarget.h's rule is that the presenter owns the images: a KMS output allocates through
// GBM, a virtual output allocates through `udmabuf`, and both hand descriptions to a renderer that
// imports them. A nested output has neither — no GBM device, no swapchain, and a parent compositor
// that states a format and a modifier list and expects buffers back — so the only thing on the
// machine that can produce one is the device that is about to draw into it. Asking it here keeps the
// seam's rule intact rather than bending it: the presenter still owns the target, it just had to ask
// somebody else to make it.
//
// **Everything in here is the caller's, and Core/Fd.h's two types are how that is said.** This object
// destroys the `VkImage` and frees its memory, and it *owns* the descriptor — an `Fd`. What the
// description carries is a `RawFd` into that same descriptor, because a `RenderTarget` is copied by
// value every frame and an owner cannot live there. So the description is valid for exactly as long
// as this object is, which is the lifetime a presenter already announces with `TargetsInvalidated`.
//
// **There is no view and nothing draws through this handle.** The renderer imports the descriptor
// back through the path it already has and builds its own image over it; this one exists only because
// Vulkan will not allocate modifier-tiled memory without an image to lay it out. Round-tripping
// rather than sharing the handle is what leaves `IRenderer` untouched — it imports every target it
// draws into already, and a second way in would be a second set of layout transitions to get wrong.
//
// **Destroyed before the device that made it**, which is Docs/Structure.md's rule for every Vulkan
// handle here. The `VkDevice` below is borrowed for the destructor's use and nothing else.
class ExportedImage
{
public:
	ExportedImage() = default;

	~ExportedImage() { Reset(); }

	ExportedImage(const ExportedImage&) = delete;
	ExportedImage& operator=(const ExportedImage&) = delete;

	ExportedImage(ExportedImage&& other) noexcept
		: m_Device{ std::exchange(other.m_Device, VK_NULL_HANDLE) },
		  m_Image{ std::exchange(other.m_Image, VK_NULL_HANDLE) },
		  m_Memory{ std::exchange(other.m_Memory, VK_NULL_HANDLE) }, m_Descriptor{ std::move(other.m_Descriptor) },
		  m_Target{ std::exchange(other.m_Target, RenderTarget{}) }
	{}

	ExportedImage& operator=(ExportedImage&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			m_Device = std::exchange(other.m_Device, VK_NULL_HANDLE);
			m_Image = std::exchange(other.m_Image, VK_NULL_HANDLE);
			m_Memory = std::exchange(other.m_Memory, VK_NULL_HANDLE);
			m_Descriptor = std::move(other.m_Descriptor);
			m_Target = std::exchange(other.m_Target, RenderTarget{});
		}

		return *this;
	}

	[[nodiscard]] bool IsValid() const noexcept { return m_Image != VK_NULL_HANDLE && m_Target.IsValid(); }

	// The description, ready to go into a presenter's target set. Its planes borrow the descriptor
	// this object owns, so a copy of it outlives nothing.
	[[nodiscard]] const RenderTarget& Target() const noexcept { return m_Target; }

	// The modifier the device actually chose out of the candidates, with the caller's fourcc. Worth
	// reading back rather than assuming: a nested output has to tell the parent which one it got.
	[[nodiscard]] PixelFormat Format() const noexcept { return m_Target.Format; }

	// Borrowed. The consumer that is handed this — a `zwp_linux_buffer_params_v1`, an `AddFB2` — must
	// duplicate it if it means to keep it, exactly as the import path duplicates what it is given.
	[[nodiscard]] RawFd Descriptor() const noexcept { return m_Descriptor.Borrow(); }

	// The allocating image. Not drawn into by anything here; exposed because a caller that wants to
	// destroy this in a particular order relative to its own work needs to be able to name it.
	[[nodiscard]] VkImage Handle() const noexcept { return m_Image; }

private:
	friend class VulkanDevice;

	void Reset() noexcept;

	// Not owned; the device outlives every image it made, which is the rule stated above.
	VkDevice m_Device = VK_NULL_HANDLE;

	VkImage m_Image = VK_NULL_HANDLE;
	VkDeviceMemory m_Memory = VK_NULL_HANDLE;
	Fd m_Descriptor;
	RenderTarget m_Target{};
};

// A timeline semaphore and the DRM syncobj descriptor that names it.
//
// **One representation, three consumers**, which is Seam/SyncPoint.h's premise and measured rather
// than assumed: anv exports a timeline semaphore as an `anon_inode:syncobj_file`, which is what
// `wp_linux_drm_syncobj_v1` imports and what a KMS commit programs as `IN_FENCE_FD`. The handle type
// asked for is `OPAQUE_FD` because Vulkan has no syncobj enumerator — on the drivers that matter the
// opaque handle *is* the syncobj, and on the ones where it is not there is no timeline to export at
// all.
//
// Owned and destroyed together for `ExportedImage`'s reason, and before the device for the same one.
class ExportedTimeline
{
public:
	ExportedTimeline() = default;

	~ExportedTimeline() { Reset(); }

	ExportedTimeline(const ExportedTimeline&) = delete;
	ExportedTimeline& operator=(const ExportedTimeline&) = delete;

	ExportedTimeline(ExportedTimeline&& other) noexcept
		: m_Device{ std::exchange(other.m_Device, VK_NULL_HANDLE) },
		  m_Semaphore{ std::exchange(other.m_Semaphore, VK_NULL_HANDLE) }, m_Descriptor{ std::move(other.m_Descriptor) }
	{}

	ExportedTimeline& operator=(ExportedTimeline&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			m_Device = std::exchange(other.m_Device, VK_NULL_HANDLE);
			m_Semaphore = std::exchange(other.m_Semaphore, VK_NULL_HANDLE);
			m_Descriptor = std::move(other.m_Descriptor);
		}

		return *this;
	}

	[[nodiscard]] bool IsValid() const noexcept { return m_Semaphore != VK_NULL_HANDLE; }

	// What a submission signals against, for a caller that is going to hand points on it out.
	[[nodiscard]] VkSemaphore Handle() const noexcept { return m_Semaphore; }

	// Borrowed, which is what `SyncPoint::Timeline` already is and for the same reason: a point sits
	// by value in a layer list that is copied per frame, and the timeline outlives every point on it.
	[[nodiscard]] RawFd Descriptor() const noexcept { return m_Descriptor.Borrow(); }

	// Where the timeline has got to. A poll rather than a wait — the frame loop never blocks — and the
	// answer a release timeline is read for: a point that has been reached is a buffer the parent has
	// finished with.
	[[nodiscard]] Result<std::uint64_t> Counter() const noexcept;

private:
	friend class VulkanDevice;

	void Reset() noexcept;

	VkDevice m_Device = VK_NULL_HANDLE;
	VkSemaphore m_Semaphore = VK_NULL_HANDLE;
	Fd m_Descriptor;
};

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
	// Whether a target in this format and modifier can also be *sampled*, which is what a gathering
	// material needs of the image it is drawing into: Render/Backdrop.h reads the backdrop out of the
	// composite target, so the target has to be legible to a shader as well as writable by one.
	//
	// **Separate from `Supports` and answered separately, because a false here is not a refusal.** A
	// target that cannot be sampled is still a target — solids, textures and everything pointwise
	// draw into it unchanged — and what it loses is the blur behind a panel, which is exactly decision
	// 34's third rung and exactly what `RenderMode::Floor` already draws. So this decides a tier for
	// that output rather than whether the output comes up, and a compressed modifier that refuses
	// sampling costs a look rather than a screen.
	[[nodiscard]] bool SupportsSampling(PixelFormat format) const noexcept;

	// Whether an *optimally-tiled* image of this format can be sampled and filled by
	// `vkCopyMemoryToImage`, which together are what a `wl_shm` client's buffer needs of a device.
	//
	// **A different question from the two above, and the difference is the word `dmabuf`.** Those ask
	// what can be done with an image whose memory came from somewhere else, so they range over DRM
	// modifiers; this asks what can be done with one this device allocated for itself, where the
	// tiling is the driver's own optimal and there is no modifier in the picture. A `wl_shm` buffer
	// has no modifier to ask about — it is bytes — so the import that copies it is the one caller
	// that needs this form.
	//
	// False on any device where `CopiesFromHost` is false, since the feature bit cannot be set
	// without the feature. Render/Textures.h is where a refusal is what that means.
	[[nodiscard]] bool SupportsHostTexture(PixelFormat format) const noexcept;

	// The first memory type in `allowed` carrying every one of `properties`, or `MemoryTypeNone`.
	//
	// Here rather than copied into each of the four callers that walk `VkPhysicalDeviceMemoryProperties`
	// for a different reason: the walk is the same walk, and the interesting part is always which
	// property bits the caller asked for.
	[[nodiscard]] std::uint32_t MemoryType(std::uint32_t allowed, VkMemoryPropertyFlags properties) const noexcept;

	[[nodiscard]] std::uint32_t ImportableMemoryTypes(RawFd descriptor) const noexcept;

	// Allocate an image of this size and format under the first workable modifier, and export it as a
	// dmabuf.
	//
	// **The list is the caller's preference order and the first survivor wins.** A nested output's
	// candidates come from the parent compositor, which has already ranked them against its own
	// scanout hardware; re-ranking here would be this file second-guessing a negotiation it was not
	// part of. What the device contributes is the veto, and there are four of them: a modifier it does
	// not list for this format, one whose tiling cannot be a colour attachment — this is a render
	// target, so that feature bit is checked before the modifier is chosen rather than discovered at
	// `vkCreateImage` — one whose plane count is more than Seam/RenderTarget.h's `MaxImagePlanes` can
	// describe, and one the driver will not create an *exportable* image with at this extent. The
	// fourth is a separate query from the first three and not implied by them: a tiling that renders
	// is not automatically a tiling the driver will hand out a descriptor for.
	//
	// **`ModifierInvalid` is refused wherever it appears in the list**, for the reason `Supports`
	// gives from the other side. Unknown is not linear. An image laid out by a tiling nobody stated is
	// one where this device's guess and the consumer's have to agree by luck, and a target gyro
	// declines to allocate is better than a window that is diagonally sheared on somebody else's
	// machine.
	//
	// `EINVAL` where no candidate survives — which includes an empty list and a fourcc
	// Render/Vulkan.h cannot name — and `ENOMEM` where the allocation itself failed. Unbounded and
	// allocating, like `Open`: it runs when an output is configured or reconfigured, never inside a
	// frame.
	[[nodiscard]] Result<ExportedImage>
	Export(PixelSize<DeviceSpace> size, std::uint32_t code, std::span<const std::uint64_t> modifiers) const;

	// A timeline semaphore this device signals, exported as a DRM syncobj descriptor.
	//
	// **What it is for is the half of explicit sync the renderer does not already cover.** The acquire
	// point — when the composite has landed — is the renderer's own timeline and reaches a presenter
	// inside a `SyncPoint`. The *release* point is the other direction: `wp_linux_drm_syncobj_v1`
	// requires a surface to name both, and nothing on the machine holds the timeline the parent
	// compositor signals when it has finished reading a buffer. That one is the nested output's to
	// create, and this is where it comes from. The same descriptor is what a KMS commit will later
	// program as `IN_FENCE_FD`.
	//
	// **`ENOTSUP` where the device cannot, and that path is measured rather than hypothetical.**
	// lavapipe advertises `VK_KHR_external_semaphore_fd` and then refuses to create an exportable
	// semaphore of either kind, which is why `Open` only enables the extension where
	// `vkGetPhysicalDeviceExternalSemaphoreProperties` said yes — see decision 108. So this reports
	// rather than asserts, and `Description().ExportsTimeline` is the same answer asked in advance.
	//
	// **What a caller does without one.** It waits on the CPU: poll `IRenderer::IsComplete` until the
	// frame has actually landed, and only then commit. That costs the overlap between drawing this
	// frame and handing the last one over — the parent is told about a frame after it is finished
	// rather than while it is being drawn, which lands as latency on a nested session and as nothing
	// at all on the floor tier, where the renderer has already finished inside `Record`. It is the
	// fallback a nested output takes, not a reason for one to refuse to come up.
	[[nodiscard]] Result<ExportedTimeline> ExportTimeline() const;

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
			"{} [{}] api {}.{}.{} {} {} {} {}",
			description.DeviceName(),
			description.DriverName(),
			VK_API_VERSION_MAJOR(description.ApiVersion),
			VK_API_VERSION_MINOR(description.ApiVersion),
			VK_API_VERSION_PATCH(description.ApiVersion),
			description.IsSoftware() ? "software" : "hardware",
			description.ExportsTimeline ? "exports" : "no-export",
			description.CopiesFromHost ? "host-copy" : "no-host-copy",
			description.MeasuresGpuTime() ? "gpu-timed" : "no-gpu-time"
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

// The same contract, for the two things a nested presenter is handed. A copied `ExportedImage` would
// destroy one `VkImage` twice and close one descriptor twice, and the second close lands on a number
// the kernel has since given to somebody else.
static_assert(!std::is_copy_constructible_v<ExportedImage> && !std::is_copy_assignable_v<ExportedImage>);
static_assert(!std::is_copy_constructible_v<ExportedTimeline> && !std::is_copy_assignable_v<ExportedTimeline>);
static_assert(std::is_nothrow_move_constructible_v<ExportedImage>, "A target set is built by moving these into it");
static_assert(std::is_nothrow_move_constructible_v<ExportedTimeline>);

// A default description names no device and claims nothing, which is what keeps a failed `Open` from
// reading as a working software device.
static_assert(!DeviceDescription{}.IsSoftware(), "Unknown is not software");
static_assert(!DeviceDescription{}.ExportsTimeline);
static_assert(!DeviceDescription{}.CopiesFromHost);
static_assert(!DeviceDescription{}.MeasuresGpuTime(), "A device nobody has described measures nothing");
static_assert(DeviceDescription{}.TimestampSpan(0, 1000) == Duration::zero());

// Tiger Lake's numbers, which is the machine this was measured on: a 19.2 MHz counter reported
// through thirty-six valid bits.
namespace Detail
{
inline constexpr DeviceDescription TigerLake{ .TimestampPeriod = 52.083332F, .TimestampValidBits = 36 };
} // namespace Detail

// A tick is not a nanosecond, and the conversion is the whole reason the period is carried: 19200
// ticks of a 19.2 MHz counter is a millisecond rather than the twenty microseconds a raw count would
// read as. Bounded rather than equal because the period is a `float` and 52.083332 times 19200 is not
// exactly a million — which is itself the reason the conversion happens in `double`.
static_assert(Detail::TigerLake.MeasuresGpuTime());
static_assert(
	Detail::TigerLake.TimestampSpan(1000, 1000 + 19200) > std::chrono::microseconds{ 999 } &&
	Detail::TigerLake.TimestampSpan(1000, 1000 + 19200) <= std::chrono::microseconds{ 1000 }
);

// **The one a bare subtraction gets wrong.** A composite that straddles the counter's wrap reads as
// a closing timestamp *below* its opening one, and the unmasked difference is the counter's whole
// period — an hour-long frame, filed into a mark that is a maximum over the next two hundred and
// fifty-six frames. Masked, it is the microsecond it actually was.
static_assert(
	Detail::TigerLake.TimestampSpan((std::uint64_t{ 1 } << 36) - 10, 9) == std::chrono::nanoseconds{ 989 },
	"A frame that straddles the timestamp counter's wrap costs what it cost"
);

// Sixty-four valid bits is the other end of the same arithmetic, and the shift that would be
// undefined there is why the mask is a branch rather than an expression.
static_assert(
	DeviceDescription{ .TimestampPeriod = 1.0F, .TimestampValidBits = 64 }.TimestampSpan(5, 105) ==
	std::chrono::nanoseconds{ 100 }
);
