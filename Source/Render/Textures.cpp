#include "Render/Textures.h"

#include <fcntl.h>
#include <unistd.h>

#include <bit>
#include <cerrno>
#include <cstring>

#include "Seam/Pixel.h"

namespace
{
// What a sampled image is created with, on both arms.
//
// `SAMPLED` is the point. `HOST_TRANSFER` is added by the mapped arm alone, because it is the bit
// `vkCopyMemoryToImage` requires and asking for it on an imported dmabuf would be asking the driver
// to lay out memory it did not lay out.
constexpr VkImageUsageFlags SampledUsage = VK_IMAGE_USAGE_SAMPLED_BIT;

// What a capture adds, and it is asked for only where the composition root asked for captures.
//
// **A usage bit narrows what a driver will agree to, which is why this is a policy and not a
// constant.** For an imported dmabuf it is the modifier set: a client's layout that this device can
// sample but cannot copy out of is one `ImportImage` refuses once the bit is on, and the client's
// window then falls back to `wl_shm` on every run of every session — a real regression to pay for a
// verb that fires when somebody is looking for a bug. Render/Readback.h makes the identical argument
// for a scanout target and `--capture` is the identical switch.
[[nodiscard]] VkImageUsageFlags CaptureUsage(const VulkanDevice& device) noexcept
{
	return device.TargetsAreReadable() ? VkImageUsageFlags{ VK_IMAGE_USAGE_TRANSFER_SRC_BIT } : VkImageUsageFlags{ 0 };
}

// The layout every adopted texture lives in, from the end of `Adopt` until it is destroyed.
//
// **`GENERAL` rather than `SHADER_READ_ONLY_OPTIMAL`, and the imported arm is why.** A client's
// dmabuf comes back from `VK_QUEUE_FAMILY_FOREIGN_EXT` on every frame that samples it, and a
// queue-family acquire has to name the same layout on both halves of the pair — which is exactly the
// argument Render/Renderer.cpp's `Transfer` already makes for render targets. Using one layout for
// both arms means the frame side has one rule rather than a branch on where the pixels came from,
// and what `GENERAL` costs on the arm that does not need it is a tiling the driver may decline to
// compress. For an image that is written once and read every frame, that is not a trade worth a
// second code path.
constexpr VkImageLayout TextureLayout = VK_IMAGE_LAYOUT_GENERAL;
} // namespace

VulkanTextures::~VulkanTextures()
{
	if (m_Device == nullptr || !m_Device->IsValid())
	{
		return;
	}

	// Everything at once and with no regard for what is in flight, because the only caller is a
	// device being torn down and Docs/Architecture.md#device-migration has the composition root
	// destroy the renderers first. The wait is what makes that ordering safe rather than assumed:
	// a renderer destroyed with work still queued has already waited, and one that has not is a bug
	// this would otherwise turn into a driver crash in somebody else's stack.
	vkDeviceWaitIdle(m_Device->Handle());

	for (std::uint32_t index = 0; index < m_DoomedCount; ++index)
	{
		Release(m_Doomed[index]);
	}

	m_DoomedCount = 0;

	for (std::uint32_t index = 0; index < m_Count; ++index)
	{
		Doomed live{ .Handle = m_Images[index].Handle,
			         .Memory = m_Images[index].Memory,
			         .View = m_Images[index].View,
			         .Set = m_Images[index].Set,
			         .At = {},
			         .Count = 0 };

		Release(live);
	}

	m_Count = 0;

	if (m_Pool != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorPool(m_Device->Handle(), m_Pool, nullptr);
		m_Pool = VK_NULL_HANDLE;
	}

	if (m_SetLayout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(m_Device->Handle(), m_SetLayout, nullptr);
		m_SetLayout = VK_NULL_HANDLE;
	}

	if (m_Sampler != VK_NULL_HANDLE)
	{
		vkDestroySampler(m_Device->Handle(), m_Sampler, nullptr);
		m_Sampler = VK_NULL_HANDLE;
	}
}

VulkanTextures::VulkanTextures(VulkanDevice& device) : m_Device{ &device }
{
	m_Status = Build();
}

