#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Render/Device.h"
#include "Render/Vulkan.h"
#include "Seam/Dressing.h"

// The offscreen a gather reads through, and the three passes that fill it.
//
// **Glass owns this and decision 60's group flattening will not want it**, which is the scoping
// question this file exists to answer once rather than twice. The two look like one mechanism and
// read in opposite directions:
//
// - A **group** *writes* an offscreen. Its members composite into a cleared, full-resolution,
//   alpha-carrying target which is then read once at the group's opacity, and the whole point is
//   that the subtree is opaque to itself while that happens.
// - A **material** *reads the attachment it is already drawing into*. It needs no target of its own
//   for the backdrop at all. Decision 60's own rule is what collapses the two cases: glass inside a
//   flattening subtree samples the composite as of the group's base, which is exactly *whatever
//   attachment is bound right now, as it stands*. So a material never learns what a group is, and a
//   group never learns what a material is.
//
// What is genuinely shared is the *reservation*, and that is the piece that would otherwise be built
// twice: decision 46's discipline of storage reserved when an output is configured and never
// allocated at the moment a transition starts. `Reserve` is called from `BindTargets`, which the
// seam already declares unbounded and allocating; nothing here allocates inside a frame. When
// flattening lands it reserves full-resolution images from the same call, beside these.
//
// **The chain is three passes and the middle one runs several times.** Extract decodes the target's
// region into linear light and downscales it; blur runs separably, ping-ponging between two images;
// dress composites the result back over the item's quad. Seam/Dressing.h decides how many and how
// wide, and decision 34's ladder is the two numbers it hands over.
//
// **The pass split is what a gather costs structurally**, and it is the thing decision 116 said was
// not built yet. Vulkan cannot sample the colour attachment it is rendering into with a neighbourhood
// read — an input attachment reads one fragment's own coordinate and a blur reads its neighbours — so
// the item ends the render pass, transitions the target to shader-read, runs the chain, transitions
// back, and resumes with `LOAD_OP_LOAD`. That is one split per dressed item. Batching the
// non-overlapping ones into a single split is available and is not taken: decision 62 is explicit
// that the correct path is the reference and the optimization is selected on top of it.

// SPEC: how many chain images one output reserves. Two, and it is a ping-pong rather than a pool —
// a separable box alternates between them for the whole chain, whatever the pass count.
inline constexpr std::uint32_t ChainImages = 2;

// What one `Reserve` builds: an extract per divisor the ladder can name, one blur, and the two corner
// variants of the dressing. Five, against Render/Pipeline.h's twenty for the quad — and both are paid
// at every binding including the first, on a boot service whose whole point is being early.
// Backdrop.Test.cpp prints the figure rather than asserting it, for that file's reason.
//
// **The chain's own conversions cost nothing here, which is the difference from the quad.** That
// lattice enumerates nine source colour states because an item may arrive in any of them; a chain
// converts the *target* to linear and back, and the target is one state per binding. So the extract
// and the dressing are one pipeline each per binding rather than a set.
inline constexpr std::size_t ChainPipelinesPerBinding = 2 + 1 + 2;

// SPEC: the smallest divisor any tier uses, which is what the chain images are sized for. Reserved
// at the finest resolution the ladder can ask for so that a tier stepping *down* uses part of an
// image already there rather than reallocating — decision 34 wants that step to be quick, and an
// allocation on the way down is the one thing that would make it slow.
inline constexpr std::uint32_t FinestDivisor = 4;

// What one output's chain is: two images, their views, and the descriptor set each is sampled
// through. Sized at `Reserve` and used in part by every item smaller than the whole output.
class Backdrop
{
public:
	Backdrop() = default;

	~Backdrop() { Destroy(); }

	Backdrop(const Backdrop&) = delete;
	Backdrop& operator=(const Backdrop&) = delete;
	Backdrop(Backdrop&&) = delete;
	Backdrop& operator=(Backdrop&&) = delete;

