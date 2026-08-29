#include "Gym/Pointer.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "Core/ColorState.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "World/Content.h"
#include "World/Node.h"

namespace
{
// The arrow, as a polygon in a design box one unit wide. Height falls out of the tail rather than
// being chosen: the box is `ArrowWidth` by `ArrowHeight`, the hotspot is its top-left corner, and
// every specimen below is this shape times a scalar.
//
// **The shape is column-convex, and that is a requirement rather than an observation.** A column
// staircase can only draw a glyph whose vertical extent at each x is a single interval. The classic
// arrow is one — the notch between the tail and the wing bites upward from below and never separates
// the shape — and a glyph that was not would need a second run of columns and a second seam problem.
constexpr double ArrowWidth = 0.70;
constexpr double ArrowHeight = 1.10;

// The top boundary: one straight edge from the tip to the wing's point. The whole reason this file
// exists is that this line is not expressible as an upright rectangle.
[[nodiscard]] constexpr double ArrowTop(double x) noexcept
{
	return x * (0.68 / ArrowWidth);
}

// The bottom boundary, in four pieces: the left edge falling back to the notch, the notch's far side
// dropping to the tail, the tail's own foot, and the wing's underside beyond it.
[[nodiscard]] constexpr double ArrowBottom(double x) noexcept
{
	if (x <= 0.24)
	{
		return 1.00 + (0.76 - 1.00) * (x / 0.24);
	}

	if (x <= 0.40)
	{
		return 0.76 + (1.10 - 0.76) * ((x - 0.24) / 0.16);
	}

	if (x <= 0.56)
	{
		return 1.10 + (1.02 - 1.10) * ((x - 0.40) / 0.16);
	}

	return 0.68;
}

// How wide the bridge over each join between columns is, as a fraction of a step. One step wide and
// centred on the join, so any device pixel narrower than a step falls entirely inside it — which is
// what removes the seam, and is the whole of the requirement.
//
// **A bridge rather than widening the columns, and the difference is the shape.** Extending each
// column past its neighbour also covers the join, and it drags the staircase outward by whatever the
// edge rose over that distance — a fifth of the glyph's width at eight steps, which is a blunter arrow
// rather than a coarser one. A bridge takes the *intersection* of the two columns it joins, so it
// cannot reach outside either of them and the silhouette is exactly the staircase it was without it.
constexpr double BridgeWidth = 1.0;

// The dark outline's width, as a fraction of the glyph's height. A cursor is read against whatever is
// behind it, so the outline is not decoration: without it a light glyph over a light window is a hole.
constexpr double Outline = 0.055;

// The bracket's arm length and thickness, as fractions of the glyph's height. Thick enough to carry an
// outline at cursor size, and unequal in the two axes by nothing — a corner that is not square reads
// as an arrow that failed rather than as a bracket.
constexpr double BracketArm = 0.62;
constexpr double BracketThickness = 0.20;

// The body and its outline. Both opaque, and both spelling their colour state rather than taking the
// default, for the reason Gym/Lanes.cpp gives: `Blit/Blit.cpp` refuses an item whose colour state is
// not the output's and one refusal loses the frame, so the field that could empty this instrument on
// the backend it exists to be looked at on is written where a reader can check it.
constexpr SolidContent Body{ .Red = 1.0F, .Green = 1.0F, .Blue = 1.0F, .Color = ColorState::Srgb() };
constexpr SolidContent Edge{ .Red = 0.0F, .Green = 0.0F, .Blue = 0.0F, .Color = ColorState::Srgb() };

// A mid-grey field, so both the light body and the dark outline are readable against one ground.
constexpr SolidContent BackdropFill{ .Red = 0.46F, .Green = 0.47F, .Blue = 0.50F, .Color = ColorState::Srgb() };

[[nodiscard]] constexpr Vector3<double> At(double x, double y) noexcept
{
	return { x, y, 0.0 };
}

[[nodiscard]] constexpr Size<SurfaceSpace, float> Extent(double width, double height) noexcept
{
	return { static_cast<float>(width), static_cast<float>(height) };
}

// Gym/Lanes.cpp's latching builder, for its reason: after the first refusal nothing further is
// created, so a half-authored glyph is an error rather than a picture with a piece of an arrow in it.
class Builder
{
public:
	explicit Builder(SceneStore& scene) noexcept : m_Scene{ &scene } {}

