#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Core/Result.h"
#include "Render/Device.h"
#include "Render/Vulkan.h"

// The quad pipeline, and the first entry in what decision 62 calls the variant lattice.
//
// **One program draws every item, and the variants that are coming are specialization constants
// rather than more files.** Decision 103 counts the lattice: four ordered pointwise elements — the
// colour-state conversion, the corner mask, per-node opacity, and the dim — make ten contiguous
// runs, a gather splits a chain into at most two of them, and there is at most one gather per item
// because `Dress` is one field. Tens of variants, over a vocabulary decision 33 closed. That is
// what makes the set enumerable at build time and the runtime work `vkCreateGraphicsPipelines`
// against a module that already exists. Compiz had to assemble a program per combination at runtime
// because any plugin could contribute a snippet and the set was unknowable until the plugin list
// loaded; closing the vocabulary is what buys the same fusion out of a table.
//
// **A pipeline per colour format, because dynamic rendering names the attachment's format at
// creation.** There is no render pass object to be compatible with, so the format is baked in.
// `Prepare` is called from `BindTargets`, which the seam already declares unbounded and allocating;
// `For` is a lookup and is what `Record` calls, because decision 62 is explicit that no frame blocks
// on compilation.

// How many distinct composite formats one renderer builds pipelines for.
//
// Two is the real number: `VulkanFormat` maps every DRM code Seam/RenderTarget.h names onto
// `B8G8R8A8_UNORM` or `A2R10G10B10_UNORM_PACK32`, and `NV12` is not a target. Four is one doubling,
// so that a format added to that table without this file being read meets a refusal rather than an
// overrun.
inline constexpr std::size_t MaxCompositeFormats = 4;

// What one draw is told, and the whole of what crosses into the shaders.
//
// **A push constant block rather than a descriptor set or a vertex buffer**, and the reason is
// decision 36. A vertex buffer is memory the frame thread would have to fill and the driver would
// have to fence, for four points that already exist in the draw item; a uniform buffer is an
// allocation and a descriptor write per item on a path that may not allocate at all. The block is
// 104 bytes against a 128-byte floor every Vulkan implementation guarantees.
//
// **`Corner` is four `vec4`s and not four `vec2`s beside four floats**, because `std140` and
// `std430` disagree about the stride of an array of `vec2` and agree about an array of `vec4`. A
// block whose layout depends on which rule the front end applied is a picture that is right on the
// machine it was written on.
struct QuadConstants
{
	// Per corner: device-space x and y, the perspective weight that corner was divided by, and one
	// word spare. The weight goes back into `gl_Position.w` so that every varying interpolates
	// perspective-correctly — see Seam/Renderer.h's `Quad::Weights`, which is where the warp this
	// avoids is named.
	float Corner[4][4]{};

	// Premultiplied, in whatever the item's colour state says its components mean.
	float Fill[4]{ 0.0F, 0.0F, 0.0F, 0.0F };

	// Extent x, extent y, corner radius, per-node opacity. Four numbers that vary per item and are
	// read by the fragment stage; grouped rather than named separately because the block's layout is
	// the thing being kept unambiguous.
	float Shape[4]{ 0.0F, 0.0F, 0.0F, 1.0F };

	// The target's extent in pixels, which turns a device-space corner into a clip-space one. It
	// belongs here rather than in the viewport because the shader divides by it, and reading it back
	// out of the viewport is not something a shader can do.
	float Target[2]{ 1.0F, 1.0F };
};

// The shader modules, the layout, and one pipeline per format seen.
//
// Neither copied nor moved for `IRenderer`'s reason one layer up: every handle in here belongs to a
// device that Docs/Architecture.md#device-migration replaces whole, so this is destroyed and rebuilt
// rather than reseated.
class QuadPipeline
{
public:
	QuadPipeline() = default;

	~QuadPipeline() { Destroy(); }

	QuadPipeline(const QuadPipeline&) = delete;
	QuadPipeline& operator=(const QuadPipeline&) = delete;
	QuadPipeline(QuadPipeline&&) = delete;
	QuadPipeline& operator=(QuadPipeline&&) = delete;

	// The parts that do not depend on a target: two shader modules and the pipeline layout. Called
	// once, from the renderer's constructor, so that a device which refuses the SPIR-V says so at
	// startup rather than at the first frame with a window in it.
	[[nodiscard]] Result<void> Create(VulkanDevice& device);

	// A pipeline for this attachment format, built if it is not there yet. `EINVAL` for a format the
	// renderer cannot name and for more distinct formats bound at once than `MaxCompositeFormats` —
	// a composition-root arrangement nobody has, reported rather than overrun — and `ENOMEM` where
	// the driver could not build one.
	[[nodiscard]] Result<void> Prepare(VkFormat format);

	// The pipeline for this format, or `VK_NULL_HANDLE` where `Prepare` was never called for it.
	// Total and allocating nothing, because this is what runs inside a frame.
	[[nodiscard]] VkPipeline For(VkFormat format) const noexcept;

	[[nodiscard]] VkPipelineLayout Layout() const noexcept { return m_Layout; }

	void Destroy() noexcept;

private:
	// One format and the pipeline built for it. A flat array rather than a map: the count is two in
	// practice and four at most, and a linear scan over four values is cheaper than any hash and
	// allocates nothing.
	struct Variant
	{
		VkFormat Format = VK_FORMAT_UNDEFINED;
		VkPipeline Pipeline = VK_NULL_HANDLE;
	};

	VulkanDevice* m_Device = nullptr;

	VkShaderModule m_Vertex = VK_NULL_HANDLE;
	VkShaderModule m_Fragment = VK_NULL_HANDLE;
	VkPipelineLayout m_Layout = VK_NULL_HANDLE;

	std::array<Variant, MaxCompositeFormats> m_Variants{};
	std::size_t m_VariantCount = 0;
};

// The layout the shaders were compiled against, held where a change to either side fails the build
// rather than the picture. glslang's own reflection reports the block at 104 bytes with `Corner` at
// 0 and a stride of 16, `Fill` at 64, `Shape` at 80, and `Target` at 96.
static_assert(std::is_trivially_copyable_v<QuadConstants> && std::is_standard_layout_v<QuadConstants>);
static_assert(sizeof(QuadConstants) == 104, "The block the shaders declare, byte for byte");
static_assert(offsetof(QuadConstants, Fill) == 64);
static_assert(offsetof(QuadConstants, Shape) == 80);
static_assert(offsetof(QuadConstants, Target) == 96);

// Well inside the 128 bytes every Vulkan implementation guarantees, which is what makes a push
// constant the right carrier rather than a gamble on a limit.
static_assert(sizeof(QuadConstants) <= 128, "maxPushConstantsSize is 128 on the weakest conforming device");
