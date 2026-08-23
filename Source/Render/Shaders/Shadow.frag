#version 450

// Decision 104's analytic shadow: a rounded rect displaced by the light and blurred by its size,
// evaluated in closed form rather than gathered from a silhouette.
//
// **No pass, no offscreen, no cache, and no input read at all** — which is why a level stays off
// decision 34's quality ladder. A floored frame keeps every shadow on screen and gives up the blur
// behind a panel, which is the degradation Docs/Experience.md promises; a silhouette blur would have
// spent the depth of the whole picture at the first sign of pressure.
//
// **No colour conversion either, and it is the one fill that needs none.** A shadow is the backdrop
// darkened, which is premultiplied black — and premultiplied black is the same four components in
// every transfer function and every set of primaries, so there is no variant lattice here and no
// specialization constant. What the blend then does is multiply what is behind by one minus this
// alpha, in whatever space the target is, which is the same `over` decision 95 fixes for everything
// else.

layout(push_constant) uniform Shadow
{
	vec4 Rect;
	vec4 Shape;
	vec4 Target;
} shadow;

layout(location = 0) out vec4 Colour;

// The signed distance to a rounded rect, negative inside. The shared half of both fields below.
float ShadowField(vec2 point, vec2 extent, float radius)
{
	vec2 corner = abs(point) - extent + radius;

	return length(max(corner, 0.0)) + min(max(corner.x, corner.y), 0.0) - radius;
}

// The error function, to about one part in ten thousand (Winitzki's approximation).
//
// **Approximated rather than tabulated, and the error is stated in the units that matter**: at the
// umbra's alpha, a thousandth of coverage is a fiftieth of an eight-bit code point, which is two
// orders below what the target can hold. A texture lookup would be a descriptor set on a pass that
// otherwise binds nothing.
float ShadowErf(float value)
{
	const float a = 0.147;

	float square = value * value;
	float inner = square * (4.0 / 3.14159265 + a * square) / (1.0 + a * square);

	return sign(value) * sqrt(1.0 - exp(-inner));
}

void main()
{
	vec2 extent = shadow.Rect.zw;
	float radius = shadow.Shape.x;

	// The occluder, displaced. One light for the whole system means the direction is the screen's and
	// not the node's, so it is a constant offset down rather than anything derived from where the
	// node sits — two windows at one level cast the same shadow wherever they are.
	vec2 lit = gl_FragCoord.xy - shadow.Rect.xy - vec2(0.0, shadow.Shape.y);

	// A blurred edge is the normal distribution's own integral, so coverage is the distance field read
	// through it. **Exact for a straight edge and wrong by a factor of two at a square corner**, where
	// it reads the distance as though the occluder continued around: the field is zero where a quarter
	// of the neighbourhood is occluded, so this says half coverage against a true quarter. Decision 130
	// carries the measurement — eleven to twenty eight-bit code points, at any softness — and names the
	// exact separable rectangle as the correction that is four `erf` calls and no loop.
	float thrown = 0.5 - 0.5 * ShadowErf(ShadowField(lit, extent, radius) / (shadow.Shape.z * 1.41421356));

	// **The node does not stand on its own shadow.** Decision 104: within one item the material samples
	// the target as of before the item began, and a shadow left under the node is that rule broken by
	// the other route — a translucent panel drawn over its own shadow darkens itself, worst where it
	// reads through most, which is a dark halo inside every glass panel that nobody would attribute to
	// elevation. Punched out here rather than avoided by drawing a ring, because the ring is eight
	// triangles and a pixel this masks costs one multiply.
	float own = clamp(0.5 - ShadowField(gl_FragCoord.xy - shadow.Rect.xy, extent, radius), 0.0, 1.0);

	Colour = vec4(0.0, 0.0, 0.0, shadow.Shape.w * thrown * (1.0 - own));
}
