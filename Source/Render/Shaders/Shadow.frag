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
}
shadow;

layout(location = 0) out vec4 Colour;

// How many standard deviations of the penumbra are followed before it is treated as nothing: past it
// the shadow is below what an eight-bit target can hold.
//
// **Deliberately one sigma wider than the quad, and it was described here as the same figure until
// 2026-08-30.** `Seam/Dressing.h`'s `Expansion` cuts the geometry at `Offset + 3σ`, so no fragment
// outside three ever reaches this shader at all. Carrying four costs nothing — it is a bound in
// arithmetic rather than an area — and it buys the margin that lets `ShadowPhi` clamp its own domain
// without the clamp ever landing on a fragment that is drawn.
const float ShadowSpan = 4.0;

// Six-point Gauss-Legendre on `[-1, 1]`. Six because decision 132 measured it: the integrand is
// analytic in the angle, so the rule converges geometrically, and six nodes over each of two panels
// is where the error stops being the rule's and starts being the reference's.
const float ShadowNode[6] = float[6](-0.93246951, -0.66120939, -0.23861919, 0.23861919, 0.66120939, 0.93246951);
const float ShadowWeight[6] = float[6](0.17132449, 0.36076157, 0.46791393, 0.46791393, 0.36076157, 0.17132449);

// The normal distribution's own integral: how much of a blurred half-plane's light survives at `x`.
// Four of these are the whole of the separable term below, so this is the hottest arithmetic in the
// shader by a wide margin, and what it is built out of is the point.
//
// **A polynomial rather than an error function, because the expensive instruction is the wrong one
// to spend here.** Decision 169. The obvious form is `erf`, which is what this was: Winitzki's
// approximation, an `exp`, a `sqrt` and a reciprocal. Those three run on the elementary function unit
// rather than on the main ALU, and that unit is deliberately narrow — a quarter of the ALU's width on
// the parts gyro most wants to be pleasant on, because an ordinary shader issues one transcendental
// for every twenty multiplies and this one issued one for every three. Measured on an Adreno 618 at
// 2160x1440, a shadow fragment cost 12.6 times a plain fill and two elevated panels cost 4.47 ms of a
// 16.67 ms refresh, which is a frame gyro does not have. The arithmetic below is not cheaper in any
// absolute sense; it is the same work moved onto the wide unit.
//
// **What makes a polynomial available at all is that the domain is bounded, and `ShadowSpan` bounds
// it.** A sigmoid over the whole line is hopeless to fit and this is not one: past four standard
// deviations the shadow is already below what an eight-bit target can hold, and the quad is cut a
// sigma tighter than that again, which is why the clamp costs nothing and never lands on a fragment
// that is drawn. So the fit is over `[-ShadowSpan, ShadowSpan]` and is *for* that number — moving
// `ShadowSpan` invalidates these coefficients rather than rescaling them.
//
// **Odd about zero, and one at the ends by construction.** Half the terms are gone because the true
// integral is symmetric about the edge, and the coefficients are constrained to sum to one so that
// the clamp meets the polynomial exactly — a shadow reaching 0.9997 at its own cutoff would leave a
// faint step around every panel where the expansion ends, which is worse than any error in the middle.
//
// A degree-thirteen minimax fit lands within 0.028 of an eight-bit code point, against 0.016 for the
// error function it replaces, and the two never differ by more than 0.041 — so nothing on screen
// changes, and what is given up is precision the target could not hold.
const float ShadowFit[7] =
	float[7](3.18810152, -8.36587794, 18.62752647, -28.66216116, 28.14849436, -15.63691075, 3.70082751);

float ShadowPhi(float x)
{
	float s = clamp(x / ShadowSpan, -1.0, 1.0);
	float q = s * s;

	float odd = ShadowFit[6];

	for (int term = 5; term >= 0; --term)
	{
		odd = odd * q + ShadowFit[term];
	}

	return 0.5 + 0.5 * s * odd;
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

	float own = clamp(0.5 - ShadowField(point, extent, radius), 0.0, 1.0);

	// **Hoisted above the integral so the fragments the node's own body hides can leave without
	// paying for it.** `own` reaches exactly 1 half a pixel inside the panel, the alpha below is
	// scaled by `1 - own`, and the blend is premultiplied against `ONE_MINUS_SRC_ALPHA` — so these
	// fragments already contribute nothing and the destination is unchanged whether they are written
	// or dropped. The output is identical bit for bit; what changes is that four `ShadowPhi` calls
	// stop running for four fifths of every shadow quad drawn. Measured on an Adreno 618 at
	// 2160x1440, the materials gym's composite goes from 8.96 ms to 7.44 ms.
	//
	// **The condition has to track the alpha expression at the bottom of this function**, and that is
	// the one hazard here: it is the same predicate written twice. A change that lets a shadow show
	// through its own node — decision 34's third rung draws the panel as a translucent fill, and the
	// day that is wanted rather than avoided — has to delete this with the `1 - own` it mirrors.
	//
	// Costing nothing where it buys nothing is why it is a discard rather than a narrower quad: the
	// saving scales with how large a node is against its own penumbra, so a window keeps almost all
	// of it and a small node keeps none, and neither pays more than the compare. A ring of eight
	// triangles would beat it — the interior is never rasterised at all, so the blend goes too — and
	// that is a change to the vertex shader and this one becomes dead.
	if (own >= 1.0)
	{
		discard;
	}

	float coverage = ShadowCoverage(thrown, extent, radius, sigma);

	// **The node does not stand on its own shadow.** Decision 104: within one item the material samples
	// the target as of before the item began, and a shadow left under the node breaks that from the
	// other side — a translucent panel drawn over its own shadow darkens itself, worst where it reads
	// through most, which is a dark halo inside every glass panel that nobody would attribute to
	// elevation. A hard mask rather than the arithmetic above, because this is the node's own coverage
	// at one pixel and not a blur of it.
	Colour = vec4(0.0, 0.0, 0.0, shadow.Shape.w * coverage * (1.0 - own));
}
