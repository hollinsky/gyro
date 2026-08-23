#include "Render/Renderer.h"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <variant>

#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Geometry/Region.h"
#include "Render/Pipeline.h"
#include "Render/Vulkan.h"
#include "Seam/Dressing.h"
#include "World/Material.h"

namespace
{
// What an empty scene composites to. Opaque black, and it is a statement rather than a
// placeholder: a frame with nothing in it shows nothing, and the alpha is one because a target's
// fourth channel is either ignored by the plane or means opaque.
constexpr VkClearValue Nothing{ .color = { .float32 = { 0.0F, 0.0F, 0.0F, 1.0F } } };

// How long the one blocking wait in this file will sit before giving up: a second, which is far
// past any frame and far short of a hang. It is only reached on a device that cannot export a
// timeline — decision 108 — where the alternative to waiting is handing out a point that says
// *nothing to wait for* while the rasterizer is still running.
constexpr std::uint64_t WaitLimitNanoseconds = 1'000'000'000;

// The rectangle a scissor gets, clipped to the image. A damage region is the caller's and is
// accumulated across frames; a target that shrank under a reconfiguration would otherwise carry
// rectangles past its own edge into a driver that is entitled to reject the whole command buffer.
[[nodiscard]] VkRect2D Clip(PixelRect<DeviceSpace> rect, PixelSize<DeviceSpace> size) noexcept
{
	const std::int32_t left = std::max(rect.Left(), 0);
	const std::int32_t top = std::max(rect.Top(), 0);
	const std::int32_t right = std::min(rect.Right(), size.Width);
	const std::int32_t bottom = std::min(rect.Bottom(), size.Height);

	if (right <= left || bottom <= top)
	{
		return { { 0, 0 }, { 0, 0 } };
	}

	return { { left, top }, { static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top) } };
}

// A queue-family transfer with no layout change on either half.
//
// **Both halves name `VK_IMAGE_LAYOUT_GENERAL` and that is what makes the pair matched.** A
// composite is read by something that is not a Vulkan queue — a display controller, a file
// writer, the test's own mapping — which is what `VK_QUEUE_FAMILY_FOREIGN_EXT` says. The spec
// requires a release and its acquire to state identical layouts, so the image is kept in
// `GENERAL` for its whole life rather than moved to `COLOR_ATTACHMENT_OPTIMAL` per frame: the
// image is linear-modifier memory somebody else can read at any moment, so there is no optimal
// layout for it to be in and the transition would be a pair that does not match.
[[nodiscard]] VkImageMemoryBarrier
Transfer(VkImage image, std::uint32_t from, std::uint32_t to, VkAccessFlags source, VkAccessFlags destination) noexcept
{
	return VkImageMemoryBarrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.pNext = nullptr,
		.srcAccessMask = source,
		.dstAccessMask = destination,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = from,
		.dstQueueFamilyIndex = to,
		.image = image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
}

// What the quad pipeline can express, asked of one item.
//
// **Every branch here is a picture somebody would otherwise have to notice was wrong.** A texture
// drawn as nothing is a window that disappears; a group drawn unflattened is a menu whose overlap
// shows through mid-fade; a dressing drawn as nothing is a panel that stops being glass; an elevation
// drawn as nothing is a dialog that stops looking lifted. Each of those composites successfully and
// is not what the scene said, which is the failure Seam/Renderer.h's *an item the renderer cannot
// express* exists to keep out of the tree.
[[nodiscard]] Result<void> Expressible(const DrawItem& item, ColorState output) noexcept
{
	if (std::holds_alternative<DrawTexture>(item.Content))
	{
		return Failure(EINVAL, "this renderer samples no textures yet");
	}

	if (std::holds_alternative<DrawGroup>(item.Content))
	{
		return Failure(EINVAL, "this renderer flattens no groups yet; decision 60's offscreen is not built");
	}

	if (item.Lift.Draws())
	{
		return Failure(EINVAL, "this renderer draws no shadows yet");
	}

	// **What is left of the colour-state branch, and it is a missing number rather than missing
	// code.** Every other conversion is built — Chain.glsl holds the curves and the six primaries
	// matrices, and `QuadVariant` selects one at pipeline creation. HLG is refused because converting
	// it needs the display's peak luminance to build BT.2100's scene-to-display step, and
	// Core/ColorState.h carries a reference white instead. Every implementation without a peak has
	// picked one silently, and a film that is subtly the wrong contrast on one panel and not another
	// is the kind of wrong nobody traces back to a shader.
	if (item.Color.Transfer == TransferFunction::Hlg || output.Transfer == TransferFunction::Hlg)
	{
		return Failure(EINVAL, "this renderer converts no HLG; ColorState carries no display peak luminance");
	}

	return {};
}

// The colour state the chain's own arithmetic lands in: linear light in the *output's* primaries.
//
// **Not the composite space, and Extract.frag argues the difference.** Decision 47 fixes the
// composite space as linear at Rec.2020 and a blur does not need to be there — a blur is a weighted
// sum, a primaries change is a matrix, and a matrix commutes with a weighted sum, so the two matrices
// the chain would otherwise spend are two that cancel. What this is used for is the tint: a
// material's tint is stated in linear light, and this is what says so to the quad pipeline when
// decision 34's third rung draws that tint with no chain under it.
[[nodiscard]] ColorState ChainState(ColorState output) noexcept
{
	return ColorState{
		output.Primaries, TransferFunction::Linear, AlphaMode::Premultiplied, 0, output.ReferenceLuminance
	};
}

// A material's tint as a solid the quad pipeline can fill with, which is decision 34's third rung
// drawn: no chain, no offscreen, no read of the backdrop at all — the fill an item would have had if
// the machine could not afford to look at what was behind it.
[[nodiscard]] DrawSolid TintFill(Material material) noexcept
{
	const MaterialTint tint = ResolvedTint(Facts(material));

	return { .Red = tint.Red, .Green = tint.Green, .Blue = tint.Blue, .Alpha = tint.Alpha };
}

// What the chain has to read so that every pixel of a dressed item has a whole neighbourhood behind
// it: the item's device-space bound grown by decision 63's declared expansion, clipped to the target.
//
// **This is that declaration's first consumer, and it is not the damage path.** Decision 63 spends
// one property three times — fusibility, damage expansion, and cacheability — and the second has
// nowhere to land yet, because Frame/Evaluator.h reports the whole output or nothing under decision
// 101. The number is load-bearing here regardless: it is exactly how far past itself a dressed item
// must read, and getting it wrong is a fringe at the panel's own edge rather than a trail behind
// something that moved.
[[nodiscard]] VkRect2D Neighbourhood(const DrawItem& item, Tier tier, PixelSize<DeviceSpace> size) noexcept
{
	const PixelRect<DeviceSpace> bound = item.Shape.PixelBounds();
	const auto reach = static_cast<std::int32_t>(std::ceil(Expansion(item.Dress, tier)));

	return Clip(
		PixelRect<DeviceSpace>::FromEdges(
			{ bound.Left() - reach, bound.Top() - reach }, { bound.Right() + reach, bound.Bottom() + reach }
		),
		size
	);
}

