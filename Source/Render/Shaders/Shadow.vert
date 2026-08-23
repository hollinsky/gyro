#version 450

// The quad a shadow is drawn over: the item's own device-space rect, grown by the reach the light
// gives it.
//
// **Its own program rather than a variant of Quad.vert, because it draws a different rectangle.**
// Every other pass places the four corners the producer projected; this one has to cover ground the
// node does not, since the whole of a shadow is what falls *outside* the thing casting it. Seam
// hands a `Quad` and Seam/Dressing.h's `Expansion` says how far past it to go, and the two together
// are four corners this stage can build without a vertex buffer for the same reason Quad.vert has
// none.
//
// **A rect rather than four corners, which is the refusal one level up read from here.**
// Render/Renderer.cpp declines to cast a shadow from a quad that is not upright, because whether a
// turned node's shadow shears with it or rides its plane is open (Docs/Open.md) — so what reaches
// this stage is always axis-aligned, the perspective weight is always one, and the geometry is a
// centre and a half-extent instead of a homography to extrapolate.

layout(push_constant) uniform Shadow
{
	// The item's rect in device pixels: centre in `xy`, half-extent in `zw`.
	vec4 Rect;

	// The corner radius in device pixels, the light's downward offset, the penumbra's standard
	// deviation, and the umbra's alpha.
	vec4 Shape;

	// The target's extent in pixels in `xy`, which turns a device-space position into a clip-space
	// one, and the expansion in `z` — how far past the rect this quad must reach for the whole
	// penumbra to have a fragment. One block declared identically by both stages.
	vec4 Target;
} shadow;

void main()
{
	// Top-left, top-right, bottom-right, then top-left, bottom-right, bottom-left — the winding
	// Quad.glsl uses, for no better reason than that a reader who has read one should not have to
	// check the other.
	const int order[6] = int[6](0, 1, 2, 0, 2, 3);
	vec2 quadrant[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

	// Symmetric, so the quad reaches as far above the rect as below it even though the shadow does
	// not. Seam/Dressing.h declares the expansion that way on purpose — it is a band of fragments
	// that evaluate to nothing against a scalar no call site can apply to the wrong axis.
	vec2 position = shadow.Rect.xy + quadrant[order[gl_VertexIndex]] * (shadow.Rect.zw + shadow.Target.z);

	gl_Position = vec4(position / shadow.Target.xy * 2.0 - 1.0, 0.0, 1.0);
}
