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