// One image dependency, spelled out per use because every one of them in this file is different in
// all four of the ways that matter and a helper that took fewer arguments would be a helper that hid
// the interesting half.
void Depend(
	VkCommandBuffer command,
	VkImage image,
	VkImageLayout from,
	VkImageLayout to,
	VkPipelineStageFlags sourceStage,
	VkAccessFlags source,
	VkPipelineStageFlags destinationStage,
	VkAccessFlags destination
) noexcept
{
	const VkImageMemoryBarrier barrier{ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		                                .pNext = nullptr,
		                                .srcAccessMask = source,
		                                .dstAccessMask = destination,
		                                .oldLayout = from,
		                                .newLayout = to,
		                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		                                .image = image,
		                                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };

	vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// One item's push constant block.
//
// The corners and their weights travel positionally, which is the winding Seam/Renderer.h states and
// the order the vertex stage indexes. The fill is premultiplied here rather than in the shader
// because it is three multiplies on the CPU against one per fragment, and because
// `AlphaMode::Straight` is a property of the item rather than of the draw.
//
// **Premultiplied in the item's own encoding, and that is not the sharp edge it looks like.**
// Docs/Architecture.md#premultiplied-alpha-is-the-sharp-edge is about applying alpha in an encoded
// space and then *blending* in a linear one. Here the two agree: a variant that converts nothing
// blends in the item's own space, so the premultiply is consistent with the blend; a variant that
// converts undoes this divide before it linearises, which is exactly the un-premultiply that section
// requires. What is refused either way is applying alpha in one space and blending in another.
[[nodiscard]] QuadConstants
Constants(const DrawItem& item, DrawSolid solid, PixelSize<DeviceSpace> target, ColorState output) noexcept
{
	QuadConstants constants{};

	for (std::size_t corner = 0; corner < 4; ++corner)
	{
		constants.Corner[corner][0] = item.Shape.Corners[corner].X;
		constants.Corner[corner][1] = item.Shape.Corners[corner].Y;
		constants.Corner[corner][2] = item.Shape.Weights[corner];
		constants.Corner[corner][3] = 0.0F;
	}

	const float premultiply = item.Color.Alpha == AlphaMode::Straight ? solid.Alpha : 1.0F;

	constants.Fill[0] = solid.Red * premultiply;
	constants.Fill[1] = solid.Green * premultiply;
	constants.Fill[2] = solid.Blue * premultiply;
	constants.Fill[3] = solid.Alpha;

	constants.Shape[0] = item.Extent.Width;
	constants.Shape[1] = item.Extent.Height;
	constants.Shape[2] = item.Radius;
	constants.Shape[3] = item.Opacity;

	constants.Target[0] = static_cast<float>(target.Width);
	constants.Target[1] = static_cast<float>(target.Height);

	// The two factors that meet an absolute transfer function and a relative one in the middle. They
	// are computed for every item rather than only for the converting ones because the branch would
	// cost more than the two stores, and a variant that does not convert never reads them.
	const QuadLuminance luminance = QuadLuminance::For(item.Color, output);

	constants.Target[2] = luminance.Decode;
	constants.Target[3] = luminance.Encode;

	return constants;
}
} // namespace

VulkanRenderer::VulkanRenderer(const IClock& clock, VulkanDevice& device, Fusion fusion)
	: m_Clock{ &clock }, m_Device{ &device }, m_Fusion{ fusion }
{
	if (!device.IsValid())
	{
		m_Status = Failure(ENODEV, "renderer constructed on an unopened device");

		return;
	}

	const VkCommandPoolCreateInfo poolInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		                                    .pNext = nullptr,
		                                    // Reset per buffer rather than per pool: the buffers are
		                                    // one per target and retire independently, so resetting
		                                    // the pool would recycle a buffer another target's
		                                    // submission is still reading.
		                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		                                    .queueFamilyIndex = device.QueueFamily() };

	if (Result<void> created =
	        Check(vkCreateCommandPool(device.Handle(), &poolInfo, nullptr, &m_Pool), "vkCreateCommandPool");
	    !created)
	{
		m_Status = created;

		return;
	}

	const VkCommandBufferAllocateInfo commandInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		                                           .pNext = nullptr,
		                                           .commandPool = m_Pool,
		                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		                                           .commandBufferCount = MaxRenderTargets };

	if (Result<void> allocated = Check(
			vkAllocateCommandBuffers(device.Handle(), &commandInfo, m_Commands.data()), "vkAllocateCommandBuffers"
		);
	    !allocated)
	{
		m_Status = allocated;
		Destroy();

		return;
	}

	// Exportable only where the device said it could be. Asking unconditionally is what fails on
	// lavapipe — `VK_ERROR_INVALID_EXTERNAL_HANDLE` from `vkCreateSemaphore`, not from the export —
	// and a renderer that aborted there would take the floor tier out entirely. Decision 108.
	const bool exportable = device.Description().ExportsTimeline;
	const VkExportSemaphoreCreateInfo exportInfo{ .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
		                                          .pNext = nullptr,
		                                          .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
	const VkSemaphoreTypeCreateInfo typeInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		                                      .pNext = exportable ? &exportInfo : nullptr,
		                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
		                                      .initialValue = 0 };
	const VkSemaphoreCreateInfo semaphoreInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		                                       .pNext = &typeInfo,
		                                       .flags = 0 };

	if (Result<void> created =
	        Check(vkCreateSemaphore(device.Handle(), &semaphoreInfo, nullptr, &m_Timeline), "vkCreateSemaphore");
	    !created)
	{
		m_Status = created;
		Destroy();

		return;
	}

	if (exportable)
	{
		const VkSemaphoreGetFdInfoKHR getInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
			                                   .pNext = nullptr,
			                                   .semaphore = m_Timeline,
			                                   .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
		int descriptor = -1;

		if (Result<void> exported =
		        Check(vkGetSemaphoreFdKHR(device.Handle(), &getInfo, &descriptor), "vkGetSemaphoreFdKHR");
		    !exported)
		{
			m_Status = exported;
			Destroy();

			return;
		}

		m_TimelineFd = Fd{ descriptor };
	}

	// Last, and it is the step that can fail on a driver that took everything above: a device which
	// refuses the SPIR-V says so at startup rather than at the first frame with a window in it. The
	// per-format pipelines are not built here — a format is a target's property and no target is
	// bound yet — which is why `BindTargets` is where `Prepare` runs.
	// **Non-fatal, and it is decision 34's third rung again.** A device that refuses the chain's
	// modules is one whose materials draw as their tint; refusing to construct the renderer would
	// give up every solid on screen for a blur nobody may have asked for.
	if (Result<void> created = m_Backdrop.Create(device); !created)
	{
		spdlog::info("gather falls to its tint on this device: {}", created.error().Context());
	}

	// **Fatal where the renderer was built for it and not otherwise**, which is the asymmetry with
	// the line above. A gather that cannot come up is decision 34's third rung and the picture
	// survives; a renderer asked for decision 62's reference execution and unable to build it has
	// nothing else it is allowed to draw, because falling back to the lattice would make an oracle
	// compare the fused path against itself and report agreement.
	if (m_Fusion == Fusion::Separate)
	{
		if (Result<void> created = m_Unfused.Create(device); !created)
		{
			m_Status = created;
			Destroy();

			return;
		}
	}

	if (Result<void> created = m_Pipeline.Create(device); !created)
	{
		m_Status = created;
		Destroy();

		return;
	}
}

VulkanRenderer::~VulkanRenderer()
{
	ReleaseTargets();
	Destroy();
}