Result<void> VulkanTextures::Build()
{
	if (m_Device == nullptr || !m_Device->IsValid())
	{
		return Failure(ENODEV, "no device to build a texture table on");
	}

	// **Bilinear and clamped, which is `Blit`'s answer and has to be.** Two renderers must produce
	// one picture: Blit/Blit.cpp argues bilinear at length — a box filter at magnification is
	// nearest-neighbour, and the firmware logo the splash continues is magnified onto every panel
	// larger than the one the GOP drew it at — and clamping to the edge is the same boundary policy
	// its filter taps already apply. A different filter here would make the handoff from the CPU
	// renderer to this one a visible change in sharpness at the exact moment it is supposed to be
	// invisible.
	//
	// No mip levels, so no `maxLod` worth stating. **What reads through this sampler is not one tap.**
	// Render/Shaders/Sample.glsl averages the image over the fragment's own footprint by splitting it
	// into sub-boxes no wider than a texel and evaluating each one through a tap of this filter, which
	// is what makes a bilinear sampler the right thing to build here rather than a compromise: the tap
	// is the closed form the box is assembled out of, and its magnification behaviour is untouched
	// because the footprint is floored at a texel.
	//
	// That covers minification out to a fourfold shrink and leaves the chain to the thumbnail, which is
	// the narrower question Docs/Open.md's *mip generation's place in `C`* is now asking: a chain would
	// have to be rebuilt on the dispatch thread at every commit, and it now only has to earn that
	// bandwidth above the shrink the footprint filter stops being exact at.
	const VkSamplerCreateInfo samplerInfo{ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		                                   .pNext = nullptr,
		                                   .flags = 0,
		                                   .magFilter = VK_FILTER_LINEAR,
		                                   .minFilter = VK_FILTER_LINEAR,
		                                   .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		                                   .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		                                   .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		                                   .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		                                   .mipLodBias = 0.0F,
		                                   .anisotropyEnable = VK_FALSE,
		                                   .maxAnisotropy = 1.0F,
		                                   .compareEnable = VK_FALSE,
		                                   .compareOp = VK_COMPARE_OP_ALWAYS,
		                                   .minLod = 0.0F,
		                                   .maxLod = 0.0F,
		                                   .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
		                                   .unnormalizedCoordinates = VK_FALSE };

	if (Result<void> created =
	        Check(vkCreateSampler(m_Device->Handle(), &samplerInfo, nullptr, &m_Sampler), "vkCreateSampler");
	    !created)
	{
		return created;
	}

	// Immutable, so a set is one image write rather than an image and a sampler, and so that no call
	// site can bind a texture through a filter the two renderers have not agreed about.
	const VkDescriptorSetLayoutBinding binding{ .binding = 0,
		                                        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		                                        .descriptorCount = 1,
		                                        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		                                        .pImmutableSamplers = &m_Sampler };
	const VkDescriptorSetLayoutCreateInfo layoutInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		                                              .pNext = nullptr,
		                                              .flags = 0,
		                                              .bindingCount = 1,
		                                              .pBindings = &binding };

	if (Result<void> created = Check(
			vkCreateDescriptorSetLayout(m_Device->Handle(), &layoutInfo, nullptr, &m_SetLayout),
			"vkCreateDescriptorSetLayout"
		);
	    !created)
	{
		return created;
	}

	// `FREE_DESCRIPTOR_SET`, because a set outlives the frame that used it and is given back one at a
	// time. The alternative is resetting the whole pool, which is a thing that can only happen when
	// no window on the machine is drawing.
	const VkDescriptorPoolSize size{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		                             .descriptorCount = MaxTextureImages };
	const VkDescriptorPoolCreateInfo poolInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		                                       .pNext = nullptr,
		                                       .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
		                                       .maxSets = MaxTextureImages,
		                                       .poolSizeCount = 1,
		                                       .pPoolSizes = &size };

	return Check(vkCreateDescriptorPool(m_Device->Handle(), &poolInfo, nullptr, &m_Pool), "vkCreateDescriptorPool");
}

void VulkanTextures::Attach(ITextureFence& fence) noexcept
{
	if (m_FenceCount >= MaxTextureFences)
	{
		return;
	}

	m_Fences[m_FenceCount] = &fence;
	++m_FenceCount;
}

