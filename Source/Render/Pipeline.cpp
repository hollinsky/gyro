#include "Render/Pipeline.h"

#include <array>
#include <cerrno>
#include <cstdint>

#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Render/Shaders/Quad.frag.h"
#include "Render/Shaders/Quad.vert.h"
#include "Render/Vulkan.h"

namespace
{
// The blend, and it is the only one. Decision 95 fixes `over` as the scene's single blend mode, and
// the fragment stage hands back premultiplied components — so the source factor is one rather than
// the source alpha, and multiplying by alpha here as well is the double-darkened edge every
// compositor has shipped at least once.
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

// Viewport and scissor only. Everything else about this pipeline is fixed by the vocabulary rather
// than by the frame: the topology is two triangles, the blend is `over`, and there is no depth to
// bias. Damage is what varies within a recording, and it varies as a scissor.
constexpr std::array<VkDynamicState, 2> Dynamics{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
} // namespace

Result<void> QuadPipeline::Create(VulkanDevice& device)
{
	if (!device.IsValid())
	{
		return Failure(ENODEV, "pipelines built against an unopened device");
	}

	m_Device = &device;

	const VkShaderModuleCreateInfo vertexInfo{ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		                                       .pNext = nullptr,
		                                       .flags = 0,
		                                       .codeSize = sizeof QuadVertexSpirv,
		                                       .pCode = QuadVertexSpirv };

	if (Result<void> created =
	        Check(vkCreateShaderModule(device.Handle(), &vertexInfo, nullptr, &m_Vertex), "vkCreateShaderModule");
	    !created)
	{
		return created;
	}

	const VkShaderModuleCreateInfo fragmentInfo{ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		                                         .pNext = nullptr,
		                                         .flags = 0,
		                                         .codeSize = sizeof QuadFragmentSpirv,
		                                         .pCode = QuadFragmentSpirv };

	if (Result<void> created =
	        Check(vkCreateShaderModule(device.Handle(), &fragmentInfo, nullptr, &m_Fragment), "vkCreateShaderModule");
	    !created)
	{
		return created;
	}

	// Both stages read the whole block. Splitting it — corners to the vertex stage, fill and shape to
	// the fragment one — would save nothing and would make the two offsets a thing to keep in step
	// with two shaders instead of one struct.
	const VkPushConstantRange range{ .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		                             .offset = 0,
		                             .size = sizeof(QuadConstants) };

	// No descriptor sets at all, which is what an untextured quad needs and is worth noticing while
	// it is true: the first `DrawTexture` is what puts a sampled image behind a set here.
	const VkPipelineLayoutCreateInfo layoutInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		                                         .pNext = nullptr,
		                                         .flags = 0,
		                                         .setLayoutCount = 0,
		                                         .pSetLayouts = nullptr,
		                                         .pushConstantRangeCount = 1,
		                                         .pPushConstantRanges = &range };

	return Check(vkCreatePipelineLayout(device.Handle(), &layoutInfo, nullptr, &m_Layout), "vkCreatePipelineLayout");
}

VkPipeline QuadPipeline::For(VkFormat format, QuadVariant variant) const noexcept
{
	for (std::size_t index = 0; index < m_BuiltCount; ++index)
	{
		if (m_Built[index].Format == format && m_Built[index].Variant == variant)
		{
			return m_Built[index].Pipeline;
		}
	}

	return VK_NULL_HANDLE;
}