	[[nodiscard]] EntityId Container(EntityId parent, const NodeProperties& properties)
	{
		return Adopt(m_Ok ? m_Scene->CreateContainer(parent, properties) : std::nullopt);
	}

	EntityId Solid(EntityId parent, const NodeProperties& properties, const SolidContent& content)
	{
		return Adopt(m_Ok ? m_Scene->CreateSolid(parent, properties, content) : std::nullopt);
	}

	[[nodiscard]] bool Ok() const noexcept { return m_Ok; }

private:
	[[nodiscard]] EntityId Adopt(std::optional<EntityId> created) noexcept
	{
		m_Ok = m_Ok && created.has_value();

		return created.value_or(EntityId{});
	}

	SceneStore* m_Scene;
	bool m_Ok = true;
};

// One staircase: the arrow's columns, grown outward by `grow` and filled with `fill`.
//
// **The extent of a column is the union over the span it covers rather than a sample at its middle**,
// so the staircase encloses the shape instead of clipping it. A sampled column loses the tip and the
// tail's corner — the two features the eye uses to decide the thing is an arrow — and loses them
// precisely at the coarse step counts this gym is meant to make a judgement about.
// `grow` is in *design* units, which the arrow's own box measures — not in the output's, which is what
// `Arms` below takes. The two differ by `ArrowHeight` and getting it wrong is not subtle: the outline
// comes out as a black rectangle around the glyph rather than a border on it, which is what the first
// run of this gym drew.
void Columns(
	Builder& builder,
	EntityId parent,
	double height,
	std::int32_t steps,
	double grow,
	const SolidContent& fill
)
{
	const double unit = height / ArrowHeight;
	const double step = ArrowWidth / static_cast<double>(steps);

	for (std::int32_t index = 0; index < steps; ++index)
	{
		const double left = step * static_cast<double>(index);
		const double right = left + step;

		// Both boundaries are monotone within a piece and the pieces break at the notch and the tail,
		// so the union over the span is the extreme at one end or the other of it — except across a
		// break, where it is neither. Sampling both ends and the breaks between them is what makes this
		// exact rather than nearly so, and there are four of them.
		double top = std::min(ArrowTop(left), ArrowTop(right));
		double bottom = std::max(ArrowBottom(left), ArrowBottom(right));

		for (const double corner : { 0.24, 0.40, 0.56 })
		{
			if (corner > left && corner < right)
			{
				top = std::min(top, ArrowTop(corner));
				bottom = std::max(bottom, ArrowBottom(corner));
			}
		}

		static_cast<void>(builder.Solid(
			parent,
			{ .Position = At((left - grow) * unit, (top - grow) * unit),
		      .Extent = Extent((right - left + 2.0 * grow) * unit, (bottom - top + 2.0 * grow) * unit) },
			fill
		));

		// The outline needs no bridge: it is the same staircase grown on every side, so consecutive
		// columns already overlap by twice that and no join is left to straddle.
		if (grow > 0.0 || index + 1 >= steps)
		{
			continue;
		}

		const double nextTop = std::min(ArrowTop(right), ArrowTop(right + step));
		const double nextBottom = std::max(ArrowBottom(right), ArrowBottom(right + step));

		const double bridgeTop = std::max(top, nextTop);
		const double bridgeBottom = std::min(bottom, nextBottom);

		// Where the two columns share no vertical run there is nothing to bridge and nothing to seam:
		// the join is a corner of the silhouette rather than a line through the middle of it.
		if (bridgeBottom <= bridgeTop)
		{
			continue;
		}

		static_cast<void>(builder.Solid(
			parent,
			{ .Position = At((right - step * BridgeWidth * 0.5) * unit, bridgeTop * unit),
		      .Extent = Extent(step * BridgeWidth * unit, (bridgeBottom - bridgeTop) * unit) },
			fill
		));
	}
}

// A point in whatever space the caller is working in. Two doubles rather than `Vector3` or `Point`,
// because everything below is plane geometry with no space attached to it and borrowing a typed one
// would say this arithmetic happens in global or surface coordinates, which it does not: it happens in
// device pixels of one output, offset from a hotspot.
struct Vertex
{
	double X = 0.0;
	double Y = 0.0;
};

// The arrow as a closed polygon, in the same design units `ArrowTop` and `ArrowBottom` measure and
// derived from them rather than restated, so the exact glyph and the staircase are the same shape and
// stay so when the shape is tuned.
//
// The wing is where the two functions stop describing one boundary: `ArrowBottom` steps from the tail's
// corner down to the wing's underside at `0.56`, and a step in a function of `x` is a vertical edge in
// the polygon. So there are two vertices at that `x` rather than one.
constexpr std::size_t ArrowVertices = 7;

[[nodiscard]] std::array<Vertex, ArrowVertices> ArrowPolygon() noexcept
{
	return { Vertex{ 0.0, 0.0 },
		     Vertex{ ArrowWidth, ArrowTop(ArrowWidth) },
		     Vertex{ 0.56, ArrowBottom(ArrowWidth) },
		     Vertex{ 0.56, ArrowBottom(0.56) },
		     Vertex{ 0.40, ArrowBottom(0.40) },
		     Vertex{ 0.24, ArrowBottom(0.24) },
		     Vertex{ 0.0, ArrowBottom(0.0) } };
}

// How far a mitred corner may reach, as a multiple of the outline's width. The arrow's sharpest corner
// is its tip at a shade under forty-six degrees, which mitres to two and a half, so this is a guard
// rather than a shape decision — and it clamps the length rather than inserting a bevel vertex,
// because a bevel changes the vertex count and this one is never reached.
constexpr double MiterLimit = 3.0;

// The most vertices a clipped polygon can carry. Each half-plane emits at most two vertices per input
// edge, and the sweep below clips four times; the arrow never comes close, and this exists so that the
// clip has somewhere to stop rather than to be tight.
constexpr std::size_t MaxClipped = 64;

// A polygon under construction, with a fixed ceiling and a latch. A clip that would overflow drops the
// vertex, which would understate an area — so the latch is read rather than assumed, and the caller
// refuses.
struct Polygon
{
	std::array<Vertex, MaxClipped> Points{};
	std::size_t Count = 0;
	bool Ok = true;

