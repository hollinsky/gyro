#include "Render/Pipeline.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <span>

#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Render/Shaders/Quad.frag.h"
#include "Render/Shaders/Quad.vert.h"
#include "Render/Shaders/Shadow.frag.h"
#include "Render/Shaders/Shadow.vert.h"
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

// Everything about a pipeline here that is not its program: two triangles, `over`, one sample, no
// depth, no cull, and a viewport and scissor supplied per recording.
//
// **Shared by both programs on purpose.** The quad and the shadow differ in their shaders and their
// push block and in nothing else, and the two must composite the same way — a shadow blended by any
// other rule than the thing casting it is a fringe at every panel's edge. Written once here, they
// cannot drift; written twice, the second copy is the one somebody forgets.
[[nodiscard]] Result<VkPipeline> Assemble(
	VulkanDevice& device,
	VkPipelineLayout layout,
	std::span<const VkPipelineShaderStageCreateInfo> stages,
	VkFormat format
)
{
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
		                                             .layout = layout,
		                                             .renderPass = VK_NULL_HANDLE,
		                                             .subpass = 0,
		                                             .basePipelineHandle = VK_NULL_HANDLE,
		                                             .basePipelineIndex = -1 };

	VkPipeline pipeline = VK_NULL_HANDLE;

	if (Result<void> created = Check(
			vkCreateGraphicsPipelines(device.Handle(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
			"vkCreateGraphicsPipelines"
		);
	    !created)
	{
		return std::unexpected{ created.error() };
	}

	return pipeline;
}
} // namespace

Result<void> QuadPipeline::Create(VulkanDevice& device, VkDescriptorSetLayout textures)
{
	if (!device.IsValid())
	{
		return Failure(ENODEV, "pipelines built against an unopened device");
	}

	if (textures == VK_NULL_HANDLE)
	{
		return Failure(EINVAL, "no texture set layout; the importer did not come up");
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

	// One set, and it is Render/Textures.h's — the first `DrawTexture` is what put it here, exactly
	// as the comment this replaces predicted *(2026-08-23)*.
	//
	// **One layout for every variant, including the twenty that sample nothing.** A pipeline layout
	// that declares a set the shader does not use is legal and costs nothing to create, and what it
	// buys is that `vkCmdPushConstants` names one layout for every item in the list — a solid drawn
	// between two textures does not force a layout change, and the call site does not carry a branch
	// for which of two layouts an item's constants belong to. Two layouts would also make the two
	// halves *incompatible* for set binding, so a bound texture would be invalidated by the next
	// solid and rebound for the one after it.
	const VkPipelineLayoutCreateInfo layoutInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		                                         .pNext = nullptr,
		                                         .flags = 0,
		                                         .setLayoutCount = 1,
		                                         .pSetLayouts = &textures,
		                                         .pushConstantRangeCount = 1,
		                                         .pPushConstantRanges = &range };

	if (Result<void> created =
	        Check(vkCreatePipelineLayout(device.Handle(), &layoutInfo, nullptr, &m_Layout), "vkCreatePipelineLayout");
	    !created)
	{
		return created;
	}

	// The shadow program, brought up beside the quad and for the same reason: a device that refuses
	// this SPIR-V says so at startup rather than at the first window with a shadow under it.
	const VkShaderModuleCreateInfo shadowVertexInfo{ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		                                             .pNext = nullptr,
		                                             .flags = 0,
		                                             .codeSize = sizeof ShadowVertexSpirv,
		                                             .pCode = ShadowVertexSpirv };

	if (Result<void> created = Check(
			vkCreateShaderModule(device.Handle(), &shadowVertexInfo, nullptr, &m_ShadowVertex), "vkCreateShaderModule"
		);
	    !created)
	{
		return created;
	}

	const VkShaderModuleCreateInfo shadowFragmentInfo{ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		                                               .pNext = nullptr,
		                                               .flags = 0,
		                                               .codeSize = sizeof ShadowFragmentSpirv,
		                                               .pCode = ShadowFragmentSpirv };

	if (Result<void> created = Check(
			vkCreateShaderModule(device.Handle(), &shadowFragmentInfo, nullptr, &m_ShadowFragment),
			"vkCreateShaderModule"
		);
	    !created)
	{
		return created;
	}

	// Its own layout, because the block is a different size — and no descriptor sets here either, which
	// is stronger than the quad's case: an analytic shadow reads nothing at all, so there is no image
	// a future feature could put behind a set without changing what decision 104 promised.
	const VkPushConstantRange shadowRange{ .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		                                   .offset = 0,
		                                   .size = sizeof(ShadowConstants) };
	const VkPipelineLayoutCreateInfo shadowLayoutInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		                                               .pNext = nullptr,
		                                               .flags = 0,
		                                               .setLayoutCount = 0,
		                                               .pSetLayouts = nullptr,
		                                               .pushConstantRangeCount = 1,
		                                               .pPushConstantRanges = &shadowRange };

	return Check(
		vkCreatePipelineLayout(device.Handle(), &shadowLayoutInfo, nullptr, &m_ShadowLayout), "vkCreatePipelineLayout"
	);
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

VkPipeline QuadPipeline::Shadow(VkFormat format) const noexcept
{
	for (std::size_t index = 0; index < m_ShadedCount; ++index)
	{
		if (m_Shaded[index].Format == format)
		{
			return m_Shaded[index].Pipeline;
		}
	}

	return VK_NULL_HANDLE;
}