void VulkanTextures::Detach(ITextureFence& fence) noexcept
{
	std::uint32_t found = m_FenceCount;

	for (std::uint32_t index = 0; index < m_FenceCount; ++index)
	{
		if (m_Fences[index] == &fence)
		{
			found = index;

			break;
		}
	}

	if (found == m_FenceCount)
	{
		return;
	}

	// **Everything waiting is released here, and the wait is what makes that correct.** A doomed
	// image's stamp names a value on *this* renderer's timeline, and the renderer is going away — so
	// there is nobody left to ask whether that value has landed, and the stamps in the other slots
	// would go on being compared against a shifting list. Waiting for the device is heavy and this
	// runs at a device migration or a shutdown, which is already the one moment a stall is invisible.
	if (m_DoomedCount > 0 && m_Device != nullptr && m_Device->IsValid())
	{
		vkDeviceWaitIdle(m_Device->Handle());

		for (std::uint32_t index = 0; index < m_DoomedCount; ++index)
		{
			Release(m_Doomed[index]);
		}

		m_DoomedCount = 0;
	}

	m_Fences[found] = m_Fences[m_FenceCount - 1];
	m_Fences[m_FenceCount - 1] = nullptr;
	--m_FenceCount;
}

VulkanTextures::Image* VulkanTextures::Lookup(TextureId id) noexcept
{
	for (std::uint32_t index = 0; index < m_Count; ++index)
	{
		if (m_Images[index].Id == id)
		{
			return &m_Images[index];
		}
	}

	return nullptr;
}

BoundTexture VulkanTextures::Find(TextureId id) const noexcept
{
	if (id.IsNull())
	{
		return {};
	}

	// `m_Count` is read once. The dispatch thread can only grow it into a slot this walk has not
	// reached or shrink it past one it has, and either way what is compared is a whole `TextureId` —
	// so a slot caught mid-move fails the generation test and reads as a stale id, which is exactly
	// what it is.
	const std::uint32_t count = m_Count;

	for (std::uint32_t index = 0; index < count; ++index)
	{
		const Image& image = m_Images[index];

		if (image.Id != id)
		{
			continue;
		}

		return { .Set = image.Set, .Foreign = image.Imported ? image.Handle : VK_NULL_HANDLE, .Size = image.Size };
	}

	return {};
}

ReadableTexture VulkanTextures::Readable(TextureId id) const noexcept
{
	if (id.IsNull())
	{
		return {};
	}

	const std::uint32_t count = m_Count;

	for (std::uint32_t index = 0; index < count; ++index)
	{
		const Image& image = m_Images[index];

		// Imported only, per the header: an image this device wrote is a `wl_shm` client's, and that
		// client's pixels reach a capture at the commit that copied them.
		if (image.Id != id || !image.Imported)
		{
			continue;
		}

		return { .Handle = image.Handle, .Size = image.Size, .Format = image.Format };
	}

	return {};
}

Result<void> VulkanTextures::Adopt(TextureId id, const TextureSource& source)
{
	Sweep();

	if (m_Device == nullptr || !m_Device->IsValid() || m_Pool == VK_NULL_HANDLE)
	{
		return Failure(ENODEV, "no device to adopt a texture onto");
	}

	if (id.IsNull())
	{
		return Failure(EINVAL, "a texture cannot be adopted against a null id");
	}

	if (!source.IsValid())
	{
		return Failure(EINVAL, "that source does not describe the image it claims");
	}

	if (VulkanFormat(source.Format.Code) == VK_FORMAT_UNDEFINED)
	{
		return Failure(EINVAL, "this device has no Vulkan format for that fourcc");
	}

	// Built to one side and only committed once it is whole, which is what makes a re-adoption that
	// fails leave the id naming the pixels it named before. The alternative is a window that goes
	// black because its *next* buffer was the one the driver refused.
	Image built{ .Id = id,
		         .Handle = VK_NULL_HANDLE,
		         .Memory = VK_NULL_HANDLE,
		         .View = VK_NULL_HANDLE,
		         .Set = VK_NULL_HANDLE,
		         .Size = source.Size,
		         .Format = source.Format,
		         .Imported = !source.IsMapped() };

	Image* const existing = Lookup(id);

	if (existing == nullptr && m_Count >= MaxTextureImages)
	{
		return Failure(ENOMEM, "no room left in the texture table");
	}

	Result<void> adopted = source.IsMapped() ? AdoptMapped(built, source) : AdoptDmabuf(built, source);

	if (adopted)
	{
		adopted = Describe(built, source.Format);
	}

	if (!adopted)
	{
		Doomed partial{
			.Handle = built.Handle, .Memory = built.Memory, .View = built.View, .Set = built.Set, .At = {}, .Count = 0
		};

		// Nothing ever named it, so nothing can be reading it. This is the one release in the file
		// that needs no deferral at all.
		Release(partial);

		return adopted;
	}

	if (existing != nullptr)
	{
		// Seam/Importer.h's replacing adopt. The image being replaced retires exactly the way a
		// forgotten one does — the caller waited for the watermark before asking, and the device may
		// still be reading what it replaced.
		Doom(*existing);
		*existing = built;

		return {};
	}

	m_Images[m_Count] = built;
	++m_Count;

	return {};
}

