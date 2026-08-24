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
//
// **The arithmetic is exact rather than the distance field every compositor reaches for**, and
// decision 132 carries the measurement that forced it: reading a rounded-rect distance field through
// the normal integral is wrong by eleven to twenty eight-bit code points at a corner, because at a
// square corner the field reads zero where only a quarter of the neighbourhood is occluded. What is
// here instead is the separable rectangle, which is exact, minus the four corner deficits, which are
// quadrature — and it lands within a twentieth of a code point.

layout(push_constant) uniform Shadow
{
	vec4 Rect;
	vec4 Shape;
	vec4 Target;
	vec4 Basis;
} shadow;

layout(location = 0) out vec4 Colour;

// How many standard deviations of the penumbra are followed before it is treated as nothing. The
// same figure Seam/Dressing.h's expansion is cut at, for the same reason: past it the shadow is below
// what an eight-bit target can hold.
const float ShadowSpan = 4.0;

// Six-point Gauss-Legendre on `[-1, 1]`. Six because decision 132 measured it: the integrand is
// analytic in the angle, so the rule converges geometrically, and six nodes over each of two panels
// is where the error stops being the rule's and starts being the reference's.
const float ShadowNode[6] =
	float[6](-0.93246951, -0.66120939, -0.23861919, 0.23861919, 0.66120939, 0.93246951);
const float ShadowWeight[6] = float[6](0.17132449, 0.36076157, 0.46791393, 0.46791393, 0.36076157, 0.17132449);

// The error function, to about one part in ten thousand (Winitzki's approximation), which is a
// fiftieth of a code point at the umbra's alpha and two orders below the arithmetic it feeds.
float ShadowErf(float value)
{
	const float a = 0.147;

	float square = value * value;
	float inner = square * (4.0 / 3.14159265 + a * square) / (1.0 + a * square);

	return sign(value) * sqrt(1.0 - exp(-inner));
}

// The normal distribution's own integral: how much of a blurred half-plane's light survives at `x`.
float ShadowPhi(float x)
{
	return 0.5 + 0.5 * ShadowErf(x * 0.70710678);
}

// The signed distance to a rounded rect, negative inside. Used for the mask at the bottom of this
// file and nowhere in the coverage arithmetic, which is the whole of decision 132.
float ShadowField(vec2 point, vec2 extent, float radius)
{
	vec2 corner = abs(point) - extent + radius;

	return length(max(corner, 0.0)) + min(max(corner.x, corner.y), 0.0) - radius;
}

// One panel of the corner deficit's quadrature, in the angle the arc is parameterised by.
//
// **Substituted by angle rather than integrated over rows, and that is what makes six nodes enough.**
// A circle's half-width has a square-root singularity in its derivative where the arc meets the
// straight edge, and Gauss-Legendre converges algebraically against one of those. In the angle both
// coordinates are sines and cosines, the integrand is analytic, and the convergence is geometric.
float ShadowPanel(float low, float high, vec2 point, vec2 extent, float radius, float sigma, float edge)
{
	if (high <= low)
	{
		return 0.0;
	}

	float span = 0.5 * (high - low);
	float middle = 0.5 * (high + low);
	float total = 0.0;

	for (int node = 0; node < 6; ++node)
	{
		float angle = middle + span * ShadowNode[node];
		float row = extent.y - radius + radius * sin(angle);
		float arc = extent.x - radius + radius * cos(angle);

		// The exact light passing through this row of the deficit — between the arc and the edge the
		// rectangle would have had — weighted by how much of the blur's mass that row carries.
		float mass = exp(-(row - point.y) * (row - point.y) / (2.0 * sigma * sigma)) / (sigma * 2.50662827);

		total += ShadowWeight[node] * span * radius * cos(angle) * mass * (edge - ShadowPhi((arc - point.x) / sigma));
	}

	return total;
}