Result<void> QuadPipeline::Prepare(VkFormat format, ColorState output)
{
	if (m_Device == nullptr || m_Layout == VK_NULL_HANDLE)
	{
		return Failure(ENODEV, "no pipeline layout; the renderer did not come up");
	}

	if (format == VK_FORMAT_UNDEFINED)
	{
		return Failure(EINVAL, "no pipeline is built for a format the renderer cannot name");
	}

	// Refused at the binding rather than per item, because an output nothing can be encoded to is a
	// black screen either way and this is the call that can say so out loud. Chain.glsl carries the
	// argument: HLG's scene-to-display step needs the display's peak luminance and a `ColorState`
	// states a reference white, so an implementation without one has picked a peak on the user's
	// behalf and the picture is wrong in a way nobody can attribute.
	if (output.Transfer == TransferFunction::Hlg)
	{
		return Failure(EINVAL, "this renderer encodes no HLG output; ColorState carries no display peak luminance");
	}

	// Every source state that can reach this output, plus the pair that converts nothing. Enumerated
	// rather than built on demand because a frame may not compile — decision 62 — so `For` has to be
	// total over what an item can ask for, and the closed vocabulary is what makes that finite.
	for (std::uint32_t rounded = 0; rounded < 2; ++rounded)
	{
		if (Result<void> built = Build(format, QuadVariant{ .Run = rounded != 0 ? QuadRunCorner : 0U }); !built)
		{
			return built;
		}
	}

	constexpr std::array<TransferFunction, 3> Transfers{ TransferFunction::Srgb,
		                                                 TransferFunction::Linear,
		                                                 TransferFunction::Pq };
	constexpr std::array<ColorPrimaries, 3> Primaries{ ColorPrimaries::Bt709,
		                                               ColorPrimaries::DciP3,
		                                               ColorPrimaries::Bt2020 };

	for (const TransferFunction transfer : Transfers)
	{
		for (const ColorPrimaries primaries : Primaries)
		{
			for (std::uint32_t rounded = 0; rounded < 2; ++rounded)
			{
				const QuadVariant variant{ .Run = QuadRunConvert | (rounded != 0 ? QuadRunCorner : 0U),
					                       .SourceTransfer = transfer,
					                       .SourcePrimaries = primaries,
					                       .TargetTransfer = output.Transfer,
					                       .TargetPrimaries = output.Primaries };

				if (Result<void> built = Build(format, variant); !built)
				{
					return built;
				}
			}
		}
	}

	return {};
}

