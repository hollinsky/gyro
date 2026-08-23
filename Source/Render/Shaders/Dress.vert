#version 450
#extension GL_GOOGLE_include_directive : require

// The dressing's own quad. Same six vertices as the content pass, placed by the same function, on
// the far side of the split a gather forces.
//
// **The block differs from `Quad.vert`'s and the placement does not**, which is the split decision 62
// describes arriving in code: a pointwise run fuses into one program and a gather cannot, so the
// material's composite is a second pass over the same geometry. Quad.glsl is what keeps the geometry
// one geometry — a dressing that placed its corners by a second copy of the arithmetic would sit a
// fraction of a pixel off the node it dresses the first time either copy grew a clamp.

#include "Quad.glsl"

layout(push_constant) uniform Item
{
	vec4 Corner[4];
	vec4 Tint;
	vec4 Shape;
	vec4 Target;
	vec4 Chain;
} item;

layout(location = 0) out vec2 Local;

void main()
{
	gl_Position = QuadPlace(gl_VertexIndex, item.Corner, item.Shape.xy, item.Target.xy, Local);
}
