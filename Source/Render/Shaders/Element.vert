#version 450
#extension GL_GOOGLE_include_directive : require

// One element of decision 62's chain, placed on the same quad the fused program draws.
//
// **The geometry is Quad.glsl's and that is the whole reason this file is three lines long.** The
// oracle asserts that the fused chain and the separate passes produce one picture, and a difference
// in *where the corners landed* would be reported as a difference in the picture — so the two paths
// place their vertices by the same function rather than by two spellings of it. Dress.vert already
// makes this argument for the far side of a gather; this is the same argument for the near side.
//
// **The block differs from Quad.vert's by one `ivec4` and that is deliberate.** A separate pass
// carries its colour-state selectors as data where the fused program carries them as specialization
// constants, which is what collapses the unfused pipeline set from one per conversion to one per
// element — see Render/Unfused.h. The arithmetic downstream is the same arithmetic either way; only
// whether the driver folded the selector before or after `vkCreateGraphicsPipelines` differs.

#include "Quad.glsl"

layout(push_constant) uniform Item
{
	vec4 Corner[4];
	vec4 Fill;
	vec4 Shape;
	vec4 Target;

	// Source transfer, source primaries, target transfer, target primaries — Chain.glsl's
	// enumerators, as numbers rather than as constants.
	ivec4 Convert;
} item;

layout(location = 0) out vec2 Local;

void main()
{
	gl_Position = QuadPlace(gl_VertexIndex, item.Corner, item.Shape.xy, item.Target.xy, Local);
}
