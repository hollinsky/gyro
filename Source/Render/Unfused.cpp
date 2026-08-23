#include "Render/Unfused.h"

#include <array>
#include <cerrno>
#include <cstdint>

#include "Core/Result.h"
#include "Render/Shaders/Element.frag.h"
#include "Render/Shaders/Element.vert.h"
#include "Render/Vulkan.h"

namespace
{
// Replaces what is under it. Every element writes the value it computed from the one before it, so
// blending would be an `over` against whatever the ping-pong left there last item.
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

// The composite, and it is the same `over` the fused program's single draw performs — same factors,
// same target, premultiplied source. Decision 95 fixes it as the only blend mode, and the oracle
// would be measuring the blend rather than the chain if the two paths reached the target differently.
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

// Whether this device can both draw into and sample from the intermediate's format with optimal
// tiling. Every image in the chain does both, because each pass's destination is the next pass's
// source.
//
// **Asked rather than assumed, even though the answer is in the specification.** Vulkan requires
// `R16G16B16A16_SFLOAT` of every implementation as a colour attachment and as a sampled image, which
// is half of why decision 118 picked it — but a renderer that read a table instead of the device
// would report a driver bug as a corrupted picture, and this costs one call at a binding.
[[nodiscard]] bool Usable(VkPhysicalDevice physical, VkFormat format) noexcept
{
	VkFormatProperties properties{};
	vkGetPhysicalDeviceFormatProperties(physical, format, &properties);

	constexpr VkFormatFeatureFlags Wanted =
		VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

	return (properties.optimalTilingFeatures & Wanted) == Wanted;
}
} // namespace

Result<void> Unfused::Create(VulkanDevice& device)
{
	if (!device.IsValid())
	{
		return Failure(ENODEV, "an unfused chain built against an unopened device");
	}

	m_Device = &device;

	Result<VkShaderModule> vertex = Module(device.Handle(), ElementVertexSpirv, sizeof ElementVertexSpirv);

	if (!vertex)
	{
		return std::unexpected{ vertex.error() };
	}

	m_Vertex = *vertex;

	Result<VkShaderModule> fragment = Module(device.Handle(), ElementFragmentSpirv, sizeof ElementFragmentSpirv);

	if (!fragment)
	{
		return std::unexpected{ fragment.error() };
	}

	m_Fragment = *fragment;

	// Nearest, and `texelFetch` ignores it. See Render/Unfused.h on why it is not linear.
	const VkSamplerCreateInfo samplerInfo{ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		                                   .pNext = nullptr,
		                                   .flags = 0,
		                                   .magFilter = VK_FILTER_NEAREST,
		                                   .minFilter = VK_FILTER_NEAREST,
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
	        Check(vkCreateSampler(device.Handle(), &samplerInfo, nullptr, &m_Sampler), "vkCreateSampler");
	    !created)
	{
		return created;
	}

	// One binding, one image, one stage: every element reads exactly one thing, which is what makes
	// the descriptor traffic a pair of sets written once at a binding and never touched in a frame.
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

	const VkPushConstantRange range{ .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		                             .offset = 0,
		                             .size = sizeof(ElementConstants) };
	const VkPipelineLayoutCreateInfo layoutInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		                                         .pNext = nullptr,
		                                         .flags = 0,
		                                         .setLayoutCount = 1,
		                                         .pSetLayouts = &m_SetLayout,
		                                         .pushConstantRangeCount = 1,
		                                         .pPushConstantRanges = &range };

	return Check(vkCreatePipelineLayout(device.Handle(), &layoutInfo, nullptr, &m_Layout), "vkCreatePipelineLayout");
}

Result<void> Unfused::Reserve(PixelSize<DeviceSpace> resolution, VkFormat format)
{
	Release();

	if (m_Device == nullptr || m_Layout == VK_NULL_HANDLE)
	{
		return Failure(ENODEV, "no unfused layout; the renderer did not come up");
	}

	if (format == VK_FORMAT_UNDEFINED)
	{
		return Failure(EINVAL, "an unfused chain reserved against a format the renderer cannot name");
	}

	if (resolution.Width <= 0 || resolution.Height <= 0)
	{
		return Failure(EINVAL, "an unfused chain reserved against an output with no extent");
	}

	if (!Usable(m_Device->Physical(), UnfusedFormat))
	{
		return Failure(ENOTSUP, "this device neither renders into nor samples the intermediate's format");
	}

	// The target's own resolution, and not a pixel less. Each element rasterizes the item's quad at
	// the position it will finally occupy, so the intermediate's grid *is* the target's grid — which
	// is what lets every pass read its own fragment coordinate with no coordinate arithmetic between
	// the two paths, and what keeps the corner mask's derivatives the ones the fused program takes.
	m_Size = resolution;

	for (Intermediate& image : m_Images)
	{
		if (Result<void> allocated = Allocate(image); !allocated)
		{
			return allocated;
		}
	}

	const VkDescriptorPoolSize size{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		                             .descriptorCount = UnfusedImages };
	const VkDescriptorPoolCreateInfo poolInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		                                       .pNext = nullptr,
		                                       .flags = 0,
		                                       .maxSets = UnfusedImages,
		                                       .poolSizeCount = 1,
		                                       .pPoolSizes = &size };

	if (Result<void> created =
	        Check(vkCreateDescriptorPool(m_Device->Handle(), &poolInfo, nullptr, &m_Pool), "vkCreateDescriptorPool");
	    !created)
	{
		return created;
	}

	for (Intermediate& image : m_Images)
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

		const VkDescriptorImageInfo binding{ .sampler = m_Sampler,
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

	for (std::size_t index = 0; index < Elements; ++index)
	{
		const auto element = static_cast<Element>(index);
		Result<VkPipeline> built = Build(element, element == Element::Composite ? format : UnfusedFormat);

		if (!built)
		{
			return std::unexpected{ built.error() };
		}

		m_Pipelines[index] = *built;
	}

	m_Ready = true;

	return {};
}

