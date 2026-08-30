#include "Render/Renderer.h"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Geometry/Region.h"
#include "Render/GpuClock.h"
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

// A unit direction on the glass. Two floats rather than Geometry's `Point`, because a direction is
// not a position and this file is the only place one is needed.
struct Axis
{
	float X = 0.0F;
	float Y = 0.0F;
};

// A quad resolved into the frame a shadow is drawn in: where its centre is, how far it reaches, and
// which way its own two axes point in device space.
struct ShadowFrame
{
	Point<DeviceSpace> Centre{};
	Size<DeviceSpace> Extent{};
	Axis Across{ 1.0F, 0.0F };
	Axis Down{ 0.0F, 1.0F };
};

// Whether a projected quad is still a rectangle, and its frame where it is.
//
// **This is decision 133's split, and the two cases it separates look nothing alike on screen.** A
// node *spun* in the plane is a photograph lying at an angle on a desk: still a rectangle, just not
// square to the screen, and its shadow is not in question — the shape turns and the light does not.
// A node *tilted out* of the plane is a card flipping over, where perspective makes the near edge
// longer than the far one and the quad stops being a rectangle at all. That one is open: whether the
// shadow stretches away from the near edge, and whether it is softer at the end that is further from
// what it falls on, wants a transition in front of it and there is none.
//
// **Verified by reconstruction rather than by testing for skew**, because the tolerance is then in the
// units that matter. A perpendicularity test on two edge vectors is a tolerance on a cross product,
// which means one number for a taskbar and another for a thumbnail; rebuilding the four corners from
// the frame and asking how far they moved is a tolerance in device pixels, at every size.
[[nodiscard]] std::optional<ShadowFrame> Rectangular(const Quad& quad) noexcept
{
	// A sixty-fourth of a pixel: far below what a target can hold and far above what single precision
	// costs a corner at the scale of a panel.
	constexpr float Tolerance = 1.0F / 64.0F;

	for (const float weight : quad.Weights)
	{
		if (std::abs(weight - 1.0F) > Tolerance)
		{
			return std::nullopt;
		}
	}

	const Axis across{ quad.Corners[1].X - quad.Corners[0].X, quad.Corners[1].Y - quad.Corners[0].Y };
	const Axis down{ quad.Corners[3].X - quad.Corners[0].X, quad.Corners[3].Y - quad.Corners[0].Y };

	const float width = std::hypot(across.X, across.Y);
	const float height = std::hypot(down.X, down.Y);

	if (width <= 0.0F || height <= 0.0F)
	{
		return std::nullopt;
	}

	const ShadowFrame frame{ .Centre = { 0.5F * (quad.Corners[0].X + quad.Corners[2].X),
		                                 0.5F * (quad.Corners[0].Y + quad.Corners[2].Y) },
		                     .Extent = { 0.5F * width, 0.5F * height },
		                     .Across = { across.X / width, across.Y / width },
		                     .Down = { down.X / height, down.Y / height } };

	// The four corners this frame would produce, against the four that arrived. A parallelogram fails
	// on the corner opposite the origin, a sheared quad on two of them, and a perspective one on all
	// four — one test rather than three, and the number it reports is a distance on the glass.
	for (std::size_t corner = 0; corner < 4; ++corner)
	{
		const float horizontal = corner == 1 || corner == 2 ? frame.Extent.Width : -frame.Extent.Width;
		const float vertical = corner == 2 || corner == 3 ? frame.Extent.Height : -frame.Extent.Height;

		const Point<DeviceSpace> rebuilt{ frame.Centre.X + horizontal * frame.Across.X + vertical * frame.Down.X,
			                              frame.Centre.Y + horizontal * frame.Across.Y + vertical * frame.Down.Y };

		if (std::abs(rebuilt.X - quad.Corners[corner].X) > Tolerance ||
		    std::abs(rebuilt.Y - quad.Corners[corner].Y) > Tolerance)
		{
			return std::nullopt;
		}
	}

	return frame;
}

// What one shadow draw is told: the item's frame, the light's three numbers, and how far past the
// rect the quad has to reach.
//
// **The radius crosses in device pixels while `DrawItem` states it in the node's own extent**, and the
// conversion is here because this is where both are known. The smaller of the two axis scales, so an
// anisotropically scaled node's shadow rounds less than its content rather than more — a shadow whose
// corner is rounder than the window's shows as a light gap at four corners, and the other direction
// hides under the node instead.
[[nodiscard]] ShadowConstants
ShadowFor(const DrawItem& item, const ShadowFrame& frame, PixelSize<DeviceSpace> target) noexcept
{
	const float width = 2.0F * frame.Extent.Width;
	const float height = 2.0F * frame.Extent.Height;

	const float horizontal = item.Extent.Width > 0.0F ? width / item.Extent.Width : 1.0F;
	const float vertical = item.Extent.Height > 0.0F ? height / item.Extent.Height : 1.0F;
	const float radius = std::min({ item.Radius * std::min(horizontal, vertical), 0.5F * width, 0.5F * height });

	ShadowConstants constants{};

	constants.Rect[0] = frame.Centre.X;
	constants.Rect[1] = frame.Centre.Y;
	constants.Rect[2] = frame.Extent.Width;
	constants.Rect[3] = frame.Extent.Height;

	constants.Shape[0] = radius;
	constants.Shape[1] = item.Lift.Offset;
	constants.Shape[2] = item.Lift.Softness;

	// The node's own opacity takes its shadow with it, which is the whole of what decision 105 means
	// by the shadow having to animate: a window that fades out and leaves its shadow behind is the
	// artefact that entry is about, at the one moment the eye is on the window.
	constants.Shape[3] = item.Lift.Opacity * item.Opacity;

	constants.Target[0] = static_cast<float>(target.Width);
	constants.Target[1] = static_cast<float>(target.Height);
	constants.Target[2] = Expansion(item.Lift);

	constants.Basis[0] = frame.Across.X;
	constants.Basis[1] = frame.Across.Y;
	constants.Basis[2] = frame.Down.X;
	constants.Basis[3] = frame.Down.Y;

	return constants;
}

