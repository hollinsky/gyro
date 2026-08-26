#include "Gym/Pointer.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <optional>

#include "Core/ColorState.h"
#include "Geometry/Space.h"
#include "World/Content.h"

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

	if (glyph == Glyph::Arrow && steps <= 0)
	{
		return Failure(EINVAL, "an arrow is approximated by columns, and this one asks for none");
	}

	Builder builder{ scene };

	// The hotspot is this node's origin, which is what makes the subtree placeable by the pointer's
	// position with nothing subtracted from it. The outline reaches above and to the left of it, and
	// that is correct: a hotspot is a point on the screen rather than a corner of the drawing.
	const EntityId root = builder.Container(parent, {});

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
		             height * (0.10 + 0.16 * static_cast<double>(row))) }
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

	for (std::size_t column = 0; column < SpecimenSizes; ++column)
	{
		const Result<EntityId> glyph =
			AuthorGlyph(scene, place(SpecimenRows, column), Glyph::Bracket, SpecimenHeights[column], 0);

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

		const EntityId beside = builder.Container(carriage, { .Position = At(SpecimenHeights[0] * 2.0, 0.0) });

		const Result<EntityId> bracket = AuthorGlyph(scene, beside, Glyph::Bracket, SpecimenHeights[0], 0);

		if (!bracket)
		{
			return std::unexpected{ bracket.error() };
		}

		pointers.MovingArrow = carriage;
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
