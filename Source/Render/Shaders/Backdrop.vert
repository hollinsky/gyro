#version 450

// The full-viewport triangle every pass of the backdrop chain is drawn with.
//
// **A triangle rather than a quad**, which is the ordinary trick and worth one line: two triangles
// meeting on a diagonal make the rasterizer shade the fragments along that seam twice, as two
// separate quads, and one oversized triangle clipped to the viewport covers the same pixels with no
// seam in them at all. It also needs no push constants, no attributes and no varyings — the fragment
// stages derive everything they read from `gl_FragCoord`, which is the destination pixel they are
// writing and the only thing either needs to know.
//
// **The chain's regions are placed by the viewport rather than by geometry.** Each pass runs over the
// sub-rectangle of an image the item actually uses, so what differs between the extract of a small
// popover and the extract of a full-width panel is a `VkViewport` and a `VkRect2D` rather than a
// different quad — which is what lets the chain images be reserved once, at their largest, and used
// in part.

void main()
{
	// (0,0), (2,0), (0,2) in UV, which is (-1,-1), (3,-1), (-1,3) in clip: a triangle whose
	// intersection with the viewport is exactly the viewport.
	vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);

	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