Result<void> VulkanTextures::Reserve(TextureId id, PixelSize<BufferSpace> size)
{
	Sweep();

	if (m_Device == nullptr || !m_Device->IsValid() || m_Pool == VK_NULL_HANDLE)
	{
		return Failure(ENODEV, "no device to allocate an image on");
	}

	if (id.IsNull())
	{
		return Failure(EINVAL, "storage cannot be allocated against a null id");
	}

	if (size.IsEmpty() || !size.IsValid())
	{
		return Failure(EINVAL, "an image with no extent");
	}

	// Premultiplied eight-bit, which is what a composite writes and what every item that samples the
	// result already expects. The atlas holds pictures gyro drew rather than pixels a client chose a
	// layout for, so there is no foreign fourcc to honour here and no modifier to relay — this image
	// is never a descriptor anybody outside the device sees.
	//
	// **Built to one side and committed only once it is whole**, for `Adopt`'s reason: a reservation
	// that failed halfway must leave whatever the id named before exactly as it was.
	Image built{ .Id = id,
		         .Handle = VK_NULL_HANDLE,
		         .Memory = VK_NULL_HANDLE,
		         .View = VK_NULL_HANDLE,
		         .Set = VK_NULL_HANDLE,
		         .Size = size,
		         .Format = PixelFormat{ .Code = StorageFormat, .Modifier = ModifierInvalid },
		         .Imported = false };

	Image* const existing = Lookup(id);

	if (existing == nullptr && m_Count >= MaxTextureImages)
	{
		return Failure(ENOMEM, "no room left in the texture table");
	}

	Result<void> allocated = ReserveStorage(built);

	if (allocated)
	{
		allocated = Describe(built, built.Format);
	}

	if (!allocated)
	{
		Doomed partial{
			.Handle = built.Handle, .Memory = built.Memory, .View = built.View, .Set = built.Set, .At = {}, .Count = 0
		};

		Release(partial);

		return allocated;
	}

	if (existing != nullptr)
	{
		// The replacing case is a device rebuild coming back through `TextureRegistry::Rebind`, and what
		// it produces is an empty atlas rather than the one that was there. Decision 46 drops the
		// retiring set on device loss rather than preserving it, so the windows that were leaving finish
		// early on the dispatch side and nothing samples the image that went with the old device.
		Doom(*existing);
		*existing = built;

		return {};
	}

	m_Images[m_Count] = built;
	++m_Count;

	return {};
}

