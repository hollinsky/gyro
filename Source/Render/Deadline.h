#pragma once

#include <cstdint>
#include <string>

#include "Core/Fd.h"
#include "Core/Time.h"

// The instant the composite has to be finished by, said to the driver that is going to run it.
//
// **This is decision 142's governing half, and the arm of it that is free and always right to send.**
// A frequency governor measures occupancy, and a frame-deadline workload is defined by *not* being
// occupancy-bound — so the healthier gyro is, the less clock it earns, and the part this was written
// against sits parked at 350 MHz of a 1250 MHz range while drawing frames it is finishing in a third
// of a refresh. `dma_fence_set_deadline` is the interface for saying the thing no occupancy
// measurement contains, and its own documentation names *"the start of a compositor's composition
// cycle"* as the case it exists for.
//
// **It costs the frame thread one ioctl and cannot block, which is the property that disqualified
// every other route to the same end.** `drm_syncobj_array_wait_timeout` sets the deadline *before* it
// enters the wait loop, and the loop returns `-ETIME` immediately at a zero timeout — so a
// `DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT` carrying `WAIT_DEADLINE` at `timeout_nsec = 0` is a non-blocking
// poll that states urgency. `ETIME` is the ordinary answer here and not a failure; the composite has
// only just been submitted, so of course it has not finished.
//
// **Stated once per submission rather than on every poll.** Decision 142 observes that re-publishing
// is idempotent, because `drm_sched_fence_set_deadline_finished` keeps the earlier of the two — which
// is exactly why repeating it buys nothing. `IRenderer::IsComplete` is a `vkGetSemaphoreCounterValue`
// on this renderer and enters no ioctl at all, so restating there would *add* a syscall per frame to
// re-send a number the kernel already has.
//
// **On every part gyro runs on today this is a no-op, and it is written anyway.**
// Docs/KernelWishlist.md carries the reading: msm is the only driver that wires the hint to
// frequency, `drm_sched` forwards it to hardware fences that amdgpu and xe do not implement, and i915
// has no plumbing at all. What that no-op buys is that gyro is already saying the true thing when a
// driver starts listening, and that the startup probe beside this has something to probe.
class FenceDeadline
{
public:
	FenceDeadline() = default;

	// Import the renderer's exported timeline onto a DRM node, so a deadline can be named against a
	// point on it.
	//
	// `renderMinor` is `VK_EXT_physical_device_drm`'s answer for the physical device Vulkan chose, and
	// the render node rather than the primary one because every syncobj ioctl is `DRM_RENDER_ALLOW`
	// and nothing here wants master. `timeline` is the descriptor `SyncPoint` already borrows — an
	// `anon_inode:syncobj_file` on the drivers that matter, which is Seam/SyncPoint.h's premise and the
	// reason this import is a handle lookup rather than a conversion.
	//
	// A negative minor, a node that will not open, and a descriptor the node will not adopt all come
	// up invalid, and `State` on an invalid one does nothing. That is the same honest nothing
	// Render/GpuClock.h reports for a clock it cannot read: a machine gyro cannot state a deadline on
	// is one that draws every frame without stating one.
	[[nodiscard]] static FenceDeadline Open(std::int64_t renderMinor, RawFd timeline);

	// Import onto a node named directly. The seam `Open` resolves onto, and the one a test drives.
	[[nodiscard]] static FenceDeadline OpenNode(const char* path, RawFd timeline);

	[[nodiscard]] bool IsValid() const noexcept { return m_Handle != 0; }

	// Say that the work signalling `point` is wanted by `deadline`. The epoch means the caller has no
	// schedule to state — an output whose clock has not been observed yet — and is not sent.
	//
	// Nothing is reported back, because there is nothing a caller could do with it. The hint is
	// advisory in the kernel by construction: a driver that does not implement `->set_deadline` takes
	// the ioctl and drops the number, which is indistinguishable from success and is the case on every
	// part gyro runs on. A failure here is a frame drawn without a hint, which is the frame gyro would
	// have drawn anyway.
	void State(std::uint64_t point, Instant deadline) noexcept;

	// `/dev/dri/renderD<minor>`. Pure and public so the mapping is testable on a machine with no node
	// to open.
	[[nodiscard]] static std::string NodePath(std::int64_t renderMinor);

	// The absolute `CLOCK_MONOTONIC` nanosecond count the ioctl's `deadline_nsec` field wants, which is
	// Core/Time.h's timeline unchanged — decision 36's *one clock* is what makes this a cast rather
	// than a conversion. Saturated at zero, because the field is unsigned and a deadline before the
	// epoch is not a thing a caller can mean.
	[[nodiscard]] static std::uint64_t Nanoseconds(Instant deadline) noexcept;

private:
	// **No destructor, and the handle is why there does not need to be one.** A syncobj handle lives in
	// the DRM file's own table, so closing the descriptor destroys it — which makes `Fd`'s ownership
	// the whole of this object's lifetime and keeps it movable without writing the four members
	// Core/Fd.h would otherwise oblige.
	Fd m_Device;

	std::uint32_t m_Handle = 0;
};
