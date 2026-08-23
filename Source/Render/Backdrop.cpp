#include "Render/Backdrop.h"

#include <array>
#include <cerrno>
#include <cstdint>

#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Render/Shaders/Backdrop.vert.h"
#include "Render/Shaders/Blur.frag.h"
#include "Render/Shaders/Dress.frag.h"
#include "Render/Shaders/Dress.vert.h"
#include "Render/Shaders/Extract.frag.h"
#include "Render/Vulkan.h"

namespace
{

// The chain's own format, and Docs/Architecture.md#precision-and-why-the-blur-chain-is-affordable is
// where it comes from: a backdrop is opaque, so the chain needs no alpha, and a packed float carries
// linear light at thirty-two bits — the same bandwidth as the `RGBA8` it replaces, with none of the
// banding linear light at eight bits has in the shadows.
//
// **The alpha it drops is not the alpha the dressing needs.** The chain holds what is *behind* the
// item, which the composite has already made opaque wherever anything was drawn; the item's own
// coverage and opacity are applied at the dressing pass, on the far side, against components that
// never went through here.
constexpr VkFormat ChainFormat = VK_FORMAT_B10G11R11_UFLOAT_PACK32;

// The fallback, for a device that will not sample or render the packed float. Twice the bandwidth
// and it carries an alpha nothing reads, which is the cost of not having a picture depend on which
// driver the machine has.
constexpr VkFormat ChainFallback = VK_FORMAT_R16G16B16A16_SFLOAT;

// Replaces what is under it. Every pass of the chain writes a whole region it computed from scratch,
// so blending would be an `over` against whatever the ping-pong left there last frame.
constexpr VkPipelineColorBlendAttachmentState Replace{
	.blendEnable = VK_FALSE,
	.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
	.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
	.colorBlendOp = VK_BLEND_OP_ADD,
	.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
	.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
	.alphaBlendOp = VK_BLEND_OP_ADD,
	.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
};

// The dressing composites `over` for the same reason every content item does, and decision 95 fixes
// it as the only blend mode. The fragment stage hands back premultiplied components, so the source
// factor is one.
constexpr VkPipelineColorBlendAttachmentState Over{
	.blendEnable = VK_TRUE,
	.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
	.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	.colorBlendOp = VK_BLEND_OP_ADD,
	.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
	.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	.alphaBlendOp = VK_BLEND_OP_ADD,
	.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
};

constexpr std::array<VkDynamicState, 2> Dynamics{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

[[nodiscard]] Result<VkShaderModule> Module(VkDevice device, const std::uint32_t* code, std::size_t size)
{
	const VkShaderModuleCreateInfo info{ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		                                 .pNext = nullptr,
		                                 .flags = 0,
		                                 .codeSize = size,
		                                 .pCode = code };
	VkShaderModule module = VK_NULL_HANDLE;

	if (Result<void> created = Check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
	    !created)
	{
		return std::unexpected{ created.error() };
	}

	return module;
}

[[nodiscard]] Result<VkSampler> Sampler(VkDevice device, VkFilter filter)
{
	// **Clamped to the edge on both axes, and that is a visible decision rather than a default.** A
	// panel at the screen's edge samples past the region that was extracted for it; repeating would
	// wrap the far side of the screen into its corner and a border colour would darken it. Clamping
	// extends the last texel, which is what a blur running off the edge of a picture should do and
	// what every other compositor does for the same reason.
	const VkSamplerCreateInfo info{ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		                            .pNext = nullptr,
		                            .flags = 0,
		                            .magFilter = filter,
		                            .minFilter = filter,
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
	VkSampler sampler = VK_NULL_HANDLE;

	if (Result<void> created = Check(vkCreateSampler(device, &info, nullptr, &sampler), "vkCreateSampler"); !created)
	{
		return std::unexpected{ created.error() };
	}

	return sampler;
}

// Whether this device can both draw into and sample from a format with optimal tiling, which is what
// every image in the chain has to do — each pass's destination is the next pass's source.
[[nodiscard]] bool Usable(VkPhysicalDevice physical, VkFormat format) noexcept
{
	VkFormatProperties properties{};
	vkGetPhysicalDeviceFormatProperties(physical, format, &properties);

	constexpr VkFormatFeatureFlags Wanted =
		VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

	return (properties.optimalTilingFeatures & Wanted) == Wanted;
}

} // namespace

Result<void> Backdrop::Create(VulkanDevice& device)
{
	if (!device.IsValid())
	{
		return Failure(ENODEV, "a backdrop chain built against an unopened device");
	}

	m_Device = &device;

	const std::array<std::pair<VkShaderModule*, std::pair<const std::uint32_t*, std::size_t>>, 5> modules{
		std::pair{ &m_Vertex, std::pair{ BackdropVertexSpirv, sizeof BackdropVertexSpirv } },
		std::pair{ &m_ExtractFragment, std::pair{ ExtractFragmentSpirv, sizeof ExtractFragmentSpirv } },
		std::pair{ &m_BlurFragment, std::pair{ BlurFragmentSpirv, sizeof BlurFragmentSpirv } },
		std::pair{ &m_DressVertex, std::pair{ DressVertexSpirv, sizeof DressVertexSpirv } },
		std::pair{ &m_DressFragment, std::pair{ DressFragmentSpirv, sizeof DressFragmentSpirv } },
	};

	for (const auto& [slot, source] : modules)
	{
		Result<VkShaderModule> built = Module(device.Handle(), source.first, source.second);

		if (!built)
		{
			return std::unexpected{ built.error() };
		}

		*slot = *built;
	}

	Result<VkSampler> nearest = Sampler(device.Handle(), VK_FILTER_NEAREST);

	if (!nearest)
	{
		return std::unexpected{ nearest.error() };
	}

	m_Nearest = *nearest;

	Result<VkSampler> linear = Sampler(device.Handle(), VK_FILTER_LINEAR);

	if (!linear)
	{
		return std::unexpected{ linear.error() };
	}

	m_Linear = *linear;

	// One binding, one image, one stage. Every pass in the chain reads exactly one thing, which is
	// what makes the descriptor traffic a fixed set written once at a binding.
	const VkDescriptorSetLayoutBinding binding{ .binding = 0,
		                                        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		                                        .descriptorCount = 1,
		                                        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		                                        .pImmutableSamplers = nullptr };
	const VkDescriptorSetLayoutCreateInfo setInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		                                           .pNext = nullptr,
		                                           .flags = 0,
		                                           .bindingCount = 1,
		                                           .pBindings = &binding };

	if (Result<void> created = Check(
			vkCreateDescriptorSetLayout(device.Handle(), &setInfo, nullptr, &m_SetLayout), "vkCreateDescriptorSetLayout"
		);
	    !created)
	{
		return created;
	}

	const VkPushConstantRange passRange{ .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		                                 .offset = 0,
		                                 .size = sizeof(PassConstants) };
	const VkPipelineLayoutCreateInfo passLayout{ .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		                                         .pNext = nullptr,
		                                         .flags = 0,
		                                         .setLayoutCount = 1,
		                                         .pSetLayouts = &m_SetLayout,
		                                         .pushConstantRangeCount = 1,
		                                         .pPushConstantRanges = &passRange };

	if (Result<void> created = Check(
			vkCreatePipelineLayout(device.Handle(), &passLayout, nullptr, &m_PassLayout), "vkCreatePipelineLayout"
		);
	    !created)
	{
		return created;
	}

	const VkPushConstantRange dressRange{ .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		                                  .offset = 0,
		                                  .size = sizeof(DressConstants) };
	const VkPipelineLayoutCreateInfo dressLayout{ .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		                                          .pNext = nullptr,
		                                          .flags = 0,
		                                          .setLayoutCount = 1,
		                                          .pSetLayouts = &m_SetLayout,
		                                          .pushConstantRangeCount = 1,
		                                          .pPushConstantRanges = &dressRange };

	return Check(
		vkCreatePipelineLayout(device.Handle(), &dressLayout, nullptr, &m_DressLayout), "vkCreatePipelineLayout"
	);
}

Result<void>
Backdrop::Reserve(PixelSize<DeviceSpace> resolution, VkFormat format, ColorState output, std::uint32_t targets)
{
	Release();

	if (m_Device == nullptr || m_DressLayout == VK_NULL_HANDLE)
	{
		return Failure(ENODEV, "no chain layout; the renderer did not come up");
	}

	if (format == VK_FORMAT_UNDEFINED)
	{
		return Failure(EINVAL, "a chain reserved against a format the renderer cannot name");
	}

	if (resolution.Width <= 0 || resolution.Height <= 0)
	{
		return Failure(EINVAL, "a chain reserved against an output with no extent");
	}

	// Refused here for Render/Pipeline.h's reason, one pass over: BT.2100's scene-to-display step
	// needs the display's peak luminance and a `ColorState` carries a reference white, so an
	// implementation without one has picked a peak on the user's behalf.
	if (output.Transfer == TransferFunction::Hlg)
	{
		return Failure(EINVAL, "no chain encodes HLG; ColorState carries no display peak luminance");
	}

	m_Format = Usable(m_Device->Physical(), ChainFormat)   ? ChainFormat :
	           Usable(m_Device->Physical(), ChainFallback) ? ChainFallback :
	                                                         VK_FORMAT_UNDEFINED;

	if (m_Format == VK_FORMAT_UNDEFINED)
	{
		return Failure(ENOTSUP, "this device renders into no floating-point format the chain can use");
	}

	// **Reserved at the finest divisor the ladder can name, and used in part below it.** Decision 46's
	// discipline — storage reserved when an output is configured, never allocated at the moment a
	// transition starts — and decision 34's requirement that stepping the tier *down* be quick. An
	// allocation on the way down is the one thing that would make it slow, so a coarser tier writes
	// into the top-left corner of an image that is already there.
	m_Size = { .Width = (resolution.Width + static_cast<std::int32_t>(FinestDivisor) - 1) /
		                static_cast<std::int32_t>(FinestDivisor),
		       .Height = (resolution.Height + static_cast<std::int32_t>(FinestDivisor) - 1) /
		                 static_cast<std::int32_t>(FinestDivisor) };

	for (Image& image : m_Chain)
	{
		if (Result<void> allocated = Allocate(image); !allocated)
		{
			return allocated;
		}
	}

	// One set per chain image and one per target the presenter may hand over. Written once each and
	// never again, so the pool is sized for the binding's whole life rather than per frame.
	const std::uint32_t sets = ChainImages + targets;
	const VkDescriptorPoolSize size{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = sets };
	const VkDescriptorPoolCreateInfo poolInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		                                       .pNext = nullptr,
		                                       .flags = 0,
		                                       .maxSets = sets,
		                                       .poolSizeCount = 1,
		                                       .pPoolSizes = &size };