Result<void> VulkanTextures::ReserveStorage(Image& into)
{
	const auto width = static_cast<std::uint32_t>(into.Size.Width);
	const auto height = static_cast<std::uint32_t>(into.Size.Height);

	// **Drawn into and sampled from, which is the whole difference from every other image here.** A
	// snapshot is written by one composite and read by the frames after it, so the storage has to be a
	// colour attachment as well as a texture — and `TextureLayout` is `GENERAL` already, which is what
	// lets one image be both without a transition between the two uses.
	const VkImageCreateInfo imageInfo{ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		                               .pNext = nullptr,
		                               .flags = 0,
		                               .imageType = VK_IMAGE_TYPE_2D,
		                               .format = VulkanFormat(into.Format.Code),
		                               .extent = { width, height, 1 },
		                               .mipLevels = 1,
		                               .arrayLayers = 1,
		                               .samples = VK_SAMPLE_COUNT_1_BIT,
		                               .tiling = VK_IMAGE_TILING_OPTIMAL,
		                               .usage = SampledUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		                               .queueFamilyIndexCount = 0,
		                               .pQueueFamilyIndices = nullptr,
		                               .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

	if (Result<void> created =
	        Check(vkCreateImage(m_Device->Handle(), &imageInfo, nullptr, &into.Handle), "vkCreateImage");
	    !created)
	{
		return created;
	}

	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(m_Device->Handle(), into.Handle, &requirements);

	const std::uint32_t type = m_Device->MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	if (type == MemoryTypeNone)
	{
		return Failure(ENOMEM, "no device-local memory type satisfies an image gyro draws into");
	}

	const VkMemoryAllocateInfo allocation{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		                                   .pNext = nullptr,
		                                   .allocationSize = requirements.size,
		                                   .memoryTypeIndex = type };

	if (Result<void> reserved =
	        Check(vkAllocateMemory(m_Device->Handle(), &allocation, nullptr, &into.Memory), "vkAllocateMemory");
	    !reserved)
	{
		return reserved;
	}

	// **Left in `UNDEFINED` for the recording to move**, rather than transitioned here. Nothing has
	// been written, so there are no contents for a transition to preserve — and the composite that
	// captures into it is the party that knows which subregion it is about to write and in what order.
	// A host transition here would also need `VK_EXT_host_image_copy`, which this path does not
	// otherwise require: a device with no host image copy can still hold an exit atlas, and refusing
	// one there would mean closing windows cut on hardware that could have faded them.
	return Check(vkBindImageMemory(m_Device->Handle(), into.Handle, into.Memory, 0), "vkBindImageMemory");
}

Result<void> VulkanTextures::AdoptMapped(Image& into, const TextureSource& source)
{
	if (!m_Device->SupportsHostTexture(source.Format))
	{
		// Named rather than generic, because the two reasons a caller would act on differently are a
		// driver too old for the extension and a format the driver will not host-fill. Both arrive
		// here and the message is what tells them apart in a log.
		return Failure(
			ENOTSUP,
			m_Device->Description().CopiesFromHost ?
				"this device will not fill an image of that format from host memory" :
				"this device has no VK_EXT_host_image_copy, so a mapped buffer cannot be imported without a queue"
		);
	}

	const MappedPixels& pixels = *source.AsMapped();
	const std::uint32_t depth = DecodableBytesPerPixel(source.Format.Code);

	if (depth == 0 || pixels.Stride % depth != 0)
	{
		return Failure(EINVAL, "that stride is not a whole number of pixels of that format");
	}

	const auto width = static_cast<std::uint32_t>(source.Size.Width);
	const auto height = static_cast<std::uint32_t>(source.Size.Height);

	// The bound the mapping actually has to cover, derived here rather than trusted: a `Length` that
	// is short by one row is a client's mistake or a compositor's, and reading past it is the same
	// crash either way. `MappedPixels` carries the allocation's own length precisely so that this is
	// checkable.
	const std::size_t needed = static_cast<std::size_t>(pixels.Stride) * height;

	if (pixels.Stride < width * depth || pixels.Length < needed)
	{
		return Failure(EINVAL, "that mapping is smaller than the image it describes");
	}

	const VkImageCreateInfo imageInfo{ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		                               .pNext = nullptr,
		                               .flags = 0,
		                               .imageType = VK_IMAGE_TYPE_2D,
		                               .format = VulkanFormat(source.Format.Code),
		                               .extent = { width, height, 1 },
		                               .mipLevels = 1,
		                               .arrayLayers = 1,
		                               .samples = VK_SAMPLE_COUNT_1_BIT,
		                               .tiling = VK_IMAGE_TILING_OPTIMAL,
		                               .usage =
		                                   SampledUsage | VK_IMAGE_USAGE_HOST_TRANSFER_BIT | CaptureUsage(*m_Device),
		                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		                               .queueFamilyIndexCount = 0,
		                               .pQueueFamilyIndices = nullptr,
		                               .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

	if (Result<void> created =
	        Check(vkCreateImage(m_Device->Handle(), &imageInfo, nullptr, &into.Handle), "vkCreateImage");
	    !created)
	{
		return created;
	}

	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(m_Device->Handle(), into.Handle, &requirements);

	const std::uint32_t type = m_Device->MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	if (type == MemoryTypeNone)
	{
		return Failure(ENOMEM, "no device-local memory type satisfies a sampled image");
	}

	const VkMemoryAllocateInfo allocation{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		                                   .pNext = nullptr,
		                                   .allocationSize = requirements.size,
		                                   .memoryTypeIndex = type };

	if (Result<void> allocated =
	        Check(vkAllocateMemory(m_Device->Handle(), &allocation, nullptr, &into.Memory), "vkAllocateMemory");
	    !allocated)
	{
		return allocated;
	}

	if (Result<void> bound =
	        Check(vkBindImageMemory(m_Device->Handle(), into.Handle, into.Memory, 0), "vkBindImageMemory");
	    !bound)
	{
		return bound;
	}

	// Out of `UNDEFINED`, which discards contents that do not exist yet — the one place in this file
	// where that is the right old layout rather than the dangerous one.
	const VkHostImageLayoutTransitionInfo transition{ .sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,
		                                              .pNext = nullptr,
		                                              .image = into.Handle,
		                                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		                                              .newLayout = TextureLayout,
		                                              .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };

	if (Result<void> moved =
	        Check(vkTransitionImageLayoutEXT(m_Device->Handle(), 1, &transition), "vkTransitionImageLayoutEXT");
	    !moved)
	{
		return moved;
	}

	// `memoryRowLength` is in *texels* where the seam's stride is in bytes, which is the one
	// conversion in this function and the one that produces a sheared picture if it is forgotten.
	const VkMemoryToImageCopy region{ .sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY,
		                              .pNext = nullptr,
		                              .pHostPointer = pixels.Pixels,
		                              .memoryRowLength = pixels.Stride / depth,
		                              .memoryImageHeight = height,
		                              .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		                              .imageOffset = { 0, 0, 0 },
		                              .imageExtent = { width, height, 1 } };
	const VkCopyMemoryToImageInfo copy{ .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO,
		                                .pNext = nullptr,
		                                .flags = 0,
		                                .dstImage = into.Handle,
		                                .dstImageLayout = TextureLayout,
		                                .regionCount = 1,
		                                .pRegions = &region };

	return Check(vkCopyMemoryToImageEXT(m_Device->Handle(), &copy), "vkCopyMemoryToImageEXT");
}

Result<void> VulkanTextures::AdoptDmabuf(Image& into, const TextureSource& source)
{
	if (!m_Device->SupportsSampling(source.Format))
	{
		return Failure(EINVAL, "this device cannot sample that format under that modifier");
	}

	const DmabufImage& image = *source.AsDmabuf();

	// One plane, which `SupportsSampling` has already agreed to for this modifier. Checked again for
	// `VulkanRenderer::Import`'s reason: the driver said the *tiling* is one plane and this says the
	// *description* is, and the two facts have different owners. A planar client buffer — decision
	// 51's video surface — needs a sampler conversion and a second view, which is a format question
	// rather than a plumbing one and is not answered here.
	if (image.PlaneCount != 1)
	{
		return Failure(EINVAL, "this importer samples one plane; a planar buffer needs a format conversion");
	}

	const DmabufPlane& plane = image.Planes[0];

	// Render/Device.h owns the create info, and what it carries for this caller is the rule that this
	// is memory somebody else laid out: the layout is stated rather than chosen, and on a device that
	// cannot state one it is read back and compared instead. A client's buffer is the case that check
	// exists for — gyro chose neither its stride nor its allocator.
	Result<VkImage> imported = m_Device->ImportImage(
		{ static_cast<std::uint32_t>(source.Size.Width), static_cast<std::uint32_t>(source.Size.Height) },
		source.Format,
		plane,
		SampledUsage | CaptureUsage(*m_Device)
	);

	if (!imported)
	{
		return std::unexpected{ imported.error() };
	}

	into.Handle = *imported;

	const std::uint32_t types = m_Device->ImportableMemoryTypes(plane.Descriptor);

	if (types == 0)
	{
		return Failure(EINVAL, "this device cannot import that descriptor as a dmabuf");
	}

	// Duplicated because `vkAllocateMemory` takes ownership of the descriptor it is handed, and
	// Seam/Importer.h's is the caller's — it belongs to whoever demarshalled the buffer and stays
	// valid until `Forget`. Importing the original would have the driver close a number the client's
	// connection still holds.
	const int duplicated = fcntl(plane.Descriptor.Value, F_DUPFD_CLOEXEC, 0);

	if (duplicated < 0)
	{
		return Failure(errno, "could not duplicate the client's descriptor for import");
	}

	const VkImportMemoryFdInfoKHR importInfo{ .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		                                      .pNext = nullptr,
		                                      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		                                      .fd = duplicated };
	const VkMemoryDedicatedAllocateInfo dedicatedInfo{ .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		                                               .pNext = &importInfo,
		                                               .image = into.Handle,
		                                               .buffer = VK_NULL_HANDLE };

	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(m_Device->Handle(), into.Handle, &requirements);

	const std::uint32_t usable = requirements.memoryTypeBits & types;

	if (usable == 0)
	{
		close(duplicated);

		return Failure(EINVAL, "no memory type satisfies both the image and the client's descriptor");
	}

	const VkMemoryAllocateInfo allocateInfo{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		                                     .pNext = &dedicatedInfo,
		                                     .allocationSize = requirements.size,
		                                     .memoryTypeIndex = static_cast<std::uint32_t>(std::countr_zero(usable)) };

	if (Result<void> allocated =
	        Check(vkAllocateMemory(m_Device->Handle(), &allocateInfo, nullptr, &into.Memory), "vkAllocateMemory");
	    !allocated)
	{
		// The descriptor becomes the driver's only on success, so a refused import leaves it ours to
		// close. Leaking it here exhausts the table across a few hundred rejected commits.
		close(duplicated);

		return allocated;
	}

	// **Nothing transitions the layout here, and that is the arm's whole simplification.** The image
	// stays in `VK_QUEUE_FAMILY_FOREIGN_EXT`'s hands until a frame wants it, and the acquire the frame
	// thread emits names `GENERAL` on both halves — so there is no first-use state for two threads to
	// agree about, and no contents to be discarded out of `UNDEFINED`.
	return Check(vkBindImageMemory(m_Device->Handle(), into.Handle, into.Memory, 0), "vkBindImageMemory");
}

Result<void> VulkanTextures::Describe(Image& into, PixelFormat format)
{
	const VkImageViewCreateInfo viewInfo{ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		                                  .pNext = nullptr,
		                                  .flags = 0,
		                                  .image = into.Handle,
		                                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
		                                  .format = VulkanFormat(format.Code),
		                                  .components = {},
		                                  .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };

	if (Result<void> created =
	        Check(vkCreateImageView(m_Device->Handle(), &viewInfo, nullptr, &into.View), "vkCreateImageView");
	    !created)
	{
		return created;
	}

	const VkDescriptorSetAllocateInfo setInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		                                       .pNext = nullptr,
		                                       .descriptorPool = m_Pool,
		                                       .descriptorSetCount = 1,
		                                       .pSetLayouts = &m_SetLayout };

	if (Result<void> allocated =
	        Check(vkAllocateDescriptorSets(m_Device->Handle(), &setInfo, &into.Set), "vkAllocateDescriptorSets");
	    !allocated)
	{
		return allocated;
	}

	// Written once, here, and never again — which is what makes `Find` a read of an immutable thing
	// on the frame thread. A set updated per frame would be the descriptor churn Render/Backdrop.h
	// argues against for the same reason.
	const VkDescriptorImageInfo binding{ .sampler = VK_NULL_HANDLE,
		                                 .imageView = into.View,
		                                 .imageLayout = TextureLayout };
	const VkWriteDescriptorSet write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		                              .pNext = nullptr,
		                              .dstSet = into.Set,
		                              .dstBinding = 0,
		                              .dstArrayElement = 0,
		                              .descriptorCount = 1,
		                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		                              .pImageInfo = &binding,
		                              .pBufferInfo = nullptr,
		                              .pTexelBufferView = nullptr };

	vkUpdateDescriptorSets(m_Device->Handle(), 1, &write, 0, nullptr);

	return {};
}