// What the quad pipeline can express, asked of one item.
//
// **Every branch here is a picture somebody would otherwise have to notice was wrong.** A group drawn
// unflattened is a menu whose overlap shows through mid-fade; a dressing drawn as nothing is a panel
// that stops being glass; an elevation drawn as nothing is a dialog that stops looking lifted. Each
// of those composites successfully and is not what the scene said, which is the failure
// Seam/Renderer.h's *an item the renderer cannot express* exists to keep out of the tree.
//
// **A `DrawTexture` is no longer among them** *(2026-08-23)*, and what replaced the refusal is
// Render/Textures.h. What did not change is the shape of the answer for an id that resolves to
// nothing: that is skipped at the draw rather than refused here, because Core/Texture.h makes a stale
// id a lifetime bug reported somewhere other than a frame.
[[nodiscard]] Result<void> Expressible(const DrawItem& item, ColorState output) noexcept
{
	if (std::holds_alternative<DrawGroup>(item.Content))
	{
		return Failure(EINVAL, "this renderer flattens no groups yet; decision 60's offscreen is not built");
	}

	// **A tilted node's shadow is an open question rather than an unbuilt feature**, which is why this
	// refuses instead of drawing something. A card flipping over is further from what it falls on at
	// one edge than the other, so the shadow both stretches and changes softness across itself, and
	// Docs/Open.md leaves which of those matters to the first transition that turns a node. There is
	// none — so what would be shipped meanwhile is whichever answer happened to be easier to write,
	// which is the way a question gets decided by accident.
	//
	// **A node spun in the plane is not that case and is not refused** (decision 133). It is a
	// photograph lying at an angle: the shape turns and the light does not, which is the one answer a
	// person would accept, and `Rectangular` is what tells the two apart.
	if (item.Lift.Draws() && !Rectangular(item.Shape))
	{
		return Failure(
			EINVAL, "this renderer casts no shadow from a node tilted out of the plane; Docs/Open.md has not settled it"
		);
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
// The four numbers a sampling variant reads out of `Fill`: the source rectangle in normalized
// texture coordinates.
//
// **Both of Seam/Renderer.h's conventions are resolved here, on the CPU.** `DrawTexture::Source` is
// in `BufferSpace` texels and *empty means the whole image*, and the shader wants neither — it wants
// an origin and an extent it can multiply a surface-local fraction by. Doing it per item is two
// divides; doing it in the shader would be two divides and the image's own size in the push block,
// which is sixteen bytes the block does not have.
[[nodiscard]] std::array<float, 4> SourceRect(const DrawTexture& texture, PixelSize<BufferSpace> size) noexcept
{
	const auto width = static_cast<float>(size.Width);
	const auto height = static_cast<float>(size.Height);

	// A zero-sized image cannot be adopted — `TextureSource::IsValid` refuses one — so this is the
	// belt on a braces, and the whole-image answer is the one that draws something rather than a NaN.
	if (texture.Source.IsEmpty() || width <= 0.0F || height <= 0.0F)
	{
		return { 0.0F, 0.0F, 1.0F, 1.0F };
	}

	return { texture.Source.Origin.X / width,
		     texture.Source.Origin.Y / height,
		     texture.Source.Extent.Width / width,
		     texture.Source.Extent.Height / height };
}

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

VulkanRenderer::VulkanRenderer(const IClock& clock, VulkanDevice& device, VulkanTextures& textures, Fusion fusion)
	: m_Clock{ &clock }, m_Device{ &device }, m_Textures{ &textures }, m_Fusion{ fusion }
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

	// **Created only where the device said the family can timestamp, and a failure here is not fatal.**
	// The renderer's job is to draw; measuring what it cost is what lets the frame loop schedule it
	// well, and a machine that cannot report one still wants a desktop. So this leaves the pool null
	// and `CollectCosts` answers nothing, which is the same state a device with no timestamp support
	// is in — one path rather than two.
	if (device.Description().MeasuresGpuTime())
	{
		const VkQueryPoolCreateInfo queryInfo{ .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			                                   .pNext = nullptr,
			                                   .flags = 0,
			                                   .queryType = VK_QUERY_TYPE_TIMESTAMP,
			                                   .queryCount = MaxStamps * MaxRenderTargets,
			                                   .pipelineStatistics = 0 };

		if (Result<void> created =
		        Check(vkCreateQueryPool(device.Handle(), &queryInfo, nullptr, &m_Queries), "vkCreateQueryPool");
		    !created)
		{
			spdlog::warn("no GPU cost measurement on this device: {}", created.error().Context());
			m_Queries = VK_NULL_HANDLE;
		}
	}

	// The overdraw counter, and it is reserved beside the timestamps rather than on demand for the
	// ordinary reason a frame path allocates nothing: a person asks for a trace when the stutter has
	// already happened, and a pool created at that moment is a `vkCreateQueryPool` on the `SCHED_FIFO`
	// thread. One query per target, holding one counter.
	if (device.Description().CountsPipelineStatistics)
	{
		const VkQueryPoolCreateInfo statisticsInfo{ .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			                                        .pNext = nullptr,
			                                        .flags = 0,
			                                        .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS,
			                                        .queryCount = MaxRenderTargets,
			                                        .pipelineStatistics =
			                                            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT };

		if (Result<void> created =
		        Check(vkCreateQueryPool(device.Handle(), &statisticsInfo, nullptr, &m_Statistics), "vkCreateQueryPool");
		    !created)
		{
			spdlog::warn("no fragment count on this device: {}", created.error().Context());
			m_Statistics = VK_NULL_HANDLE;
		}
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

		// Decision 142's hint, opened once here rather than per frame: an import is a node open and an
		// ioctl, and the timeline it names lives exactly as long as this renderer does. It logs its own
		// absence and comes up invalid where the node will not adopt the descriptor, because a frame
		// drawn without a deadline is the frame gyro would have drawn anyway.
		m_Deadline = FenceDeadline::Open(device.Description().RenderMinor, m_TimelineFd.Borrow());
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

	if (Result<void> created = m_Pipeline.Create(device, textures.SetLayout()); !created)
	{
		m_Status = created;
		Destroy();

		return;
	}

	// Last, and only once everything above came up. A renderer registered and then abandoned would
	// leave the importer holding a pointer to an object whose timeline never advances, which stalls
	// every retirement on the machine behind a renderer that never draws.
	textures.Attach(*this);
}

VulkanRenderer::~VulkanRenderer()
{
	// **Before anything is torn down**, because `Detach` is what waits for this renderer's queued work
	// and releases the images that were waiting on it. Doing it after `Destroy` would destroy the
	// timeline the wait is against.
	if (m_Textures != nullptr)
	{
		m_Textures->Detach(*this);
	}

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

	// Seam/Capture.h's staging, and it is reserved here for the same reason the unfused intermediates
	// above are: this is where the shape is known and it is not inside a frame. Only where the policy
	// asked for readback, and a failure is *not* the bind's — a machine that cannot stage a capture
	// still composites, and refusing to come up over a debugging convenience would be the tail wagging
	// the compositor. It is logged by whoever presses the key, through the refusal `ReadTarget` gives.
	if (m_Device->TargetsAreReadable() && m_TargetCount > 0)
	{
		(void)m_Readback.Open(*m_Device, m_Slots[0].Size, targets[0].Format);
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

	// **`SAMPLED` where the modifier permits it, because a gather reads the target back.**
	// Render/Backdrop.h extracts the backdrop out of the composite itself rather than out of a second
	// copy of it, so a dressed output needs its own target legible to a shader. Asking for it
	// unconditionally would refuse the bind on a compressed modifier that renders fine, so it is asked
	// for where the driver lists it and the material falls to its tint where it does not.
	//
	// **`TRANSFER_SRC` where the policy asked for readback**, and it has to be asked for on both arms
	// or neither: Render/Device.h's export path states the same set, and an image allocated under a
	// narrower usage than the one it is imported under is a difference the driver is entitled to
	// notice. Seam/Capture.h is why it is a policy rather than always on.
	const VkImageUsageFlags usage =
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		(samplable ? VkImageUsageFlags{ VK_IMAGE_USAGE_SAMPLED_BIT } : VkImageUsageFlags{ 0 }) |
		(m_Device->TargetsAreReadable() ? VkImageUsageFlags{ VK_IMAGE_USAGE_TRANSFER_SRC_BIT } :
	                                      VkImageUsageFlags{ 0 });

	// Render/Device.h owns the create info, because stating a layout the presenter already committed
	// to is the same problem here and in Render/Textures.h and the arm that has no modifier to state
	// it with must not exist in two copies.
	Result<VkImage> imported = m_Device->ImportImage(
		{ static_cast<std::uint32_t>(target.Size.Width), static_cast<std::uint32_t>(target.Size.Height) },
		target.Format,
		plane,
		usage
	);

	if (!imported)
	{
		return std::unexpected{ imported.error() };
	}

	slot.Image = *imported;

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
	// Before the early return, because a reservation outlives an empty target set: a bind that failed
	// leaves the count at zero and the staging buffer still holding a mapping onto a device the root
	// is about to tear down.
	m_Readback.Reset();

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

		const bool samples = std::holds_alternative<DrawTexture>(item.Content);

		if (m_Pipeline.For(slot.Format, QuadVariant::For(item.Color, m_Output, item.Radius > 0.0F, samples)) ==
		    VK_NULL_HANDLE)
		{
			return Failure(EINVAL, "no pipeline was built for this item's colour conversion on this target's format");
		}

		if (item.Lift.Draws() && m_Pipeline.Shadow(slot.Format) == VK_NULL_HANDLE)
		{
			return Failure(EINVAL, "no shadow pipeline was built for this target's format");
		}
	}

	// The distinct client images this list reads, collected in the same *nothing has been recorded
	// yet* window as the checks above, because overflowing the batch is the one thing about sampling
	// that has to be a refusal rather than a skip: a texture drawn without its ownership acquired is
	// not a missing window, it is a window drawn out of a cache the client wrote behind.
	const std::uint32_t sampled = GatherSampled(request.Items);

	if (sampled > MaxSampledImages)
	{
		return Failure(EINVAL, "this frame samples more distinct images than one composite acquires at once");
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

	// **The reset is recorded rather than performed from the host, and that is one fewer device
	// feature to enable.** `vkResetQueryPool` needs `hostQueryReset`, which this device does not ask
	// for; `vkCmdResetQueryPool` is core and costs nothing here, because the pair being reset belongs
	// to this target and the `LastSubmit` poll above has already established that its previous
	// submission has landed.
	// **How many of the target's stamps this frame resets is how many it may write.** A trace ring
	// armed on this thread buys the whole run of them; otherwise the pair is all there is, which is
	// exactly what Frame/Budget.h has always been fed. Resetting only what will be written keeps the
	// untraced frame's cost identical to what it was.
	m_Stamping = Stamping{ .Target = request.Target, .Count = 0, .Active = m_Queries != VK_NULL_HANDLE && IsTracing() };

	// Cleared whole, so that a record that fails after this point cannot leave a pending cost naming
	// a run of stamps this command buffer never wrote.
	PendingCost& pending = m_Pending[request.Target];
	pending = PendingCost{};

	if (m_Queries != VK_NULL_HANDLE)
	{
		const std::uint32_t reserved = m_Stamping.Active ? MaxStamps : 2;
		vkCmdResetQueryPool(command, m_Queries, MaxStamps * request.Target, reserved);
		vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_Queries, MaxStamps * request.Target);

		// The first span, opened at the moment the GPU reaches this batch and named for what it
		// covers: the ownership transfers, the clear, and every item drawn into the target before the
		// first glass panel interrupts the pass.
		pending.Names[0] = "composite";
		m_Stamping.Count = 1;
	}

	// The whole command buffer, begun outside every render pass instance because it spans several of
	// them. Only while tracing: this counter is nobody's input, and the frame loop must not pay for an
	// instrument it does not read.
	if (m_Statistics != VK_NULL_HANDLE && m_Stamping.Active)
	{
		vkCmdResetQueryPool(command, m_Statistics, request.Target, 1);
		vkCmdBeginQuery(command, m_Statistics, request.Target, 0);
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

	// Before the render pass opens, because a queue-family ownership transfer is not something Vulkan
	// permits inside a rendering instance — and because everything the pass draws may read them.
	TransferSampled(command, sampled, true);

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
			else if (item.Lift.Draws())
			{
				// Nothing here reads the target, so the shadow simply goes first — under the item, which
				// is what preorder already means for the tile and the window above it in decision 99's
				// overview thumbnail. The dressed case is `Dress`'s, because a chain has to have read the
				// target before this darkens it.
				Shade(command, slot, item, std::span{ rects.data(), count }, bound);
			}

			const DrawSolid* solid = std::get_if<DrawSolid>(&item.Content);
			const DrawTexture* texture = std::get_if<DrawTexture>(&item.Content);

			// A dressing with nothing to draw. What is left after the branch above is an item whose
			// whole content was a dressing nobody set — a caller's bug that draws nothing, which is
			// the same answer Seam/Renderer.h gives an empty `DrawGroup`.
			if (solid == nullptr && texture == nullptr)
			{
				continue;
			}

			BoundTexture image{};

			if (texture != nullptr)
			{
				image = m_Textures->Find(texture->Texture);

				// Core/Texture.h's answer, and the only silent skip in this loop: a client destroyed
				// the buffer while a published snapshot still named it, which resolves to nothing
				// rather than to whatever took the slot. Drawing a black rectangle instead would put
				// a hole in the screen for a condition the dispatch thread has already handled.
				if (!image.IsValid())
				{
					continue;
				}
			}

			// Decision 62's reference execution, where this renderer was built for it: the same
			// chain, one pass per element, through an intermediate that rounds.
			//
			// **It draws fills only, and a texture under it is skipped rather than refused.** The
			// reference path has no sampling element yet, so there is nothing for the oracle to
			// compare a textured item against — and this renderer is only ever constructed this way
			// by a test that asked for it by name. Docs/Open.md carries what is owed.
			if (m_Fusion == Fusion::Separate)
			{
				if (solid != nullptr)
				{
					Separate(command, slot, item, *solid, request, std::span{ rects.data(), count }, bound);
				}

				continue;
			}

			const VkPipeline pipeline = m_Pipeline.For(
				slot.Format, QuadVariant::For(item.Color, m_Output, item.Radius > 0.0F, texture != nullptr)
			);

			if (pipeline != bound)
			{
				vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				bound = pipeline;
			}

			// Bound per textured item rather than tracked, because a set binding is cheap and the
			// state that would have to be tracked is invalidated by the dressing and shadow programs
			// this loop interleaves. Nothing is bound for a fill: those variants declare the set and
			// make no static use of it, so Vulkan requires none.
			if (image.Set != VK_NULL_HANDLE)
			{
				vkCmdBindDescriptorSets(
					command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline.Layout(), 0, 1, &image.Set, 0, nullptr
				);
			}

			QuadConstants constants = Constants(item, solid != nullptr ? *solid : DrawSolid{}, slot.Size, m_Output);

			if (texture != nullptr)
			{
				const std::array<float, 4> source = SourceRect(*texture, image.Size);

				constants.Fill[0] = source[0];
				constants.Fill[1] = source[1];
				constants.Fill[2] = source[2];
				constants.Fill[3] = source[3];
			}
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

	// Handed back in the same breath as the target, and for the mirror of the target's reason: these
	// pixels are a client's, and holding ownership past the frame that read them would leave the next
	// acquire without a release to match.
	TransferSampled(command, sampled, false);

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

	// After the release barrier rather than before it, so that the span covers everything this
	// submission asks the GPU to do — the handback to `VK_QUEUE_FAMILY_FOREIGN_EXT` included, since a
	// presenter cannot read the target until it has happened.
	if (m_Statistics != VK_NULL_HANDLE && m_Stamping.Active)
	{
		vkCmdEndQuery(command, m_Statistics, request.Target);
	}

	// **The closing stamp goes at the end of the run rather than at index one, and it always goes.**
	// It is what turns the last open span into a span and what `GpuCost` is the difference across, so
	// a submission that spent its whole stamp budget on glass panels still reports what the frame
	// cost — the closer displaces the last mark rather than being dropped behind it.
	if (m_Queries != VK_NULL_HANDLE)
	{
		const std::uint32_t closing = m_Stamping.Count < MaxStamps ? m_Stamping.Count : MaxStamps - 1;
		vkCmdWriteTimestamp(
			command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_Queries, MaxStamps * request.Target + closing
		);
		pending.Stamps = closing + 1;
	}

	if (Result<void> ended = Check(vkEndCommandBuffer(command), "vkEndCommandBuffer"); !ended)
	{
		return std::unexpected{ ended.error() };
	}

	const std::uint64_t value = m_Submitted.load(std::memory_order_relaxed) + 1;
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

	// **Published before the submit rather than after, and that order is the retirement rule.**
	// Render/Textures.h stamps a doomed image with this number on the dispatch thread, so a value that
	// lags the queue by even one submission is an image freed while a command buffer naming it is
	// still executing — which is the whole of what Seam/Importer.h's second half is about. Storing
	// first can only *overstate* what is in flight, and that costs a retirement one dispatch iteration.
	m_Submitted.store(value, std::memory_order_release);

	if (Result<void> submitted =
	        Check(vkQueueSubmit(m_Device->Queue(), 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit");
	    !submitted)
	{
		// Nothing reached the queue, so nothing can be reading anything — and leaving the number
		// raised would have every retirement on the machine waiting for a value the timeline will
		// never signal. The next successful submit takes this same value, so a stamp taken in between
		// is satisfied rather than stranded.
		m_Submitted.store(value - 1, std::memory_order_release);

		return std::unexpected{ submitted.error() };
	}

	slot.LastSubmit = value;

	// **Decision 142, and this is the earliest instant it can be said.** A timeline point has no fence
	// behind it until something is submitted against it, so the deadline could not have been stated
	// with the request; a microsecond after `vkQueueSubmit` it can, and the driver has it before it has
	// begun running the batch. One ioctl that cannot block — see Render/Deadline.h — so it is inside
	// the frame section on purpose rather than despite it.
	m_Deadline.State(value, request.Deadline);

	// Only after the submit succeeded. A refused submission wrote no timestamps, and claiming one is
	// pending would leave a pair the next record resets while `CollectCosts` is still expecting it.
	// The names and the stamp count were filled in as the buffer was recorded, so this fills in the
	// rest rather than replacing the record — a fresh `PendingCost` here would throw away the run this
	// submission just wrote.
	if (m_Queries != VK_NULL_HANDLE)
	{
		pending.Submit = value;
		pending.Generation = request.CostGeneration;
		pending.Mode = request.Mode;
		pending.Trace = request.Trace;
		pending.Frame = request.Frame;

		// Taken here rather than before the submit, so that the instant is one the batch was certainly
		// queued by. See `PendingCost::SubmittedAt` for why an anchor this loose is still one a run may
		// be drawn from.
		pending.SubmittedAt = m_Clock->Now();

		// Read here, near the work, rather than at collection frames later where the clock has moved and
		// the part has parked again. Both halves: on the frame path the actual clock is very often a
		// reading of a gated GPU, and the commanded point beside it is what tells that apart from a part
		// genuinely running slowly.
		const GpuClock::Reading reading = m_Device->ReadClock();
		pending.ClockMhz = reading.ActualMhz;
		pending.RequestedMhz = reading.RequestedMhz;
	}

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

void VulkanRenderer::Mark(VkCommandBuffer command, const char* name) noexcept
{
	// One short of the ceiling, because the closing stamp needs the last slot. Running out stops the
	// cutting and loses nothing else: the spans already opened still close, and the last one simply
	// runs to the end of the frame.
	if (!m_Stamping.Active || m_Stamping.Count + 1 >= MaxStamps)
	{
		return;
	}

	// **`BOTTOM_OF_PIPE` on every mark, which is what makes the spans tile.** A top-of-pipe stamp is
	// written when the GPU *reaches* the command, which on a pipelined part is long before the work in
	// front of it has finished — so consecutive top-of-pipe spans overlap and their sum is larger than
	// the frame. Bottom-of-pipe is written when everything recorded before it has completed, so span
	// *n* ends exactly where span *n + 1* begins and the run adds up to the pair the budget reads.
	// This costs nothing here only because every call site sits on a barrier the composite already
	// had; see `MaxStamps`.
	vkCmdWriteTimestamp(
		command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_Queries, MaxStamps * m_Stamping.Target + m_Stamping.Count
	);

	m_Pending[m_Stamping.Target].Names[m_Stamping.Count] = name;
	++m_Stamping.Count;
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

void VulkanRenderer::Shade(
	VkCommandBuffer command,
	const Slot& slot,
	const DrawItem& item,
	std::span<const VkClearRect> rects,
	VkPipeline& bound
) const noexcept
{
	const VkPipeline pipeline = m_Pipeline.Shadow(slot.Format);

	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

	// The outer loop's memo is invalidated rather than updated, for `Dress`'s reason: what is bound now
	// belongs to a different layout, so the next item has to bind again whatever it wanted.
	bound = VK_NULL_HANDLE;

	// Total, because `Expressible` refused every item this could fail for before recording began — the
	// same contract the pipeline lookup above runs under.
	const std::optional<ShadowFrame> frame = Rectangular(item.Shape);

	if (!frame)
	{
		return;
	}

	const ShadowConstants constants = ShadowFor(item, *frame, slot.Size);

	vkCmdPushConstants(
		command,
		m_Pipeline.ShadowLayout(),
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

		// No chain, so nothing has read the target and the shadow can go down before the tint. It has
		// to: decision 34's third rung draws the material as a translucent fill rather than an opaque
		// one, so a shadow left under it would show *through* the panel — the same darkening at the
		// same edges that the chained path avoids by ordering, arriving by transparency instead.
		if (item.Lift.Draws())
		{
			Shade(command, slot, item, rects, bound);
		}

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

	// The composite so far is over here, and everything from here to the resumed pass is this panel's
	// glass. Three marks cut it into the extract that reads the screen, the separable chain that blurs
	// it, and the dressing itself — each of them at a barrier the chain already needed.
	Mark(command, "extract");

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

	Mark(command, "blur");

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

	Mark(command, "composite");

	BeginTarget(command, slot, request);

	const VkViewport viewport{ .x = 0.0F,
		                       .y = 0.0F,
		                       .width = static_cast<float>(slot.Size.Width),
		                       .height = static_cast<float>(slot.Size.Height),
		                       .minDepth = 0.0F,
		                       .maxDepth = 1.0F };
	vkCmdSetViewport(command, 0, 1, &viewport);

	// **Here, and this is the position decision 104 spends a section on.** The extract above has
	// already sampled the target, so what the blur carries is the picture as of before this item
	// began — and the shadow lands after it, where the panel about to be drawn cannot read it. A line
	// earlier and every glass panel on screen has a dark halo just inside its own edge, worst where it
	// is most transparent, which is exactly where a person is looking when they judge whether the
	// glass is any good.
	if (item.Lift.Draws())
	{
		Shade(command, slot, item, rects, bound);
	}

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

std::uint32_t VulkanRenderer::GatherSampled(std::span<const DrawItem> items) noexcept
{
	std::uint32_t count = 0;

	for (const DrawItem& item : items)
	{
		const DrawTexture* const texture = std::get_if<DrawTexture>(&item.Content);

		if (texture == nullptr)
		{
			continue;
		}

		const VkImage image = m_Textures->Find(texture->Texture).Foreign;

		// Null for an image this device filled itself, which never left this queue's ownership, and
		// null for an id that resolves to nothing — which the draw loop skips for Core/Texture.h's
		// reason and which therefore has nothing to acquire either.
		if (image == VK_NULL_HANDLE)
		{
			continue;
		}

		bool seen = false;

		// A linear scan, because the same client image appearing twice in one list is the ordinary
		// case rather than the exception — decision 99's overview draws every window's own texture as
		// a tile *and* the focused one at full size — and an unmatched second acquire on an image
		// already owned is not a pair. At these sizes the scan is cheaper than anything with a hash
		// in it, and it allocates nothing, which is the constraint that decides it.
		for (std::uint32_t index = 0; index < count && index < MaxSampledImages; ++index)
		{
			seen = seen || m_Sampled[index] == image;
		}

		if (seen)
		{
			continue;
		}

		if (count < MaxSampledImages)
		{
			m_Sampled[count] = image;
		}

		// Counted past the cap so the caller can refuse on the true number rather than on a clamped
		// one, which would silently draw the first hundred and twenty-eight correctly and the rest
		// out of a stale cache.
		++count;
	}

	return count;
}

void VulkanRenderer::TransferSampled(VkCommandBuffer command, std::uint32_t count, bool acquiring) const noexcept
{
	if (count == 0)
	{
		return;
	}

	const std::uint32_t family = m_Device->QueueFamily();

	// On the stack rather than a member, and it is not an allocation: decision 36 forbids the heap
	// inside the frame section and says nothing about automatic storage, which is what every other
	// arena in this function already uses.
	std::array<VkImageMemoryBarrier, MaxSampledImages> barriers{};

	for (std::uint32_t index = 0; index < count; ++index)
	{
		barriers[index] =
			acquiring ? Transfer(m_Sampled[index], VK_QUEUE_FAMILY_FOREIGN_EXT, family, 0, VK_ACCESS_SHADER_READ_BIT) :
						Transfer(m_Sampled[index], family, VK_QUEUE_FAMILY_FOREIGN_EXT, 0, 0);
	}

	// `TOP_OF_PIPE` to `FRAGMENT_SHADER` acquiring, and `FRAGMENT_SHADER` to `BOTTOM_OF_PIPE`
	// releasing — the same two directions the target's own pair uses one screenful up, because the
	// dependency is the same shape: nothing in this submission produced these pixels, and nothing in
	// it consumes them after the pass.
	vkCmdPipelineBarrier(
		command,
		acquiring ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		acquiring ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0,
		0,
		nullptr,
		0,
		nullptr,
		count,
		barriers.data()
	);
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

Result<void> VulkanRenderer::ReadTarget(const TargetReadback& request)
{
	if (!m_Readback.IsOpen())
	{
		return Failure(ENOTSUP, "this device's targets were not created for readback; start gyro with --capture");
	}

	if (request.Target >= m_TargetCount || m_Slots[request.Target].Image == VK_NULL_HANDLE)
	{
		return Failure(EINVAL, "reading back a target that is not bound");
	}

	// **The composite's own work, waited for here rather than by the copy's submit.** A submit-time
	// wait would want the timeline as a `VkSemaphore` in a `VkTimelineSemaphoreSubmitInfo`, which is
	// the same wait one indirection further away — and this way the wait and the copy are two failures
	// with two sentences rather than one that cannot say which half stalled.
	//
	// A point this renderer did not issue is treated as reached, which is `IsComplete`'s rule above and
	// is right for the same reason: of the two answers available, the one that does not park a
	// `SCHED_FIFO` thread forever on a descriptor nothing here will ever signal.
	if (!request.After.IsImmediate() && m_Timeline != VK_NULL_HANDLE && request.After.Timeline == m_TimelineFd.Borrow())
	{
		const std::uint64_t value = request.After.Value;
		const VkSemaphoreWaitInfo wait{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			                            .pNext = nullptr,
			                            .flags = 0,
			                            .semaphoreCount = 1,
			                            .pSemaphores = &m_Timeline,
			                            .pValues = &value };

		constexpr std::uint64_t Second = 1'000'000'000;

		if (const Result<void> reached = Check(vkWaitSemaphores(m_Device->Handle(), &wait, Second), "vkWaitSemaphores");
		    !reached)
		{
			return reached;
		}
	}

	// `GENERAL`, which is what this renderer keeps a composite in for its whole life — see `Transfer`
	// at the top of this file for why there is no optimal layout for an image somebody else may be
	// scanning out.
	return m_Readback.Read(m_Slots[request.Target].Image, VK_IMAGE_LAYOUT_GENERAL, request.Into, request.Stride);
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

std::size_t VulkanRenderer::CollectCosts(std::span<GpuCost> into)
{
	if (m_Queries == VK_NULL_HANDLE || into.empty())
	{
		return 0;
	}

	// Oldest first, which the seam asks for and the storage does not give: the pending set is indexed
	// by target, and a target index says nothing about submission order. Insertion sort over at most
	// eight entries, for `OrderByDeadline`'s reasons — the set is tiny, fixed and nearly ordered, and
	// nothing inside the frame section may allocate a comparator's scratch.
	//
	// The order also decides what a short `into` leaves behind, and what it leaves is the newest —
	// which stay pending and arrive on the next iteration rather than being lost, because a pair is
	// only ever reset by a fresh record into the target that owns it.
	std::array<std::uint8_t, MaxRenderTargets> order{};
	std::size_t ready = 0;

	for (std::uint32_t target = 0; target < MaxRenderTargets; ++target)
	{
		const PendingCost& pending = m_Pending[target];

		// The timeline before the query pool. A submission the queue has not signalled cannot have
		// written its closing timestamp, so this answers the common case without entering the driver
		// at all — and it is the same poll `IsComplete` is, for the same reason.
		if (pending.Submit == 0 || !Reached(pending.Submit))
		{
			continue;
		}

		std::size_t position = ready;

		while (position > 0 && m_Pending[order[position - 1]].Submit > pending.Submit)
		{
			order[position] = order[position - 1];
			--position;
		}

		order[position] = static_cast<std::uint8_t>(target);
		++ready;
	}

	std::size_t written = 0;

	for (std::size_t index = 0; index < ready && written < into.size(); ++index)
	{
		const std::uint32_t target = order[index];
		const PendingCost& pending = m_Pending[target];
		std::array<std::uint64_t, MaxStamps> stamps{};

		// No `VK_QUERY_RESULT_WAIT_BIT`. The timeline above says the work is done, so this is expected
		// to be available every time; `VK_NOT_READY` is nonetheless a legal answer, and the response to
		// it is to leave the sample pending rather than to file a value the driver did not write.
		const VkResult result = vkGetQueryPoolResults(
			m_Device->Handle(),
			m_Queries,
			MaxStamps * target,
			pending.Stamps,
			pending.Stamps * sizeof(std::uint64_t),
			stamps.data(),
			sizeof(std::uint64_t),
			VK_QUERY_RESULT_64_BIT
		);

		if (result != VK_SUCCESS)
		{
			continue;
		}

		// The frame's own figure is the first stamp against the last, whatever the marks in between
		// did. Frame/Budget.h's input is unchanged by any of this, which is the property that lets the
		// instrument be switched on mid-session without moving the tier a panel is drawing at.
		const DeviceDescription& description = m_Device->Description();
		into[written] = GpuCost{ .Cost = description.TimestampSpan(stamps[0], stamps[pending.Stamps - 1]),
			                     .Generation = pending.Generation,
			                     .Mode = pending.Mode,
			                     .ClockMhz = pending.ClockMhz,
			                     .RequestedMhz = pending.RequestedMhz };
		++written;

		Report(pending, std::span{ stamps.data(), pending.Stamps }, target);

		m_Pending[target] = PendingCost{};
	}

	return written;
}

void VulkanRenderer::Report(const PendingCost& pending, std::span<const std::uint64_t> stamps, std::uint32_t target)
{
	if (!IsTracing() || pending.Stamps < 2)
	{
		return;
	}

	// **Refreshed here rather than held from construction, and the frame that is being placed is the
	// one that just finished.** The device counter and `CLOCK_MONOTONIC` are separate oscillators, so
	// an anchor taken at startup would be tens of milliseconds out by the time a person asks for a
	// trace an hour into a session — a GPU track drawn a whole refresh away from the frame that
	// submitted it, which is worse than no track. Taken now, the stamps being converted are a couple
	// of frames old and the drift across them is nanoseconds. It costs about ten microseconds on the
	// hardware this was written against, once per iteration, and only while a ring is armed.
	if (Result<GpuCalibration> anchor = m_Device->Calibrate(); anchor)
	{
		m_Calibration = *anchor;
	}

	if (!m_Calibration.IsValid())
	{
		// **A mark per frame rather than silence, because the GPU row opens as a missing track on a
		// device that cannot read its clock and the host's together.** The absence was the answer to
		// *where did the GPU timings go* — a question the row itself can now answer.
		//
		// **It is a mark beside the run rather than instead of it now**, which is the correction. The
		// return that used to stand here threw away every figure below, and only one of them needed a
		// calibration to be true: the *durations* are differences between two stamps of one counter and
		// are exact whatever the host clock is doing. What was missing was a place to put them, and a
		// device with no calibration still has `PendingCost::SubmittedAt`. So the row draws from the
		// submission and this mark is what says the placement is a bound rather than an observation.
		TraceMark("uncalibrated", pending.Trace);
	}

	const DeviceDescription& description = m_Device->Description();

	// Where a raw device stamp lands on the host's clock: the calibrated pairing where the device
	// offers one, and otherwise the submission plus however much of the run precedes this stamp. Both
	// preserve every interval exactly — the two differ only in where the run as a whole is put — which
	// is what lets every emission below be written once.
	const auto at = [&](std::uint64_t stamp) noexcept {
		return m_Calibration.IsValid() ? TimestampAt(description, m_Calibration, stamp) :
		                                 Advanced(pending.SubmittedAt, description.TimestampSpan(stamps[0], stamp));
	};

	const Instant opened = at(stamps[0]);
	const Instant closed = at(stamps[pending.Stamps - 1]);

	// **The batch is a slice and the passes are inside it, which is the whole of what this row was
	// missing.** A frame's device work is a run — composite, extract, blur, resumed composite, and
	// twice more for a second glass node — and it used to be emitted as that run and nothing else:
	// seven flat siblings, three of them named `composite`, with no boundary saying where one frame's
	// work stopped and the next began. A reader asking *what did the GPU spend on this frame* was
	// summing slices by eye.
	//
	// **Named for the frame rather than for the submission, and that is what replaced the arrows.**
	// The run used to carry a flow id on every one of its seven spans, so clicking any of them fanned
	// eight lines across four rows and a seven-second capture held three thousand of them — every one
	// saying only *these are the same frame*, which the name now says without drawing anything. The
	// number is the output's own frame sequence, so this slice and the one on the frame thread's row
	// are the same words and a search finds both.
	TraceSpanAt("frame", opened, closed, pending.Trace, TraceTag(pending.Frame));

	// **Each pass carries its position in the run**, because three siblings called `composite` inside
	// one parent is still three siblings called `composite`. `composite 0`, `extract 1`, `blur 2`,
	// `composite 3` reads as the chain it is, and the numbers are the stamp indices the driver actually
	// wrote — so a run that lost a pass shows as a gap in the counting rather than as nothing.
	for (std::uint32_t index = 0; index + 1 < pending.Stamps; ++index)
	{
		if (pending.Names[index] == nullptr)
		{
			continue;
		}

		TraceSpanAt(pending.Names[index], at(stamps[index]), at(stamps[index + 1]), pending.Trace, TraceTag(index));
	}

	// **What the frame actually cost on the device, beside what it was predicted to cost.** Frame/Loop.h
	// samples `gpu mark` where the admission test reads it; this is the measurement that mark is an
	// estimate of, and the two are only comparable because they are on the same axis in nanoseconds.
	// A trace that carried only the prediction was asking a reader to trust it.
	TraceElapsedAt("gpu cost", closed, description.TimestampSpan(stamps[0], stamps[pending.Stamps - 1]), pending.Trace);

	// The operating point beside the spans, so a reader sees the clock the composite was measured at
	// without leaving the trace — decision 142's number, and the one that turns the fragment rate below
	// from a slow part into a parked one. Emitted whether or not the device counts fragments, because it
	// answers a question the timestamps alone cannot: whether a long span was work or a low clock.
	//
	// **Both halves, because one of them is routinely zero.** A row that dropped to zero every frame
	// and said nothing else would read as a broken instrument; the commanded point beside it is what
	// makes the same picture legible as a part that parks between composites, which is the behaviour
	// decision 142 exists to act on and the one a reader most needs to see happening.
	TraceCountAt("clock (MHz)", closed, static_cast<std::int64_t>(pending.ClockMhz), pending.Trace);
	TraceCountAt("clock requested (MHz)", closed, static_cast<std::int64_t>(pending.RequestedMhz), pending.Trace);

	if (m_Statistics == VK_NULL_HANDLE)
	{
		return;
	}

	std::uint64_t fragments = 0;

	if (vkGetQueryPoolResults(
			m_Device->Handle(),
			m_Statistics,
			target,
			1,
			sizeof fragments,
			&fragments,
			sizeof fragments,
			VK_QUERY_RESULT_64_BIT
		) != VK_SUCCESS)
	{
		return;
	}

	// Stamped at the end of the batch rather than at the start, because a counter in Perfetto steps at
	// the sample and holds until the next one — and what this figure describes is a submission that
	// has finished, not one that is about to.
	TraceCountAt("fragments", closed, static_cast<std::int64_t>(fragments), pending.Trace);
}

void VulkanRenderer::Destroy() noexcept
{
	if (m_Device == nullptr || !m_Device->IsValid())
	{
		return;
	}

	m_Pipeline.Destroy();

	if (m_Queries != VK_NULL_HANDLE)
	{
		vkDestroyQueryPool(m_Device->Handle(), m_Queries, nullptr);
		m_Queries = VK_NULL_HANDLE;
	}

	if (m_Statistics != VK_NULL_HANDLE)
	{
		vkDestroyQueryPool(m_Device->Handle(), m_Statistics, nullptr);
		m_Statistics = VK_NULL_HANDLE;
	}

	m_Pending = {};

	// Before the descriptor it was imported from, so that the node this holds open is gone by the time
	// the timeline it names is.
	m_Deadline = FenceDeadline{};

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