	if (Result<void> created =
	        Check(vkCreateDescriptorPool(m_Device->Handle(), &poolInfo, nullptr, &m_Pool), "vkCreateDescriptorPool");
	    !created)
	{
		return created;
	}

	for (Image& image : m_Chain)
	{
		const VkDescriptorSetAllocateInfo info{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			                                    .pNext = nullptr,
			                                    .descriptorPool = m_Pool,
			                                    .descriptorSetCount = 1,
			                                    .pSetLayouts = &m_SetLayout };

		if (Result<void> allocated =
		        Check(vkAllocateDescriptorSets(m_Device->Handle(), &info, &image.Set), "vkAllocateDescriptorSets");
		    !allocated)
		{
			return allocated;
		}

		const VkDescriptorImageInfo binding{ .sampler = m_Linear,
			                                 .imageView = image.View,
			                                 .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		const VkWriteDescriptorSet write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			                              .pNext = nullptr,
			                              .dstSet = image.Set,
			                              .dstBinding = 0,
			                              .dstArrayElement = 0,
			                              .descriptorCount = 1,
			                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			                              .pImageInfo = &binding,
			                              .pBufferInfo = nullptr,
			                              .pTexelBufferView = nullptr };

		vkUpdateDescriptorSets(m_Device->Handle(), 1, &write, 0, nullptr);
	}

