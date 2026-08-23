#ifndef GYRO_RENDER_SHADERS_CHAIN
#define GYRO_RENDER_SHADERS_CHAIN

// Decision 103's pointwise chain, one function per element, and the only place any of them is
// written.
//
// **This file exists so that decision 62's oracle compares two compositions rather than two
// spellings.** That decision draws the fused chain and the unfused one and asserts they agree, and
// names the hard part: a fused chain keeps intermediates in registers while separate passes round
// through a render target and round at every boundary. That comparison is only readable if the
// arithmetic on both sides is the *same arithmetic* — otherwise a disagreement is ambiguous between
// a fusion bug and two sRGB curves that differ in the last bit, and the ambiguous one is the reading
// a tired person takes. Two hand-written copies would also fail the test they look like they pass:
// both get written the same afternoon from the same paragraph of the same spec, so a misread goes
// into both and the oracle reports agreement.
//
// What is given up is real and worth naming: an element whose arithmetic is wrong is wrong
// identically on both sides, so the oracle cannot see it. That was never what it was for. Element
// correctness is a value test against a curve — an sRGB midpoint is a number in a standard — and it
// is checkable without a second implementation, while composition and precision are not.
//
// **Every selector below is a specialization constant rather than a uniform**, which is decision
// 109's closed vocabulary applied one level down: `ColorPrimaries` and `TransferFunction` are three
// and four enumerators, so a conversion is one of a build-time-known set of compile-time constants.
// The alternative is carrying the primaries matrix as data, and it fails twice over — a `mat3` is
// forty-eight bytes against the twenty-four the push constant block has left, and even where it fit
// it would be nine multiplies per fragment on the identity every ordinary frame is made of. As a
// constant the identity costs nothing because it is not there.

// Mirrors of Core/ColorState.h's enumerators. Render/Pipeline.h holds the two sides together with a
// `static_assert` per enumerator, so a reordering there fails the build rather than the picture.
const int ChainTransferSrgb = 0;
const int ChainTransferLinear = 1;
const int ChainTransferPq = 2;
const int ChainTransferHlg = 3;

const int ChainPrimariesBt709 = 0;
const int ChainPrimariesDciP3 = 1;
const int ChainPrimariesBt2020 = 2;

// The run mask: which elements of the chain this variant was built with. A bit rather than a
// uniform for the reason above — the driver folds a specialization constant at
// `vkCreateGraphicsPipelines` and the branch and its body are gone, which is the whole of what
// decision 62 means by fusing a run.
const uint ChainRunCorner = 1u;
const uint ChainRunConvert = 2u;

// SMPTE ST 2084, and the constants are the ratios the standard states rather than the decimals a
// blog post rounded them to.
const float PqM1 = 2610.0 / 16384.0;
const float PqM2 = 2523.0 / 32.0;
const float PqC1 = 3424.0 / 4096.0;
const float PqC2 = 2413.0 / 128.0;
const float PqC3 = 2392.0 / 128.0;

// The piecewise sRGB curve, not a pure 2.2 gamma — Core/ColorState.h says so and the difference is
// visible in the shadows, which is where a compositor's own solids live.
vec3 ChainSrgbToLinear(vec3 encoded)
{
	// Floored for `ChainEncode`'s reason read backwards: an encoded signal is non-negative by
	// definition, and a `pow` of a negative is the one way this returns a NaN rather than a colour.
	const vec3 signal = max(encoded, vec3(0.0));
	const bvec3 low = lessThanEqual(signal, vec3(0.04045));

	return mix(pow((signal + 0.055) / 1.055, vec3(2.4)), signal / 12.92, low);
}

vec3 ChainLinearToSrgb(vec3 light)
{
	const bvec3 low = lessThanEqual(light, vec3(0.0031308));

	return mix(1.055 * pow(light, vec3(1.0 / 2.4)) - 0.055, light * 12.92, low);
}

// Absolute, in cd/m². PQ is the one transfer function whose numbers mean nits rather than a fraction
// of somebody's reference white, which is why the scales around the conversion below are two floats
// and not one ratio.
vec3 ChainPqToLinear(vec3 encoded)
{
	const vec3 signal = pow(max(encoded, vec3(0.0)), vec3(1.0 / PqM2));
	const vec3 numerator = max(signal - PqC1, vec3(0.0));
	const vec3 denominator = PqC2 - PqC3 * signal;

	return 10000.0 * pow(numerator / denominator, vec3(1.0 / PqM1));
}

