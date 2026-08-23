#version 450
#extension GL_GOOGLE_include_directive : require

// One element of decision 62's pointwise chain, executed on its own, reading what the element before
// it wrote and writing what the element after it will read.
//
// **This is the unfused half of the oracle and it is deliberately the slow one.** Decision 62 says
// every effect exists as a standalone pass and that the fused form is never required for
// correctness; this file is that sentence in code. A chain of four elements is four rasterizations
// of the same quad and three round trips through an intermediate render target, against one program
// that keeps every intermediate in a register — and the difference between the two pictures is
// exactly the rounding those round trips cost, which is the thing the oracle measures.
//
// **The elements are Chain.glsl's, not copies of them.** That file states the reason at length: two
// hand-written implementations of an sRGB curve are written the same afternoon from the same
// paragraph of the same spec, so a misreading goes into both and the oracle reports agreement. What
// is being compared here is *composition and precision*, and the only way that comparison is
// readable is for the arithmetic on both sides to be the same arithmetic.
//
// **The source is read with `texelFetch`, which is what keeps a filter out of the reference.** Every
// pass in this chain is one to one — the intermediates are the target's own resolution and the quad
// is rasterized into them at the position it will finally occupy — so the pixel this fragment wants
// is the one under `gl_FragCoord`, exactly. A normalized coordinate through a sampler would put a
// half-texel convention and a filtering mode between the two paths, and a disagreement of half a
// pixel at an edge would then be indistinguishable from a fusion bug.

#include "Chain.glsl"

// Which element this pipeline is. One pipeline per element, and the count does not grow with the
// conversion lattice — Render/Unfused.h is where that trade is argued.
const int ElementEmit = 0;
const int ElementConvert = 1;
const int ElementCorner = 2;
const int ElementScale = 3;
const int ElementComposite = 4;

layout(constant_id = 0) const int Element = ElementEmit;

layout(set = 0, binding = 0) uniform sampler2D Source;

layout(push_constant) uniform Item
{
	vec4 Corner[4];
	vec4 Fill;
	vec4 Shape;
	vec4 Target;
	ivec4 Convert;
} item;

layout(location = 0) in vec2 Local;

layout(location = 0) out vec4 Colour;

void main()
{
	// The first element has nothing behind it: it is where the item's own fill enters the chain, and
	// the descriptor bound for it names an image nobody reads. Every other element reads the pixel it
	// is about to replace.
	if (Element == ElementEmit)
	{
		Colour = item.Fill;

		return;
	}

	const vec4 source = texelFetch(Source, ivec2(gl_FragCoord.xy), 0);

	if (Element == ElementConvert)
	{
		Colour = ChainConvert(
			source, item.Convert.x, item.Convert.y, item.Convert.z, item.Convert.w, item.Target.z, item.Target.w
		);

		return;
	}

	// Coverage, on its own, against premultiplied components — which is what makes it a multiply
	// rather than a blend. The derivatives are taken over the same varying the fused program takes
	// them over, on the same six vertices under the same viewport, so the arc is the same arc.
	if (Element == ElementCorner)
	{
		Colour = source * ChainCorner(Local, item.Shape.xy, item.Shape.z);

		return;
	}

	// Per-node opacity with decision 62's dim folded into it, as its own pass. Render/Pipeline.h
	// argues that these are one multiply and not two elements; what is one multiply in the fused
	// program is one pass here, and it is a pass rather than being folded into the composite because
	// folding two elements together is the thing this path exists not to do.
	if (Element == ElementScale)
	{
		Colour = source * item.Shape.w;

		return;
	}

	// `ElementComposite`: the chain's result, handed to the blender. Nothing is applied here — the
	// pipeline is the one built against the target's format with `over` enabled, and the `over` is
	// the same one the fused program's single draw performs.
	Colour = source;
}
