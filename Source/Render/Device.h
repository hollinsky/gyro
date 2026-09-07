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
#include "Render/GpuClock.h"
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

	// The primary DRM minor of the device that is driving the panel, or -1 where nothing is — a nested
	// session, a virtual output, a test.
	//
	// **A preference rather than a requirement, and the asymmetry is deliberate.** Where a candidate
	// reports this minor it wins outright regardless of rank, because compositing on the part that
	// scans out is worth more than compositing on the faster part: the alternative is every target
	// allocated on one device and read by another across PRIME, which in the good case costs a copy
	// per frame and in the ordinary case leaves the two with no modifier in common but linear — the
	// regime Seam/Allocator.h measured at 7.4ms against 2.8ms for the same composite.
	//
	// Where no candidate reports it — an old driver with no `VK_EXT_physical_device_drm`, or a split
	// display engine whose scanout device is not a Vulkan device at all, which is the common shape on
	// a SoC — selection falls back to rank and the composition root says which device it ended up on.
	// That is the honest answer there: on a split part the two minors *never* match, and refusing to
	// come up would be refusing to run on the hardware this arrangement is most normal on.
	std::int64_t ScanoutMinor = -1;

	// Whether composite targets are created so that Render/Readback.h can copy one back.
	//
	// **Off unless the composition root was asked for captures, and that default is the whole point.**
	// A copy source needs `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` and usage is fixed at allocation, so the
	// bit has to be asked for before anybody presses the key. Asking for it always would put it into
	// the modifier negotiation on every run — and a modifier the driver will not create under this
	// usage is one dropped from the list a panel and a device have to agree on, which is a window that
	// stops being scanned out directly. That is a real cost on every frame of every session, paid for
	// a verb that fires when a person is looking for a bug. So it is a policy, `--capture` turns it
	// on, and Input/Chord.h's screenshot verb says so where it is off rather than failing quietly.
	bool Readable = false;
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

	// Whether an image's tiling can be *stated* — a DRM modifier gyro names on the way in and reads
	// back on the way out — or is only ever the driver's own linear.
	//
	// **`VK_EXT_image_drm_format_modifier`, and it is optional for the reason `CopiesFromHost` is
	// rather than the reason `ExportsTimeline` is.** It is not a driver that advertises and refuses;
	// it is a driver that genuinely does not have it, and lavapipe on Mesa 25 is that driver. Requiring
	// it meant the one device that is present on every machine — the floor tier decision 40 was
	// written around — could not be opened at all, so a laptop with a GPU gyro cannot bring up had no
	// picture rather than a slow one.
	//
	// **What false costs is the tiling and nothing else.** Every image is then created
	// `VK_IMAGE_TILING_LINEAR` and described as `DRM_FORMAT_MOD_LINEAR`, which is the pre-modifier
	// path every compositor took until 2019 and is what mutter still takes today whenever its
	// `DISABLE_MODIFIERS` flag is set — for a cursor plane, a secondary GPU, or a card whose driver it
	// distrusts. A composite goes at the same rate; what is lost is the compressed and tiled layouts a
	// real GPU would rather scan out of, which no software device has anyway.
	//
	// **The one thing it is not permitted to do is guess a stride.** An import states the offsets and
	// strides the *allocator* committed to, and without the extension there is nowhere to say them —
	// so `ImportImage` creates the image, reads back the layout the driver picked, and refuses where
	// the two disagree. A named refusal rather than a window read at the wrong pitch, which is the
	// failure this whole path is one mistake away from.
	bool StatesModifiers = false;

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

	// Whether the device will read its own clock and `CLOCK_MONOTONIC` in the same breath, which is
	// the difference between knowing how *long* a composite took and knowing *when* it happened.
	//
	// A timestamp pair is a subtraction and needs none of this — Frame/Budget.h has never asked where
	// on the wall the interval sat. What needs it is a trace: a GPU slice drawn against the frame
	// thread's own slices has to be in their domain, and the two clocks share no origin. `DEVICE`
	// beside `CLOCK_MONOTONIC` is what Core/Time.h stamps everything else in, so the conversion is a
	// subtraction in ticks and an addition in nanoseconds rather than a fit.
	bool CalibratesTimestamps = false;

	// Whether this device counts what the pipeline did, of which gyro reads exactly one figure:
	// fragment shader invocations.
	//
	// **It is the denominator the timestamp pair has no way to supply.** A composite that takes 2.8
	// milliseconds is either a lot of pixels or a slow device, and the two are indistinguishable from
	// the outside — a shadow is drawn over a quad larger than the node it belongs to, every dressed
	// panel is drawn twice, and a damage region that resolves to the whole screen redraws everything
	// under everything. Invocations divided by the panel's pixel count is that overdraw as a number,
	// and invocations divided by the elapsed span is the rate the hardware is actually achieving,
	// which is the figure a person compares against what the part is supposed to do.
	bool CountsPipelineStatistics = false;

	// Whether the driver will say how much of the GPU's memory this process is holding and how much it
	// is currently willing to hand over.
	//
	// **`VK_EXT_memory_budget`, and it is the only thing in the trace that can explain a slow frame the
	// timestamps make look like slow hardware.** A composite whose textures have been paged out draws
	// exactly the same picture, in exactly the same passes, at exactly the same clock, and takes twice
	// as long — and on an integrated part, where the budget is a share of the same memory every other
	// process on the machine is competing for, that happens because somebody opened a browser rather
	// than because anything gyro did changed. Without the pair a reader has a slow frame and no
	// candidate; with it the collapse in headroom is visible on the row beside the span.
	//
	// **Listed is the capability, the way `StatesModifiers` is rather than the way `ExportsTimeline`
	// is.** There is no feature to enable and no device creation to fail — what the extension adds is a
	// structure to chain onto a query gyro already makes — so decision 108's *ask the query* has nowhere
	// to bite in advance. What answers it instead is the reading: `ReadMemory` reports an invalid
	// `GpuMemory` where a listed extension comes back with a zero budget, and the counters are simply
	// absent, which is the same honest nothing Render/GpuClock.h reports for a clock it cannot read.
	bool ReportsMemoryBudget = false;

	// The primary DRM node's minor number, or -1 where `VK_EXT_physical_device_drm` did not answer.
	//
	// **Not a Vulkan concept — it is the bridge from the device Vulkan chose to the sysfs entry the
	// kernel driver hangs its frequency nodes off**, which is what decision 142's clock reader needs and
	// the reason it is a device property rather than a guess. On a machine with more than one GPU it is
	// what makes the clock the *right* one's; -1 leaves Render/GpuClock.h invalid and every `GpuCost`
	// reporting a zero clock, which is honest rather than wrong.
	std::int64_t PrimaryMinor = -1;

	// The render node's minor, or -1 where the same extension did not answer for it.
	//
	// **A second minor rather than the primary one plus a hundred and twenty-eight**, which is the
	// arithmetic everybody does and which stops being true on a machine with enough devices to run the
	// minors together. What wants it is decision 142's other half: every `drm_syncobj` ioctl is
	// `DRM_RENDER_ALLOW`, so Render/Deadline.h states a frame's deadline through a node it opens
	// without asking for master — which is a thing gyro holds on the primary node and must not risk
	// taking twice.
	std::int64_t RenderMinor = -1;

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

	// The signed distance between two raw timestamp results, for a caller placing one *relative to*
	// another rather than measuring an interval it already knows the direction of.
	//
	// **`TimestampSpan` cannot answer this and quietly gives the wrong number if asked.** It reads the
	// modular difference as a forward one, which is right for a pair the same batch wrote in order and
	// wrong for a batch that finished before the clocks were last read together — where the honest
	// answer is negative and the modular one is the counter's whole period minus a millisecond. So the
	// difference is taken in the shorter direction: more than half the counter's range apart is read
	// as the other way round, which is unambiguous for any offset shorter than half of fifty-nine
	// minutes and is the only case a frame can produce.
	[[nodiscard]] constexpr Duration TimestampOffset(std::uint64_t from, std::uint64_t to) const noexcept
	{
		if (!MeasuresGpuTime())
		{
			return Duration::zero();
		}

		const std::uint64_t width =
			TimestampValidBits >= 64 ? ~std::uint64_t{ 0 } : ((std::uint64_t{ 1 } << TimestampValidBits) - 1);
		const std::uint64_t ticks = (to - from) & width;
		const double signedTicks =
			ticks > width / 2 ? -static_cast<double>((width - ticks) + 1) : static_cast<double>(ticks);

		return Duration{ static_cast<std::int64_t>(signedTicks * static_cast<double>(TimestampPeriod)) };
	}

	// Where on Core/Time.h's timeline a raw device timestamp sat, read through an anchor.
	//
	// Declared after `GpuCalibration` would be circular and it is not worth the indirection, so the
	// anchor is taken by its two members' types — which is also what makes it usable from a constant
	// expression the tests below are written as.
	[[nodiscard]] constexpr Instant TimestampAt(Instant host, std::uint64_t device, std::uint64_t ticks) const noexcept
	{
		return Advanced(host, TimestampOffset(device, ticks));
	}

	[[nodiscard]] std::string_view DeviceName() const noexcept { return { Name.data() }; }

	[[nodiscard]] std::string_view DriverName() const noexcept { return { Driver.data() }; }

	// Whether this is decision 40's permanently-occupied floor tier.
	[[nodiscard]] constexpr bool IsSoftware() const noexcept { return Type == VK_PHYSICAL_DEVICE_TYPE_CPU; }
};