	void Push(Vertex point) noexcept
	{
		if (Count >= MaxClipped)
		{
			Ok = false;

			return;
		}

		Points[Count++] = point;
	}
};

// Twice the signed area, which is the shoelace sum with the halving left off. Sign carries the
// winding, which is what the offset below needs and what the areas do not.
[[nodiscard]] double DoubleSignedArea(std::span<const Vertex> polygon) noexcept
{
	double sum = 0.0;

	for (std::size_t index = 0; index < polygon.size(); ++index)
	{
		const Vertex& here = polygon[index];
		const Vertex& next = polygon[(index + 1) % polygon.size()];

		sum += here.X * next.Y - next.X * here.Y;
	}

	return sum;
}

// The polygon grown outward by `grow` on every side, by mitring each vertex along the bisector of its
// two edge normals. Exact for a simple polygon whose features are wider than twice `grow`, which the
// arrow's are: its thinnest place is the wing, and this grows the silhouette rather than insetting the
// body precisely because insetting would collapse it.
[[nodiscard]] std::array<Vertex, ArrowVertices>
Grown(const std::array<Vertex, ArrowVertices>& polygon, double grow) noexcept
{
	const double sign = DoubleSignedArea(polygon) >= 0.0 ? 1.0 : -1.0;

	const auto normal = [&](std::size_t index) {
		const Vertex& here = polygon[index];
		const Vertex& next = polygon[(index + 1) % ArrowVertices];

		const double dx = next.X - here.X;
		const double dy = next.Y - here.Y;
		const double length = std::hypot(dx, dy);

		return length > 0.0 ? Vertex{ sign * dy / length, -sign * dx / length } : Vertex{};
	};

	std::array<Vertex, ArrowVertices> out{};

	for (std::size_t index = 0; index < ArrowVertices; ++index)
	{
		const Vertex before = normal((index + ArrowVertices - 1) % ArrowVertices);
		const Vertex after = normal(index);

		// The mitre: the bisector scaled so the offset edges still pass through it. Degenerate only
		// where the two edges double back on each other, which a simple polygon's vertex does not.
		const double denominator = 1.0 + before.X * after.X + before.Y * after.Y;

		Vertex miter = denominator > 1e-9 ?
		                   Vertex{ (before.X + after.X) / denominator, (before.Y + after.Y) / denominator } :
		                   after;

		const double reach = std::hypot(miter.X, miter.Y);

		if (reach > MiterLimit)
		{
			miter = { miter.X * MiterLimit / reach, miter.Y * MiterLimit / reach };
		}

		out[index] = { polygon[index].X + grow * miter.X, polygon[index].Y + grow * miter.Y };
	}

	return out;
}

// Sutherland-Hodgman against one edge of an axis-aligned rectangle. Clipping a simple polygon to a
// convex region this way can leave zero-area seams along the boundary where a concave piece was cut in
// two, and that is exactly why it is the right tool here: a seam contributes nothing to the shoelace
// sum, so the area is right even where the outline is not.
void ClipHalfPlane(const Polygon& in, Polygon& out, bool horizontal, double bound, bool keepAbove) noexcept
{
	out.Count = 0;
	out.Ok = in.Ok;

	if (in.Count == 0)
	{
		return;
	}

	const auto coordinate = [&](const Vertex& point) { return horizontal ? point.X : point.Y; };
	const auto inside = [&](const Vertex& point) {
		return keepAbove ? coordinate(point) >= bound : coordinate(point) <= bound;
	};

	for (std::size_t index = 0; index < in.Count; ++index)
	{
		const Vertex& here = in.Points[index];
		const Vertex& next = in.Points[(index + 1) % in.Count];

		const bool hereIn = inside(here);
		const bool nextIn = inside(next);

		if (hereIn)
		{
			out.Push(here);
		}

		if (hereIn == nextIn)
		{
			continue;
		}

		const double from = coordinate(here);
		const double to = coordinate(next);
		const double span = to - from;
		const double t = span == 0.0 ? 0.0 : (bound - from) / span;

		out.Push({ here.X + t * (next.X - here.X), here.Y + t * (next.Y - here.Y) });
	}
}

// The polygon clipped to one axis-aligned slab, which is two half-planes.
void ClipSlab(const Polygon& in, Polygon& out, bool horizontal, double low, double high) noexcept
{
	Polygon scratch{};

	ClipHalfPlane(in, scratch, horizontal, low, true);
	ClipHalfPlane(scratch, out, horizontal, high, false);
}

// The area of a clipped piece, which is a coverage once the clip was a unit pixel. Unsigned, because
// the winding was the offset's business and is nobody's here.
[[nodiscard]] double Area(const Polygon& polygon) noexcept
{
	return std::abs(DoubleSignedArea(std::span{ polygon.Points.data(), polygon.Count })) * 0.5;
}

// One rectangle the sweep decided on, in device pixels of the output the glyph is authored against.
struct Span
{
	std::int32_t Left = 0;
	std::int32_t Top = 0;
	std::int32_t Width = 0;
	double Alpha = 0.0;
};

// Two coverages are the same span when they agree to within this. A pixel apart by less than a
// quantum of the eight-bit value the composite lands in is one the merge may as well take, and merging
// is what keeps the interior of the glyph a single rectangle per row rather than one per pixel.
constexpr double SameAlpha = 1.0 / 512.0;

// Everything an opacity below this would contribute, which is nothing a panel can show. Dropped rather
// than emitted, so a row's leading and trailing pixels do not each cost a node to draw nothing.
constexpr double NoAlpha = 1.0 / 512.0;

// The sweep: both polygons clipped to every device pixel they touch, turned into runs of equal
// coverage.
//
// **`dark` must contain `light`,** which is what makes the conditional coverage below a probability
// rather than an arbitrary ratio, and it holds by construction because `light` is what `dark` was
// grown from.
[[nodiscard]] bool Sweep(
	const std::array<Vertex, ArrowVertices>& dark,
	const std::array<Vertex, ArrowVertices>& light,
	std::vector<Span>& edges,
	std::vector<Span>& bodies
)
{
	Polygon outer{};
	Polygon inner{};

	for (const Vertex& point : dark)
	{
		outer.Push(point);
	}

	for (const Vertex& point : light)
	{
		inner.Push(point);
	}

	double top = dark.front().Y;
	double bottom = dark.front().Y;
	double left = dark.front().X;
	double right = dark.front().X;

	for (const Vertex& point : dark)
	{
		top = std::min(top, point.Y);
		bottom = std::max(bottom, point.Y);
		left = std::min(left, point.X);
		right = std::max(right, point.X);
	}

	const auto floorOf = [](double value) { return static_cast<std::int32_t>(std::floor(value)); };
	const auto ceilOf = [](double value) { return static_cast<std::int32_t>(std::ceil(value)); };

	for (std::int32_t row = floorOf(top); row < ceilOf(bottom); ++row)
	{
		Polygon outerRow{};
		Polygon innerRow{};

		ClipSlab(outer, outerRow, false, row, row + 1);
		ClipSlab(inner, innerRow, false, row, row + 1);

		if (!outerRow.Ok || !innerRow.Ok)
		{
			return false;
		}

		// A run in progress, one per pass, flushed when the coverage changes or the row ends. This is
		// the whole of the node count: a diagonal's interior is one rectangle per row and only the two
		// pixels its edges fall in cost anything extra.
		Span edge{};
		Span body{};

		const auto flush = [&](Span& run, std::vector<Span>& into) {
			if (run.Width > 0 && run.Alpha > NoAlpha)
			{
				into.push_back(run);
			}

			run = {};
		};

		const auto extend = [&](Span& run, std::vector<Span>& into, std::int32_t column, double alpha) {
			if (run.Width > 0 && run.Left + run.Width == column && std::abs(run.Alpha - alpha) < SameAlpha)
			{
				++run.Width;

				return;
			}

			flush(run, into);

			run = { .Left = column, .Top = row, .Width = 1, .Alpha = alpha };
		};

		for (std::int32_t column = floorOf(left); column < ceilOf(right); ++column)
		{
			Polygon outerPixel{};
			Polygon innerPixel{};

			ClipSlab(outerRow, outerPixel, true, column, column + 1);
			ClipSlab(innerRow, innerPixel, true, column, column + 1);

			if (!outerPixel.Ok || !innerPixel.Ok)
			{
				return false;
			}

			const double covered = std::clamp(Area(outerPixel), 0.0, 1.0);
			const double filled = std::clamp(Area(innerPixel), 0.0, covered);

			// The outline's conditional coverage. `1 - filled` is what the body left of the pixel, and
			// `covered - filled` is what the outline has to reach through it; where the body took the
			// whole pixel there is nothing left and nothing to draw.
			const double outline = filled >= 1.0 - NoAlpha ? 0.0 : (covered - filled) / (1.0 - filled);

			extend(edge, edges, column, outline);
			extend(body, bodies, column, filled);
		}

		flush(edge, edges);
		flush(body, bodies);
	}

	return true;
}

// The bracket's two arms, grown outward by `grow`. They overlap at the corner rather than abutting, so
// the seam the arrow needs an overlap to avoid cannot arise here at all.
void Arms(Builder& builder, EntityId parent, double height, double grow, const SolidContent& fill)
{
	const double arm = BracketArm * height;
	const double thickness = BracketThickness * height;

	static_cast<void>(builder.Solid(
		parent, { .Position = At(-grow, -grow), .Extent = Extent(arm + 2.0 * grow, thickness + 2.0 * grow) }, fill
	));

	static_cast<void>(builder.Solid(
		parent, { .Position = At(-grow, -grow), .Extent = Extent(thickness + 2.0 * grow, arm + 2.0 * grow) }, fill
	));
}
} // namespace

