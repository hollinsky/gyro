#version 450
#extension GL_GOOGLE_include_directive : require

// The quad's coverage and its colour: decision 103's pointwise chain, with the elements this variant
// was built without compiled out rather than multiplied by one.
//
// **The run mask is a specialization constant, so an element that is not in it does not exist.** The
// driver folds the constant at `vkCreateGraphicsPipelines` and eliminates the branch and its body,
// which is what decision 62 means by fusing a contiguous run and what decision 109 means by the
// runtime work being a pipeline creation rather than a compile. The elements themselves live in
// Chain.glsl, shared with the unfused path, for the reason stated at the top of that file.
//
// **What is not antialiased is a straight edge that does not lie on the grid** — a rotated or
// perspective-projected quad has hard edges, because coverage there is the rasterizer's and this
// shader does not add to it. Decision 67 makes settled geometry land on the device grid, so the
// still case is exact; what is left is the moving case, and it is a gap rather than a design.

#include "Chain.glsl"

// The defaults are the identity chain — corners masked, nothing converted — so a pipeline created
// with no specialization at all draws what the renderer drew before any of this existed. That is the
// direction a default should fail in: a variant nobody specialized is the picture, not a black quad.
layout(constant_id = 0) const uint Run = ChainRunCorner;
layout(constant_id = 1) const int SourceTransfer = ChainTransferSrgb;
layout(constant_id = 2) const int SourcePrimaries = ChainPrimariesBt709;
layout(constant_id = 3) const int TargetTransfer = ChainTransferSrgb;
layout(constant_id = 4) const int TargetPrimaries = ChainPrimariesBt709;

// The image a sampling variant reads. Declared unconditionally because a specialization constant
// cannot guard a declaration — what the constant folds away is the *use*, and a resource with no
// static use is one Vulkan does not require a set to be bound for. So the twenty variants that fill
// declare this and never touch it, and Render/Pipeline.cpp binds nothing for them.
layout(set = 0, binding = 0) uniform sampler2D Source;

layout(push_constant) uniform Item
{
	vec4 Corner[4];
	vec4 Fill;
	vec4 Shape;
	vec4 Target;
} item;

layout(location = 0) in vec2 Local;

layout(location = 0) out vec4 Colour;

void main()
{
	vec4 colour = item.Fill;

	if ((Run & ChainRunSample) != 0u)
	{
		// `Local` is surface-local in `[0, Extent]`, and `Fill` carries the source rectangle already
		// normalized against the image — so this is one multiply-add and the texture's own size never
		// had to cross the seam. Render/Pipeline.h states the packing.
		//
		// **The divide is guarded because a zero extent is expressible.** Seam/Renderer.h lets a
		// caller build an item whose extent is nothing; the quad it produces has no area and rasterizes
		// no fragments on any conforming device, but a NaN coordinate reaching a sampler is the kind of
		// thing one driver turns into a black rectangle and another into a hang.
		const vec2 extent = max(item.Shape.xy, vec2(1.0));

		colour = texture(Source, item.Fill.xy + (Local / extent) * item.Fill.zw);

		// Before everything, and Docs/Architecture.md#premultiplied-alpha-is-the-sharp-edge is the
		// reason it is here rather than after the conversion: every element below this line assumes
		// premultiplied components, including `ChainConvert`, whose first act is to divide the alpha
		// back out.
		if ((Run & ChainRunPremultiply) != 0u)
		{
			colour = vec4(colour.rgb * colour.a, colour.a);
		}
	}

	if ((Run & ChainRunConvert) != 0u)
	{
		colour = ChainConvert(
			colour, SourceTransfer, SourcePrimaries, TargetTransfer, TargetPrimaries, item.Target.z, item.Target.w
		);
	}

	// Per-node opacity, with decision 62's dim already folded into it by the renderer: both are one
	// multiply by a number the frame knows, so a variant that distinguished them would be two
	// pipelines for one instruction. Decision 103 counted them as two elements of the chain and they
	// are one — see Render/Pipeline.h, which carries the corrected count.
	float scale = item.Shape.w;

	// The corner mask is the one element whose absence is worth a variant on its own: it is two
	// hardware derivatives, a length and a divide, and an unrounded node — a wallpaper, a tiled
	// window, decision 106's X11 client — pays all of it to be multiplied by one.
	if ((Run & ChainRunCorner) != 0u)
	{
		scale *= ChainCorner(Local, item.Shape.xy, item.Shape.z);
	}

	// Premultiplied throughout, so coverage and opacity scale all four components together and the
	// blend is `over` with no second factor. Decision 95 fixes `over` as the only blend mode.
	Colour = colour * scale;
}
