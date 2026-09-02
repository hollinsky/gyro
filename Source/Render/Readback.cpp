#include "Render/Readback.h"

#include <cstring>

#include "Seam/Pixel.h"

Result<void> TargetReadbackBuffer::Open(VulkanDevice& device, PixelSize<DeviceSpace> size, PixelFormat format)
{
	if (IsOpen() && m_Device == &device && m_Size == size && m_Format == format)
	{
		return {};
	}

	Reset();

	const std::uint32_t bytesPerPixel = DecodableBytesPerPixel(format.Code);

	if (bytesPerPixel == 0 || !size.IsValid() || size.IsEmpty())
	{
		return Failure(EINVAL, "a target readback wants a real extent and a decodable sample width");
	}

	m_Device = &device;
	m_Size = size;
	m_Format = format;
	m_Stride = static_cast<std::uint32_t>(size.Width) * bytesPerPixel;
	m_Length = static_cast<VkDeviceSize>(m_Stride) * static_cast<VkDeviceSize>(size.Height);

	const VkBufferCreateInfo bufferInfo{ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		                                 .pNext = nullptr,
		                                 .flags = 0,
		                                 .size = m_Length,
		                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		                                 .queueFamilyIndexCount = 0,
		                                 .pQueueFamilyIndices = nullptr };

	if (const Result<void> made =
	        Check(vkCreateBuffer(device.Handle(), &bufferInfo, nullptr, &m_Buffer), "vkCreateBuffer");
	    !made)
	{
		Reset();

		return made;
	}

	VkMemoryRequirements requirements{};
	vkGetBufferMemoryRequirements(device.Handle(), m_Buffer, &requirements);

	// Coherent as well as visible, so the read below needs no invalidate. A capture is one buffer once
	// per keypress and the coherent heap is the one every driver has; picking the faster cached type
	// would buy nothing measurable and cost a `vkInvalidateMappedMemoryRanges` that is easy to forget
	// and silent when it is.
	const std::uint32_t type = device.MemoryType(
		requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
	);

	if (type == MemoryTypeNone)
	{
		Reset();

		return Failure(ENOTSUP, "this device has no host-visible memory to read a target back into");
	}

	const VkMemoryAllocateInfo allocation{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		                                   .pNext = nullptr,
		                                   .allocationSize = requirements.size,
		                                   .memoryTypeIndex = type };

	if (const Result<void> got =
	        Check(vkAllocateMemory(device.Handle(), &allocation, nullptr, &m_Memory), "vkAllocateMemory");
	    !got)
	{
		Reset();

		return got;
	}

	if (const Result<void> bound =
	        Check(vkBindBufferMemory(device.Handle(), m_Buffer, m_Memory, 0), "vkBindBufferMemory");
	    !bound)
	{
		Reset();

		return bound;
	}

	// Mapped once and left mapped for the life of the reservation, which is what keeps `Read` free of
	// a `vkMapMemory` on the frame path.
	void* mapped = nullptr;

	if (const Result<void> map =
	        Check(vkMapMemory(device.Handle(), m_Memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
	    !map)
	{
		Reset();

		return map;
	}

	m_Mapped = static_cast<std::byte*>(mapped);

	// Its own pool rather than the renderer's, because the renderer resets its own per target and a
	// capture's command buffer must not be recycled underneath a copy that is still in flight.
	const VkCommandPoolCreateInfo poolInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		                                    .pNext = nullptr,
		                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		                                    .queueFamilyIndex = device.QueueFamily() };

	if (const Result<void> made =
	        Check(vkCreateCommandPool(device.Handle(), &poolInfo, nullptr, &m_Pool), "vkCreateCommandPool");
	    !made)
	{
		Reset();

		return made;
	}

	const VkCommandBufferAllocateInfo commandInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		                                           .pNext = nullptr,
		                                           .commandPool = m_Pool,
		                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		                                           .commandBufferCount = 1 };

	if (const Result<void> got =
	        Check(vkAllocateCommandBuffers(device.Handle(), &commandInfo, &m_Command), "vkAllocateCommandBuffers");
	    !got)
	{
		Reset();

		return got;
	}

	const VkFenceCreateInfo fenceInfo{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0 };

	if (const Result<void> made = Check(vkCreateFence(device.Handle(), &fenceInfo, nullptr, &m_Fence), "vkCreateFence");
	    !made)
	{
		Reset();

		return made;
	}

	return {};
}

void TargetReadbackBuffer::Reset() noexcept
{
	if (m_Device == nullptr)
	{
		return;
	}

	const VkDevice device = m_Device->Handle();

	if (m_Fence != VK_NULL_HANDLE)
	{
		vkDestroyFence(device, m_Fence, nullptr);
		m_Fence = VK_NULL_HANDLE;
	}

	// The pool takes the command buffer with it, so there is no second free — freeing it first and then
	// destroying the pool is legal and is one more call that can be got wrong on a teardown path.
	if (m_Pool != VK_NULL_HANDLE)
	{
		vkDestroyCommandPool(device, m_Pool, nullptr);
		m_Pool = VK_NULL_HANDLE;
		m_Command = VK_NULL_HANDLE;
	}

	if (m_Memory != VK_NULL_HANDLE)
	{
		if (m_Mapped != nullptr)
		{
			vkUnmapMemory(device, m_Memory);
			m_Mapped = nullptr;
		}

		vkFreeMemory(device, m_Memory, nullptr);
		m_Memory = VK_NULL_HANDLE;
	}

	if (m_Buffer != VK_NULL_HANDLE)
	{
		vkDestroyBuffer(device, m_Buffer, nullptr);
		m_Buffer = VK_NULL_HANDLE;
	}

	m_Length = 0;
	m_Stride = 0;
	m_Size = {};
	m_Format = {};
	m_Device = nullptr;
}