// The two clocks read together: the device's counter and Core/Time.h's timebase, sampled by the
// driver in one window.
//
// **A reading and not a fit**, which is the honest shape for what it is used for. `maxDeviation` on
// the Tiger Lake this was measured against is about ten microseconds — the width of the window the
// driver sampled both clocks in — and the call itself costs the same ten. A GPU span converted through
// an anchor taken in the same iteration that read the span back is therefore placed on the timeline to
// within ten microseconds, against slices hundreds of microseconds long. Two anchors and a slope would
// beat that and would also be a clock discipline in a file whose subject is a device; the fix if it is
// ever needed is to sample more often, not to model.
struct GpuCalibration
{
	Instant Host{};
	std::uint64_t Device = 0;

	[[nodiscard]] constexpr bool IsValid() const noexcept { return Host != Instant{}; }
};

// What the GPU's memory looks like from inside this process: how much of the device-local heap gyro
// is holding, and how much the driver is currently willing to let it hold.
//
// **Two numbers rather than one, because either alone is unreadable.** Usage on its own does not say
// whether it is a lot — a gigabyte is nothing on a discrete part and everything on a laptop sharing
// memory with a browser — and a budget on its own does not say how much of it is spoken for. What a
// person reading a stutter actually asks is *how close was this to being paged out*, and that is the
// distance between them.
//
// **A zero budget is the absence of an answer rather than a heap with nothing in it.** No driver
// reports a device-local heap it will lend nothing of, so zero is what an extension that is listed and
// unanswered looks like, and `IsValid` is the one place that reading is made.
struct GpuMemory
{
	std::uint64_t UsageBytes = 0;
	std::uint64_t BudgetBytes = 0;