// What one rounded corner takes away from the rectangle: the curvilinear triangle inside the corner
// and outside the arc, blurred. The query point arrives mirrored into this corner's own quadrant, so
// one piece of arithmetic serves all four.
float ShadowDeficit(vec2 point, vec2 extent, float radius, float sigma)
{
	if (radius <= 0.0)
	{
		return 0.0;
	}

	// **Two early exits that are most of the cost of this shader.** A fragment out along an edge, or
	// past the far side of a corner, has no blur mass over this deficit at all — so the common case is
	// one corner evaluated and three abandoned before an `erf`, and a fragment in the middle of an
	// edge evaluates none, where the separable term below is exact on its own.
	if (point.x < extent.x - radius - ShadowSpan * sigma || point.x > extent.x + ShadowSpan * sigma)
	{
		return 0.0;
	}

	float base = extent.y - radius;
	float low = clamp((point.y - ShadowSpan * sigma - base) / radius, 0.0, 1.0);
	float high = clamp((point.y + ShadowSpan * sigma - base) / radius, 0.0, 1.0);

	if (high <= low)
	{
		return 0.0;
	}

	low = asin(low);
	high = asin(high);

	// Split at forty-five degrees, where the arc stops being mostly vertical and starts being mostly
	// horizontal. One panel across the whole quarter is enough where the softness is comparable to the
	// radius and visibly is not where it is much smaller — decision 132 measured twelve code points at
	// a ratio of twelve, and two panels take that to a thirtieth of one.
	float middle = clamp(0.78539816, low, high);
	float edge = ShadowPhi((extent.x - point.x) / sigma);

	return ShadowPanel(low, middle, point, extent, radius, sigma, edge) +
	       ShadowPanel(middle, high, point, extent, radius, sigma, edge);
}

// The blurred coverage of the whole rounded rect, in its own frame.
//
// **The rectangle is exact and the corners are the correction.** A Gaussian is separable and a
// rectangle is the product of two intervals, so the first term is a closed form with no error in it
// at all; everything approximate is confined to four corner terms that vanish over most of the quad.
float ShadowCoverage(vec2 point, vec2 extent, float radius, float sigma)
{
	float coverage = (ShadowPhi((extent.x - point.x) / sigma) - ShadowPhi((-extent.x - point.x) / sigma)) *
	                 (ShadowPhi((extent.y - point.y) / sigma) - ShadowPhi((-extent.y - point.y) / sigma));

	coverage -= ShadowDeficit(vec2(point.x, point.y), extent, radius, sigma);
	coverage -= ShadowDeficit(vec2(-point.x, point.y), extent, radius, sigma);
	coverage -= ShadowDeficit(vec2(point.x, -point.y), extent, radius, sigma);
	coverage -= ShadowDeficit(vec2(-point.x, -point.y), extent, radius, sigma);

	return clamp(coverage, 0.0, 1.0);
}

void main()
{
	vec2 extent = shadow.Rect.zw;
	float radius = shadow.Shape.x;
	float sigma = shadow.Shape.z;

	vec2 across = shadow.Basis.xy;
	vec2 down = shadow.Basis.zw;
	vec2 offset = gl_FragCoord.xy - shadow.Rect.xy;

	// **The shape turns and the light does not**, which is decision 133 in one subtraction. The
	// displacement is taken in device space, before the point is resolved into the node's own frame —
	// so a node lying at an angle keeps its shadow falling down the screen, where a displacement
	// applied inside the frame would swing it round with the node and light a tilted photograph from
	// the side.
	vec2 lit = offset - vec2(0.0, shadow.Shape.y);

	vec2 point = vec2(dot(offset, across), dot(offset, down));
	vec2 thrown = vec2(dot(lit, across), dot(lit, down));

	float coverage = ShadowCoverage(thrown, extent, radius, sigma);

	// **The node does not stand on its own shadow.** Decision 104: within one item the material samples
	// the target as of before the item began, and a shadow left under the node breaks that from the
	// other side — a translucent panel drawn over its own shadow darkens itself, worst where it reads
	// through most, which is a dark halo inside every glass panel that nobody would attribute to
	// elevation. A hard mask rather than the arithmetic above, because this is the node's own coverage
	// at one pixel and not a blur of it.
	float own = clamp(0.5 - ShadowField(point, extent, radius), 0.0, 1.0);

	Colour = vec4(0.0, 0.0, 0.0, shadow.Shape.w * coverage * (1.0 - own));
}