Result<void> VulkanRenderer::BindTargets(std::span<const RenderTarget> targets, ColorState output)
{
	if (!m_Status)
	{
		return m_Status;
	}

	// Binding a new set implies releasing the old one, per the seam. Done first so that a set which
	// fails half way leaves nothing bound rather than a mix of two generations — partial success is
	// not expressible, because a set is what `AcquireTarget` indexes into.
	ReleaseTargets();

	// Before the imports, so that a binding which fails part way has already recorded what it was
	// asked to encode to. Nothing is drawable until a later bind succeeds either way, and the
	// alternative is a renderer whose held state and built pipelines disagree about the output.
	m_Output = output;

	if (targets.size() > MaxRenderTargets)
	{
		return Failure(EINVAL, "target set is larger than this renderer binds");
	}

	for (const RenderTarget& target : targets)
	{
		if (!target.IsValid())
		{
			ReleaseTargets();

			return Failure(EINVAL, "target set contains a half-built description");
		}

		// Seam/RenderTarget.h names this exact miswiring: a Vulkan device handed a CPU mapping is a
		// composition-root error rather than something to cast around. `Blit` is what binds those.
		if (target.IsMapped())
		{
			ReleaseTargets();

			return Failure(EINVAL, "the Vulkan renderer imports dmabufs; a CPU mapping belongs to Blit");
		}

		// **Counted before it is filled, and that ordering is the whole of it.** `Import` can fail
		// after it has created an image and imported memory — a format the driver takes and a
		// descriptor it will not, say — and `ReleaseTargets` only walks the slots the count covers.
		// Incremented afterwards, a half-built slot is one `VkImage` and one `VkDeviceMemory` that
		// nothing ever destroys, and the memory owns the duplicated descriptor, so a few hundred
		// rejected mode sets exhaust the descriptor table.
		Slot& slot = m_Slots[m_TargetCount];
		++m_TargetCount;

		if (Result<void> imported = Import(target, output, slot); !imported)
		{
			ReleaseTargets();

			return imported;
		}
	}

	if (Result<void> settled = Settle(); !settled)
	{
		ReleaseTargets();

		return settled;
	}

	Reserve(targets, output);

	// Only where this renderer draws that way, and a failure is the bind's rather than a tier.
	// Render/Unfused.h says why: there is no third thing for an unfused renderer to draw. An empty
	// set reserves nothing and is not a failure, which is the answer `Reserve` above already gives —
	// there is no size or format to build against and nothing will be drawn.
	if (m_Fusion == Fusion::Separate && m_TargetCount > 0)
	{
		if (Result<void> reserved = m_Unfused.Reserve(m_Slots[0].Size, m_Slots[0].Format); !reserved)
		{
			ReleaseTargets();

			return reserved;
		}
	}

	return {};
}

void VulkanRenderer::Reserve(std::span<const RenderTarget> targets, ColorState output)
{
	// **A failure here is a tier and not an error, which is why nothing above checks it.** Decision
	// 34's third rung is *the material is not rendered — an opaque or simply tinted fill*, so an
	// output whose device will not give it a floating-point offscreen, or whose targets carry a
	// modifier that cannot be sampled, draws every material as its tint and everything else exactly
	// as before. Refusing the bind instead would turn a look into a black screen, which is the
	// trade decision 35 makes in the other direction all the way down.
	if (m_TargetCount == 0)
	{
		return;
	}

	if (Result<void> reserved =
	        m_Backdrop.Reserve(m_Slots[0].Size, m_Slots[0].Format, output, static_cast<std::uint32_t>(targets.size()));
	    !reserved)
	{
		spdlog::info("gather falls to its tint on this output: {}", reserved.error().Context());

		return;
	}

	for (std::uint32_t index = 0; index < m_TargetCount; ++index)
	{
		if (!m_Slots[index].Samplable)
		{
			spdlog::info("gather falls to its tint on this output: a target's modifier cannot be sampled");
			m_Backdrop.Release();

			return;
		}

		Result<VkDescriptorSet> set = m_Backdrop.AllocateTargetSet(m_Slots[index].View);

		if (!set)
		{
			spdlog::info("gather falls to its tint on this output: {}", set.error().Context());
			m_Backdrop.Release();

			return;
		}

		m_Slots[index].Backdrop = *set;
	}
}

Result<void> VulkanRenderer::Import(const RenderTarget& target, ColorState output, Slot& slot)
{
	if (!m_Device->Supports(target.Format))
	{
		return Failure(EINVAL, "this device cannot render into that format and modifier");
	}

	const DmabufImage* image = target.AsDmabuf();

	const bool samplable = m_Device->SupportsSampling(target.Format);

	// One plane, which `VulkanDevice::Supports` has already agreed to for this modifier. Checked
	// again because the two facts have different owners: the driver said the *tiling* is one plane,
	// and this says the *description* is, and a presenter that filled in two is a bug here rather
	// than a mismatch to paper over.
	if (image->PlaneCount != 1)
	{
		return Failure(EINVAL, "a composite target is one plane");
	}

	const DmabufPlane& plane = image->Planes[0];
	const VkSubresourceLayout layout{
		.offset = plane.Offset, .size = 0, .rowPitch = plane.Stride, .arrayPitch = 0, .depthPitch = 0
	};
	const VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
		.pNext = nullptr,
		.drmFormatModifier = target.Format.Modifier,
		.drmFormatModifierPlaneCount = 1,
		.pPlaneLayouts = &layout
	};

	// **Explicit rather than a modifier list, and the difference matters.** A list asks the driver to
	// pick a tiling and lay the image out itself; explicit states the layout the *allocator* already
	// committed to, which is the only correct thing to do for memory this renderer did not allocate.
	// Getting this backwards produces an image that imports cleanly and reads at the wrong stride.
	const VkExternalMemoryImageCreateInfo externalInfo{ .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		                                                .pNext = &modifierInfo,
		                                                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
	const VkImageCreateInfo imageInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &externalInfo,
		.flags = 0,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VulkanFormat(target.Format.Code),
		.extent = { static_cast<std::uint32_t>(target.Size.Width), static_cast<std::uint32_t>(target.Size.Height), 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		// **`SAMPLED` where the modifier permits it, because a gather reads the target back.**
		// Render/Backdrop.h extracts the backdrop out of the composite itself rather than out of a
		// second copy of it, so a dressed output needs its own target legible to a shader. Asking for
		// it unconditionally would refuse the bind on a compressed modifier that renders fine, so it
		// is asked for where the driver lists it and the material falls to its tint where it does not.
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		         (samplable ? VkImageUsageFlags{ VK_IMAGE_USAGE_SAMPLED_BIT } : VkImageUsageFlags{ 0 }),
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = nullptr,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
	};

	if (Result<void> created =
	        Check(vkCreateImage(m_Device->Handle(), &imageInfo, nullptr, &slot.Image), "vkCreateImage");
	    !created)
	{
		return created;
	}

	const std::uint32_t types = m_Device->ImportableMemoryTypes(plane.Descriptor);

	if (types == 0)
	{
		return Failure(EINVAL, "this device cannot import that descriptor as a dmabuf");
	}

	// The descriptor is duplicated because `vkAllocateMemory` takes ownership of the one it is
	// handed, and Seam/RenderTarget.h's is *borrowed* — it belongs to the presenter and stays valid
	// until `TargetsInvalidated`. Importing the original would have the driver close the presenter's
	// descriptor out from under it, which is a use-after-close of a number the kernel has since
	// handed to somebody else.
	const int duplicated = fcntl(plane.Descriptor.Value, F_DUPFD_CLOEXEC, 0);

	if (duplicated < 0)
	{
		return Failure(errno, "could not duplicate the target's descriptor for import");
	}

	const VkImportMemoryFdInfoKHR importInfo{ .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		                                      .pNext = nullptr,
		                                      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		                                      .fd = duplicated };

	// Dedicated because an imported dmabuf is one image's memory and nothing else's: there is no
	// suballocation to do, and a driver that was not told so may refuse the bind.
	const VkMemoryDedicatedAllocateInfo dedicatedInfo{ .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		                                               .pNext = &importInfo,
		                                               .image = slot.Image,
		                                               .buffer = VK_NULL_HANDLE };

	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(m_Device->Handle(), slot.Image, &requirements);

	const std::uint32_t usable = requirements.memoryTypeBits & types;

	if (usable == 0)
	{
		close(duplicated);

		return Failure(EINVAL, "no memory type satisfies both the image and the imported descriptor");
	}

	const VkMemoryAllocateInfo allocateInfo{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		                                     .pNext = &dedicatedInfo,
		                                     .allocationSize = requirements.size,
		                                     .memoryTypeIndex = static_cast<std::uint32_t>(std::countr_zero(usable)) };

	if (Result<void> allocated =
	        Check(vkAllocateMemory(m_Device->Handle(), &allocateInfo, nullptr, &slot.Memory), "vkAllocateMemory");
	    !allocated)
	{
		// The descriptor is the driver's only on success. A failed import leaves it ours to close,
		// and leaking it here would exhaust the table across a few hundred rejected mode sets.
		close(duplicated);

		return allocated;
	}

	if (Result<void> bound =
	        Check(vkBindImageMemory(m_Device->Handle(), slot.Image, slot.Memory, 0), "vkBindImageMemory");
	    !bound)
	{
		return bound;
	}

	const VkImageViewCreateInfo viewInfo{ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		                                  .pNext = nullptr,
		                                  .flags = 0,
		                                  .image = slot.Image,
		                                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
		                                  .format = VulkanFormat(target.Format.Code),
		                                  .components = {},
		                                  .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };

	if (Result<void> created =
	        Check(vkCreateImageView(m_Device->Handle(), &viewInfo, nullptr, &slot.View), "vkCreateImageView");
	    !created)
	{
		return created;
	}

	slot.Size = target.Size;
	slot.LastSubmit = 0;
	slot.Format = VulkanFormat(target.Format.Code);
	slot.Samplable = samplable;

	// **Here rather than at the first `Record`, because a bind is where building one is legal.** The
	// seam declares this call unbounded and allocating and says in as many words that it may build
	// pipelines against a format it has not seen; `Record` may do neither. A driver that cannot build
	// the pipeline is an output that cannot be drawn, reported now, rather than a frame that fails
	// during a transition.
	return m_Pipeline.Prepare(slot.Format, output);
}

