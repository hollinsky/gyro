#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Render/Device.h"
#include "Render/Vulkan.h"

// The quad pipeline, and the first entry in what decision 62 calls the variant lattice.
//
// **One program draws every item, and the variants are specialization constants rather than more
// files.** The driver folds a specialization constant at `vkCreateGraphicsPipelines` and eliminates
// the branch it guards, so an element that is not in a variant's run mask is not in its object code
// — which is what decision 62 means by fusing a contiguous run and what decision 109 means by the
// runtime work being a pipeline creation rather than a compile. Compiz had to assemble a program per
// combination at runtime because any plugin could contribute a snippet and the set was unknowable
// until the plugin list loaded; closing the vocabulary is what buys the same fusion out of a table.
//
// **The count is not decision 103's ten, and the correction is worth more than the number.**
// *(Counted against code 2026-08-22.)* That entry reads four ordered pointwise elements — the
// colour-state conversion, the corner mask, per-node opacity, and decision 62's dim — and applies
// decision 62's contiguous-runs formula to get ten. Three of the four premises do not survive
// contact:
//
// - **Opacity and the dim are one multiply, and it is data.** Both are a scalar on premultiplied
//   components, so they commute with each other and fold into one number the renderer computes per
//   item. There is nothing for a variant to switch on: eliding a multiply by a push constant saves
//   one instruction and costs a pipeline. Two of the four elements are therefore not axes at all.
// - **The conversion is not one element but three stages with selectors**, and its presence bit is
//   not separate from them: `TransferFunction::Linear` *is* the absent decode and matching
//   `ColorPrimaries` *is* the absent matrix, so the selector is the mask. What it costs is a product
//   over closed enumerations rather than a bit.
// - **Nothing splits this chain, so the contiguous-runs formula counts a splitting the vocabulary
//   cannot express.** Decision 62 counts runs because a gather forces materialisation *in the
//   middle* of a chain. Here there is at most one gather per item — `Dress` is one field — and
//   decision 99 places it over the node's own content, so it lands at exactly one position: after
//   the content's conversion and before the mask and the scalar. One fixed split point over a chain
//   admits two runs, not ten, and every additional run the formula counts is a chain shape no
//   `DrawItem` can encode.
//
// So the run mask is **two bits** — `QuadRunCorner` and `QuadRunConvert` — and the lattice's size is
// the conversion's selector rather than the mask. See `QuadVariantsPerBinding` for what that comes
// to. Decision 103's conclusion survives: the set is enumerable at build time, which is the property
// decision 109 rests on. Its arithmetic does not.
//
// **A pipeline per colour format, because dynamic rendering names the attachment's format at
// creation.** There is no render pass object to be compatible with, so the format is baked in.
// `Prepare` is called from `BindTargets`, which the seam already declares unbounded and allocating;
// `For` is a lookup and is what `Record` calls, because decision 62 is explicit that no frame blocks
// on compilation. That is also why `Prepare` is told the *output's* colour state rather than reading
// it per frame: the target end of every conversion is fixed for as long as a binding lasts, and it
// is the only end a frame cannot afford to discover.

// How many distinct *bindings* one renderer builds pipelines for, where a binding is a composite
// format and an output colour state together.
//
// **It counts pairs rather than formats, and the difference is new.** *(Widened 2026-08-22.)* A
// format alone used to name a pipeline; now the output's colour state is the target end of every
// conversion, so two states over one format are two sets. `VulkanFormat` maps every DRM code
// Seam/RenderTarget.h names onto `B8G8R8A8_UNORM` or `A2R10G10B10_UNORM_PACK32` and `NV12` is not a
// target, so the formats are two — and a renderer is per output, so the states it sees are the ones
// that output has been configured to since it came up. Nothing is dropped when a state stops being
// used: a pipeline is cheap to hold and freeing one that a submitted frame still names is not, so an
// output cycled through more than four distinct pairs meets a refusal at a bind rather than an
// overrun in a frame.
inline constexpr std::size_t MaxCompositeFormats = 4;

// The run mask, mirroring Chain.glsl's `ChainRun*`. Two bits, for the reason counted above.
inline constexpr std::uint32_t QuadRunCorner = 1U << 0;
inline constexpr std::uint32_t QuadRunConvert = 1U << 1;

// Source colour states one binding builds a converting variant for: every pair of primaries and
// transfer function except the ones naming `TransferFunction::Hlg`, which Render/Renderer.cpp
// refuses by name because converting it needs a display peak luminance `ColorState` does not carry.
inline constexpr std::size_t QuadSourceStates = 3 * 3;

