#pragma once

#include <cstddef>
#include <optional>

#include "Geometry/AxisTransform.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Space.h"
#include "Seam/Renderer.h"

// Where a node lands on an output, and whether it lands there at all.
//
// Seam/Renderer.h's `DrawItem` states the contract in one sentence — "animations are evaluated, the
// transform chain is composed and projected, back faces are culled, and anything entirely outside the
// target is gone" — and this file is the second half of it. Evaluation is decision 50's and
// composition is Geometry/NodeTransform.h's `ComposedTransform`; what is here is the projection, the
// winding, and the two culls, which is everything between the walk's transform stack and the four
// corners it hands a renderer.
//
// **It is in `Frame` rather than at either waist, and the placement is forced from both sides.**
// `Quad` is in `Seam` and `ComposedTransform` is in `Geometry`, and `Geometry` may not say `Seam`, so
// the assembly cannot live with either of the two types it joins. It is not `Seam`'s either:
// Docs/Structure.md calls that waist "every interface with more than one implementation and the data
// crossing it", and this is neither — one caller, one implementation, and no backend on the far end
// of it. Decision 82 has `Frame` build the draw list into its own arena, and this is the arithmetic
// that fills one item of it.
//
// **What is deliberately not here is `DrawItem::Sampling`.** Renderer.h insists a renderer cannot
// recover a `TransformClass` from four floats and that the producer must derive it, and that is still
// true and still owed. It is not foreclosed: the chain is taken by reference and outlives the call, so
// the reduction that asks whether a node's surface adapter and its chain compose to an
// `AxisTransform` reads the same object this one projects, beside this one, when there is a surface
// adapter to reduce. Nothing about a quad is what that question is asked of.

// One output's view of the world, and the three things about it a node's quad is measured against.
//
// **The placement arrives already composed**, as Geometry/AxisTransform.h's output adapter —
// decision 52's "global onto an output's device grid" — and that is the whole reason there is no
// output-layout record here. The adapter is one fact assembled from two, authored by different
// parties at rates far apart: the *grid* is the mode's and changes on a hotplug or a settings
// change, while the *origin* is the world's and moves at pointer rate for as long as somebody is
// dragging a monitor around in a settings panel. Decision 87's axis is how often a fact moves, so
// those two do not share a carrier — and a walk handed them separately would have to know which came
// from where. Taking them composed keeps that question out of this file entirely. Which carrier each
// half gets is not settled in the log yet, and nothing here depends on the answer.
//
// **Nothing about the view animates, and the reason is worth stating before somebody springs it.**
// An arrangement change is motion of the *nodes*, which resample on their own terms and snap when
// they settle (decision 67). A springy adapter would instead put the entire output at a fractional
// offset for the length of the animation, so every window on it goes soft at once and none of them
// can reach the settled snap until it ends — decision 52's "no integer flows backwards into the
// model", read from the far end. The drag itself is the exception that is not one: while somebody
// moves an output, its origin genuinely is between pixels, and showing that is the honest depiction
// of the thing being manipulated rather than an artefact of it.
//
// It is a class rather than an aggregate because two of its three members are read off the first one.
// A public mirror flag beside a matrix that disagreed with it is decision 87's objection to giving one
// fact two spellings, at the one place where the disagreement empties a screen.
class OutputView
{
public:
	// An output with no placement and no mode: the identity view onto a zero-sized target, which
	// draws nothing. That is the honest default rather than a null state to check for — a view
	// nobody configured has no pixels to put a window on.
	OutputView() = default;

	explicit OutputView(AxisTransform<GlobalSpace, DeviceSpace> placement, PixelSize<DeviceSpace> resolution) noexcept
		: m_Root{ ComposedTransform::ForView(placement) }, m_Resolution{ resolution }
	{
		// Whether the view mirrors, from the matrix rather than from the adapter it was built out of,
		// so the fact cannot come loose from the transform it describes. It is the sign of the
		// planar determinant, which catches a flipped orientation and a negative scale alike — the
		// second being a malformed adapter (`AxisTransform::IsValid`) rather than a configuration,
		// but one whose picture would otherwise be an empty output rather than a mirrored one.
		m_Mirrored = m_Root.M[0][0] * m_Root.M[1][1] - m_Root.M[0][1] * m_Root.M[1][0] < 0.0;
	}

	// What a walk starts from. Every chain handed to `Project` must descend from this by
	// `ComposedTransform::Push`, which is what puts the origin fold in front of every node instead of
	// inside each of them — see `ForView`.
	[[nodiscard]] const ComposedTransform& Root() const noexcept { return m_Root; }

	// The composite target's extent, which is `OutputConfiguration::Resolution` and is named here in
	// the grid it is measured on rather than being taken as a bare pair.
	[[nodiscard]] PixelSize<DeviceSpace> Resolution() const noexcept { return m_Resolution; }