Result<void> VulkanRenderer::Settle()
{
	if (m_TargetCount == 0)
	{
		return {};
	}

	const VkCommandBuffer command = m_Commands[0];
	const VkCommandBufferBeginInfo beginInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		                                      .pNext = nullptr,
		                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		                                      .pInheritanceInfo = nullptr };

	if (Result<void> begun = Check(vkBeginCommandBuffer(command, &beginInfo), "vkBeginCommandBuffer"); !begun)
	{
		return begun;
	}

	std::array<VkImageMemoryBarrier, MaxRenderTargets> barriers{};

	for (std::uint32_t index = 0; index < m_TargetCount; ++index)
	{
		barriers[index] = VkImageMemoryBarrier{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.pNext = nullptr,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = m_Slots[index].Image,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
	}

	vkCmdPipelineBarrier(
		command,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0,
		0,
		nullptr,
		0,
		nullptr,
		m_TargetCount,
		barriers.data()
	);

	if (Result<void> ended = Check(vkEndCommandBuffer(command), "vkEndCommandBuffer"); !ended)
	{
		return ended;
	}

	const VkSubmitInfo submitInfo{ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		                           .pNext = nullptr,
		                           .waitSemaphoreCount = 0,
		                           .pWaitSemaphores = nullptr,
		                           .pWaitDstStageMask = nullptr,
		                           .commandBufferCount = 1,
		                           .pCommandBuffers = &command,
		                           .signalSemaphoreCount = 0,
		                           .pSignalSemaphores = nullptr };

	if (Result<void> submitted =
	        Check(vkQueueSubmit(m_Device->Queue(), 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit");
	    !submitted)
	{
		return submitted;
	}

	// Waited for, and this one is not decision 108's wait: it is a bind, which the seam already
	// declares unbounded, and the alternative would be a first `Record` racing the layout transition
	// its own barrier assumes has happened.
	return Check(vkQueueWaitIdle(m_Device->Queue()), "vkQueueWaitIdle");
}

void VulkanRenderer::ReleaseTargets() noexcept
{
	if (m_Device == nullptr || !m_Device->IsValid() || m_TargetCount == 0)
	{
		m_TargetCount = 0;

		return;
	}

	// Everything in flight has to land before the images go. The seam calls this at
	// `TargetsInvalidated`, which is not inside a frame, so the wait is legal here — and destroying
	// an image a queued command buffer still names is the one failure that shows up as a corrupted
	// frame on somebody else's machine rather than as an error on this one.
	vkQueueWaitIdle(m_Device->Queue());

	for (std::uint32_t index = 0; index < m_TargetCount; ++index)
	{
		Slot& slot = m_Slots[index];

		if (slot.View != VK_NULL_HANDLE)
		{
			vkDestroyImageView(m_Device->Handle(), slot.View, nullptr);
		}

		if (slot.Image != VK_NULL_HANDLE)
		{
			vkDestroyImage(m_Device->Handle(), slot.Image, nullptr);
		}

		// This is what closes the duplicated descriptor: the driver took ownership of it at import
		// and releases it with the memory.
		if (slot.Memory != VK_NULL_HANDLE)
		{
			vkFreeMemory(m_Device->Handle(), slot.Memory, nullptr);
		}

		slot = Slot{};
	}

	// The intermediates go with the targets they were sized against. `Backdrop` is released by its
	// next `Reserve` instead, which works because a bind always follows; this one is released here
	// because a `Fusion::Separate` renderer that is unbound holds two full-resolution images, and an
	// output that goes away should not keep paying for them.
	m_Unfused.Release();

	m_TargetCount = 0;
}

Result<Submission> VulkanRenderer::Record(const RecordRequest& request)
{
	const Instant started = m_Clock->Now();

	if (!m_Status)
	{
		return std::unexpected{ m_Status.error() };
	}

	if (request.Target >= m_TargetCount)
	{
		return Failure(EINVAL, "record names an unbound target");
	}

	Slot& slot = m_Slots[request.Target];

	// **Every item is checked before anything is recorded, and that ordering is the point.** A
	// refusal discovered half way through a command buffer leaves a buffer that has been begun and
	// not ended, and the caller has no way to know how much of its frame reached the queue. Walking
	// the list first costs one pass over an array the caller just built, and makes the failure the
	// same shape as `BindTargets`'s: nothing happened.
	// **The variant is checked in the same pass, and it is the same argument one level down.** A
	// missing pipeline discovered mid-recording is the failure above with a command buffer already
	// open, and the check is a lookup either way. `Prepare` enumerated every variant this output can
	// want, so a miss here is a renderer that came up wrong rather than a scene that asked for
	// something new — but it is still reported, because the alternative is a `vkCmdBindPipeline` with
	// a null handle and a driver entitled to reject the whole buffer.
	for (const DrawItem& item : request.Items)
	{
		if (Result<void> expressible = Expressible(item, m_Output); !expressible)
		{
			return std::unexpected{ expressible.error() };
		}

		if (m_Pipeline.For(slot.Format, QuadVariant::For(item.Color, m_Output, item.Radius > 0.0F)) == VK_NULL_HANDLE)
		{
			return Failure(EINVAL, "no pipeline was built for this item's colour conversion on this target's format");
		}
	}

	// Nothing to redraw. The seam is explicit that this is not the same as the caller skipping the
	// frame — a target stale by age still wants the composite — so it is the caller's judgement that
	// arrived here as an empty region, and honouring it costs a submission rather than saving one.
	if (request.Damage.IsEmpty())
	{
		return Submission{ .Point = SyncPoint::Immediate(), .RecordCost = Elapsed(started, m_Clock->Now()) };
	}

	// A poll rather than a wait, for `IsComplete`'s reason: blocking here would put the GPU's
	// schedule on the `SCHED_FIFO` thread. A target whose previous composite has not landed is one
	// the presenter should not have handed back yet, so this is a transient refusal rather than an
	// error the caller has to understand.
	if (slot.LastSubmit != 0 && !Reached(slot.LastSubmit))
	{
		return Failure(EBUSY, "the previous composite into this target has not completed");
	}

	const VkCommandBuffer command = m_Commands[request.Target];

	if (Result<void> reset = Check(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer"); !reset)
	{
		return std::unexpected{ reset.error() };
	}

	const VkCommandBufferBeginInfo beginInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		                                      .pNext = nullptr,
		                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		                                      .pInheritanceInfo = nullptr };

	if (Result<void> begun = Check(vkBeginCommandBuffer(command, &beginInfo), "vkBeginCommandBuffer"); !begun)
	{
		return std::unexpected{ begun.error() };
	}

	const std::uint32_t family = m_Device->QueueFamily();
	const VkImageMemoryBarrier acquire =
		Transfer(slot.Image, VK_QUEUE_FAMILY_FOREIGN_EXT, family, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
	vkCmdPipelineBarrier(
		command,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0,
		0,
		nullptr,
		0,
		nullptr,
		1,
		&acquire
	);

	// **Both intermediates into a shader-readable layout once, out of `UNDEFINED`.** They carry
	// nothing across frames — every element writes what the next one reads within a single item — so
	// this discards whatever was in them and establishes the invariant `Apply` relies on: a pass
	// finds its source already legible and leaves its destination the same way.
	if (m_Fusion == Fusion::Separate)
	{
		for (std::uint32_t index = 0; index < UnfusedImages; ++index)
		{
			Depend(
				command,
				m_Unfused.Image(index),
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				0,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				VK_ACCESS_SHADER_READ_BIT
			);
		}
	}

	BeginTarget(command, slot, request);

	// The arena is a member-sized array rather than anything grown here: decision 36 forbids
	// allocating inside the frame section, and a damage region holds at most `Region::Capacity`
	// rectangles by construction.
	std::array<VkClearRect, Region<DeviceSpace>::Capacity> rects{};
	std::uint32_t count = 0;

	for (const PixelRect<DeviceSpace>& rect : request.Damage.Rects())
	{
		const VkRect2D clipped = Clip(rect, slot.Size);

		if (clipped.extent.width == 0 || clipped.extent.height == 0)
		{
			continue;
		}

		rects[count] = VkClearRect{ .rect = clipped, .baseArrayLayer = 0, .layerCount = 1 };
		++count;
	}

	if (count > 0)
	{
		const VkClearAttachment clear{ .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			                           .colorAttachment = 0,
			                           .clearValue = Nothing };
		vkCmdClearAttachments(command, 1, &clear, count, rects.data());
	}

	if (count > 0 && !request.Items.empty())
	{
		// **Bound per item rather than once, and only where it changes.** An item's variant is a
		// function of its colour state and whether it is rounded, so a list of ordinary windows on one
		// output binds once and every item after the first is a push and a draw. The comparison is a
		// handle against a handle; the alternative — sorting the list by variant — would reorder the
		// painter's order, which decision 55 fixes as the list's own.
		VkPipeline bound = VK_NULL_HANDLE;

		// The whole target, not the damage. The vertex stage divides a device-space corner by
		// `Target` to reach clip space, so the viewport has to be the grid those corners are measured
		// on; damage is the scissor's job and is set per rectangle below.
		const VkViewport viewport{ .x = 0.0F,
			                       .y = 0.0F,
			                       .width = static_cast<float>(slot.Size.Width),
			                       .height = static_cast<float>(slot.Size.Height),
			                       .minDepth = 0.0F,
			                       .maxDepth = 1.0F };
		vkCmdSetViewport(command, 0, 1, &viewport);

		// **Items outside and rectangles inside, and a gather is what decided the nesting.**
		// *(Inverted 2026-08-22.)* It was rectangles outside — one scissor set, the whole list drawn
		// into it, repeated per rectangle — which preserves the painter's order within every pixel
		// because the rectangles are disjoint, and which a gather makes wrong: a material reads a
		// neighbourhood that can cross into a damage rectangle the loop has not reached yet, so it
		// would blur last frame's pixels into this frame's panel along an edge nobody drew. Items
		// outside means everything below a dressed item has been drawn *everywhere* before the chain
		// extracts, which is the property decision 60's backdrop rule already assumes. It is also
		// cheaper on the axis the old comment cared about — a pipeline bind per item instead of one
		// per item per rectangle, against a scissor set per rectangle, and a scissor is the cheap one.
		for (const DrawItem& item : request.Items)
		{
			const bool gathering = item.Dress != Material::None && Facts(item.Dress).Gathering;

			// **The dressing draws first and the content over it**, which is what a material *is*: a
			// panel's background, with whatever the node carries on top. Decision 104 fixes the order
			// from the other end — within one item the material samples the target as of before the
			// item began — and the reverse is a blurred backdrop covering the label that was supposed
			// to sit on it.
			if (gathering)
			{
				if (Result<void> dressed = Dress(command, slot, item, request, std::span{ rects.data(), count }, bound);
				    !dressed)
				{
					return std::unexpected{ dressed.error() };
				}
			}

			const DrawSolid* solid = std::get_if<DrawSolid>(&item.Content);

			// A dressing with nothing to draw. What is left after the branch above is an item whose
			// whole content was a dressing nobody set — a caller's bug that draws nothing, which is
			// the same answer Seam/Renderer.h gives an empty `DrawGroup`.
			if (solid == nullptr)
			{
				continue;
			}

			// Decision 62's reference execution, where this renderer was built for it: the same
			// chain, one pass per element, through an intermediate that rounds.
			if (m_Fusion == Fusion::Separate)
			{
				Separate(command, slot, item, *solid, request, std::span{ rects.data(), count }, bound);

				continue;
			}

			const VkPipeline pipeline =
				m_Pipeline.For(slot.Format, QuadVariant::For(item.Color, m_Output, item.Radius > 0.0F));

			if (pipeline != bound)
			{
				vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				bound = pipeline;
			}

			const QuadConstants constants = Constants(item, *solid, slot.Size, m_Output);
			vkCmdPushConstants(
				command,
				m_Pipeline.Layout(),
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				0,
				sizeof constants,
				&constants
			);

			for (std::uint32_t index = 0; index < count; ++index)
			{
				vkCmdSetScissor(command, 0, 1, &rects[index].rect);
				vkCmdDraw(command, 6, 1, 0, 0);
			}
		}
	}

	vkCmdEndRendering(command);

	const VkImageMemoryBarrier release =
		Transfer(slot.Image, family, VK_QUEUE_FAMILY_FOREIGN_EXT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
	vkCmdPipelineBarrier(
		command,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0,
		0,
		nullptr,
		0,
		nullptr,
		1,
		&release
	);

	if (Result<void> ended = Check(vkEndCommandBuffer(command), "vkEndCommandBuffer"); !ended)
	{
		return std::unexpected{ ended.error() };
	}

	const std::uint64_t value = m_Submitted + 1;
	const VkTimelineSemaphoreSubmitInfo timelineInfo{ .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
		                                              .pNext = nullptr,
		                                              .waitSemaphoreValueCount = 0,
		                                              .pWaitSemaphoreValues = nullptr,
		                                              .signalSemaphoreValueCount = 1,
		                                              .pSignalSemaphoreValues = &value };
	const VkSubmitInfo submitInfo{ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		                           .pNext = &timelineInfo,
		                           .waitSemaphoreCount = 0,
		                           .pWaitSemaphores = nullptr,
		                           .pWaitDstStageMask = nullptr,
		                           .commandBufferCount = 1,
		                           .pCommandBuffers = &command,
		                           .signalSemaphoreCount = 1,
		                           .pSignalSemaphores = &m_Timeline };

	if (Result<void> submitted =
	        Check(vkQueueSubmit(m_Device->Queue(), 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit");
	    !submitted)
	{
		return std::unexpected{ submitted.error() };
	}

	m_Submitted = value;
	slot.LastSubmit = value;

	// **Decision 108 in five lines.** A device that cannot export a timeline has no descriptor to put
	// in a `SyncPoint`, and an invalid one there means *nothing to wait for* — which would be a lie
	// while the rasterizer is still running. So the frame is finished here and the point is honestly
	// immediate. That is not a workaround: a sync point exists so two devices can overlap, and the
	// device this branch is taken on is the CPU, which is the same one that would be doing the
	// waiting.
	if (!m_TimelineFd.IsValid())
	{
		const VkSemaphoreWaitInfo waitInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			                                .pNext = nullptr,
			                                .flags = 0,
			                                .semaphoreCount = 1,
			                                .pSemaphores = &m_Timeline,
			                                .pValues = &value };

		if (Result<void> waited =
		        Check(vkWaitSemaphores(m_Device->Handle(), &waitInfo, WaitLimitNanoseconds), "vkWaitSemaphores");
		    !waited)
		{
			return std::unexpected{ waited.error() };
		}

		return Submission{ .Point = SyncPoint::Immediate(), .RecordCost = Elapsed(started, m_Clock->Now()) };
	}

	return Submission{ .Point = SyncPoint{ .Timeline = m_TimelineFd.Borrow(), .Value = value },
		               .RecordCost = Elapsed(started, m_Clock->Now()) };
}

void VulkanRenderer::Apply(
	VkCommandBuffer command,
	Element element,
	std::uint32_t source,
	std::uint32_t destination,
	VkRect2D region,
	const VkViewport& pane,
	const ElementConstants& constants
) const noexcept
{
	// **Every pass begins and ends with both intermediates legible to a shader**, which is what lets
	// this be written without a layout tracked per image across a recording. The destination comes
	// out of `SHADER_READ_ONLY_OPTIMAL` for the write and goes straight back into it afterwards, and
	// the source is already there; `Record` is what puts both there once, out of `UNDEFINED`, at the
	// top of the frame. `Pass` takes the other approach one function down — `UNDEFINED` as the old
	// layout, discarding — which it can because the blur chain's ping-pong is a fixed alternation
	// this one is not.
	Depend(
		command,
		m_Unfused.Image(destination),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
	);

	// `DONT_CARE`, because the pass writes everything downstream of it will read: the next element
	// rasterizes the same quad and reads its own fragment coordinate, so the only pixels ever read
	// out of this image are the ones this draw is about to cover.
	const VkRenderingAttachmentInfo attachment{ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		                                        .pNext = nullptr,
		                                        .imageView = m_Unfused.View(destination),
		                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                                        .resolveMode = VK_RESOLVE_MODE_NONE,
		                                        .resolveImageView = VK_NULL_HANDLE,
		                                        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		                                        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		                                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		                                        .clearValue = Nothing };
	const VkRenderingInfo rendering{ .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		                             .pNext = nullptr,
		                             .flags = 0,
		                             .renderArea = region,
		                             .layerCount = 1,
		                             .viewMask = 0,
		                             .colorAttachmentCount = 1,
		                             .pColorAttachments = &attachment,
		                             .pDepthAttachment = nullptr,
		                             .pStencilAttachment = nullptr };

	const VkDescriptorSet set = m_Unfused.Set(source);

	vkCmdBeginRendering(command, &rendering);
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Unfused.For(element));
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Unfused.Layout(), 0, 1, &set, 0, nullptr);

	// The target's grid, not the region's. Render/Shaders/Quad.glsl divides a device-space corner by
	// the target's extent to reach clip space, so a viewport over the region would place the quad
	// somewhere else entirely — and the two paths would then be compared on where they drew rather
	// than on what they computed.
	vkCmdSetViewport(command, 0, 1, &pane);
	vkCmdSetScissor(command, 0, 1, &region);
	vkCmdPushConstants(
		command,
		m_Unfused.Layout(),
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		0,
		sizeof constants,
		&constants
	);
	vkCmdDraw(command, 6, 1, 0, 0);
	vkCmdEndRendering(command);

	Depend(
		command,
		m_Unfused.Image(destination),
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT
	);
}

void VulkanRenderer::Separate(
	VkCommandBuffer command,
	const Slot& slot,
	const DrawItem& item,
	DrawSolid solid,
	const RecordRequest& request,
	std::span<const VkClearRect> rects,
	VkPipeline& bound
) const noexcept
{
	// **The item's own bound, and every element runs over all of it.** The elements are pointwise, so
	// a pixel's value does not depend on which damage rectangle it fell in; running the chain once
	// over the whole item and honouring damage at the composite is both cheaper and — the half that
	// matters — the arrangement in which the corner mask's hardware derivatives are taken over the
	// same fragments the fused program takes them over.
	const VkRect2D region = Clip(item.Shape.PixelBounds(), slot.Size);

	if (region.extent.width == 0 || region.extent.height == 0)
	{
		return;
	}

	// **The same variant the fused program would have been built with, read as a list.** A run mask
	// says which elements a specialization compiled in; here it says which passes to run. That the
	// two are the same function of the same item is the whole basis of the comparison — an oracle
	// whose two sides disagreed about *which* elements the chain had would be measuring the
	// disagreement rather than the rounding.
	const QuadVariant variant = QuadVariant::For(item.Color, m_Output, item.Radius > 0.0F);

	ElementConstants constants{ .Item = Constants(item, solid, slot.Size, m_Output) };

	constants.Convert[0] = static_cast<std::int32_t>(item.Color.Transfer);
	constants.Convert[1] = static_cast<std::int32_t>(item.Color.Primaries);
	constants.Convert[2] = static_cast<std::int32_t>(m_Output.Transfer);
	constants.Convert[3] = static_cast<std::int32_t>(m_Output.Primaries);

	std::array<Element, 4> chain{};
	std::uint32_t length = 0;

	chain[length++] = Element::Emit;

	if ((variant.Run & QuadRunConvert) != 0U)
	{
		chain[length++] = Element::Convert;
	}

	if ((variant.Run & QuadRunCorner) != 0U)
	{
		chain[length++] = Element::Corner;
	}

	// Always, and never folded into the composite. Render/Pipeline.h counts the opacity and decision
	// 62's dim as one multiply the renderer does on the CPU, which makes them one element rather
	// than none — and folding that element into the blend is a fusion, which is the one thing this
	// path is not allowed to do.
	chain[length++] = Element::Scale;

	const VkViewport pane{ .x = 0.0F,
		                   .y = 0.0F,
		                   .width = static_cast<float>(slot.Size.Width),
		                   .height = static_cast<float>(slot.Size.Height),
		                   .minDepth = 0.0F,
		                   .maxDepth = 1.0F };

	vkCmdEndRendering(command);

	// Which intermediate holds the chain's current value. It starts at one so that the emit — which
	// reads nothing and has the set bound only because the layout has one — writes into zero.
	std::uint32_t held = 1;

	for (std::uint32_t index = 0; index < length; ++index)
	{
		Apply(command, chain[index], held, 1U - held, region, pane, constants);
		held = 1U - held;
	}

	BeginTarget(command, slot, request);
	vkCmdSetViewport(command, 0, 1, &pane);

	const VkDescriptorSet set = m_Unfused.Set(held);

	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Unfused.For(Element::Composite));
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Unfused.Layout(), 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(
		command,
		m_Unfused.Layout(),
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		0,
		sizeof constants,
		&constants
	);

	// The outer loop's memo is invalidated rather than updated, for `Dress`'s reason: what is bound
	// now belongs to a different layout.
	bound = VK_NULL_HANDLE;

	for (const VkClearRect& rect : rects)
	{
		vkCmdSetScissor(command, 0, 1, &rect.rect);
		vkCmdDraw(command, 6, 1, 0, 0);
	}
}

void VulkanRenderer::Pass(
	VkCommandBuffer command,
	VkPipeline pipeline,
	VkDescriptorSet source,
	std::uint32_t destination,
	VkRect2D used,
	const VkViewport& pane,
	const PassConstants& constants,
	VkImage read
) const noexcept
{
	// The image this pass is about to read was written as an attachment by the pass before it, and
	// the descriptor set naming it was written with the sampled layout — so this is a real transition
	// and not only a dependency. The extract passes null because its source is the target, which the
	// caller transitioned once for the whole chain.
	if (read != VK_NULL_HANDLE)
	{
		Depend(
			command,
			read,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT
		);
	}

	// **`UNDEFINED` as the old layout, deliberately, and it is a discard rather than an oversight.**
	// Every pass writes the whole region it is given, so nothing already in the destination survives
	// or is wanted — which lets the transition throw the contents away instead of preserving a
	// tiling the driver would otherwise have to keep. The dependency is still real: it is the
	// write-after-read against the pass two steps back, which read this same image.
	Depend(
		command,
		m_Backdrop.ChainImage(destination),
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
	);

	// `DONT_CARE` for the same reason: the pass covers its whole render area, so loading what was
	// there is bandwidth spent on pixels about to be overwritten.
	const VkRenderingAttachmentInfo attachment{ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		                                        .pNext = nullptr,
		                                        .imageView = m_Backdrop.ChainView(destination),
		                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                                        .resolveMode = VK_RESOLVE_MODE_NONE,
		                                        .resolveImageView = VK_NULL_HANDLE,
		                                        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		                                        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		                                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		                                        .clearValue = Nothing };
	const VkRenderingInfo rendering{ .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		                             .pNext = nullptr,
		                             .flags = 0,
		                             .renderArea = used,
		                             .layerCount = 1,
		                             .viewMask = 0,
		                             .colorAttachmentCount = 1,
		                             .pColorAttachments = &attachment,
		                             .pDepthAttachment = nullptr,
		                             .pStencilAttachment = nullptr };

	vkCmdBeginRendering(command, &rendering);
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindDescriptorSets(
		command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Backdrop.PassLayout(), 0, 1, &source, 0, nullptr
	);
	vkCmdSetViewport(command, 0, 1, &pane);
	vkCmdSetScissor(command, 0, 1, &used);
	vkCmdPushConstants(command, m_Backdrop.PassLayout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants, &constants);
	vkCmdDraw(command, 3, 1, 0, 0);
	vkCmdEndRendering(command);
}

void VulkanRenderer::BeginTarget(VkCommandBuffer command, const Slot& slot, const RecordRequest& request) const noexcept
{
	// `LOAD` rather than `CLEAR`, and it is the whole of what damage means. The target holds the
	// previous composite — that is why a ring is worth having and why the caller accumulates damage
	// across the frames since this image was last drawn — so everything outside the region must
	// survive. A `CLEAR` here would repaint the whole target every frame and make the damage
	// argument decorative. It is also what makes `Dress` able to resume: the pass it split has
	// already written pixels this one must not lose.
	//
	// **`GENERAL` rather than `COLOR_ATTACHMENT_OPTIMAL`**, which is what lets a gather read the
	// target back with an execution barrier and no layout transition — Render/Backdrop.cpp's
	// descriptor comment is the other half.
	const VkRenderingAttachmentInfo attachment{ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		                                        .pNext = nullptr,
		                                        .imageView = slot.View,
		                                        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
		                                        .resolveMode = VK_RESOLVE_MODE_NONE,
		                                        .resolveImageView = VK_NULL_HANDLE,
		                                        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		                                        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		                                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		                                        .clearValue = Nothing };
	const VkRenderingInfo renderingInfo{ .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		                                 .pNext = nullptr,
		                                 .flags = 0,
		                                 .renderArea = Clip(request.Damage.Bounds(), slot.Size),
		                                 .layerCount = 1,
		                                 .viewMask = 0,
		                                 .colorAttachmentCount = 1,
		                                 .pColorAttachments = &attachment,
		                                 .pDepthAttachment = nullptr,
		                                 .pStencilAttachment = nullptr };
	vkCmdBeginRendering(command, &renderingInfo);
}

Result<void> VulkanRenderer::Dress(
	VkCommandBuffer command,
	const Slot& slot,
	const DrawItem& item,
	const RecordRequest& request,
	std::span<const VkClearRect> rects,
	VkPipeline& bound
)
{
	const bool rounded = item.Radius > 0.0F;
	const ChainPlan plan = ChainPlan::For(item.Dress, request.Quality);
	const bool chained = request.Mode == RenderMode::Planned && m_Backdrop.IsReady() &&
	                     slot.Backdrop != VK_NULL_HANDLE && plan.Passes > 0 &&
	                     m_Backdrop.Extract(plan.Divisor) != VK_NULL_HANDLE;

	if (!chained)
	{
		// Decision 34's third rung, and the quad pipeline is what draws it: the material's tint as an
		// ordinary fill, converted out of linear light by the same chain every other item goes
		// through. No offscreen, no read of the backdrop, nothing on the frame path that a floored
		// frame cannot afford.
		DrawItem tinted = item;
		tinted.Color = ChainState(m_Output);

		// The tint is an ordinary fill, so it takes whichever execution this renderer draws fills
		// with. The *chained* branch below is not offered the same choice and does not need it: a
		// gather forces materialisation by definition, so decision 62 never fuses it and there is no
		// second execution of it to compare against.
		if (m_Fusion == Fusion::Separate)
		{
			Separate(command, slot, tinted, TintFill(item.Dress), request, rects, bound);

			return {};
		}

		const VkPipeline pipeline = m_Pipeline.For(slot.Format, QuadVariant::For(tinted.Color, m_Output, rounded));

		if (pipeline == VK_NULL_HANDLE)
		{
			return Failure(EINVAL, "no pipeline was built for a material's tint on this target's format");
		}

		if (pipeline != bound)
		{
			vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			bound = pipeline;
		}

		const QuadConstants constants = Constants(tinted, TintFill(item.Dress), slot.Size, m_Output);
		vkCmdPushConstants(
			command,
			m_Pipeline.Layout(),
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			0,
			sizeof constants,
			&constants
		);

		for (const VkClearRect& rect : rects)
		{
			vkCmdSetScissor(command, 0, 1, &rect.rect);
			vkCmdDraw(command, 6, 1, 0, 0);
		}

		return {};
	}

	const VkRect2D region = Neighbourhood(item, request.Quality, slot.Size);

	if (region.extent.width == 0 || region.extent.height == 0)
	{
		return {};
	}

	const PixelSize<DeviceSpace> chain = m_Backdrop.ChainSize();
	const VkRect2D used{ .offset = { 0, 0 },
		                 .extent = { (region.extent.width + plan.Divisor - 1) / plan.Divisor,
		                             (region.extent.height + plan.Divisor - 1) / plan.Divisor } };
	const VkViewport pane{ .x = 0.0F,
		                   .y = 0.0F,
		                   .width = static_cast<float>(used.extent.width),
		                   .height = static_cast<float>(used.extent.height),
		                   .minDepth = 0.0F,
		                   .maxDepth = 1.0F };

	vkCmdEndRendering(command);

	// The target's writes have to land before the extract reads them, and the layout does not change
	// — `GENERAL` is already legible to a shader. An execution and memory dependency, nothing more.
	Depend(
		command,
		slot.Image,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT
	);

	PassConstants constants{};

	constants.Source[0] = static_cast<float>(region.offset.x);
	constants.Source[1] = static_cast<float>(region.offset.y);
	constants.Source[2] = 1.0F / static_cast<float>(slot.Size.Width);
	constants.Source[3] = 1.0F / static_cast<float>(slot.Size.Height);
	constants.Step[0] = static_cast<float>(plan.Divisor);
	constants.Step[1] = static_cast<float>(plan.Divisor);

	Pass(command, m_Backdrop.Extract(plan.Divisor), slot.Backdrop, 0, used, pane, constants);

	// Then the separable passes, alternating direction and ping-ponging between the two images. The
	// count is twice the tier's, because a box is separable and each of its passes is two.
	constants.Source[0] = 0.0F;
	constants.Source[1] = 0.0F;
	constants.Source[2] = 1.0F / static_cast<float>(chain.Width);
	constants.Source[3] = 1.0F / static_cast<float>(chain.Height);
	constants.Step[0] = 1.0F;
	constants.Step[1] = 1.0F;
	constants.Kernel[0] = plan.HalfWidth;

	std::uint32_t source = 0;

	for (std::uint32_t index = 0; index < plan.Passes * 2; ++index)
	{
		constants.Step[2] = index % 2 == 0 ? 1.0F : 0.0F;
		constants.Step[3] = index % 2 == 0 ? 0.0F : 1.0F;

		Pass(
			command,
			m_Backdrop.Blur(),
			m_Backdrop.ChainSet(source),
			1 - source,
			used,
			pane,
			constants,
			m_Backdrop.ChainImage(source)
		);

		source = 1 - source;
	}

	// The last write has to land before the dressing samples it, and this one *is* a layout change:
	// a chain image is written as an attachment and read as a sampled image, and the descriptor sets
	// were written naming the read layout.
	Depend(
		command,
		m_Backdrop.ChainImage(source),
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT
	);

	// And the extract's read of the target has to be finished before the resumed pass writes over it.
	Depend(
		command,
		slot.Image,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
	);

	BeginTarget(command, slot, request);

	const VkViewport viewport{ .x = 0.0F,
		                       .y = 0.0F,
		                       .width = static_cast<float>(slot.Size.Width),
		                       .height = static_cast<float>(slot.Size.Height),
		                       .minDepth = 0.0F,
		                       .maxDepth = 1.0F };
	vkCmdSetViewport(command, 0, 1, &viewport);

	const VkPipeline pipeline = m_Backdrop.Dress(rounded);
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

	// The outer loop's memo is invalidated rather than updated: what is bound now belongs to a
	// different layout, so the next content item has to bind again whatever it wanted.
	bound = VK_NULL_HANDLE;

	const VkDescriptorSet set = m_Backdrop.ChainSet(source);
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Backdrop.DressLayout(), 0, 1, &set, 0, nullptr);

	const MaterialTint tint = ResolvedTint(Facts(item.Dress));
	DressConstants dress{};

	for (std::size_t corner = 0; corner < 4; ++corner)
	{
		dress.Corner[corner][0] = item.Shape.Corners[corner].X;
		dress.Corner[corner][1] = item.Shape.Corners[corner].Y;
		dress.Corner[corner][2] = item.Shape.Weights[corner];
	}

	dress.Tint[0] = tint.Red;
	dress.Tint[1] = tint.Green;
	dress.Tint[2] = tint.Blue;
	dress.Tint[3] = tint.Alpha;

	dress.Shape[0] = item.Extent.Width;
	dress.Shape[1] = item.Extent.Height;
	dress.Shape[2] = item.Radius;
	dress.Shape[3] = item.Opacity;

	dress.Target[0] = static_cast<float>(slot.Size.Width);
	dress.Target[1] = static_cast<float>(slot.Size.Height);
	dress.Target[2] = 1.0F / static_cast<float>(chain.Width);
	dress.Target[3] = 1.0F / static_cast<float>(chain.Height);

	dress.Chain[0] = static_cast<float>(region.offset.x);
	dress.Chain[1] = static_cast<float>(region.offset.y);
	dress.Chain[2] = static_cast<float>(plan.Divisor);

	vkCmdPushConstants(
		command,
		m_Backdrop.DressLayout(),
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		0,
		sizeof dress,
		&dress
	);

	for (const VkClearRect& rect : rects)
	{
		vkCmdSetScissor(command, 0, 1, &rect.rect);
		vkCmdDraw(command, 6, 1, 0, 0);
	}

	return {};
}

bool VulkanRenderer::Reached(std::uint64_t value) const noexcept
{
	std::uint64_t counter = 0;

	if (vkGetSemaphoreCounterValue(m_Device->Handle(), m_Timeline, &counter) != VK_SUCCESS)
	{
		// A device that will not answer is a device that is gone, and reporting *not finished* would
		// wedge the loop against work that will never land. Complete is the answer that lets the
		// composition root notice on the next call that fails properly.
		return true;
	}

	return counter >= value;
}

bool VulkanRenderer::IsComplete(SyncPoint point) const
{
	if (point.IsImmediate())
	{
		return true;
	}

	// A point this renderer did not issue. The signature has no way to say `EINVAL`, and of the two
	// answers available, *complete* is the one that does not stall a frame loop forever on a
	// descriptor nothing here will ever signal.
	if (!m_Status || point.Timeline != m_TimelineFd.Borrow())
	{
		return true;
	}

	return Reached(point.Value);
}

std::size_t VulkanRenderer::CollectCosts(std::span<GpuCost>)
{
	return 0;
}

void VulkanRenderer::Destroy() noexcept
{
	if (m_Device == nullptr || !m_Device->IsValid())
	{
		return;
	}

	m_Pipeline.Destroy();

	m_TimelineFd = Fd{};

	if (m_Timeline != VK_NULL_HANDLE)
	{
		vkDestroySemaphore(m_Device->Handle(), m_Timeline, nullptr);
		m_Timeline = VK_NULL_HANDLE;
	}

	// The buffers go with the pool rather than separately, which is what `vkDestroyCommandPool`
	// already promises and what keeps the two from being freed in the wrong order.
	if (m_Pool != VK_NULL_HANDLE)
	{
		vkDestroyCommandPool(m_Device->Handle(), m_Pool, nullptr);
		m_Pool = VK_NULL_HANDLE;
		m_Commands.fill(VK_NULL_HANDLE);
	}
}