vec3 ChainLinearToPq(vec3 nits)
{
	const vec3 light = pow(max(nits, vec3(0.0)) / 10000.0, vec3(PqM1));

	return pow((PqC1 + PqC2 * light) / (1.0 + PqC3 * light), vec3(PqM2));
}

// To linear light in the source's own units: relative to its reference white for the relative
// transfers, absolute for PQ. The caller multiplies by the scale that makes both absolute.
//
// `ChainTransferHlg` is absent rather than approximated. Its scene-to-display step needs the
// display's peak luminance and Core/ColorState.h carries a reference white instead, so every HLG
// implementation that does not have one has silently picked a peak. Render/Renderer.cpp refuses the
// enumerator by name; a wrong picture nobody can attribute is the failure that refusal exists for.
vec3 ChainDecode(vec3 encoded, int transfer)
{
	if (transfer == ChainTransferSrgb)
	{
		return ChainSrgbToLinear(encoded);
	}

	if (transfer == ChainTransferPq)
	{
		return ChainPqToLinear(encoded);
	}

	return encoded;
}

// **Clipped at zero on the way in, and it is a gamut clip rather than a rounding guard.** A
// conversion into narrower primaries produces negative components for anything outside the target's
// triangle — Bt2020's own red is outside Bt709's by a long way — and there is no encoding for
// negative light. Left alone it is `pow` of a negative, which GLSL leaves undefined and drivers
// answer with a NaN that blends into the whole quad.
//
// The clip is the crude answer and it is deliberately the one taken here: it desaturates a
// saturated colour instead of losing it, and every alternative is a gamut-mapping curve with a
// perceptual argument behind it — a number wanting a screen, which is Open.md's territory rather
// than a first cut's. What must not happen is the picture depending on which driver the machine has.
//
// The upper end is not clipped, because it is not out of range: brightness-relative compositing
// puts HDR headroom above 1.0 on purpose, and the target format is what limits it.
vec3 ChainEncode(vec3 light, int transfer)
{
	const vec3 inGamut = max(light, vec3(0.0));

	if (transfer == ChainTransferSrgb)
	{
		return ChainLinearToSrgb(inGamut);
	}

	if (transfer == ChainTransferPq)
	{
		return ChainLinearToPq(inGamut);
	}

	return inGamut;
}

// The six non-identity conversions between three primary sets, each `inverse(toXYZ(target)) *
// toXYZ(source)` at D65, evaluated exactly once — here — rather than per fragment. Written
// column-major because that is what `mat3`'s constructor takes, and a matrix transposed by accident
// is a picture that is merely tinted rather than obviously broken.
mat3 ChainPrimaries(int from, int to)
{
	if (from == ChainPrimariesBt709 && to == ChainPrimariesDciP3)
	{
		// clang-format off
		return mat3(0.822461969, 0.033194199, 0.017082631,
		            0.177538031, 0.966805801, 0.072397441,
		            0.000000000, 0.000000000, 0.910519929);
		// clang-format on
	}

	if (from == ChainPrimariesBt709 && to == ChainPrimariesBt2020)
	{
		// clang-format off
		return mat3(0.627403896, 0.069097289, 0.016391439,
		            0.329283038, 0.919540395, 0.088013308,
		            0.043313066, 0.011362316, 0.895595253);
		// clang-format on
	}

	if (from == ChainPrimariesDciP3 && to == ChainPrimariesBt709)
	{
		// clang-format off
		return mat3( 1.224940176, -0.042056955, -0.019637555,
		            -0.224940176,  1.042056955, -0.078636046,
		             0.000000000,  0.000000000,  1.098273600);
		// clang-format on
	}

	if (from == ChainPrimariesDciP3 && to == ChainPrimariesBt2020)
	{
		// clang-format off
		return mat3(0.753833034, 0.045743849, -0.001210340,
		            0.198597369, 0.941777220,  0.017601717,
		            0.047569597, 0.012478931,  0.983608623);
		// clang-format on
	}

	if (from == ChainPrimariesBt2020 && to == ChainPrimariesBt709)
	{
		// clang-format off
		return mat3( 1.660491002, -0.124550475, -0.018150763,
		            -0.587641139,  1.132899897, -0.100578898,
		            -0.072849863, -0.008349423,  1.118729661);
		// clang-format on
	}

	if (from == ChainPrimariesBt2020 && to == ChainPrimariesDciP3)
	{
		// clang-format off
		return mat3( 1.343578253, -0.065297453,  0.002821787,
		            -0.282179671,  1.075787916, -0.019598495,
		            -0.061398582, -0.010490463,  1.016776707);
		// clang-format on
	}

	return mat3(1.0);
}

