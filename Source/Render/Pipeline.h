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
// So the pointwise chain's run mask is **two bits** — `QuadRunCorner` and `QuadRunConvert` — and the
// lattice's size is the conversion's selector rather than the mask. Decision 103's conclusion
// survives: the set is enumerable at build time, which is the property decision 109 rests on. Its
// arithmetic does not.
//
// **Two more bits arrived with the first `DrawTexture`, and neither is a chain element**
// *(2026-08-23)*. `QuadRunSample` says where the item's colour comes from, which is upstream of the
// chain rather than in it; `QuadRunPremultiply` is the one place the count above was wrong rather
// than merely different, because it rests on an alpha mode being foldable on the CPU and that is
// only true of a fill. See `QuadVariantsPerBinding` for what the four bits come to together.
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

// The run mask, mirroring Chain.glsl's `ChainRun*`.
//
// The first two are the pointwise chain's, counted above. The second two arrived with the first
// `DrawTexture` and are the two ways sampling differs from filling *(2026-08-23)*.
inline constexpr std::uint32_t QuadRunCorner = 1U << 0;
inline constexpr std::uint32_t QuadRunConvert = 1U << 1;

// Where the item's colour comes from: the bound image rather than the push block.
//
// **A specialization constant rather than a white texture bound behind every solid.** The two are
// the same picture, and the difference is one texture fetch on the most common draw the compositor
// makes — a solid fill is the wallpaper, every panel's ground, and every window's shadowless
// rectangle, and a dependent read per fragment on all of them to save twenty pipelines at a mode set
// is the wrong end of that trade. What it costs is that a set has to be *bound* for the sampling
// variants and not for the others, which is one branch at the call site rather than a lattice
// dimension.
inline constexpr std::uint32_t QuadRunSample = 1U << 2;

// Whether the sampled texel needs its alpha applied before anything else touches it.
//
// **The one place Pipeline.h's own reasoning did not survive the second content kind**
// *(2026-08-23).* `QuadVariant::For` says below that the alpha mode is absorbed without a fragment
// doing anything, and its argument is *a solid is four numbers* — three multiplies on the CPU, once
// per item. A texture is not four numbers. A client that commits straight-alpha content has an alpha
// per texel and the fold has to happen per texel, after the fetch and before every element that
// assumes premultiplied components — which is all of them, since Chain.glsl converts on the
// assumption and `over` blends on it.
//
// Meaningless without `QuadRunSample`, and never set without it: a straight-alpha *solid* is still
// folded on the CPU, exactly as before.
inline constexpr std::uint32_t QuadRunPremultiply = 1U << 3;

// Source colour states one binding builds a converting variant for: every pair of primaries and
// transfer function except the ones naming `TransferFunction::Hlg`, which Render/Renderer.cpp
// refuses by name because converting it needs a display peak luminance `ColorState` does not carry.
inline constexpr std::size_t QuadSourceStates = 3 * 3;

// What one `Prepare` builds for items whose colour is in the push block: the two corner variants
// that convert nothing, and a converting pair for every source state.
//
// Twenty, and about twenty-five milliseconds of `vkCreateGraphicsPipelines` for them on the floor
// tier — measured by Pipeline.Test.cpp, which prints the figure rather than asserting it, because a
// threshold here would be an assertion about somebody else's machine. A converting variant is the
// expensive kind at roughly 1.3ms against a plain one's 0.6ms, which is why the count and the cost
// are not proportional and why deriving the second from the first was wrong the first time it was
// tried. Enumerating the *target* end as well would be a hundred and sixty-four variants and over two
// hundred milliseconds per format, at every binding, which is the whole of why `Prepare` is told a
// colour state instead.
inline constexpr std::size_t QuadFilledVariants = 2 + (QuadSourceStates * 2);

