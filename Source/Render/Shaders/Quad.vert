#version 450

// One quad, placed by the four corners its producer already projected.
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
	// Top-left, top-right, bottom-right, then top-left, bottom-right, bottom-left. Both triangles
	// wind the same way, and neither winding is asked about: decision 55 culls back faces at the
	// producer, so the pipeline culls nothing and a mirrored output is drawn rather than dropped.
	const int order[6] = int[6](0, 1, 2, 0, 2, 3);

	int index = order[gl_VertexIndex];
	vec2 local[4] = vec2[4](vec2(0.0, 0.0), vec2(item.Shape.x, 0.0), item.Shape.xy, vec2(0.0, item.Shape.y));
	vec4 placed = item.Corner[index];
	vec2 clip = placed.xy / item.Target.xy * 2.0 - 1.0;

	Local = local[index];
	gl_Position = vec4(clip * placed.z, 0.0, placed.z);
}
