#version 450

// The quad's coverage and its colour, which for a solid is one distance and one multiply.
//
// **The corner radius is a distance field rather than geometry**, because the radius is animated:
// decision 96 rounds a window's frame rect and a spring moves it, so a tessellated corner would be a
// vertex count that changes during a transition. A field is the same six vertices at every radius,
// and it is exact at every radius rather than at the ones a fan happened to sample.
//
// **The field is the distance to the corner arcs and to nothing else**, which is what keeps a square
// corner square. The straight edges of the quad are the rasterizer's, and they are already exactly
// where the producer put them; a full rounded-box distance would re-answer them, and its gradient
// has a crease along the diagonal that a hardware derivative straddles — which comes out as four
// dimmed pixels on the corners of every unrounded rectangle on the screen.
//
// **The footprint is measured from the varying and never from the field**, for the same reason. The
// gradient of the field is known in closed form here, and `Local` is a projective varying whose
// derivatives are smooth everywhere, so the pixel's reach along the gradient is one dot product per
// axis. Differentiating the field instead would put the crease back and would also contaminate the
// pixel *beside* a corner, since a derivative is estimated across a two-by-two quad.
//
// **What is not antialiased is a straight edge that does not lie on the grid** — a rotated or
// perspective-projected quad has hard edges, because coverage there is the rasterizer's and this
// shader does not add to it. Decision 67 makes settled geometry land on the device grid, so the
// still case is exact; what is left is the moving case, and it is a gap rather than a design.

layout(push_constant) uniform Item
{
	vec4 Corner[4];
	vec4 Fill;
	vec4 Shape;
	vec2 Target;
} item;

layout(location = 0) in vec2 Local;

layout(location = 0) out vec4 Colour;

void main()
{
	vec2 centre = item.Shape.xy * 0.5;

	// Clamped to the extent, because a radius larger than the node is a capsule rather than an
	// error — and an unclamped one inverts the field and punches a hole in the middle of the node.
	float radius = min(item.Shape.z, min(centre.x, centre.y));

	// How far outside the inset rectangle this fragment sits, per axis and never negative. Zero on
	// both axes is everything that is not in a corner, which is most of a node.
	vec2 outside = max(abs(Local - centre) - (centre - radius), vec2(0.0));
	float reach = length(outside);
	float edge = reach - radius;

	// The field's own gradient, in surface-local units and pointing out of the nearest corner.
	vec2 direction = reach > 0.0 ? sign(Local - centre) * (outside / reach) : vec2(0.0);

	// That direction measured in pixels: how far one pixel step moves along it, which is the
	// footprint the coverage below is a fraction of. It is correct under a projection rather than
	// only under a translation, because it is derived from the varying the projection produced.
	float width = length(vec2(dot(direction, dFdx(Local)), dot(direction, dFdy(Local))));

	// Zero width is everything the arcs do not reach, where the answer is the sign alone.
	float coverage = width > 0.0 ? clamp(0.5 - edge / width, 0.0, 1.0) : step(edge, 0.0);

	// Premultiplied throughout, so coverage and opacity scale all four components together and the
	// blend is `over` with no second factor. Decision 95 fixes `over` as the only blend mode.
	Colour = item.Fill * (coverage * item.Shape.w);
}
