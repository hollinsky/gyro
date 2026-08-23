#version 450

// One quad, placed by the four corners its producer already projected.
//
// **The placement itself is in Quad.glsl**, shared with the dressing pass that draws the same quad
// on the far side of a gather. What is here is this pass's block and its one varying.
//
// **Nothing is read from a buffer and nothing is bound.** Seam/Renderer.h hands a `Quad` whose
// corners are device-space positions, so the vertex work is a lookup and a divide rather than a
// transform, and six indices off `gl_VertexIndex` are two triangles. A vertex buffer here would be
// memory the frame thread has to fill and the driver has to fence, for four points that already
// exist in the draw item.
//
// **The fourth component of `gl_Position` is the corner's own weight, and that is what keeps a
// texture from swimming.** `Quad::Weights` is the accumulated perspective divisor the projection
// discarded; putting it back as `w` — with the position pre-multiplied by it so the divide lands
// where it started — makes every varying interpolate perspective-correctly instead of
// affine-per-triangle. It costs nothing on the orthographic case, where the weight is one.

#extension GL_GOOGLE_include_directive : require

#include "Quad.glsl"

layout(push_constant) uniform Item
{
	// Per corner: device-space x and y, the perspective weight, and one word spare. A `vec4` array
	// rather than a `vec2` array beside a `vec4` of weights because `std140` and `std430` disagree
	// about the stride of the second and agree about the first — and a push constant block whose
	// layout depends on which rule the compiler applied is a picture that is wrong on one driver.
	vec4 Corner[4];

	// The fill, premultiplied, in whatever the item's colour state said its components mean.
	vec4 Fill;

	// The node's own extent in x and y, the corner radius, and the per-node opacity.
	vec4 Shape;

	// The target's extent in pixels in `xy`, which turns a device-space position into a clip-space
	// one, and the fragment stage's two luminance scales in `zw`. One block declared identically by
	// both stages rather than two ranges to keep in step with two files.
	vec4 Target;
} item;

// Surface-local coordinates, which is what the corner radius is measured in. `Extent` maps the four
// corners onto `[0, Width] x [0, Height]` in the same winding Seam/Renderer.h states.
layout(location = 0) out vec2 Local;

void main()
{
	gl_Position = QuadPlace(gl_VertexIndex, item.Corner, item.Shape.xy, item.Target.xy, Local);
}
