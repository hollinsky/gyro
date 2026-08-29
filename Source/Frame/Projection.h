#pragma once

#include <cmath>
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
// **`DrawItem::Sampling` is here too, and is `Classify` at the foot of this file rather than part of
// `Project`.** Renderer.h insists a renderer cannot recover a `TransformClass` from four floats and
// that the producer must derive it; this is where the producer does. It is a second pass over the same
// chain rather than a second return value, because the two questions have different inputs — a quad is
// a function of the extent alone, and the resample is a function of the extent against the texels
// under it — and only the nodes that carry pixels ever ask the second.

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

// SPEC: how far a device-space number may sit from a whole one and still be called exact.
//
// A 256th of a pixel is `wl_fixed`'s resolution, so it is the finest position the protocol can state
// and therefore the finest difference anything downstream could have meant. It is not a fudge factor
// for accumulated error: the chain a promotable node arrives on is a product of exact powers of two
// and one output scale, and the quantity being tested is the one decision 67's snap has already
// rounded. What the tolerance actually absorbs is the last bit of `(round(x / w) * w) / w`.
inline constexpr double SamplingTolerance = 1.0 / 256.0;

// How a node's texels land on the output's pixels, which is `DrawItem::Sampling` and the input to
// decision 152's promotion.
//
// **It is the buffer-to-device question rather than the surface-to-device one, and the difference is
// the whole reason this takes a source rectangle.** The chain says how many device pixels a surface
// unit covers; the source says how many texels that unit holds. Only their product answers *is this
// image sharp*, and the two factors routinely cancel — the pointer is baked at the output's density
// and sized in units at one over it, so a chain scaling by two carries an image with two texels per
// unit and reaches the panel texel for texel. Classifying the chain alone would call that a
// magnification and refuse to promote the one node on screen that is always exact.
//
// **An empty source is not classified.** World/Content.h makes empty mean *the whole image*, and the
// whole image is a texel count this walk does not have — the evaluator sees an id and never the pixels
// behind it. So an author that does not state its source gets the unreduced class, which promotes
// nothing and calls no resample free. That is the conservative direction of both readings, and it is
// what a client surface reports until its buffer size crosses the waist.
//
// **Everything not reduced reports the whole ladder unset**, exactly as a general affine does in
// Geometry/AxisTransform.h: perspective in the chain, a shear, a turn off the axes, or a node behind
// the viewer are all *nothing is known* rather than a rung of it.
[[nodiscard]] inline TransformClass
Classify(const ComposedTransform& chain, Size<SurfaceSpace> extent, Rect<BufferSpace> source) noexcept
{
	// A projective chain has no scale to speak of — the factor varies across the quad — so it reduces
	// to nothing rather than to its value at the origin.
	if (chain.M[3][0] != 0.0 || chain.M[3][1] != 0.0 || chain.M[3][2] != 0.0)
	{
		return {};
	}

	const double weight = chain.M[3][3];

	if (!(weight >= static_cast<double>(Projected::MinimumWeight)) || extent.IsEmpty() || source.IsEmpty())
	{
		return {};
	}

	// The two rows the quad actually travels through: a node's own space is z = 0 at every corner it
	// is projected at, so the third column reaches nothing here.
	const double xx = chain.M[0][0] / weight;
	const double xy = chain.M[0][1] / weight;
	const double yx = chain.M[1][0] / weight;
	const double yy = chain.M[1][1] / weight;

	// **Every test below is in device pixels across this node, and that is what makes one tolerance
	// enough.** An off-axis coefficient is a length per surface unit, so what it costs on screen is
	// that coefficient times the extent it acts over — which is the corner's displacement, the thing a
	// person could see. Comparing the coefficients themselves would hold a wide window to the same bar
	// as a glyph, and a right angle authored as a quaternion is never exactly one anyway: `sin(pi/4)`
	// squared misses a half by an ulp, so an exact test would refuse every quarter turn in the system
	// and admit none of the ones it was written to catch.
	const double slideX = std::abs(xy) * static_cast<double>(extent.Height);
	const double slideY = std::abs(yx) * static_cast<double>(extent.Width);
	const double spanX = std::abs(xx) * static_cast<double>(extent.Width);
	const double spanY = std::abs(yy) * static_cast<double>(extent.Height);

	// Diagonal is upright or flipped, anti-diagonal is a quarter turn either way, and anything between
	// them is an angle, which filters.
	const bool diagonal = slideX <= SamplingTolerance && slideY <= SamplingTolerance;
	const bool turned = spanX <= SamplingTolerance && spanY <= SamplingTolerance;

	if (!diagonal && !turned)
	{
		return {};
	}

	// Device pixels per surface unit, named along the *surface's* axes so that the comparison below is
	// against the extent measured on the same ones. Under a quarter turn the node's x is the panel's y,
	// which is exactly the swap this picks up.
	const double alongX = std::abs(diagonal ? xx : yx);
	const double alongY = std::abs(diagonal ? yy : xy);

	// The image's own pixels, on the panel. One texel per pixel is the whole question, and it is asked
	// as two lengths rather than as a ratio: a ratio divides twice and puts the tolerance on a
	// dimensionless number, where the thing being promised — this image reaches the glass as its own
	// texels — is a promise about pixels.
	const double width = alongX * static_cast<double>(extent.Width);
	const double height = alongY * static_cast<double>(extent.Height);

	const bool exact = std::abs(width - static_cast<double>(source.Extent.Width)) <= SamplingTolerance &&
	                   std::abs(height - static_cast<double>(source.Extent.Height)) <= SamplingTolerance;

	const double originX = chain.M[0][3] / weight;
	const double originY = chain.M[1][3] / weight;

	const bool whole = std::abs(originX - std::round(originX)) <= SamplingTolerance &&
	                   std::abs(originY - std::round(originY)) <= SamplingTolerance;

	return { .AxisAligned = true,
		     // No turn and no flip, which is the sign of the diagonal rather than its magnitude — a
		     // mirrored output flips every node on it and the flip is still one a plane cannot express.
		     .Upright = diagonal && xx > 0.0 && yy > 0.0,
		     .UnitScale = exact,
		     .IntegerOffset = whole };
}
