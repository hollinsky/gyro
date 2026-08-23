#version 450
#extension GL_GOOGLE_include_directive : require

// The last pass of a gather: the blurred backdrop, tinted, encoded back into the output's light, and
// masked to the node's own rounded extent.
//
// **The tint is over the backdrop rather than mixed with it**, which is what makes `Smoke`'s
// obligation arithmetic. Decision 103 has `Smoke` sitting over content gyro did not choose — a film,
// a white page, a photograph — so its opacity is set from a worst-case contrast floor, and a floor on
// an `over` composite is a guarantee about the worst backdrop while a floor on a mix is a guarantee
// about the average one. Seam/Dressing.h applies the floor; what is here is the composite that makes
// it mean something.
//
// **The tint is not adapted to what is behind it**, which decision 103 rejects by name: a tint that
// tracks backdrop luminance breathes, and a panel over a playing film changing tone at every cut is
// *quality does not visibly fluctuate* failing in the one place a person is looking.
//
// **The mask is the node's own extent, whatever the node's kind.** Decision 99, and it is why the
// corner variant exists here as well as on the content pass — a dressed container has no content
// under it and still has corners.

#include "Chain.glsl"

layout(constant_id = 0) const uint Run = ChainRunCorner;

// The output's transfer function, which the chain's linear light is encoded back into. One per
// binding, so this is a pipeline rather than a set of them.
layout(constant_id = 1) const int TargetTransfer = ChainTransferSrgb;

layout(push_constant) uniform Item
{
	vec4 Corner[4];

	// The material's tint, premultiplied, in linear light and in the output's own primaries — which
	// is the space the chain holds, per Extract.frag.
	vec4 Tint;

	// The node's extent in `xy`, its corner radius, and its per-node opacity.
	vec4 Shape;

	// The target's extent in pixels in `xy`, which places the quad, and one over the chain image's
	// extent in texels in `zw`.
	vec4 Target;

	// The extracted region's origin in device pixels in `xy` and the chain's resolution divisor in
	// `z`. Together they map this fragment's device position onto the chain texel that holds its
	// backdrop.
	vec4 Chain;
} item;

layout(set = 0, binding = 0) uniform sampler2D Backdrop;

layout(location = 0) in vec2 Local;

layout(location = 0) out vec4 Colour;

void main()
{
	// Device position back into the chain. The extract wrote the region's origin at the chain's own
	// origin, so the mapping is a translate and a divide — and it is bilinear on the way back up,
	// which is the reason Seam/Dressing.h's resolution rung stops where it does.
	vec2 texel = (gl_FragCoord.xy - item.Chain.xy) / item.Chain.z;
	vec3 light = texture(Backdrop, texel * item.Target.zw).rgb;

	// `over`, premultiplied: the tint's own alpha is how much of the backdrop it hides.
	vec3 dressed = item.Tint.rgb + light * (1.0 - item.Tint.a);

	// Back into the output's encoding. No matrix, because the chain never left the output's
	// primaries — see Extract.frag, where the two that would have cancelled are not spent.
	vec4 colour = vec4(ChainEncode(dressed, TargetTransfer), 1.0);

	float scale = item.Shape.w;

	if ((Run & ChainRunCorner) != 0u)
	{
		scale *= ChainCorner(Local, item.Shape.xy, item.Shape.z);
	}

	// Premultiplied throughout, so coverage and opacity scale all four components together and the
	// blend is `over` with no second factor. A dressing at full opacity replaces what is under it,
	// which is right: what is under it is the backdrop this pass just read.
	Colour = colour * scale;
}