// And the whole set: the same twenty again for a sampled item, doubled by whether its alpha has to
// be folded per texel.
//
// **Sixty, and about seventy-five milliseconds a binding** *(2026-08-23)*. That is three times what
// it was, at every mode set and every output that comes up, and it is worth saying plainly rather
// than leaving in the arithmetic: a hotplug on a four-monitor machine spends a third of a second in
// the driver where it used to spend a tenth. `Prepare` is called from `BindTargets`, which the seam
// declares unbounded and which no frame is inside — so what this delays is the first frame on a
// newly configured output, not a frame already being served on another one.
//
// **The premultiply arm doubles only the sampled half**, because a straight-alpha solid is still
// folded on the CPU. Doubling the whole lattice would build twenty programs that differ in an
// element none of them contains.
inline constexpr std::size_t QuadVariantsPerBinding = QuadFilledVariants * 3;

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

	// What an item wants: where its colour comes from, the corner mask where it is rounded, and a
	// conversion where its light is not the output's.
	//
	// **The alpha mode is a reason to convert only where the item samples**, which is the correction
	// `QuadRunPremultiply` carries. For a fill it is still absorbed without a fragment doing anything
	// — a solid is four numbers, so a straight-alpha fill becomes a premultiplied one with three
	// multiplies on the CPU. For a texture there is an alpha per texel and nowhere to fold it but the
	// shader. Everything else — primaries, transfer function, what the content calls its own 1.0 — is
	// arithmetic per fragment either way.
	[[nodiscard]] static constexpr QuadVariant
	For(ColorState item, ColorState output, bool rounded, bool sampled = false) noexcept
	{
		QuadVariant variant{ .Run = rounded ? QuadRunCorner : 0U };

		if (sampled)
		{
			variant.Run |= QuadRunSample;

			if (item.Alpha == AlphaMode::Straight)
			{
				variant.Run |= QuadRunPremultiply;
			}
		}

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
// 112 bytes against a 128-byte floor every Vulkan implementation guarantees.
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

	// What the item's colour comes from, read two ways by two variants.
	//
	// Without `QuadRunSample` it is the fill: four components, premultiplied unless
	// `QuadRunPremultiply`, in whatever the item's colour state says they mean.
	//
	// With it, it is the source rectangle in *normalized* texture coordinates — origin in the first
	// two, extent in the last two — so that a fragment's coordinate is `Fill.xy + (Local / Extent) *
	// Fill.zw` and the image's own size never has to cross. Both the normalization and the *empty
	// means the whole image* rule from Seam/Renderer.h's `DrawTexture` are resolved on the CPU, where
	// they are two divides per item rather than per fragment.
	//
	// **One `vec4` read two ways rather than sixteen more bytes**, and the block is why: it is 112
	// against a 128-byte floor, and `ElementConstants` — which is this block plus four selectors —
	// is at exactly 128. Growing this would put the unfused path over what the weakest conforming
	// device guarantees. What makes the overload honest rather than a squeeze is that the two
	// readings are disjoint by construction: a `DrawSolid` has no source and a `DrawTexture` has no
	// colour, so there is no item for which both meanings exist.
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

// What one shadow draw is told. A second block rather than more fields on `QuadConstants`, because
// the two programs share no argument but the target's extent: a shadow has no fill, no colour state
// and no corners, and a quad has no light.
//
// **Forty-eight bytes and no variants behind it**, which is the whole of what decision 104's analytic
// shadow costs a renderer. Shadow.frag argues why there is no lattice here: premultiplied black is
// the same four components in every colour state, so there is nothing for a specialization constant
// to select.
struct ShadowConstants
{
	// The item's rect in device pixels: centre in the first two, half-extent in the last two.
	float Rect[4]{};

	// The corner radius in device pixels, the light's downward offset, the penumbra's standard
	// deviation, and the umbra's alpha — Seam/Dressing.h's `Shadow` with the radius put beside it,
	// since the closed form needs the shape and the light in one place.
	float Shape[4]{};

	// The target's extent in pixels in the first two, which the vertex stage divides by, and the
	// expansion in the third — how far past the rect the drawn quad reaches.
	float Target[4]{ 1.0F, 1.0F, 0.0F, 0.0F };

	// The node's own axes in device space, unit length: across in the first two, down in the last two.
	// The identity for a node square to the screen and a rotation for one spun in the plane, which is
	// decision 133's half of the refusal — a tilted node never reaches this block at all.
	float Basis[4]{ 1.0F, 0.0F, 0.0F, 1.0F };
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
	//
	// `textures` is Render/Textures.h's set layout, which every sampling variant is built against and
	// which the shadow program does not name — a shadow reads nothing. It is passed in rather than
	// created here because the sets allocated against it belong to the importer, which outlives every
	// binding and is shared by every output's renderer; a layout minted here would be a second one
	// that happens to match, and a set allocated against one layout and bound against another is
	// undefined behaviour that works until a driver checks.
	[[nodiscard]] Result<void> Create(VulkanDevice& device, VkDescriptorSetLayout textures);

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

	// The shadow program for this attachment format, or `VK_NULL_HANDLE` where `Prepare` built none.
	//
	// **Held by this class rather than by one of its own, and the two programs are why.** They differ
	// in their push block and in nothing else that a pipeline states: the same two triangles, the same
	// `over`, the same culled-nothing rasterizer, the same dynamic viewport and scissor. A second
	// class would be that state block copied, and the copy would drift the first time either side
	// gained a blend — which is a shadow that composites differently from the thing casting it.
	[[nodiscard]] VkPipeline Shadow(VkFormat format) const noexcept;

	[[nodiscard]] VkPipelineLayout ShadowLayout() const noexcept { return m_ShadowLayout; }

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

	// One format and the shadow pipeline built for it. No variant, because there is none.
	struct Shaded
	{
		VkFormat Format = VK_FORMAT_UNDEFINED;
		VkPipeline Pipeline = VK_NULL_HANDLE;
	};

	// One variant, built with the specialization the key names.
	[[nodiscard]] Result<void> Build(VkFormat format, QuadVariant variant);

	// The one shadow pipeline this format wants.
	[[nodiscard]] Result<void> BuildShadow(VkFormat format);

	VulkanDevice* m_Device = nullptr;

	VkShaderModule m_Vertex = VK_NULL_HANDLE;
	VkShaderModule m_Fragment = VK_NULL_HANDLE;
	VkPipelineLayout m_Layout = VK_NULL_HANDLE;

	VkShaderModule m_ShadowVertex = VK_NULL_HANDLE;
	VkShaderModule m_ShadowFragment = VK_NULL_HANDLE;
	VkPipelineLayout m_ShadowLayout = VK_NULL_HANDLE;

	std::array<Built, MaxCompositeFormats * QuadVariantsPerBinding> m_Built{};
	std::size_t m_BuiltCount = 0;

	std::array<Shaded, MaxCompositeFormats> m_Shaded{};
	std::size_t m_ShadedCount = 0;
};

// The layout the shaders were compiled against, held where a change to either side fails the build
// rather than the picture. glslang's own reflection reports the block at 112 bytes with `Corner` at
// 0 and a stride of 16, `Fill` at 64, `Shape` at 80, and `Target` at 96.
static_assert(std::is_trivially_copyable_v<QuadConstants> && std::is_standard_layout_v<QuadConstants>);
static_assert(sizeof(QuadConstants) == 112, "The block the shaders declare, byte for byte");
static_assert(offsetof(QuadConstants, Fill) == 64);
static_assert(offsetof(QuadConstants, Shape) == 80);
static_assert(offsetof(QuadConstants, Target) == 96);

// The same for the shadow block, which both of its stages declare whole. glslang reports it at 64
// bytes with `Rect` at 0, `Shape` at 16, `Target` at 32, and `Basis` at 48.
static_assert(std::is_trivially_copyable_v<ShadowConstants> && std::is_standard_layout_v<ShadowConstants>);
static_assert(sizeof(ShadowConstants) == 64, "The block Shadow.vert and Shadow.frag declare");
static_assert(offsetof(ShadowConstants, Shape) == 16);
static_assert(offsetof(ShadowConstants, Target) == 32);
static_assert(offsetof(ShadowConstants, Basis) == 48);

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
