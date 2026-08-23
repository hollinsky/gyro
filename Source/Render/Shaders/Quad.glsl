#ifndef GYRO_RENDER_SHADERS_QUAD
#define GYRO_RENDER_SHADERS_QUAD

// Where one of a quad's six vertices lands, and the surface-local coordinate that goes with it.
//
// **Shared between the content pass and the dressing pass for Chain.glsl's reason**, one level up: a
// gather splits a chain, so decision 62's separate pass draws the *same quad* as the fused one, and
// the two agree about where a corner is only if the arithmetic that placed it is the same
// arithmetic. Two copies would be written the same afternoon and would drift the first time either
// grew a clamp — which is a dressing sliding a fraction of a pixel off the node it dresses, on a
// panel whose whole job is to sit exactly on something.

// `corner` is per corner: device-space x and y, the perspective weight, and one word spare.
// `extent` is the node's own size, `target` the attachment's in pixels. `local` comes back as the
// surface-local coordinate a corner radius is measured in.
vec4 QuadPlace(int vertex, vec4 corner[4], vec2 extent, vec2 target, out vec2 local)
{
	// Top-left, top-right, bottom-right, then top-left, bottom-right, bottom-left. Both triangles
	// wind the same way, and neither winding is asked about: decision 55 culls back faces at the
	// producer, so the pipeline culls nothing and a mirrored output is drawn rather than dropped.
	const int order[6] = int[6](0, 1, 2, 0, 2, 3);

	int index = order[vertex];
	vec2 corners[4] = vec2[4](vec2(0.0, 0.0), vec2(extent.x, 0.0), extent, vec2(0.0, extent.y));
	vec4 placed = corner[index];
	vec2 clip = placed.xy / target * 2.0 - 1.0;

	local = corners[index];

	// The weight goes back into `w` with the position pre-multiplied by it, so the divide lands where
	// the projection took it from and every varying interpolates perspective-correctly rather than
	// affine-per-triangle. It costs nothing on the orthographic case, where the weight is one.
	return vec4(clip * placed.z, 0.0, placed.z);
}

#endif
