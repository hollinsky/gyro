#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Render/Device.h"
#include "Render/Pipeline.h"
#include "Render/Vulkan.h"

// Decision 62's other execution: every element of a pointwise chain as a pass of its own, through an
// intermediate render target, with nothing fused into anything.
//
// **It is the reference and not the fallback, and the difference is what this file is for.**
// Render/Pipeline.h is the lattice — one program per contiguous run, with the elements a variant was
// built without compiled out. Decision 62 is explicit that the lattice is an optimization and that
// the separate-pass form is what correctness is defined against, which is only a real claim if the
// separate-pass form exists and runs. Two implementations of one rendering, one of them readable and
// always available, is a test: draw both, assert they agree. Integration/RenderImport.Test.cpp is
// where that assertion lives.
//
// **The set is one pipeline per element and does not grow with the lattice**, which is the property
// that makes keeping this path around affordable. The quad's lattice is twenty per binding because
// the conversion's source end is a product of two closed enumerations and every combination has to
// exist before a frame needs it; here the same selectors travel in the push constant block as four
// integers, so one convert pipeline serves all nine source states. Five pipelines against twenty,
// and the five do not move when the vocabulary grows a pointwise element — a new element is a new
// pipeline here and a new dimension there.
//
// **What is given up by making the selectors data is nothing the oracle needs.** Decision 109 argues
// them as specialization constants because a primaries matrix as a uniform is nine multiplies per
// fragment on the identity every ordinary frame is made of, and because a `mat3` does not fit in what
// the push constant block has left. Neither reaches a path that runs once in a test: the arithmetic
// is Chain.glsl's either way and produces the same numbers, and what differs is only whether the
// driver folded the branch before or after `vkCreateGraphicsPipelines`.
//
// **A gather is not here, because a gather is already unfused.** Decision 62 segments a chain at
// every gathering effect — a neighbourhood read cannot consume a value that has not been written —
// so Render/Backdrop.h's blur chain is separate passes by construction and has no fused form to be
// compared against. What this file covers is exactly the run the lattice fuses: the fill, the
// colour-state conversion, the corner mask, and the scalar.

// SPEC: how many intermediates one output reserves. Two, and it is a ping-pong rather than a chain
// of one per element — each element reads the previous element's output and writes the next one's
// input, so two images alternate for a chain of any length. Sized at the target's own resolution,
// because the elements are rasterized at the position the item will finally occupy and a scaled
// intermediate would put a resample between the two paths.
inline constexpr std::uint32_t UnfusedImages = 2;

// The intermediate's storage format, which decision 118 is the argument for.
//
// **Sixteen-bit float, and the short version is that it has to be finer than the output without
// being finer than a machine would really use.** A separate-pass chain rounds at every boundary and
// a fused one does not, so the intermediate's depth is the whole of what the two paths can disagree
// by: at eight bits the disagreement is one output code point per boundary and the pass count
// becomes visible banding, and at thirty-two it is nothing at all — which would make the oracle pass
// against a reference no machine would ever run, proving nothing about the format a real unfused
// frame would use. Half float rounds at 2⁻¹¹ relative, about a quarter of an eight-bit code point at
// the top of its range and far less below it, so a handful of boundaries stays inside one code point
// with margin.
//
// **It carries alpha at the same depth as colour**, which rules out the packed float
// Render/Backdrop.h's chain uses: a blur holds what is behind an item and is opaque, and this holds
// the item itself, premultiplied, mid-fade. It is also unbounded above, so the HDR headroom decision
// 47 puts over 1.0 survives a round trip that a `UNORM` intermediate would clamp.
//
// **And it is the float format Vulkan requires of a colour attachment**, so the reference path exists
// on every device gyro can come up on — including the floor tier, which is the one that has to be
// able to run it.
inline constexpr VkFormat UnfusedFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Which element of the chain a pipeline runs. Mirrors Element.frag's constants, which is what the
// specialization data carries.
enum class Element : std::int32_t
{
	// The item's fill, entering the chain. Reads nothing.
	Emit = 0,

	// The colour-state conversion, with both ends read out of the push constant block.
	Convert = 1,

	// The corner mask, as coverage against premultiplied components.
	Corner = 2,

	// Per-node opacity with decision 62's dim already folded into it by the renderer.
	Scale = 3,

	// The chain's result, blended `over` into the composite target. Not an element of the chain —
	// it is the blend every item ends in, and it is a pass here because the chain's last write
	// landed somewhere the presenter cannot see.
	Composite = 4,
};

inline constexpr std::size_t Elements = 5;

// What one `Reserve` builds: one pipeline per element. The four that write an intermediate are built
// against `UnfusedFormat` and the composite against the target's, which is why this is a per-binding
// cost rather than a per-device one — dynamic rendering names an attachment's format at pipeline
// creation.
inline constexpr std::size_t UnfusedPipelinesPerBinding = Elements;

// What one element pass is told: everything the fused program is told, and the conversion's four
// selectors after it.
//
// **The first 112 bytes are `QuadConstants` verbatim and that is load-bearing.** The two paths draw
// the same quad from the same corners at the same extent with the same radius and the same opacity,
// and the renderer builds that block once and hands it to both — so a disagreement in the picture
// cannot be a disagreement about what was asked for. Composed rather than copied for the same
// reason: a second spelling of the layout is a second thing to keep in step with two shaders.
struct ElementConstants
{
	QuadConstants Item{};