Result<void> Unfused::Allocate(Intermediate& image)
{
	const VkImageCreateInfo info{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = UnfusedFormat,
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

	// Device-local, which is the whole point of an offscreen nothing outside the GPU ever reads.
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
		return Failure(ENOMEM, "no device-local memory type satisfies an unfused intermediate");
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
		                                  .format = UnfusedFormat,
		                                  .components = { VK_COMPONENT_SWIZZLE_IDENTITY,
		                                                  VK_COMPONENT_SWIZZLE_IDENTITY,
		                                                  VK_COMPONENT_SWIZZLE_IDENTITY,
		                                                  VK_COMPONENT_SWIZZLE_IDENTITY },
		                                  .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };

	return Check(vkCreateImageView(m_Device->Handle(), &viewInfo, nullptr, &image.View), "vkCreateImageView");
}

Result<VkPipeline> Unfused::Build(Element element, VkFormat format)
{
	const auto selector = static_cast<std::int32_t>(element);
	const VkSpecializationMapEntry entry{ .constantID = 0, .offset = 0, .size = sizeof selector };
	const VkSpecializationInfo specialization{
		.mapEntryCount = 1, .pMapEntries = &entry, .dataSize = sizeof selector, .pData = &selector
	};

	const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
		VkPipelineShaderStageCreateInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		                                 .pNext = nullptr,
		                                 .flags = 0,
		                                 .stage = VK_SHADER_STAGE_VERTEX_BIT,
		                                 .module = m_Vertex,
		                                 .pName = "main",
		                                 .pSpecializationInfo = nullptr },
		VkPipelineShaderStageCreateInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		                                 .pNext = nullptr,
		                                 .flags = 0,
		                                 .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		                                 .module = m_Fragment,
		                                 .pName = "main",
		                                 .pSpecializationInfo = &specialization },
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
	const VkPipelineColorBlendAttachmentState& attachment = element == Element::Composite ? Over : Replace;
	const VkPipelineColorBlendStateCreateInfo blending{ .sType =
		                                                    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		                                                .pNext = nullptr,
		                                                .flags = 0,
		                                                .logicOpEnable = VK_FALSE,
		                                                .logicOp = VK_LOGIC_OP_CLEAR,
		                                                .attachmentCount = 1,
		                                                .pAttachments = &attachment,
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
		                                             .layout = m_Layout,
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

VkPipeline Unfused::For(Element element) const noexcept
{
	const auto index = static_cast<std::size_t>(element);

	return index < m_Pipelines.size() ? m_Pipelines[index] : VK_NULL_HANDLE;
}

void Unfused::Release() noexcept
{
	m_Ready = false;

	if (m_Device == nullptr || !m_Device->IsValid())
	{
		return;
	}

	for (VkPipeline& pipeline : m_Pipelines)
	{
		vkDestroyPipeline(m_Device->Handle(), pipeline, nullptr);
		pipeline = VK_NULL_HANDLE;
	}

	// The pool goes before the images for Render/Backdrop.cpp's reason: a descriptor set is freed by
	// its pool being destroyed, and freeing one that names a view this loop is about to destroy in
	// the other order is a use after free the validation layer would be right about.
	if (m_Pool != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorPool(m_Device->Handle(), m_Pool, nullptr);
		m_Pool = VK_NULL_HANDLE;
	}

	for (Intermediate& image : m_Images)
	{
		vkDestroyImageView(m_Device->Handle(), image.View, nullptr);
		vkDestroyImage(m_Device->Handle(), image.Image, nullptr);
		vkFreeMemory(m_Device->Handle(), image.Memory, nullptr);
		image = Intermediate{};
	}

	m_Size = {};
}

void Unfused::Destroy() noexcept
{
	Release();

	if (m_Device == nullptr || !m_Device->IsValid())
	{
		return;
	}

	if (m_Layout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(m_Device->Handle(), m_Layout, nullptr);
		m_Layout = VK_NULL_HANDLE;
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

	for (VkShaderModule* module : { &m_Fragment, &m_Vertex })
	{
		vkDestroyShaderModule(m_Device->Handle(), *module, nullptr);
		*module = VK_NULL_HANDLE;
	}
}
