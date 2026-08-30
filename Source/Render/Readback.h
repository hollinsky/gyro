#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Render/Device.h"
#include "Render/Vulkan.h"
#include "Seam/RenderTarget.h"

// Seam/Capture.h's readback on a real device: a composite target copied off the GPU into linear rows.
//
// **It is a `vkCmdCopyImageToBuffer` rather than a mapping, because a scanout image is tiled.** The
// bytes behind a target under `I915_FORMAT_MOD_Y_TILED` are not a picture and no amount of arithmetic
// on this side turns them into one — the tiling is the driver's and only the driver is obliged to
// know it. A copy to a linear buffer makes the driver do the detile, which is the same reason
// Render/Textures.h copies the other way rather than handing a client's pool to the display engine.
//
// **The target has to have been created for it, which is why this is a policy rather than a
// capability.** A copy source needs `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, and usage is fixed at
// allocation — so a target that was not allocated for readback cannot acquire one later. Asking for
// the bit unconditionally would change which modifiers the device and the panel can agree on, on
// every run, in exchange for a verb almost nobody presses; a modifier dropped from that negotiation
// is a window that stops being scanned out directly, which is a real regression to pay for a
// debugging convenience. So `VulkanDevicePolicy::Readable` is off unless the composition root was
// asked for captures, and the chord says so rather than failing quietly — which is the shape
// `--trace` already has one module over.
//
// **Everything it needs is reserved at `BindTargets` and nothing is allocated per capture.** The
// staging buffer is the size of one target, the command buffer and the fence are one each, and they
// are built where the target set is — off the frame path. `Read` itself runs inside
// Core/FrameSection.h, so a `vkCreateBuffer` there would be decision 36's abort.
class TargetReadbackBuffer
{
public:
	TargetReadbackBuffer() = default;

	~TargetReadbackBuffer() { Reset(); }

	TargetReadbackBuffer(const TargetReadbackBuffer&) = delete;
	TargetReadbackBuffer& operator=(const TargetReadbackBuffer&) = delete;

	// Reserve for a target set of this shape. Idempotent for a shape already reserved, so a rebind
	// that did not change the mode costs nothing; a different shape frees and rebuilds.
	//
	// Called from `BindTargets` and never from the frame path.
	[[nodiscard]] Result<void> Open(VulkanDevice& device, PixelSize<DeviceSpace> size, PixelFormat format);

	void Reset() noexcept;

	[[nodiscard]] bool IsOpen() const noexcept { return m_Buffer != VK_NULL_HANDLE; }

	// Copy `image` out and write its rows into `into` at `stride` bytes per row.
	//
	// **`image` must already be idle.** The wait for the composite's own work is the caller's, because
	// the caller is the party holding the timeline — see `VulkanRenderer::ReadTarget`. What this waits
	// for is only its own copy.
	//
	// `layout` is what the target is in when the copy is submitted, which for a composite that has
	// been recorded and presented is whatever `Record` left it in; naming it here rather than assuming
	// keeps this honest against a renderer that changes its mind about the final transition.
	[[nodiscard]] Result<void>
	Read(VkImage image, VkImageLayout layout, std::span<std::byte> into, std::uint32_t stride) noexcept;

private:
	VulkanDevice* m_Device = nullptr;

	VkBuffer m_Buffer = VK_NULL_HANDLE;
	VkDeviceMemory m_Memory = VK_NULL_HANDLE;
	std::byte* m_Mapped = nullptr;
	VkDeviceSize m_Length = 0;

	VkCommandPool m_Pool = VK_NULL_HANDLE;
	VkCommandBuffer m_Command = VK_NULL_HANDLE;
	VkFence m_Fence = VK_NULL_HANDLE;

	PixelSize<DeviceSpace> m_Size{};
	PixelFormat m_Format{};
	std::uint32_t m_Stride = 0;
};