	// Source transfer, source primaries, target transfer, target primaries — Core/ColorState.h's
	// enumerators as integers, which is what `Element.frag` hands to `ChainConvert`.
	std::int32_t Convert[4]{};
};

// The intermediates, their descriptor sets, and the five pipelines one binding needs.
//
// Neither copied nor moved for `Backdrop`'s reason one file over: every handle in here belongs to a
// device Docs/Architecture.md#device-migration replaces whole, so this is destroyed and rebuilt
// rather than reseated.
class Unfused
{
public:
	Unfused() = default;

	~Unfused() { Destroy(); }

	Unfused(const Unfused&) = delete;
	Unfused& operator=(const Unfused&) = delete;
	Unfused(Unfused&&) = delete;
	Unfused& operator=(Unfused&&) = delete;

	// The parts that depend on no target: two shader modules, the descriptor set layout, the sampler
	// and the pipeline layout. From the renderer's constructor, so a device that refuses the SPIR-V
	// says so at startup.
	[[nodiscard]] Result<void> Create(VulkanDevice& device);

	// The images, the descriptor pool and every pipeline. From `BindTargets`, and only where the
	// renderer was built to draw this way — the path is one a test asks for, and reserving two
	// full-resolution images on every output that never uses them is a cost a boot service should
	// not pay for a reference nobody is reading.
	//
	// `format` is the target's, because the composite pass writes into it.
	[[nodiscard]] Result<void> Reserve(PixelSize<DeviceSpace> resolution, VkFormat format);

	// Everything a binding owns. The modules, the layout and the sampler outlive it.
	void Release() noexcept;

	void Destroy() noexcept;

	// Whether a chain can be run at all. Unlike `Backdrop::IsReady` this is never a quality
	// question: a renderer told to draw unfused and unable to reserve the images has no second way
	// to draw, and `BindTargets` refuses rather than falling back to the lattice — because the one
	// failure this whole path must not have is an oracle that silently compares the fused execution
	// against itself and reports agreement.
	[[nodiscard]] bool IsReady() const noexcept { return m_Ready; }

	[[nodiscard]] VkPipeline For(Element element) const noexcept;

	[[nodiscard]] VkPipelineLayout Layout() const noexcept { return m_Layout; }

	[[nodiscard]] VkDescriptorSet Set(std::uint32_t index) const noexcept { return m_Images[index].Set; }

	[[nodiscard]] VkImage Image(std::uint32_t index) const noexcept { return m_Images[index].Image; }

	[[nodiscard]] VkImageView View(std::uint32_t index) const noexcept { return m_Images[index].View; }

private:
	struct Intermediate
	{
		VkImage Image = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		VkImageView View = VK_NULL_HANDLE;
		VkDescriptorSet Set = VK_NULL_HANDLE;
	};

	[[nodiscard]] Result<void> Allocate(Intermediate& image);

	[[nodiscard]] Result<VkPipeline> Build(Element element, VkFormat format);

	VulkanDevice* m_Device = nullptr;

	VkShaderModule m_Vertex = VK_NULL_HANDLE;
	VkShaderModule m_Fragment = VK_NULL_HANDLE;

	VkDescriptorSetLayout m_SetLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_Layout = VK_NULL_HANDLE;

	// Nearest, and `texelFetch` ignores it. It is here because a combined image sampler needs one,
	// and it is nearest rather than linear so that the day somebody replaces the fetch with a sample
	// the reference does not quietly acquire a filter.
	VkSampler m_Sampler = VK_NULL_HANDLE;

	VkDescriptorPool m_Pool = VK_NULL_HANDLE;

	std::array<Intermediate, UnfusedImages> m_Images{};
	std::array<VkPipeline, Elements> m_Pipelines{};

	PixelSize<DeviceSpace> m_Size{};

	bool m_Ready = false;
};

// The block both stages of Element.frag and Element.vert declare, byte for byte. `QuadConstants` is
// 112 and the selectors take it to exactly the 128 every conforming Vulkan implementation
// guarantees — which is the ceiling, so the next thing a separate pass needs displaces one of these
// rather than joining them.
static_assert(std::is_trivially_copyable_v<ElementConstants> && std::is_standard_layout_v<ElementConstants>);
static_assert(sizeof(ElementConstants) == 128, "maxPushConstantsSize is 128 on the weakest conforming device");
static_assert(offsetof(ElementConstants, Convert) == sizeof(QuadConstants));

// Element.frag mirrors these by value, and a specialization constant carries the value rather than
// the name — so a reordering here would select a different element with no diagnostic anywhere.
static_assert(static_cast<int>(Element::Emit) == 0, "Element.frag's ElementEmit");
static_assert(static_cast<int>(Element::Convert) == 1, "Element.frag's ElementConvert");
static_assert(static_cast<int>(Element::Corner) == 2, "Element.frag's ElementCorner");
static_assert(static_cast<int>(Element::Scale) == 3, "Element.frag's ElementScale");
static_assert(static_cast<int>(Element::Composite) == 4, "Element.frag's ElementComposite");