// What one `Prepare` builds: the two corner variants that convert nothing, and a converting pair for
// every source state.
//
// Twenty, and about twenty-five milliseconds of `vkCreateGraphicsPipelines` for them on the floor
// tier — measured by Pipeline.Test.cpp, which prints the figure rather than asserting it, because a
// threshold here would be an assertion about somebody else's machine. A converting variant is the
// expensive kind at roughly 1.3ms against a plain one's 0.6ms, which is why the count and the cost
// are not proportional and why deriving the second from the first was wrong the first time it was
// tried. Enumerating the *target* end as well would be a hundred and sixty-four variants and over two
// hundred milliseconds per format, at every binding, which is the whole of why `Prepare` is told a
// colour state instead.
inline constexpr std::size_t QuadVariantsPerBinding = 2 + (QuadSourceStates * 2);

// Which program draws one item onto one output.
//
// **The selectors are normalised when nothing is converted**, so that two items whose colour states
// differ but which both need no conversion name one pipeline rather than two identical ones. A key
// that distinguishes programs that are the same program is a lattice that grows without drawing
// anything new.
struct QuadVariant
{
	std::uint32_t Run = QuadRunCorner;

	TransferFunction SourceTransfer = TransferFunction::Srgb;
	ColorPrimaries SourcePrimaries = ColorPrimaries::Bt709;
	TransferFunction TargetTransfer = TransferFunction::Srgb;
	ColorPrimaries TargetPrimaries = ColorPrimaries::Bt709;

	friend constexpr bool operator==(QuadVariant, QuadVariant) noexcept = default;

	// What an item wants: the corner mask where it is rounded, and a conversion where its light is
	// not the output's.
	//
	// **The alpha mode is deliberately not a reason to convert.** It is the one difference the
	// renderer absorbs without a fragment doing anything: a solid is four numbers, so a straight-alpha
	// fill becomes a premultiplied one with three multiplies on the CPU. Everything else — primaries,
	// transfer function, what the content calls its own 1.0 — is arithmetic per fragment.
	[[nodiscard]] static constexpr QuadVariant For(ColorState item, ColorState output, bool rounded) noexcept
	{
		QuadVariant variant{ .Run = rounded ? QuadRunCorner : 0U };

		// Reference luminance counts. Two states alike in both enumerators still describe different
		// light, and the conversion between them is a decode, a scale and an encode with no matrix —
		// which is a variant, and a picture that is visibly the wrong brightness if it is skipped.
		if (item.Primaries == output.Primaries && item.Transfer == output.Transfer &&
		    item.ReferenceLuminance == output.ReferenceLuminance)
		{
			return variant;
		}

		variant.Run |= QuadRunConvert;
		variant.SourceTransfer = item.Transfer;
		variant.SourcePrimaries = item.Primaries;
		variant.TargetTransfer = output.Transfer;
		variant.TargetPrimaries = output.Primaries;

		return variant;
	}
};

// The two numbers that carry a conversion's luminance, which is the only part of a colour state that
// is data rather than a selector — a reference white is a float and has no closed enumeration to be
// one of.
//
// **They are two rather than one ratio because PQ is absolute.** `ChainDecode` hands back cd/m² for
// `TransferFunction::Pq` and a fraction of the source's own reference white for everything else, so
// the chain meets in the middle at absolute light: the first factor takes the decoded value to
// cd/m², the second takes cd/m² to the target's units, and an absolute end passes 1.0 for the one it
// owns. A single ratio would be right for every pair except the ones with PQ on one side, which is
// the pair the field exists for.
struct QuadLuminance
{
	float Decode = 1.0F;
	float Encode = 1.0F;

	[[nodiscard]] static constexpr QuadLuminance For(ColorState item, ColorState output) noexcept
	{
		return { item.Transfer == TransferFunction::Pq ? 1.0F : item.ReferenceLuminance,
			     output.Transfer == TransferFunction::Pq ? 1.0F : 1.0F / output.ReferenceLuminance };
	}
};

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

	// The target's extent in pixels in the first two, which turns a device-space corner into a
	// clip-space one — it belongs here rather than in the viewport because the shader divides by it,
	// and reading it back out of the viewport is not something a shader can do — and `QuadLuminance`'s
	// two factors in the last two.
	//
	// **Four rather than two plus a separate pair**, because the block is declared identically by both
	// stages and a `vec2` followed by two floats is a layout question with two answers. The two the
	// vertex stage does not read cost it nothing.
	float Target[4]{ 1.0F, 1.0F, 1.0F, 1.0F };
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

	// Every variant this attachment format and this output's colour state can want, built if they are
	// not there yet.
	//
	// **The whole set rather than the ones seen so far, and that is what the colour state buys.** A
	// frame may not compile, so a variant an item needs and does not find is a refused frame; the only
	// way `For` can be total is for `Prepare` to have enumerated everything reachable. The source end
	// is enumerable on its own — decision 103's closed vocabulary — but the target end is not, so it
	// is told rather than guessed, and telling it is what turns a hundred milliseconds of pipeline
	// creation per binding into twelve.
	//
	// `EINVAL` for a format the renderer cannot name, for an output whose transfer function this
	// renderer will not convert to, and for more distinct bindings at once than `MaxCompositeFormats`
	// — a composition-root arrangement nobody has, reported rather than overrun. `ENOMEM` where the
	// driver could not build one.
	[[nodiscard]] Result<void> Prepare(VkFormat format, ColorState output);

	// The pipeline for this format and variant, or `VK_NULL_HANDLE` where `Prepare` built none.
	// Total and allocating nothing, because this is what runs inside a frame.
	[[nodiscard]] VkPipeline For(VkFormat format, QuadVariant variant) const noexcept;

	[[nodiscard]] VkPipelineLayout Layout() const noexcept { return m_Layout; }

	void Destroy() noexcept;