Result<EntityId> AuthorGlyph(SceneStore& scene, EntityId parent, Glyph glyph, double height, std::int32_t steps)
{
	if (height <= 0.0)
	{
		return Failure(EINVAL, "a glyph is authored at a height, and this one has none");
	}

	if (glyph == Glyph::Exact)
	{
		return Failure(EINVAL, "an exactly covered glyph is authored against a density, so ask AuthorExactGlyph");
	}

	if (glyph == Glyph::Arrow && steps <= 0)
	{
		return Failure(EINVAL, "an arrow is approximated by columns, and this one asks for none");
	}

	Builder builder{ scene };

	// The hotspot is this node's origin, which is what makes the subtree placeable by the pointer's
	// position with nothing subtracted from it. The outline reaches above and to the left of it, and
	// that is correct: a hotspot is a point on the screen rather than a corner of the drawing.
	//
	// **`Node::Snap` is on the root and on nothing below it, which is the flag working as intended.**
	// A glyph is a body of rectangles whose sub-pixel relationships are the drawing, so the grid is
	// taken once here and inherited; flagging the rectangles would round each of them apart. Without
	// it this gym's sliding specimen visibly boils — the outline is a stroke a pixel and a bit wide,
	// and at a fractional device position it is one dark pixel at one phase and two grey ones at the
	// next.
	const EntityId root = builder.Container(parent, { .Flags = Node::Snap });

	// The outline first and the body over it, which is the whole of the z-order here — decision 55
	// makes it the list order, so the two staircases are one sibling pair rather than a group.
	if (glyph == Glyph::Arrow)
	{
		Columns(builder, root, height, steps, Outline * ArrowHeight, Edge);
		Columns(builder, root, height, steps, 0.0, Body);
	}
	else
	{
		Arms(builder, root, height, Outline * height, Edge);
		Arms(builder, root, height, 0.0, Body);
	}

	if (!builder.Ok())
	{
		return Failure(ENOSPC, "the entity index space is exhausted, so the glyph has no nodes");
	}

	return root;
}

