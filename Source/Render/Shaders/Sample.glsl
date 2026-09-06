#ifndef GYRO_RENDER_SHADERS_SAMPLE
#define GYRO_RENDER_SHADERS_SAMPLE

// How a fragment reads the image behind it, and the only place any pass does.
//
// **A screen pixel covers an area of the image rather than a point in it**, and what the picture
// should hold is the average over that area. A plain `texture()` asks a different question — what is
// the value at this one coordinate — and the answer moves as the coordinate does. On a window being
// shrunk that is texels being skipped outright: on a 125% output, which decision 56 makes the
// ordinary case rather than the exception, every window is minified for as long as it is on screen,
// so the skipping is not a transition artefact but the steady state of somebody's desktop.
//
// The area is integrated exactly, as a box, by splitting it into sub-boxes no wider than a texel and
// evaluating each one in closed form through a single bilinear tap. `SampleRamp` is that closed
// form: a box narrower than a texel convolved with the piecewise-constant image is a linear ramp
// across the texel boundary, and a bilinear tap *is* a linear ramp, so choosing where to put the tap
// chooses the ramp's width. One tap, no chain, no per-commit work on the dispatch thread.
//
// **The footprint is floored at one texel, so nothing is ever sharpened below what a bilinear tap
// already gives.** Below the floor the exact answer is a box narrower than a texel, which is
// nearest-neighbour — and Blit/Blit.cpp argues at length why that is the one filter the firmware
// handoff cannot have, since the logo it continues is magnified onto every panel larger than the one
// the GOP drew it at and stair steps where the firmware had drawn a curve. So magnification and unit
// scale come out of this function bit for bit what they came out of `texture()`, and only
// minification changes.
//
// **It does not reach a thumbnail.** Four sub-boxes per axis is an exact box out to a four-fold
// shrink, which covers every fractional output scale and every transition in
// Animation/Author/Catalog.h with a wide margin; past that this under-filters and degrades rather
// than failing, and a mip chain is the answer — Docs/Open.md's *mip generation's place in C* is the
// entry, and this is what makes its threshold a measurement rather than a guess, because the chain
// now only has to earn its bandwidth above the shrink this stops being exact at.

// Sub-boxes per axis, and the ceiling on what one fragment costs. Blit/Blit.cpp mirrors it with the
// same name and the same value, because two renderers that filter differently are two pictures.
const int SampleMaximumTaps = 4;

// Where to put a bilinear tap so that it returns the average of the image over a box of `width`
// texels centred on `at`, exactly, for a width no greater than one.
//
// The tap's own ramp runs the whole way from one texel centre to the next. Compressing the
// fragment's position within that span by the box's width is what narrows the ramp to the box: at a
// width of one the map is the identity and this is an ordinary bilinear tap, and as the width falls
// the ramp steepens toward the step function a box narrower than a texel actually is.
vec2 SampleRamp(vec2 at, vec2 width)
{
	// The centre of the texel this box sits in, on the lattice a bilinear tap interpolates between —
	// which is offset half a texel from the image's own grid, since a tap blends the two texels whose
	// centres straddle the coordinate.
	const vec2 centre = floor(at - 0.5) + 0.5;

	return centre + clamp((at - centre - 0.5) / width + 0.5, 0.0, 1.0);
}

// One item's texel, averaged over the fragment's footprint. `rect` is the source rectangle in
// normalized coordinates — origin in the first two, extent in the last two, exactly as
// Render/Pipeline.h packs it into `Fill`.
vec4 SampleImage(sampler2D image, vec2 uv, vec4 rect)
{
	const vec2 size = vec2(textureSize(image, 0));
	const vec2 at = uv * size;

	// **The footprint comes from the varying's derivatives and is taken before the loop**, which is
	// what keeps them in uniform control flow — a derivative inside a loop whose trip count differs
	// across a two-by-two fragment quad is undefined, and the trip count below is derived from this.
	//
	// Per axis and conservative: a quarter turn puts the node's x along the panel's y, and taking the
	// larger of the two ways an axis can be reached covers that without asking which case this is.
	const vec2 width = max(max(abs(dFdx(at)), abs(dFdy(at))), vec2(1.0));

	const ivec2 taps = min(ivec2(ceil(width)), ivec2(SampleMaximumTaps));
	const vec2 span = width / vec2(taps);

	// The first sub-box's centre. The sub-boxes tile the footprint end to end, so their average is the
	// average over the whole of it and no part of the area is weighted twice.
	const vec2 first = at - 0.5 * (width - span);

	// **Clamped to the item's own source rectangle rather than to the image**, which the single tap
	// this replaces did not have to do: a footprint wider than a texel reaches further than the edge
	// of an atlas slot, and what is on the other side of that edge is a different window. Half a texel
	// in from the boundary is where clamping stops changing the answer, because past a texel's centre
	// a tap returns that texel alone.
	const vec2 inset = 0.5 / size;
	const vec2 lower = rect.xy + inset;
	const vec2 upper = rect.xy + rect.zw - inset;

	vec4 sum = vec4(0.0);

	for (int y = 0; y < taps.y; ++y)
	{
		for (int x = 0; x < taps.x; ++x)
		{
			const vec2 centre = first + vec2(x, y) * span;

			sum += texture(image, clamp(SampleRamp(centre, span) / size, lower, upper));
		}
	}

	return sum / float(taps.x * taps.y);
}

#endif