	// Every pipeline the ladder can reach, built here because decision 62 forbids a frame blocking on
	// compilation and Seam/Dressing.h's tier set is closed. Two extracts, one blur, two dressings.
	for (std::uint32_t index = 0; index < m_Extract.size(); ++index)
	{
		const std::array<std::int32_t, 2> values{ static_cast<std::int32_t>(output.Transfer),
			                                      static_cast<std::int32_t>(FinestDivisor << index) };
		Result<VkPipeline> built = BuildPass(m_ExtractFragment, values.data(), sizeof(values));

		if (!built)
		{
			return std::unexpected{ built.error() };
		}

		m_Extract[index] = *built;
	}

	Result<VkPipeline> blur = BuildPass(m_BlurFragment, nullptr, 0);

	if (!blur)
	{
		return std::unexpected{ blur.error() };
	}

	m_Blur = *blur;

	for (std::uint32_t rounded = 0; rounded < 2; ++rounded)
	{
		Result<VkPipeline> built = BuildDress(rounded != 0, format, output);

		if (!built)
		{
			return std::unexpected{ built.error() };
		}

		m_Dress[rounded] = *built;
	}

	m_Ready = true;

	return {};
}

Result<void> Backdrop::Allocate(Image& image)
{
	const VkImageCreateInfo info{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = m_Format,
		.extent = { static_cast<std::uint32_t>(m_Size.Width), static_cast<std::uint32_t>(m_Size.Height), 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = nullptr,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
	};

	if (Result<void> created = Check(vkCreateImage(m_Device->Handle(), &info, nullptr, &image.Image), "vkCreateImage");
	    !created)
	{
		return created;
	}

	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(m_Device->Handle(), image.Image, &requirements);

	VkPhysicalDeviceMemoryProperties properties{};
	vkGetPhysicalDeviceMemoryProperties(m_Device->Physical(), &properties);

	// Device-local, which is the whole point of an offscreen nothing outside the GPU ever reads. The
	// first type that satisfies both the image and that property, which is the ordinary answer and
	// the one every driver orders usefully.
	std::uint32_t chosen = properties.memoryTypeCount;

	for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		const bool allowed = (requirements.memoryTypeBits & (1U << index)) != 0;
		const bool local = (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;

		if (allowed && local)
		{
			chosen = index;

			break;
		}
	}

	if (chosen == properties.memoryTypeCount)
	{
		return Failure(ENOMEM, "no device-local memory type satisfies a chain image");
	}

	const VkMemoryAllocateInfo allocation{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		                                   .pNext = nullptr,
		                                   .allocationSize = requirements.size,
		                                   .memoryTypeIndex = chosen };

	if (Result<void> allocated =
	        Check(vkAllocateMemory(m_Device->Handle(), &allocation, nullptr, &image.Memory), "vkAllocateMemory");
	    !allocated)
	{
		return allocated;
	}

	if (Result<void> bound =
	        Check(vkBindImageMemory(m_Device->Handle(), image.Image, image.Memory, 0), "vkBindImageMemory");
	    !bound)
	{
		return bound;
	}

	const VkImageViewCreateInfo viewInfo{ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		                                  .pNext = nullptr,
		                                  .flags = 0,
		                                  .image = image.Image,
		                                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
		                                  .format = m_Format,
		                                  .components = { VK_COMPONENT_SWIZZLE_IDENTITY,
		                                                  VK_COMPONENT_SWIZZLE_IDENTITY,
		                                                  VK_COMPONENT_SWIZZLE_IDENTITY,
		                                                  VK_COMPONENT_SWIZZLE_IDENTITY },
		                                  .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };

	return Check(vkCreateImageView(m_Device->Handle(), &viewInfo, nullptr, &image.View), "vkCreateImageView");
}

Result<VkDescriptorSet> Backdrop::AllocateTargetSet(VkImageView view)
{
	if (m_Pool == VK_NULL_HANDLE)
	{
		return Failure(ENODEV, "no descriptor pool; the chain was not reserved");
	}

	const VkDescriptorSetAllocateInfo info{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		                                    .pNext = nullptr,
		                                    .descriptorPool = m_Pool,
		                                    .descriptorSetCount = 1,
		                                    .pSetLayouts = &m_SetLayout };
	VkDescriptorSet set = VK_NULL_HANDLE;

	if (Result<void> allocated =
	        Check(vkAllocateDescriptorSets(m_Device->Handle(), &info, &set), "vkAllocateDescriptorSets");
	    !allocated)
	{
		return std::unexpected{ allocated.error() };
	}

	// Nearest, because Extract.frag decodes every tap before it sums them and a filtered tap would
	// have averaged encoded texels in hardware before this shader could decode anything — which is
	// exactly the darkening decision 34's first rung exists to avoid.
	// **`GENERAL`, matching the layout the composite target is already in.** Render/Renderer.cpp
	// binds it as an attachment in `GENERAL` rather than `COLOR_ATTACHMENT_OPTIMAL`, and `GENERAL`
	// permits a shader read as well — so the chain needs an execution barrier between the write and
	// the read and no layout transition at all, which is one fewer thing to get wrong per dressed
	// item and one fewer thing for a driver to decompress.
	const VkDescriptorImageInfo binding{ .sampler = m_Nearest,
		                                 .imageView = view,
		                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
	const VkWriteDescriptorSet write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		                              .pNext = nullptr,
		                              .dstSet = set,
		                              .dstBinding = 0,
		                              .dstArrayElement = 0,
		                              .descriptorCount = 1,
		                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		                              .pImageInfo = &binding,
		                              .pBufferInfo = nullptr,
		                              .pTexelBufferView = nullptr };

	vkUpdateDescriptorSets(m_Device->Handle(), 1, &write, 0, nullptr);

	return set;
}

VkPipeline Backdrop::Extract(std::uint32_t divisor) const noexcept
{
	for (std::uint32_t index = 0; index < m_Extract.size(); ++index)
	{
		if ((FinestDivisor << index) == divisor)
		{
			return m_Extract[index];
		}
	}

	return VK_NULL_HANDLE;
}

Result<VkPipeline> Backdrop::Build(
	VkShaderModule vertex,
	VkShaderModule fragment,
	const void* specialization,
	std::size_t size,
	std::uint32_t constants,
	VkPipelineLayout layout,
	VkFormat format,
	const VkPipelineColorBlendAttachmentState& blend
)
{
	std::array<VkSpecializationMapEntry, 2> entries{};

	for (std::uint32_t constant = 0; constant < constants; ++constant)
	{
		entries[constant] =
			VkSpecializationMapEntry{ .constantID = constant,
			                          .offset = constant * static_cast<std::uint32_t>(sizeof(std::int32_t)),
			                          .size = sizeof(std::int32_t) };
	}

	const VkSpecializationInfo info{
		.mapEntryCount = constants, .pMapEntries = entries.data(), .dataSize = size, .pData = specialization
	};

	const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
		VkPipelineShaderStageCreateInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		                                 .pNext = nullptr,
		                                 .flags = 0,
		                                 .stage = VK_SHADER_STAGE_VERTEX_BIT,
		                                 .module = vertex,
		                                 .pName = "main",
		                                 .pSpecializationInfo = nullptr },
		VkPipelineShaderStageCreateInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		                                 .pNext = nullptr,
		                                 .flags = 0,
		                                 .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		                                 .module = fragment,
		                                 .pName = "main",
		                                 .pSpecializationInfo = constants != 0 ? &info : nullptr },
	};

	const VkPipelineVertexInputStateCreateInfo vertexInput{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.vertexBindingDescriptionCount = 0,
		.pVertexBindingDescriptions = nullptr,
		.vertexAttributeDescriptionCount = 0,
		.pVertexAttributeDescriptions = nullptr
	};
	const VkPipelineInputAssemblyStateCreateInfo assembly{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		.primitiveRestartEnable = VK_FALSE
	};
	const VkPipelineViewportStateCreateInfo viewport{ .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		                                              .pNext = nullptr,
		                                              .flags = 0,
		                                              .viewportCount = 1,
		                                              .pViewports = nullptr,
		                                              .scissorCount = 1,
		                                              .pScissors = nullptr };
	const VkPipelineRasterizationStateCreateInfo raster{ .sType =
		                                                     VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		                                                 .pNext = nullptr,
		                                                 .flags = 0,
		                                                 .depthClampEnable = VK_FALSE,
		                                                 .rasterizerDiscardEnable = VK_FALSE,
		                                                 .polygonMode = VK_POLYGON_MODE_FILL,
		                                                 .cullMode = VK_CULL_MODE_NONE,
		                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		                                                 .depthBiasEnable = VK_FALSE,
		                                                 .depthBiasConstantFactor = 0.0F,
		                                                 .depthBiasClamp = 0.0F,
		                                                 .depthBiasSlopeFactor = 0.0F,
		                                                 .lineWidth = 1.0F };
	const VkPipelineMultisampleStateCreateInfo multisample{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		.sampleShadingEnable = VK_FALSE,
		.minSampleShading = 0.0F,
		.pSampleMask = nullptr,
		.alphaToCoverageEnable = VK_FALSE,
		.alphaToOneEnable = VK_FALSE
	};
	const VkPipelineColorBlendStateCreateInfo blending{ .sType =
		                                                    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		                                                .pNext = nullptr,
		                                                .flags = 0,
		                                                .logicOpEnable = VK_FALSE,
		                                                .logicOp = VK_LOGIC_OP_CLEAR,
		                                                .attachmentCount = 1,
		                                                .pAttachments = &blend,
		                                                .blendConstants = { 0.0F, 0.0F, 0.0F, 0.0F } };
	const VkPipelineDynamicStateCreateInfo dynamic{ .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		                                            .pNext = nullptr,
		                                            .flags = 0,
		                                            .dynamicStateCount = static_cast<std::uint32_t>(Dynamics.size()),
		                                            .pDynamicStates = Dynamics.data() };
	const VkPipelineRenderingCreateInfo rendering{ .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		                                           .pNext = nullptr,
		                                           .viewMask = 0,
		                                           .colorAttachmentCount = 1,
		                                           .pColorAttachmentFormats = &format,
		                                           .depthAttachmentFormat = VK_FORMAT_UNDEFINED,
		                                           .stencilAttachmentFormat = VK_FORMAT_UNDEFINED };
	const VkGraphicsPipelineCreateInfo pipelineInfo{ .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		                                             .pNext = &rendering,
		                                             .flags = 0,
		                                             .stageCount = static_cast<std::uint32_t>(stages.size()),
		                                             .pStages = stages.data(),
		                                             .pVertexInputState = &vertexInput,
		                                             .pInputAssemblyState = &assembly,
		                                             .pTessellationState = nullptr,
		                                             .pViewportState = &viewport,
		                                             .pRasterizationState = &raster,
		                                             .pMultisampleState = &multisample,
		                                             .pDepthStencilState = nullptr,
		                                             .pColorBlendState = &blending,
		                                             .pDynamicState = &dynamic,
		                                             .layout = layout,
		                                             .renderPass = VK_NULL_HANDLE,
		                                             .subpass = 0,
		                                             .basePipelineHandle = VK_NULL_HANDLE,
		                                             .basePipelineIndex = -1 };

	VkPipeline pipeline = VK_NULL_HANDLE;

	if (Result<void> created = Check(
			vkCreateGraphicsPipelines(m_Device->Handle(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
			"vkCreateGraphicsPipelines"
		);
	    !created)
	{
		return std::unexpected{ created.error() };
	}

	return pipeline;
}

Result<VkPipeline> Backdrop::BuildPass(VkShaderModule fragment, const void* specialization, std::size_t size)
{
	return Build(
		m_Vertex,
		fragment,
		specialization,
		size,
		static_cast<std::uint32_t>(size / sizeof(std::int32_t)),
		m_PassLayout,
		m_Format,
		Replace
	);
}

Result<VkPipeline> Backdrop::BuildDress(bool rounded, VkFormat format, ColorState output)
{
	const std::array<std::int32_t, 2> values{ rounded ? 1 : 0, static_cast<std::int32_t>(output.Transfer) };

	return Build(m_DressVertex, m_DressFragment, values.data(), sizeof(values), 2, m_DressLayout, format, Over);
}

void Backdrop::Release() noexcept
{
	m_Ready = false;

	if (m_Device == nullptr || !m_Device->IsValid())
	{
		return;
	}

	for (VkPipeline& pipeline : m_Dress)
	{
		vkDestroyPipeline(m_Device->Handle(), pipeline, nullptr);
		pipeline = VK_NULL_HANDLE;
	}

	if (m_Blur != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(m_Device->Handle(), m_Blur, nullptr);
		m_Blur = VK_NULL_HANDLE;
	}

	for (VkPipeline& pipeline : m_Extract)
	{
		vkDestroyPipeline(m_Device->Handle(), pipeline, nullptr);
		pipeline = VK_NULL_HANDLE;
	}

	// The pool goes before the images, and the sets go with it — a descriptor set is freed by its
	// pool being destroyed, and freeing one that names a view this loop is about to destroy in the
	// other order is a use after free the validation layer would be right about.
	if (m_Pool != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorPool(m_Device->Handle(), m_Pool, nullptr);
		m_Pool = VK_NULL_HANDLE;
	}

	for (Image& image : m_Chain)
	{
		vkDestroyImageView(m_Device->Handle(), image.View, nullptr);
		vkDestroyImage(m_Device->Handle(), image.Image, nullptr);
		vkFreeMemory(m_Device->Handle(), image.Memory, nullptr);
		image = Image{};
	}

	m_Size = {};
	m_Format = VK_FORMAT_UNDEFINED;
}

void Backdrop::Destroy() noexcept
{
	Release();

	if (m_Device == nullptr || !m_Device->IsValid())
	{
		return;
	}

	if (m_DressLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(m_Device->Handle(), m_DressLayout, nullptr);
		m_DressLayout = VK_NULL_HANDLE;
	}

	if (m_PassLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(m_Device->Handle(), m_PassLayout, nullptr);
		m_PassLayout = VK_NULL_HANDLE;
	}

	if (m_SetLayout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(m_Device->Handle(), m_SetLayout, nullptr);
		m_SetLayout = VK_NULL_HANDLE;
	}

	for (VkSampler* sampler : { &m_Linear, &m_Nearest })
	{
		vkDestroySampler(m_Device->Handle(), *sampler, nullptr);
		*sampler = VK_NULL_HANDLE;
	}

	for (VkShaderModule* module : { &m_DressFragment, &m_DressVertex, &m_BlurFragment, &m_ExtractFragment, &m_Vertex })
	{
		vkDestroyShaderModule(m_Device->Handle(), *module, nullptr);
		*module = VK_NULL_HANDLE;
	}
}