Result<EntityId> AuthorExactGlyph(SceneStore& scene, EntityId parent, double height, Scale density)
{
	if (height <= 0.0)
	{
		return Failure(EINVAL, "a glyph is authored at a height, and this one has none");
	}

	const double scale = density.ToDouble();

	// Design units to device pixels in one factor. The glyph's box is `ArrowHeight` design units tall
	// and `height` output units, and an output unit is `density` device pixels.
	const double unit = (height / ArrowHeight) * scale;

	std::array<Vertex, ArrowVertices> light = ArrowPolygon();

	for (Vertex& point : light)
	{
		point = { point.X * unit, point.Y * unit };
	}

	const std::array<Vertex, ArrowVertices> dark = Grown(light, Outline * ArrowHeight * unit);

	std::vector<Span> edges;
	std::vector<Span> bodies;

	if (!Sweep(dark, light, edges, bodies))
	{
		return Failure(EINVAL, "the glyph's polygon clipped to more vertices than the sweep carries");
	}

	if (edges.size() + bodies.size() > MaxParts)
	{
		return Failure(
			E2BIG, "the glyph decomposes into more rectangles than a scene will hold, so it is authored too large"
		);
	}

	Builder builder{ scene };

	// `Node::Snap` for `AuthorGlyph`'s reason and one more that is particular to this construction: the
	// coverage below was computed for the shape sitting exactly on the device grid, so a subtree at a
	// fractional position would be drawing one alignment's answer at another's. The staircase merely
	// looks worse when that happens; this is simply wrong.
	const EntityId root = builder.Container(parent, { .Flags = Node::Snap });

	// Back from device pixels to the output's own units, which is the space a node's extent is in. One
	// division rather than authoring in output units throughout, because the grid the areas were taken
	// against is the device's and doing the arithmetic anywhere else invites a rounding that moves an
	// edge by a pixel.
	const auto emit = [&](const std::vector<Span>& spans, const SolidContent& fill) {
		for (const Span& span : spans)
		{
			static_cast<void>(builder.Solid(
				root,
				{ .Position = At(static_cast<double>(span.Left) / scale, static_cast<double>(span.Top) / scale),
			      .Extent = Extent(static_cast<double>(span.Width) / scale, 1.0 / scale),
			      .Opacity = static_cast<float>(span.Alpha) },
				fill
			));
		}
	};

	// The outline first and the body over it, which is the order the conditional coverage was derived
	// under: `p` is what the outline needs *given* that the body has not already taken the pixel, and
	// swapping the two would make it the answer to a question nobody asked.
	emit(edges, Edge);
	emit(bodies, Body);

	if (!builder.Ok())
	{
		return Failure(ENOSPC, "the entity index space is exhausted, so the glyph has no nodes");
	}

	return root;
}