private:
	// One format, one variant, and the pipeline built for the pair. A flat array rather than a map:
	// the scan is over eighty entries of twelve bytes, which is a kilobyte the cache holds whole, and
	// it allocates nothing — which the map would, on a lookup path decision 36 forbids allocating on.
	struct Built
	{
		VkFormat Format = VK_FORMAT_UNDEFINED;
		QuadVariant Variant{};
		VkPipeline Pipeline = VK_NULL_HANDLE;
	};

	// One variant, built with the specialization the key names.
	[[nodiscard]] Result<void> Build(VkFormat format, QuadVariant variant);

	VulkanDevice* m_Device = nullptr;

	VkShaderModule m_Vertex = VK_NULL_HANDLE;
	VkShaderModule m_Fragment = VK_NULL_HANDLE;
	VkPipelineLayout m_Layout = VK_NULL_HANDLE;

	std::array<Built, MaxCompositeFormats * QuadVariantsPerBinding> m_Built{};
	std::size_t m_BuiltCount = 0;
};

// The layout the shaders were compiled against, held where a change to either side fails the build
// rather than the picture. glslang's own reflection reports the block at 112 bytes with `Corner` at
// 0 and a stride of 16, `Fill` at 64, `Shape` at 80, and `Target` at 96.
static_assert(std::is_trivially_copyable_v<QuadConstants> && std::is_standard_layout_v<QuadConstants>);
static_assert(sizeof(QuadConstants) == 112, "The block the shaders declare, byte for byte");
static_assert(offsetof(QuadConstants, Fill) == 64);
static_assert(offsetof(QuadConstants, Shape) == 80);
static_assert(offsetof(QuadConstants, Target) == 96);

// Well inside the 128 bytes every Vulkan implementation guarantees, which is what makes a push
// constant the right carrier rather than a gamble on a limit. Sixteen bytes of headroom is also the
// number that decides the primaries matrix: a `mat3` is forty-eight, so it could not be data here
// even if it should be, and Chain.glsl argues that it should not be either way.
static_assert(sizeof(QuadConstants) <= 128, "maxPushConstantsSize is 128 on the weakest conforming device");

// Chain.glsl mirrors these by value, and a specialization constant carries the value rather than the
// name — so a reordering in Core/ColorState.h would silently select a different curve. Held here
// because this file is what writes the specialization data.
static_assert(static_cast<int>(TransferFunction::Srgb) == 0, "Chain.glsl's ChainTransferSrgb");
static_assert(static_cast<int>(TransferFunction::Linear) == 1, "Chain.glsl's ChainTransferLinear");
static_assert(static_cast<int>(TransferFunction::Pq) == 2, "Chain.glsl's ChainTransferPq");
static_assert(static_cast<int>(TransferFunction::Hlg) == 3, "Chain.glsl's ChainTransferHlg");
static_assert(static_cast<int>(ColorPrimaries::Bt709) == 0, "Chain.glsl's ChainPrimariesBt709");
static_assert(static_cast<int>(ColorPrimaries::DciP3) == 1, "Chain.glsl's ChainPrimariesDciP3");
static_assert(static_cast<int>(ColorPrimaries::Bt2020) == 2, "Chain.glsl's ChainPrimariesBt2020");

// The two states that draw today, and what they must and must not cost. A solid whose light is the
// output's takes the same program it took before any of this existed; one that differs by reference
// luminance alone still converts, which is the case a comparison of the two enumerators would miss.
static_assert(QuadVariant::For(ColorState::Srgb(), ColorState::Srgb(), false).Run == 0U);
static_assert(QuadVariant::For(ColorState::Srgb(), ColorState::Srgb(), true).Run == QuadRunCorner);
static_assert(
	(QuadVariant::For(
		 ColorState{ ColorPrimaries::Bt709, TransferFunction::Srgb, AlphaMode::Premultiplied, 0, 100.0F },
		 ColorState::Srgb(),
		 false
	 )
         .Run &
     QuadRunConvert) != 0U,
	"Reference luminance is part of a colour state's identity"
);

// A straight-alpha fill is the one difference the renderer absorbs on the CPU, so it must not open a
// variant. Two items alike in light and differing only in alpha mode draw with one pipeline.
static_assert(
	QuadVariant::For(ColorState::Srgb(), ColorState::Srgb(), true) ==
	QuadVariant::For(
		ColorState{ ColorPrimaries::Bt709, TransferFunction::Srgb, AlphaMode::Straight, 0, 203.0F },
		ColorState::Srgb(),
		true
	)
);