Result<void> QuadPipeline::Build(VkFormat format, QuadVariant variant)
{
	if (For(format, variant) != VK_NULL_HANDLE)
	{
		return {};
	}

	if (m_BuiltCount == m_Built.size())
	{
		return Failure(EINVAL, "more distinct bindings at once than this renderer builds pipelines for");
	}

	// The five constants Quad.frag declares, laid out contiguously so that one entry per constant can
	// name an offset into this object. Four bytes each and no padding to state: a specialization
	// entry is a size and an offset rather than a struct the front end has to agree about.
	const std::array<std::int32_t, 5> values{ static_cast<std::int32_t>(variant.Run),
		                                      static_cast<std::int32_t>(variant.SourceTransfer),
		                                      static_cast<std::int32_t>(variant.SourcePrimaries),
		                                      static_cast<std::int32_t>(variant.TargetTransfer),
		                                      static_cast<std::int32_t>(variant.TargetPrimaries) };
	std::array<VkSpecializationMapEntry, 5> entries{};

	for (std::uint32_t constant = 0; constant < entries.size(); ++constant)
	{
		entries[constant] =
			VkSpecializationMapEntry{ .constantID = constant,
			                          .offset = constant * static_cast<std::uint32_t>(sizeof(std::int32_t)),
			                          .size = sizeof(std::int32_t) };
	}

	const VkSpecializationInfo specialization{ .mapEntryCount = static_cast<std::uint32_t>(entries.size()),
		                                       .pMapEntries = entries.data(),
		                                       .dataSize = sizeof(values),
		                                       .pData = values.data() };

	// The vertex stage takes none of it. Nothing it does varies by variant — it places four corners
	// the producer already projected — and specializing it anyway would be one more vertex module per
	// variant for a program that is byte for byte the same.
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

	// Nothing bound and nothing described. The corners arrive as push constants and the six vertices
	// are `gl_VertexIndex`, so there is no buffer for the frame thread to fill and none for the
	// driver to fence.
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

	// Counted here and supplied at record time, which is what `VK_DYNAMIC_STATE_VIEWPORT` means.
	const VkPipelineViewportStateCreateInfo viewport{ .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		                                              .pNext = nullptr,
		                                              .flags = 0,
		                                              .viewportCount = 1,
		                                              .pViewports = nullptr,
		                                              .scissorCount = 1,
		                                              .pScissors = nullptr };

	// **Culls nothing, and that is decision 55 read from the far end.** Back faces are removed by the
	// producer, from the sign of the projected quad's own area, so a quad that reaches here is one
	// that faces the viewer. Asking the rasterizer to decide again would use a winding this pipeline
	// has no business having an opinion about — and would drop every node on a mirrored output.
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

	// One sample. Coverage is the fragment stage's distance field rather than the rasterizer's
	// sample count, which is what keeps an animated corner radius from costing a multisampled target
	// the size of the screen.
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
		                                                .pAttachments = &Over,
		                                                .blendConstants = { 0.0F, 0.0F, 0.0F, 0.0F } };
	const VkPipelineDynamicStateCreateInfo dynamic{ .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		                                            .pNext = nullptr,
		                                            .flags = 0,
		                                            .dynamicStateCount = static_cast<std::uint32_t>(Dynamics.size()),
		                                            .pDynamicStates = Dynamics.data() };

	// Dynamic rendering, so the attachment's format is stated here and there is no render pass object
	// to be compatible with — which is the whole reason this is one pipeline per format rather than
	// one pipeline.
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
		                                             // No depth attachment, so no depth state. Decision
		                                             // 55 composites in strict tree order and the list
		                                             // is already in it; a depth buffer would be a
		                                             // second answer to a question the order settles.
		                                             .pDepthStencilState = nullptr,
		                                             .pColorBlendState = &blending,
		                                             .pDynamicState = &dynamic,
		                                             .layout = m_Layout,
		                                             .renderPass = VK_NULL_HANDLE,
		                                             .subpass = 0,
		                                             .basePipelineHandle = VK_NULL_HANDLE,
		                                             .basePipelineIndex = -1 };

	// Counted after it is filled, which is the opposite of `BindTargets`'s rule and right for the
	// opposite reason: a failed creation writes nothing, so an entry incremented first would be a
	// null pipeline that `For` hands back as if it had been built.
	VkPipeline pipeline = VK_NULL_HANDLE;

	if (Result<void> created = Check(
			vkCreateGraphicsPipelines(m_Device->Handle(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
			"vkCreateGraphicsPipelines"
		);
	    !created)
	{
		return created;
	}

	m_Built[m_BuiltCount] = Built{ .Format = format, .Variant = variant, .Pipeline = pipeline };
	++m_BuiltCount;

	return {};
}

void QuadPipeline::Destroy() noexcept
{
	if (m_Device == nullptr || !m_Device->IsValid())
	{
		return;
	}

	for (std::size_t index = 0; index < m_BuiltCount; ++index)
	{
		vkDestroyPipeline(m_Device->Handle(), m_Built[index].Pipeline, nullptr);
		m_Built[index] = Built{};
	}

	m_BuiltCount = 0;

	if (m_Layout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(m_Device->Handle(), m_Layout, nullptr);
		m_Layout = VK_NULL_HANDLE;
	}

	// The modules go with everything built from them and not before: a `VkShaderModule` may be
	// destroyed as soon as the pipelines naming it exist, and doing that early buys nothing here
	// while making the teardown order matter.
	if (m_Fragment != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(m_Device->Handle(), m_Fragment, nullptr);
		m_Fragment = VK_NULL_HANDLE;
	}

	if (m_Vertex != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(m_Device->Handle(), m_Vertex, nullptr);
		m_Vertex = VK_NULL_HANDLE;
	}
}