// One item's colour state converted to the target's, on premultiplied components.
//
// **Un-premultiplied first, and Docs/Architecture.md#premultiplied-alpha-is-the-sharp-edge is the
// whole reason.** A premultiplied value in an encoded transfer function has had its alpha applied in
// the wrong space, so linearising it directly is wrong by `alpha^1.2` — about thirteen percent at
// half alpha, on every soft edge in the system. The divide is inside this function rather than at
// the call site because it is part of what a conversion *is*, and because a variant that does not
// convert never executes it.
//
// The two scales are what makes an absolute transfer and a relative one meet: the caller hands the
// factor that takes the decoded value to cd/m² and the factor that takes cd/m² back to the target's
// own units, so PQ passes 1.0 for both ends it owns and a relative state passes its reference white.
vec4 ChainConvert(
	vec4 premultiplied,
	int sourceTransfer,
	int sourcePrimaries,
	int targetTransfer,
	int targetPrimaries,
	float decodeScale,
	float encodeScale
)
{
	const float alpha = premultiplied.a;
	const vec3 straight = alpha > 0.0 ? premultiplied.rgb / alpha : premultiplied.rgb;

	vec3 light = ChainDecode(straight, sourceTransfer) * decodeScale;

	if (sourcePrimaries != targetPrimaries)
	{
		light = ChainPrimaries(sourcePrimaries, targetPrimaries) * light;
	}

	return vec4(ChainEncode(light * encodeScale, targetTransfer) * alpha, alpha);
}

// The corner-radius mask, as coverage in `[0, 1]`.
//
// **The field is the distance to the corner arcs and to nothing else**, which is what keeps a square
// corner square. The straight edges of the quad are the rasterizer's and are already exactly where
// the producer put them; a full rounded-box distance would re-answer them, and its gradient has a
// crease along the diagonal that a hardware derivative straddles — four dimmed pixels on the corners
// of every unrounded rectangle on the screen.
//
// **The footprint is measured from the varying and never from the field**, for the same reason. The
// field's gradient is known in closed form here and `local` is a projective varying whose
// derivatives are smooth everywhere, so the pixel's reach along the gradient is one dot product per
// axis. Differentiating the field would put the crease back and would contaminate the pixel *beside*
// a corner as well, since a derivative is estimated across a two-by-two quad.
//
// **A distance field rather than geometry**, because the radius is animated: decision 96 rounds a
// window's frame rect and a spring moves it, so a tessellated corner would be a vertex count that
// changes during a transition. A field is the same six vertices at every radius and exact at all of
// them rather than at the ones a fan happened to sample.
float ChainCorner(vec2 local, vec2 extent, float radius)
{
	const vec2 centre = extent * 0.5;

	// Clamped to the extent, because a radius larger than the node is a capsule rather than an
	// error — and an unclamped one inverts the field and punches a hole in the middle of the node.
	const float clamped = min(radius, min(centre.x, centre.y));

	// How far outside the inset rectangle this fragment sits, per axis and never negative. Zero on
	// both axes is everything that is not in a corner, which is most of a node.
	const vec2 outside = max(abs(local - centre) - (centre - clamped), vec2(0.0));
	const float reach = length(outside);
	const float edge = reach - clamped;

	// The field's own gradient, in surface-local units and pointing out of the nearest corner.
	const vec2 direction = reach > 0.0 ? sign(local - centre) * (outside / reach) : vec2(0.0);

	// That direction measured in pixels: how far one pixel step moves along it, which is the
	// footprint the coverage below is a fraction of. It is correct under a projection rather than
	// only under a translation, because it is derived from the varying the projection produced.
	const float width = length(vec2(dot(direction, dFdx(local)), dot(direction, dFdy(local))));

	// Zero width is everything the arcs do not reach, where the answer is the sign alone.
	return width > 0.0 ? clamp(0.5 - edge / width, 0.0, 1.0) : step(edge, 0.0);
}

#endif
