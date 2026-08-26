#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Render/Device.h"
#include "Seam/Allocator.h"
#include "Seam/Buffer.h"
#include "Seam/RenderTarget.h"

// The device that is about to draw into a target, asked to allocate it.
//
// **It exists because a nested output has nothing else to allocate with**, which is decision 120. A
// KMS output allocates through GBM and a virtual output through `udmabuf`; a nested one has neither,
// no swapchain, and a parent compositor that states a format and a modifier list and expects buffers
// back. `VulkanDevice::Export` is the machinery — the same modifier query the import path already
// runs, plus `VkExportMemoryAllocateInfo` and one `vkGetMemoryFdKHR` — and this is that machinery
// behind the interface a presenter names, so that `Nested` never names `Render`.
//
// **What it adds over `Export` is the ownership**, and that is the whole of the file. An exported
// image is a `VkImage`, its `VkDeviceMemory`, and a descriptor taken out of them; closing the
// descriptor alone leaves the allocation on the device. Seam/Buffer.h's backing is what carries the
// other two, so a target set released at a window resize releases the memory with it rather than
// growing the device's footprint by a screen every time somebody drags an edge.
//
// **One modifier per call, walked by the caller.** Seam/Allocator.h argues the shape: the host ranks
// the candidates in its dmabuf feedback against its own hardware, the device vetoes, and a nested
// output walks the ranking calling this until one is accepted. So `EINVAL` here is an ordinary answer
// to one candidate rather than the end of a negotiation, and the modifier that comes back is always
// the one that was asked for — `Export` reads the layout back off the driver and refuses a
// substitution, which is what a parent compositor importing the buffer is entitled to rely on.
//
// **No mapping, ever.** A device allocation is not host memory and `Pixels()` on one of these is
// empty, which is exactly what makes a `Blit` bound to a nested output fail at `BindTargets` rather
// than composite into nothing. Virtual/Heap.h is the allocator for the renderer that writes with the
// CPU.
class VulkanAllocator final : public IDmabufAllocator
{
public:
	// The device is a reference for `VirtualOutput`'s reason one layer up: it is neither copyable nor
	// movable, one device serves every output on a machine, and the composition root owns it. It
	// outlives this, and this outlives every buffer it handed out — which is Render/Device.h's rule
	// that a Vulkan handle is destroyed before the device that made it, restated at the one place a
	// presenter could get it wrong.
	explicit VulkanAllocator(const VulkanDevice& device) noexcept : m_Device{ &device } {}

	[[nodiscard]] Result<DmabufBuffer>
	Allocate(PixelSize<DeviceSpace> size, std::uint32_t code, std::span<const std::uint64_t> modifiers) override;

	// **Both halves, because a device that renders into a format need not hand one out.** This used to
	// ask only whether the device could draw into the format, on the reading that a renderer's
	// allocator and its renderer agree by construction. They do not: lavapipe renders into linear and
	// exports nothing, so the one-sided answer was a provider promising a buffer it would then refuse
	// to allocate — which reached the panel as an output that failed to build its targets and named
	// the modifier list as the culprit. Decision 151's chain needs a provider's `Supports` to mean
	// *I can produce this*, since that is what selects a rung.
	[[nodiscard]] bool Supports(PixelFormat format) const noexcept override
	{
		return m_Device->Supports(format) && m_Device->Exports(format);
	}

	[[nodiscard]] std::string_view Name() const noexcept override { return "vulkan export"; }

	// How many images this has handed out, matching Virtual/Heap.h's counter and there for the same
	// reason: a test that wants to say a reconfiguration really did build a new set.
	std::uint64_t Allocations = 0;

private:
	const VulkanDevice* m_Device = nullptr;
};