	[[nodiscard]] constexpr bool IsValid() const noexcept { return BudgetBytes != 0; }

	// What is left before the driver starts taking memory back, and it is signed on purpose: a process
	// is permitted to be over its budget, and that is precisely the state a reader is hunting for. An
	// unsigned difference would wrap it into sixteen exabytes of headroom on the one frame the number
	// exists to explain.
	[[nodiscard]] constexpr std::int64_t HeadroomBytes() const noexcept
	{
		return static_cast<std::int64_t>(BudgetBytes) - static_cast<std::int64_t>(UsageBytes);
	}
};

// Bytes as a counter row reads them. A trace carries an integer and Perfetto draws it without a unit,
// so a heap in bytes is eleven digits of axis nobody can compare two of by eye; mebibytes is the
// granularity a texture atlas moves in and the one a person already thinks in. Truncating rather than
// rounding, in both directions from zero, because the figure is a scale rather than a total.
[[nodiscard]] constexpr std::int64_t Mebibytes(std::int64_t bytes) noexcept
{
	return bytes / (std::int64_t{ 1 } << 20);
}

// The heap a composite is drawn out of, picked from everything the device reports.
//
// **The largest device-local heap, and reporting every heap was the alternative.** A machine has two
// to four of them — device-local, host-visible, and on a discrete part a small host-visible window into
// device memory — and a row per heap is four rows a reader has to know the memory model to interpret.
// Only one of them can evict a texture mid-composite, and it is the big device-local one on every part
// gyro runs on: integrated devices report a single device-local heap that is system memory, and
// discrete ones report the board's memory as the largest. Where nothing is device-local — a software
// device, which has no budget to be over — this reports the invalid reading rather than falling back to
// a host heap, because a heap the compositor cannot be evicted from answers a question nobody asked.
[[nodiscard]] constexpr GpuMemory DeviceLocalMemory(
	const VkPhysicalDeviceMemoryProperties& heaps,
	const VkPhysicalDeviceMemoryBudgetPropertiesEXT& budget
) noexcept
{
	GpuMemory reading;
	VkDeviceSize largest = 0;

	const std::uint32_t count =
		heaps.memoryHeapCount < VK_MAX_MEMORY_HEAPS ? heaps.memoryHeapCount : std::uint32_t{ VK_MAX_MEMORY_HEAPS };

	for (std::uint32_t index = 0; index < count; ++index)
	{
		if ((heaps.memoryHeaps[index].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0)
		{
			continue;
		}

		if (heaps.memoryHeaps[index].size <= largest)
		{
			continue;
		}

		largest = heaps.memoryHeaps[index].size;
		reading = GpuMemory{ .UsageBytes = budget.heapUsage[index], .BudgetBytes = budget.heapBudget[index] };
	}

	return reading;
}

// The same conversion with the anchor as one argument, which is how every caller has it.
[[nodiscard]] constexpr Instant
TimestampAt(const DeviceDescription& description, const GpuCalibration& anchor, std::uint64_t ticks) noexcept
{
	return description.TimestampAt(anchor.Host, anchor.Device, ticks);
}

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
		  m_QueueFamily{ std::exchange(other.m_QueueFamily, 0) }, m_Description{ other.m_Description },
		  m_Readable{ std::exchange(other.m_Readable, false) }, m_GpuClock{ std::move(other.m_GpuClock) }
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
			m_Readable = std::exchange(other.m_Readable, false);
			m_GpuClock = std::move(other.m_GpuClock);
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

	// Whether targets on this device were created for Render/Readback.h. See
	// `VulkanDevicePolicy::Readable` for why this is not simply always true.
	[[nodiscard]] bool TargetsAreReadable() const noexcept { return m_Readable; }

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

	// Every DRM modifier this device will sample `code` under, written into `into`, and how many were
	// written.
	//
	// **The plural of `SupportsSampling`, and it exists because somebody has to be *asked* what to
	// offer.** A client allocating a buffer for a compositor picks a format and a modifier off a list
	// the compositor advertised, and `zwp_linux_dmabuf_v1` is that list — so a compositor with only a
	// yes/no predicate can advertise nothing except the pairs it happened to think of, which in
	// practice means linear, which in practice means every GPU client on the machine allocating an
	// untiled buffer and paying for it on every read. This is the query that lets the advertisement be
	// the driver's own answer.
	//
	// `ModifierInvalid` is never written, for `Export`'s reason: unknown is not linear, and a pair
	// gyro offered under it would be one where the client's layout and this device's guess have to
	// agree by luck.
	//
	// Truncated silently at `into.size()`, because a caller that gave a fixed array asked for as many
	// as fit and a driver listing more than that has already listed the ones anybody uses.
	[[nodiscard]] std::size_t SamplingModifiers(std::uint32_t code, std::span<std::uint64_t> into) const noexcept;

	// Whether this device will hand out a dmabuf for an image of this format at all.
	//
	// **A separate question from `Supports`, and lavapipe is why it had to become one.** That asks
	// whether a target can be *drawn into*; this asks whether one can be *produced*, and the two come
	// apart on any device that imports without exporting. lavapipe is exactly that device —
	// `vkGetPhysicalDeviceImageFormatProperties2` reports `IMPORTABLE` without `EXPORTABLE` for every
	// format and both tilings — so a renderer that composites perfectly well cannot allocate the thing
	// it composites into.
	//
	// **It is asked of the driver rather than inferred**, which is decision 108's rule for the third
	// time on this device: the extensions are all listed, the memory type is importable, and the only
	// thing that says no is the capability query. Render/Allocator.h is the caller, so that a provider
	// which cannot produce a buffer declines by name and the composition root walks to the next rung
	// rather than consulting a bit about the device.
	//
	// The extent is not part of the question — a size too large for the device is a failure of the
	// allocation rather than of the capability — so this asks about a nominal one and
	// Docs/Decisions.md decision 151's chain does the rest.
	[[nodiscard]] bool Exports(PixelFormat format) const noexcept;

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

	// Create an image over memory *somebody else laid out*, ready for that memory to be bound to it.
	//
	// **One function for two callers because the half that is easy to get wrong is the half they
	// share.** A target coming back from a presenter and a client's buffer coming in over
	// `zwp_linux_dmabuf_v1` are the same problem — an allocation this device did not make, whose
	// offset and stride are facts rather than choices — and the rule for both is that the layout is
	// *stated*, never picked. Asking the driver to choose a tiling for memory it did not allocate
	// produces an image that imports cleanly and reads at the wrong pitch, which reaches a person as a
	// window sheared diagonally across the screen and reaches a log as nothing at all.
	//
	// Where `Description().StatesModifiers` is true that statement is
	// `VkImageDrmFormatModifierExplicitCreateInfoEXT` and the driver is bound by it. Where it is
	// false there is no way to say it, so the image is created `VK_IMAGE_TILING_LINEAR` and the
	// driver's own layout is *read back and compared* — same offset, same row pitch, or the import is
	// refused. That is the whole of what the extension's absence costs, and it is checked rather than
	// assumed because the two layouts agreeing is a property of the driver rather than of the spec.
	//
	// The image is the caller's from here: every failure destroys nothing and returns, and a success
	// hands back a handle the caller must `vkDestroyImage`. `EINVAL` where the format is not one
	// Render/Vulkan.h names, where the plane's offset is one a linear import cannot honour, or where
	// the driver's layout disagreed.
	//
	// **The extent is Vulkan's own type rather than one of Geometry/Space.h's**, because the two
	// callers do not agree on the space and neither is wrong: a composite target is measured in device
	// pixels and a client's buffer in buffer pixels. What this function does with the number is name
	// an image's texel grid, which is the one place the distinction has genuinely been discharged.
	[[nodiscard]] Result<VkImage>
	ImportImage(VkExtent2D size, PixelFormat format, const DmabufPlane& plane, VkImageUsageFlags usage) const;

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

	// Both clocks read at once, for a caller that has device timestamps and wants them on the wall.
	//
	// **On the frame thread and therefore rate-limited by its caller rather than by this.** Ten
	// microseconds is nothing against a refresh and everything against nothing, so the renderer asks
	// only while a trace ring is armed — which is decision 139's shape throughout: the instrument is
	// continuous, and the parts of it that cost something are the parts a person switched on.
	//
	// `ENOTSUP` where `Description().CalibratesTimestamps` is false, which is the same answer asked in
	// advance and the reason a caller that checks it will never see this.
	[[nodiscard]] Result<GpuCalibration> Calibrate() const;

	// The GPU's clock pair now — what the part is doing and what it has been told to do, in MHz, or zero
	// for a half that could not be read. Decision 142's operating point, the number a `GpuCost` span in
	// seconds is meaningless without, and its companion the parked case cannot be told apart without.
	//
	// **Two `pread`s of small sysfs attributes, so `Record` calls it every frame and pays about a
	// microsecond and a half for it.** Not `const` because the last good value per half is state kept
	// across a momentarily failing read; unlike `Calibrate`, this does not gate on a description bit,
	// because an unreadable clock is a zero rather than an error a caller acts on.
	[[nodiscard]] GpuClock::Reading ReadClock() noexcept { return m_GpuClock.Read(); }

	// What this process is holding of the GPU's memory and what it is currently allowed to hold, or an
	// invalid reading where the device does not report it.
	//
	// **On the frame thread, and asked only while a trace ring is armed** — the same rule `Calibrate`
	// is under and for the same reason. Nothing schedules on this: Frame/Budget.h admits a composite on
	// what it cost and what the clock was, and a `vkGetPhysicalDeviceMemoryProperties2` every frame on a
	// machine nobody is looking at is a driver call bought for nothing. It allocates nothing and takes
	// no lock, which is what makes it callable from inside Core/FrameSection.h's guard at all.
	[[nodiscard]] GpuMemory ReadMemory() const noexcept;

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

	// `VulkanDevicePolicy::Readable` as it was asked for, kept because target usage is decided long
	// after the policy has gone out of scope — at every bind and at every modifier query.
	bool m_Readable = false;

	// The per-driver frequency reader, resolved from `m_Description.PrimaryMinor` at `Open`. Invalid on
	// a device gyro cannot read a clock off — `Blit`'s never gets one, since `Blit` is not a
	// `VulkanDevice` — and then `ReadClock` answers zeros.
	GpuClock m_GpuClock;
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
static_assert(!DeviceDescription{}.CalibratesTimestamps);
static_assert(!DeviceDescription{}.CountsPipelineStatistics);
static_assert(!DeviceDescription{}.ReportsMemoryBudget);

// A reading nobody filled in claims no budget, so the renderer emits no memory counters rather than a
// pair of zeros a reader would have to recognise as *not measured* separately from *nothing left*.
static_assert(!GpuMemory{}.IsValid());
static_assert(GpuMemory{ .UsageBytes = 3, .BudgetBytes = 1 }.HeadroomBytes() == -2, "Over budget is a negative");
static_assert(Mebibytes(-(std::int64_t{ 3 } << 20)) == -3);
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

// The offset a calibration is read through, in both directions. A batch that finished before the
// clocks were last read together is behind the anchor, and reading that as the counter's whole period
// less a millisecond would draw the composite fifty-nine minutes into the future.
static_assert(Detail::TigerLake.TimestampOffset(1000, 1000 + 19200) > std::chrono::microseconds{ 999 });
static_assert(Detail::TigerLake.TimestampOffset(1000 + 19200, 1000) < -std::chrono::microseconds{ 999 });
static_assert(
	Detail::TigerLake.TimestampOffset(9, (std::uint64_t{ 1 } << 36) - 10) == -std::chrono::nanoseconds{ 989 }
);