	// The parts that depend on no target: three shader modules, the sampler pair, the descriptor set
	// layout and the two pipeline layouts. From the renderer's constructor, so a device that refuses
	// the SPIR-V says so at startup rather than at the first frame with a panel in it.
	[[nodiscard]] Result<void> Create(VulkanDevice& device);

	// The images, the descriptor pool, and every pipeline this binding can want. From `BindTargets`.
	//
	// `output` is the target's colour state, which fixes both ends of the chain's conversions — the
	// extract decodes out of it and the dress encodes back into it — so it is what decides which
	// pipelines exist, exactly as decision 116 has it for the quad.
	//
	// `resolution` is the output's, and the chain is that over `FinestDivisor`.
	//
	// `format` is the target's, because the dressing composites into the target and dynamic rendering
	// names an attachment's format at pipeline creation.
	[[nodiscard]] Result<void>
	Reserve(PixelSize<DeviceSpace> resolution, VkFormat format, ColorState output, std::uint32_t targets);

	// Everything a binding owns: the images, the pool, and the pipelines. The modules, layouts and
	// samplers outlive it, because a rebinding changes an output's size and colour state and never
	// changes what the chain *is*.
	void Release() noexcept;

	// Release, and then the parts `Create` built. From the destructor, since a chain is torn down and
	// rebuilt whole with the device it belongs to.
	void Destroy() noexcept;

	// Whether a chain can be run at all: the images exist, the pipelines were built, and the target's
	// format could be sampled. False falls a material to decision 34's third rung — the tint alone,
	// which is what `RenderMode::Floor` draws — rather than refusing the frame.
	[[nodiscard]] bool IsReady() const noexcept { return m_Ready; }

	// A descriptor set that samples one imported target, for the extract to read the backdrop
	// through. It belongs to the target slot rather than to this class, which owns only the pool it
	// came out of.
	//
	// **Written once at the binding and never updated again**, which is what lets the chain do no
	// per-frame descriptor traffic at all: the set of distinct images any pass reads is the target
	// ring plus the two chain images, and every one of them is known when the binding is. A set
	// updated between a bind and a submission would be a validation error rather than a slow path,
	// so having none to update is the whole design.
	[[nodiscard]] Result<VkDescriptorSet> AllocateTargetSet(VkImageView view);

	[[nodiscard]] VkDescriptorSet ChainSet(std::uint32_t index) const noexcept { return m_Chain[index].Set; }

	[[nodiscard]] VkImage ChainImage(std::uint32_t index) const noexcept { return m_Chain[index].Image; }

	[[nodiscard]] VkImageView ChainView(std::uint32_t index) const noexcept { return m_Chain[index].View; }

	[[nodiscard]] PixelSize<DeviceSpace> ChainSize() const noexcept { return m_Size; }

	[[nodiscard]] VkPipelineLayout PassLayout() const noexcept { return m_PassLayout; }

	[[nodiscard]] VkPipelineLayout DressLayout() const noexcept { return m_DressLayout; }

	// The extract pipeline for one divisor, or null where the ladder cannot ask for that divisor.
	[[nodiscard]] VkPipeline Extract(std::uint32_t divisor) const noexcept;

	[[nodiscard]] VkPipeline Blur() const noexcept { return m_Blur; }

	// The dressing pipeline, with the corner mask elided where the node is square — the one axis
	// decision 116 found survives as a real variant.
	[[nodiscard]] VkPipeline Dress(bool rounded) const noexcept { return rounded ? m_Dress[1] : m_Dress[0]; }

private:
	struct Image
	{
		VkImage Image = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		VkImageView View = VK_NULL_HANDLE;
		VkDescriptorSet Set = VK_NULL_HANDLE;
	};

	[[nodiscard]] Result<void> Allocate(Image& image);

	[[nodiscard]] Result<VkPipeline> Build(
		VkShaderModule vertex,
		VkShaderModule fragment,
		const void* specialization,
		std::size_t size,
		std::uint32_t constants,
		VkPipelineLayout layout,
		VkFormat format,
		const VkPipelineColorBlendAttachmentState& blend
	);

