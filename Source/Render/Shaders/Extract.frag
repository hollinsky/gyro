#version 450
#extension GL_GOOGLE_include_directive : require

// The first pass of a gather: the backdrop, decoded into linear light and downscaled to the chain's
// own resolution.
//
// **The decode and the downscale are one pass because the order between them is not optional.**
// Decision 34 admits chain internal resolution as its cheapest rung *because* a result about to be
// blurred tolerates a downscale, and decision 47 is the whole of what makes that true: averaging in
// an encoded space darkens the average, so a downscale performed before the decode would shift the
// picture's brightness at every tier step and make the invisible lever the conspicuous one. Every
// tap below is decoded before it is summed.
//
// **The primaries are not converted and that is deliberate.** Decision 47 fixes the composite space
// as linear at wide primaries, and the chain is neither — it holds the *output's* primaries, made
// linear. A blur is a weighted sum, a primaries change is a matrix, and a matrix commutes with a
// weighted sum: blurring in the output's own primaries and blurring in Rec.2020 produce the same
// picture, so the two matrices this would otherwise spend are two matrices that cancel. It also
// removes the reason Docs/Architecture.md gives for wanting Rec.2020 in the packed float — a value
// that came out of the target's own encoding is non-negative before any matrix touches it.
//
// **The taps are a box over the block being collapsed, and every one of them is a point sample.** A
// bilinear tap would average encoded texels in the sampler, which is the error the paragraph above
// exists to avoid — hardware filtering happens before a shader can decode anything. So the sampler
// this pass is bound with is `VK_FILTER_NEAREST` and the box is spelled out.

#include "Chain.glsl"

// The output's own transfer function, which is what the target is encoded in. One value per binding,
// so this is one pipeline rather than a set.
layout(constant_id = 0) const int SourceTransfer = ChainTransferSrgb;

// The chain's resolution divisor, which is also the width of the box collapsed per fragment.
// Decision 34's first rung, as a constant so the loop below unrolls.
layout(constant_id = 1) const int Divisor = 4;

layout(push_constant) uniform Pass
{
	// The source region's origin in source texels in `xy`, and one over the source image's extent in
	// texels in `zw`.
	vec4 Source;

	// Source texels per destination fragment in `xy`, and the tap direction in source texels in `zw`
	// — unused here, and read by Blur.frag out of the same block so that one pipeline layout serves
	// both passes.
	vec4 Step;

	// The half-width in source texels in `x`, and three spare. Also Blur.frag's.
	vec4 Kernel;
} pass;

layout(set = 0, binding = 0) uniform sampler2D Source;

layout(location = 0) out vec4 Colour;

void main()
{
	// Where in the source this destination fragment's block starts. `gl_FragCoord` is at a pixel
	// centre, so the half is taken back off before scaling and the block's first texel centre is
	// added on.
	vec2 origin = pass.Source.xy + (gl_FragCoord.xy - 0.5) * pass.Step.xy + 0.5;

	vec4 sum = vec4(0.0);

	for (int y = 0; y < Divisor; ++y)
	{
		for (int x = 0; x < Divisor; ++x)
		{
			vec4 texel = texture(Source, (origin + vec2(x, y)) * pass.Source.zw);

			// Decoded per tap, before anything is added to anything. The alpha is carried untouched:
			// a composite target is opaque where anything has been drawn, and a transfer function
			// applies to light rather than to coverage.
			sum += vec4(ChainDecode(texel.rgb, SourceTransfer), texel.a);
		}
	}

	Colour = sum / float(Divisor * Divisor);
}
