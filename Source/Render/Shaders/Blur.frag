#version 450

// One separable box pass over the chain, in linear light.
//
// **A box rather than a Gaussian, and the reason is decision 63 rather than cost.** A Gaussian has
// infinite support, so declaring how far a material's output exceeds its input means picking a
// number of standard deviations and accepting a fringe below it — under-declared damage, which that
// decision says leaves a trail at the edge of something that moved. A box is compactly supported:
// `passes` of them reach exactly `passes` half-widths and not one texel further, so Seam/Dressing.h's
// `Expansion` is an exact bound rather than a truncation somebody chose. Three of them approximate a
// Gaussian to within a few percent, which is the classical result and why the pass count starts
// there.
//
// **The half-width is fractional and comes in as data.** Seam/Dressing.h solves it from the
// material's sigma and the tier's structure, so that decision 34's ladder changes the pass count and
// the chain's resolution while the blur a person sees stays the same number. An integer half-width
// would round the sigma to whatever the chain's grid allowed, which is the radius arriving on the
// ladder through the back door.
//
// **Taps are paired onto bilinear samples.** Two adjacent unit weights are one sample at their
// midpoint with twice the weight, which halves the fetches, and the pairing is exact rather than an
// approximation because the sampler's own interpolation is the average being asked for. The last
// pair carries the fractional edge weight and its offset moves off the midpoint to match.

layout(push_constant) uniform Pass
{
	vec4 Source;
	vec4 Step;
	vec4 Kernel;
} pass;

layout(set = 0, binding = 0) uniform sampler2D Source;

layout(location = 0) out vec4 Colour;

// One offset's weight in the discrete kernel Seam/Dressing.h's `BoxVariance` describes: one out to
// `floor(w)`, the remainder one past it, nothing beyond.
float Weight(float offset, float whole, float fraction)
{
	return offset <= whole ? 1.0 : (offset <= whole + 1.0 ? fraction : 0.0);
}

void main()
{
	float width = pass.Kernel.x;
	float whole = floor(width);
	float fraction = width - whole;

	vec2 uv = (pass.Source.xy + gl_FragCoord.xy) * pass.Source.zw;
	vec2 stride = pass.Step.zw * pass.Source.zw;

	vec4 sum = texture(Source, uv);
	float total = 1.0;

	// Pairs, outward. `whole + 1` is the last offset that can carry weight, so the bound is the
	// number of pairs that reach it.
	int pairs = int(ceil((whole + 1.0) * 0.5));

	for (int index = 0; index < pairs; ++index)
	{
		float near = float(index * 2 + 1);
		float far = near + 1.0;

		float a = Weight(near, whole, fraction);
		float b = Weight(far, whole, fraction);
		float weight = a + b;

		if (weight <= 0.0)
		{
			break;
		}

		// The bilinear sample that *is* the weighted pair: at the weights' own centre of mass, the
		// sampler returns exactly `(a·near + b·far) / (a + b)`.
		float offset = (near * a + far * b) / weight;

		sum += weight * (texture(Source, uv + stride * offset) + texture(Source, uv - stride * offset));
		total += 2.0 * weight;
	}

	Colour = sum / total;
}