	[[nodiscard]] Result<VkPipeline> BuildPass(VkShaderModule fragment, const void* specialization, std::size_t size);

	[[nodiscard]] Result<VkPipeline> BuildDress(bool rounded, VkFormat format, ColorState output);

	VulkanDevice* m_Device = nullptr;

	VkShaderModule m_Vertex = VK_NULL_HANDLE;
	VkShaderModule m_ExtractFragment = VK_NULL_HANDLE;
	VkShaderModule m_BlurFragment = VK_NULL_HANDLE;
	VkShaderModule m_DressVertex = VK_NULL_HANDLE;
	VkShaderModule m_DressFragment = VK_NULL_HANDLE;

	VkDescriptorSetLayout m_SetLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_PassLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_DressLayout = VK_NULL_HANDLE;

	// Nearest for the extract, linear for everything downstream of it. Two samplers rather than one
	// because the extract decodes every tap before summing it, and a filtered tap would have averaged
	// encoded texels in hardware before this shader could decode anything — Extract.frag's whole
	// first paragraph.
	VkSampler m_Nearest = VK_NULL_HANDLE;
	VkSampler m_Linear = VK_NULL_HANDLE;

	VkDescriptorPool m_Pool = VK_NULL_HANDLE;

	std::array<Image, ChainImages> m_Chain{};

	// One per divisor the ladder can name, indexed so that entry `i` is `FinestDivisor << i`.
	std::array<VkPipeline, 2> m_Extract{};
	VkPipeline m_Blur = VK_NULL_HANDLE;
	std::array<VkPipeline, 2> m_Dress{};

	PixelSize<DeviceSpace> m_Size{};
	VkFormat m_Format = VK_FORMAT_UNDEFINED;

	bool m_Ready = false;
};

// What the extract and the blur are told. Both read one block so that one pipeline layout serves the
// pair, which is what lets the chain bind a layout once and change only the pipeline between passes.
struct PassConstants
{
	// The source region's origin in source texels in the first two, and one over the source image's
	// extent in texels in the last two.
	float Source[4]{ 0.0F, 0.0F, 1.0F, 1.0F };

	// Source texels per destination fragment in the first two — the divisor, for the extract — and
	// the tap direction in source texels in the last two.
	float Step[4]{ 1.0F, 1.0F, 0.0F, 0.0F };

	// The half-width in source texels, and three spare.
	float Kernel[4]{ 0.0F, 0.0F, 0.0F, 0.0F };
};

// What the dressing pass is told: the quad, the tint, the shape, and where in the chain this item's
// backdrop landed.
struct DressConstants
{
	float Corner[4][4]{};

	// Premultiplied, in linear light and in the output's primaries, with `Smoke`'s contrast floor
	// already applied by Seam/Dressing.h.
	float Tint[4]{};

	// Extent x, extent y, corner radius, per-node opacity — the same four the content pass reads,
	// deliberately, so that a dressing and the content under it agree about the shape they share.
	float Shape[4]{ 0.0F, 0.0F, 0.0F, 1.0F };

	// The target's extent in pixels in the first two and one over the chain's extent in texels in the
	// last two.
	float Target[4]{ 1.0F, 1.0F, 1.0F, 1.0F };

	// The extracted region's origin in device pixels, the chain's divisor, and one spare.
	float Chain[4]{ 0.0F, 0.0F, 1.0F, 0.0F };
};

static_assert(sizeof(PassConstants) == 48, "The block Extract.frag and Blur.frag declare, byte for byte");

// Exactly the 128 bytes every conforming Vulkan implementation guarantees, and there is nothing
// spare. The next field this pass needs displaces one rather than joining them, which is a real
// constraint on what a dressing may carry and is stated here rather than discovered by a driver.
static_assert(sizeof(DressConstants) == 128, "The block Dress.vert and Dress.frag declare, byte for byte");
static_assert(offsetof(DressConstants, Tint) == 64);
static_assert(offsetof(DressConstants, Shape) == 80);
static_assert(offsetof(DressConstants, Target) == 96);
static_assert(offsetof(DressConstants, Chain) == 112);