void VulkanTextures::Forget(TextureId id) noexcept
{
	Sweep();

	Image* const held = Lookup(id);

	if (held == nullptr)
	{
		return;
	}

	Doom(*held);

	// The slot is cleared immediately, which is the half of the rule the watermark already licenses:
	// the caller waited for it, so no published snapshot names this id and no frame can start
	// recording it. What the sweep defers is the `VkImage`, which a frame that started *before* the
	// watermark moved may still be reading.
	*held = m_Images[m_Count - 1];
	m_Images[m_Count - 1] = Image{};
	--m_Count;
}

void VulkanTextures::Doom(Image& image) noexcept
{
	if (image.Handle == VK_NULL_HANDLE && image.View == VK_NULL_HANDLE && image.Set == VK_NULL_HANDLE)
	{
		return;
	}

	if (m_DoomedCount >= MaxTextureImages)
	{
		// Nowhere to defer to. The remaining answers are to leak the image or to free one a queue may
		// be reading, and a wait is the one that is merely slow — this is a table already holding a
		// thousand doomed images, which is a machine in trouble for some other reason.
		if (m_Device != nullptr && m_Device->IsValid())
		{
			vkDeviceWaitIdle(m_Device->Handle());
		}

		for (std::uint32_t index = 0; index < m_DoomedCount; ++index)
		{
			Release(m_Doomed[index]);
		}

		m_DoomedCount = 0;
	}

	Doomed& doomed = m_Doomed[m_DoomedCount];

	doomed = Doomed{ .Handle = image.Handle,
		             .Memory = image.Memory,
		             .View = image.View,
		             .Set = image.Set,
		             .At = {},
		             .Count = m_FenceCount };

	for (std::uint32_t index = 0; index < m_FenceCount; ++index)
	{
		doomed.At[index] = m_Fences[index]->Submitted();
	}

	++m_DoomedCount;
}