Result<void> TargetReadbackBuffer::Read(
	VkImage image,
	VkImageLayout layout,
	PixelSize<DeviceSpace> extent,
	std::span<std::byte> into,
	std::uint32_t stride
) noexcept
{
	if (!IsOpen())
	{
		return Failure(EINVAL, "reading an image back through a reservation that was never opened");
	}

	if (!extent.IsValid() || extent.IsEmpty() || extent.Width > m_Size.Width || extent.Height > m_Size.Height)
	{
		return Failure(EINVAL, "reading back an image larger than the reservation was sized for");
	}

	// The staging buffer is packed at the copy's own width rather than the reservation's, because
	// `bufferRowLength` zero means *tight for this region*. So the row a `memcpy` below moves is this
	// image's, not the panel's, and a window narrower than the screen is not read out with the
	// neighbouring garbage after it.
	const std::size_t rows = static_cast<std::size_t>(extent.Height);
	const std::uint32_t packed =
		m_Stride / static_cast<std::uint32_t>(m_Size.Width) * static_cast<std::uint32_t>(extent.Width);

	if (stride < packed || into.size() < rows * stride)
	{
		return Failure(EINVAL, "reading an image back into a slab too small to hold it");
	}

	const VkDevice device = m_Device->Handle();

	if (const Result<void> reset = Check(vkResetFences(device, 1, &m_Fence), "vkResetFences"); !reset)
	{
		return reset;
	}

	const VkCommandBufferBeginInfo begin{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		                                  .pNext = nullptr,
		                                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		                                  .pInheritanceInfo = nullptr };

	if (const Result<void> began = Check(vkBeginCommandBuffer(m_Command, &begin), "vkBeginCommandBuffer"); !began)
	{
		return began;
	}

	// **An acquire from `VK_QUEUE_FAMILY_FOREIGN_EXT` and a release back to it, with no layout change
	// on either half.** Render/Renderer.cpp keeps a composite in `VK_IMAGE_LAYOUT_GENERAL` for its
	// whole life and hands it to the foreign family when it is done, because the next reader is a
	// display controller rather than a Vulkan queue. This copy is a second such reader and takes the
	// image the same way the renderer's own next frame does; a transition to
	// `TRANSFER_SRC_OPTIMAL` would be a release whose acquire states a different layout, which the
	// spec does not permit and which on a real driver is a frame of garbage rather than an error.
	const VkImageMemoryBarrier acquire{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.pNext = nullptr,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = layout,
		.newLayout = layout,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		.dstQueueFamilyIndex = m_Device->QueueFamily(),
		.image = image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};

	vkCmdPipelineBarrier(
		m_Command,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0,
		0,
		nullptr,
		0,
		nullptr,
		1,
		&acquire
	);

	// `bufferRowLength` zero means tightly packed at the copy's own width, which is what the staging
	// buffer was sized for. The destination stride the caller asked for is applied on the way out of
	// the mapping rather than here, so a slab wider than the picture is one memcpy per row rather than
	// a second copy region.
	const VkBufferImageCopy region{
		.bufferOffset = 0,
		.bufferRowLength = 0,
		.bufferImageHeight = 0,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageOffset = { 0, 0, 0 },
		.imageExtent = { static_cast<std::uint32_t>(extent.Width), static_cast<std::uint32_t>(extent.Height), 1 },
	};

	vkCmdCopyImageToBuffer(m_Command, image, layout, m_Buffer, 1, &region);

	const VkImageMemoryBarrier release{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.pNext = nullptr,
		.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.dstAccessMask = 0,
		.oldLayout = layout,
		.newLayout = layout,
		.srcQueueFamilyIndex = m_Device->QueueFamily(),
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		.image = image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};

	vkCmdPipelineBarrier(
		m_Command,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0,
		0,
		nullptr,
		0,
		nullptr,
		1,
		&release
	);

	if (const Result<void> ended = Check(vkEndCommandBuffer(m_Command), "vkEndCommandBuffer"); !ended)
	{
		return ended;
	}

	const VkSubmitInfo submit{ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		                       .pNext = nullptr,
		                       .waitSemaphoreCount = 0,
		                       .pWaitSemaphores = nullptr,
		                       .pWaitDstStageMask = nullptr,
		                       .commandBufferCount = 1,
		                       .pCommandBuffers = &m_Command,
		                       .signalSemaphoreCount = 0,
		                       .pSignalSemaphores = nullptr };

	if (const Result<void> sent = Check(vkQueueSubmit(m_Device->Queue(), 1, &submit, m_Fence), "vkQueueSubmit"); !sent)
	{
		return sent;
	}

	// **The stall Seam/Capture.h promises, bounded so that a wedged device does not take the frame
	// thread with it.** A second is far past any real copy of a screen and far short of a hang nobody
	// notices; `VK_TIMEOUT` comes back as a refusal and the frame that asked for it draws on.
	constexpr std::uint64_t Second = 1'000'000'000;

	if (const Result<void> waited = Check(vkWaitForFences(device, 1, &m_Fence, VK_TRUE, Second), "vkWaitForFences");
	    !waited)
	{
		return waited;
	}

	for (std::size_t y = 0; y < rows; ++y)
	{
		std::memcpy(into.data() + y * stride, m_Mapped + y * packed, packed);
	}

	return {};
}