	// A node's quad on this output, or nothing where the node is not drawn.
	//
	// `chain` is the composed transform down to the node and `extent` is the node's own, so the quad
	// is the image of `[0, Width] x [0, Height]` at z = 0 — the convention surface-local space
	// already uses and the one `BoundingRadius` is stated against.
	//
	// **Absence is the whole answer and there is no partial one.** All three of the culls below
	// remove the node entire: decision 92 rejects near-plane clipping, decision 55 culls back faces
	// with no per-node override, and a quad off the target has nothing to intersect against. An
	// optional is what makes that unforgettable at the call site, where a quad and a separate flag
	// would let a caller emit the first without consulting the second.
	[[nodiscard]] std::optional<Quad> Project(const ComposedTransform& chain, Size<SurfaceSpace> extent) const noexcept
	{
		// Top-left, top-right, bottom-right, bottom-left, which is Seam/Renderer.h's stated winding
		// and the order a Y-down space visits its own extent in. Spelled out rather than generated,
		// because "the four corners" names two different orders and the other one is a diagonal tear
		// rather than an error.
		const Vector3<float> local[4]{ { 0.0F, 0.0F, 0.0F },
			                           { extent.Width, 0.0F, 0.0F },
			                           { extent.Width, extent.Height, 0.0F },
			                           { 0.0F, extent.Height, 0.0F } };

		Quad quad{};

		// The projected positions before they narrow. Device space is single precision and that is
		// what a renderer is handed, but the back-face test below is a difference of products that
		// cancels to nothing on a quad seen edge-on, so it is answered at the width the projection
		// produced rather than at the width it is reported in.
		Vector3<double> position[4]{};

		for (std::size_t corner = 0; corner < 4; ++corner)
		{
			const Projected projected = chain.Project(local[corner]);

			// A corner at or below the weight floor has passed through the eye of some node above it
			// and is behind the viewer. One corner is the whole node, per decision 92: trimming it
			// would turn the quad into a three-to-five-gon and cost decision 82's flat draw list the
			// four corners it is made of.
			if (!projected.IsVisible())
			{
				return std::nullopt;
			}

			position[corner] = projected.Position;
			quad.Corners[corner] = { static_cast<float>(projected.Position.X),
				                     static_cast<float>(projected.Position.Y) };
			quad.Weights[corner] = projected.Weight;
		}

		if (!FacesViewer(position))
		{
			return std::nullopt;
		}

		if (!Reaches(quad.Bounds()))
		{
			return std::nullopt;
		}

		return quad;
	}

private:
	// The back-face test, answered from the projected quad rather than from the transforms that
	// produced it.
	//
	// **The signed area is the direct question and the per-node predicate is not.** Decision 86 said
	// back faces go by `NodeTransform::FacesViewer` over the composed chain, and there is no composed
	// chain to ask: that predicate reads one node's rotation and scale signs, and the answers do not
	// multiply — two nodes each turned eighty degrees both face front and their composition, at a
	// hundred and sixty, does not. The shoelace sum over the corners already computed is six
	// multiplies, needs no transform, and accounts for the perspective as well, which is the part a
	// decomposed answer could not see at all. See decision 93.
	[[nodiscard]] bool FacesViewer(const Vector3<double> (&position)[4]) const noexcept
	{
		// Twice the signed area. Positive is a front face under the winding above, because a Y-down
		// space and that order put the identity quad's shoelace sum at +2·Width·Height.
		double twiceArea = 0.0;

		for (std::size_t corner = 0; corner < 4; ++corner)
		{
			const Vector3<double>& from = position[corner];
			const Vector3<double>& to = position[(corner + 1) % 4];

			twiceArea += from.X * to.Y - to.X * from.Y;
		}

		// **A mirrored output is a configuration and not a back face.** Every quad on it is mirrored,
		// including the text, because that is what the person asked for — so the sign a front face
		// carries is the view's, and a fixed comparison here would empty the screen of a mirrored
		// projector rather than culling the two or three nodes anybody meant.
		//
		// Exactly zero is edge-on, has no area, and fails both readings. So does a NaN, which is why
		// the comparisons are written in this direction: a quad the arithmetic could not place
		// disappears rather than reaching a damage bound.
		return m_Mirrored ? twiceArea < 0.0 : twiceArea > 0.0;
	}

	// Whether the quad touches the target at all. Against the exact real bound rather than
	// `PixelBounds`, because rounding outward first would keep a quad that misses the target by a
	// third of a pixel — and the outward rounding exists for damage, where keeping too much is the
	// safe direction, which this is the opposite of.
	//
	// Half-open, so a quad ending exactly on the left edge or beginning exactly on the right one
	// covers no pixel and is gone. That is the same convention `Rect` uses for its own edges.
	[[nodiscard]] bool Reaches(Rect<DeviceSpace> bounds) const noexcept
	{
		const float width = static_cast<float>(m_Resolution.Width);
		const float height = static_cast<float>(m_Resolution.Height);

		return bounds.Right() > 0.0F && bounds.Bottom() > 0.0F && bounds.Left() < width && bounds.Top() < height;
	}

	ComposedTransform m_Root{};
	PixelSize<DeviceSpace> m_Resolution{};
	bool m_Mirrored = false;
};