Result<void> QuadPipeline::Prepare(VkFormat format, ColorState output)
{
	if (m_Device == nullptr || m_Layout == VK_NULL_HANDLE || m_ShadowLayout == VK_NULL_HANDLE)
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

	// One shadow pipeline and no lattice, which is what an analytic shadow costs a binding: three
	// milliseconds against the quad's twenty-five, because there is nothing to enumerate. Shadow.frag
	// carries the reason — premultiplied black is the same in every colour state.
	if (Result<void> built = BuildShadow(format); !built)
	{
		return built;
	}

	// Every source state that can reach this output, plus the pair that converts nothing — and the
	// whole of that again for each way an item can get its colour. Enumerated rather than built on
	// demand because a frame may not compile: a variant an item wants and does not find is a refused
	// frame, so `For` can only be total if this has already made everything reachable.
	//
	// **Three content arms rather than four**, per `QuadVariantsPerBinding`: filled, sampled
	// premultiplied, and sampled straight. There is no fourth, because folding a fill's alpha is
	// three multiplies the renderer does on the CPU before the constants are written.
	constexpr std::array<std::uint32_t, 3> Content{ 0U, QuadRunSample, QuadRunSample | QuadRunPremultiply };

	for (const std::uint32_t content : Content)
	{
		for (std::uint32_t rounded = 0; rounded < 2; ++rounded)
		{
			const QuadVariant variant{ .Run = content | (rounded != 0 ? QuadRunCorner : 0U) };

			if (Result<void> built = Build(format, variant); !built)
			{
				return built;
			}
		}
	}

	constexpr std::array<TransferFunction, 3> Transfers{ TransferFunction::Srgb,
		                                                 TransferFunction::Linear,
		                                                 TransferFunction::Pq };
	constexpr std::array<ColorPrimaries, 3> Primaries{ ColorPrimaries::Bt709,
		                                               ColorPrimaries::DciP3,
		                                               ColorPrimaries::Bt2020 };

	for (const std::uint32_t content : Content)
	{
		for (const TransferFunction transfer : Transfers)
		{
			for (const ColorPrimaries primaries : Primaries)
			{
				for (std::uint32_t rounded = 0; rounded < 2; ++rounded)
				{
					const QuadVariant variant{ .Run = content | QuadRunConvert | (rounded != 0 ? QuadRunCorner : 0U),
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

	// Counted after it is filled, which is the opposite of `BindTargets`'s rule and right for the
	// opposite reason: a failed creation writes nothing, so an entry incremented first would be a
	// null pipeline that `For` hands back as if it had been built.
	const Result<VkPipeline> pipeline = Assemble(*m_Device, m_Layout, std::span{ stages }, format);

	if (!pipeline)
	{
		return std::unexpected{ pipeline.error() };
	}

	m_Built[m_BuiltCount] = Built{ .Format = format, .Variant = variant, .Pipeline = *pipeline };
	++m_BuiltCount;

	return {};
}

Result<void> QuadPipeline::BuildShadow(VkFormat format)
{
	if (Shadow(format) != VK_NULL_HANDLE)
	{
		return {};
	}

	if (m_ShadedCount == m_Shaded.size())
	{
		return Failure(EINVAL, "more distinct formats at once than this renderer builds shadow pipelines for");
	}

	// Neither stage is specialized, and there is nothing to specialize: the whole program is one
	// distance field and one blend, with no element a variant could compile out.
	const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
		VkPipelineShaderStageCreateInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		                                 .pNext = nullptr,
		                                 .flags = 0,
		                                 .stage = VK_SHADER_STAGE_VERTEX_BIT,
		                                 .module = m_ShadowVertex,
		                                 .pName = "main",
		                                 .pSpecializationInfo = nullptr },
		VkPipelineShaderStageCreateInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		                                 .pNext = nullptr,
		                                 .flags = 0,
		                                 .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		                                 .module = m_ShadowFragment,
		                                 .pName = "main",
		                                 .pSpecializationInfo = nullptr },
	};

	const Result<VkPipeline> pipeline = Assemble(*m_Device, m_ShadowLayout, std::span{ stages }, format);

	if (!pipeline)
	{
		return std::unexpected{ pipeline.error() };
	}

	m_Shaded[m_ShadedCount] = Shaded{ .Format = format, .Pipeline = *pipeline };
	++m_ShadedCount;

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

	for (std::size_t index = 0; index < m_ShadedCount; ++index)
	{
		vkDestroyPipeline(m_Device->Handle(), m_Shaded[index].Pipeline, nullptr);
		m_Shaded[index] = Shaded{};
	}

	m_ShadedCount = 0;

	if (m_ShadowLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(m_Device->Handle(), m_ShadowLayout, nullptr);
		m_ShadowLayout = VK_NULL_HANDLE;
	}

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

	if (m_ShadowFragment != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(m_Device->Handle(), m_ShadowFragment, nullptr);
		m_ShadowFragment = VK_NULL_HANDLE;
	}

	if (m_ShadowVertex != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(m_Device->Handle(), m_ShadowVertex, nullptr);
		m_ShadowVertex = VK_NULL_HANDLE;
	}
}