Result<PointerScene> AuthorPointers(SceneStore& scene)
{
	if (scene.Outputs().empty())
	{
		return Failure(ENODEV, "a gym is laid out against an output, and the store carries none");
	}

	// The first output rather than the union of them, for `AuthorLanes`' reason: a gym is one
	// instrument to look at, and spreading it would make which specimen is on which panel a property of
	// the hotplug order.
	const Rect<GlobalSpace> bounds = scene.Outputs().front().Bounds;

	if (bounds.IsEmpty())
	{
		return Failure(EINVAL, "the first output has no bounds in global space, so a gym has nowhere to lay out");
	}

	const double width = bounds.Extent.Width;
	const double height = bounds.Extent.Height;

	PointerScene pointers{};
	Builder builder{ scene };

	pointers.Stage = builder.Container({}, { .Position = At(bounds.Left(), bounds.Top()) });
	pointers.Backdrop = builder.Solid(pointers.Stage, { .Extent = Extent(width, height) }, BackdropFill);

	// Checked before the glyphs rather than only at the end, because `AuthorGlyph` builds against its
	// own latch: handed a parent that was never created it would author onto the top level instead,
	// which is a specimen loose on the output rather than a refusal.
	if (!builder.Ok())
	{
		return Failure(ENOSPC, "the entity index space is exhausted, so the gym has no stage");
	}

	// **The specimen heights are the output's own units and not fractions of it**, which is the one
	// place this gym parts company with Gym/Lanes.h's rule. A lane is a fraction because the instrument
	// should be the same shape on a 1080p panel and a 4K one; a glyph is the opposite, because the
	// question is how many steps land in a device pixel and a cursor that scaled with the panel would
	// answer the same everywhere. Twenty-four units is about what a pointer is.
	const auto place = [&](std::size_t row, std::size_t column) {
		return builder.Container(
			pointers.Stage,
			{ .Position =
		          At(width * (0.08 + 0.30 * static_cast<double>(column)),
		             height * (0.10 + 0.14 * static_cast<double>(row))) }
		);
	};

	// Rows are step counts and columns are sizes, so reading down a column is the trade this gym is for
	// and reading across a row is the same approximation under a scaled output.
	for (std::size_t row = 0; row < SpecimenRows; ++row)
	{
		for (std::size_t column = 0; column < SpecimenSizes; ++column)
		{
			const Result<EntityId> glyph =
				AuthorGlyph(scene, place(row, column), Glyph::Arrow, SpecimenHeights[column], SpecimenSteps[row]);

			if (!glyph)
			{
				return std::unexpected{ glyph.error() };
			}

			pointers.Arrows[row][column] = *glyph;
		}
	}

	// The exact row, directly under the staircases and at the same three sizes, which is the comparison
	// the whole grid exists to put in one frame. It is authored against the output's own density,
	// because coverage taken against any other grid is a shape that is neither exact nor a staircase.
	for (std::size_t column = 0; column < SpecimenSizes; ++column)
	{
		const Result<EntityId> glyph = AuthorExactGlyph(
			scene, place(SpecimenRows, column), SpecimenHeights[column], scene.Outputs().front().Density
		);

		if (!glyph)
		{
			return std::unexpected{ glyph.error() };
		}

		pointers.ExactArrows[column] = *glyph;
	}

	for (std::size_t column = 0; column < SpecimenSizes; ++column)
	{
		const Result<EntityId> glyph =
			AuthorGlyph(scene, place(SpecimenRows + 1, column), Glyph::Bracket, SpecimenHeights[column], 0);

		if (!glyph)
		{
			return std::unexpected{ glyph.error() };
		}

		pointers.Brackets[column] = *glyph;
	}

	// The two that move, at the size a cursor actually is and side by side, so the crawl one of them may
	// have is read against the other in the same frame rather than remembered from a different run.
	{
		const EntityId carriage = builder.Container(pointers.Stage, { .Position = At(0.0, height * 0.80) });

		const Result<EntityId> arrow =
			AuthorGlyph(scene, carriage, Glyph::Arrow, SpecimenHeights[0], SpecimenSteps[SpecimenRows - 1]);

		if (!arrow)
		{
			return std::unexpected{ arrow.error() };
		}

		const EntityId alongside = builder.Container(carriage, { .Position = At(SpecimenHeights[0] * 2.0, 0.0) });

		const Result<EntityId> exact =
			AuthorExactGlyph(scene, alongside, SpecimenHeights[0], scene.Outputs().front().Density);

		if (!exact)
		{
			return std::unexpected{ exact.error() };
		}

		const EntityId beside = builder.Container(carriage, { .Position = At(SpecimenHeights[0] * 4.0, 0.0) });

		const Result<EntityId> bracket = AuthorGlyph(scene, beside, Glyph::Bracket, SpecimenHeights[0], 0);

		if (!bracket)
		{
			return std::unexpected{ bracket.error() };
		}

		pointers.MovingArrow = carriage;
		pointers.MovingExact = alongside;
		pointers.MovingBracket = beside;

		pointers.SlideNear = At(width * 0.08, height * 0.80);
		pointers.SlideFar = At(width * 0.72, height * 0.80);
	}

	if (!builder.Ok())
	{
		return Failure(ENOSPC, "the entity index space is exhausted, so the gym has no scene");
	}

	return pointers;
}