void VulkanTextures::Sweep() noexcept
{
	std::uint32_t index = 0;

	while (index < m_DoomedCount)
	{
		Doomed& doomed = m_Doomed[index];
		bool finished = true;

		// Every renderer that was registered when the image was given up, and only those: one
		// attached since cannot have recorded an id it never saw. The count travels with the entry
		// rather than being read from `m_FenceCount` for exactly that reason.
		for (std::uint32_t fence = 0; fence < doomed.Count && finished; ++fence)
		{
			finished = m_Fences[fence] != nullptr && m_Fences[fence]->Reached(doomed.At[fence]);
		}

		if (!finished)
		{
			++index;

			continue;
		}

		Release(doomed);

		doomed = m_Doomed[m_DoomedCount - 1];
		m_Doomed[m_DoomedCount - 1] = Doomed{};
		--m_DoomedCount;
	}
}

void VulkanTextures::Release(Doomed& doomed) noexcept
{
	if (m_Device == nullptr || !m_Device->IsValid())
	{
		return;
	}

	if (doomed.Set != VK_NULL_HANDLE)
	{
		vkFreeDescriptorSets(m_Device->Handle(), m_Pool, 1, &doomed.Set);
	}

	if (doomed.View != VK_NULL_HANDLE)
	{
		vkDestroyImageView(m_Device->Handle(), doomed.View, nullptr);
	}

	if (doomed.Handle != VK_NULL_HANDLE)
	{
		vkDestroyImage(m_Device->Handle(), doomed.Handle, nullptr);
	}

	// Last: freeing memory an image is still bound to is the ordering every driver is entitled to
	// crash on, and the two lines are far enough apart in a diff to be swapped by accident.
	if (doomed.Memory != VK_NULL_HANDLE)
	{
		vkFreeMemory(m_Device->Handle(), doomed.Memory, nullptr);
	}

	doomed = Doomed{};
}
