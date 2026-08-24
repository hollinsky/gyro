#include "Gym/Cards.h"

#include <cerrno>
#include <optional>

#include "Core/ColorState.h"
#include "Geometry/Space.h"
#include "World/Content.h"

namespace
{
// The layout, as fractions of the output's own rectangle. See Gym/Cards.h for why it is fractions
// everywhere except the still copy.
//
// Two rows: three copies across the top at a third of the output's height each, and the travelling one
// alone underneath so that its run is the width of the panel rather than the width of a cell. A tenth
// of the width is kept at each side, so a card that has left its track has left it into visible space.
constexpr double SideMargin = 0.06;
constexpr double UpperRow = 0.08;
constexpr double LowerRow = 0.58;
constexpr double CardSide = 0.34;

// The backdrop. Deliberately a different dark from the card's own ground, so the edge of every copy is
// visible and a card that did not import is a hole rather than more panel.
constexpr SolidContent BackdropFill{ .Red = 0.05F, .Green = 0.06F, .Blue = 0.09F, .Color = ColorState::Srgb() };

[[nodiscard]] constexpr Vector3<double> At(double x, double y) noexcept
{
	return { x, y, 0.0 };
}

[[nodiscard]] constexpr Size<SurfaceSpace, float> Extent(double width, double height) noexcept
{
	return { static_cast<float>(width), static_cast<float>(height) };
}

[[nodiscard]] constexpr Vector3<float> Middle(Size<SurfaceSpace, float> extent) noexcept
{
	return { extent.Width * 0.5F, extent.Height * 0.5F, 0.0F };
}

// Gym/Lanes.cpp's builder, for its reason: the store refuses a create only by exhausting the index
// space, and a scene half-authored is a picture missing the copy that would have said what was wrong.
// After the first refusal nothing further is created and the whole scene is answered with an error.
class Builder
{
public:
	explicit Builder(SceneStore& scene) noexcept : m_Scene{ &scene } {}

	[[nodiscard]] EntityId Container(EntityId parent, const NodeProperties& properties)
	{
		return Adopt(m_Ok ? m_Scene->CreateContainer(parent, properties) : std::nullopt);
	}

	[[nodiscard]] EntityId Solid(EntityId parent, const NodeProperties& properties, const SolidContent& content)
	{
		return Adopt(m_Ok ? m_Scene->CreateSolid(parent, properties, content) : std::nullopt);
	}

	[[nodiscard]] EntityId Image(EntityId parent, const NodeProperties& properties, const ImageContent& content)
	{
		return Adopt(m_Ok ? m_Scene->CreateImage(parent, properties, content) : std::nullopt);
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
} // namespace

Result<CardScene> AuthorCards(SceneStore& scene, TextureId texture)
{
	if (scene.Outputs().empty())
	{
		return Failure(ENODEV, "a gym is laid out against an output, and the store carries none");
	}

	const Rect<GlobalSpace> bounds = scene.Outputs().front().Bounds;

	if (bounds.IsEmpty())
	{
		return Failure(EINVAL, "the first output has no bounds in global space, so a gym has nowhere to lay out");
	}

	if (texture.IsNull())
	{
		return Failure(EINVAL, "a card scene with no image is four nodes that draw nothing and say nothing");
	}

	const double width = bounds.Extent.Width;
	const double height = bounds.Extent.Height;

	// **Empty source and empty frame, which are the whole image and the whole extent.** World/Content.h
	// makes both sentinels mean *all of it*, and spelling them out is what a viewport or a client-drawn
	// shadow would change — neither of which this scene has, and both of which are worth being able to
	// see the absence of.
	const ImageContent card{ .Texture = texture, .Source = {}, .Frame = {}, .Color = ColorState::Srgb() };

	CardScene cards{};
	Builder builder{ scene };

	cards.Stage = builder.Container({}, { .Position = At(bounds.Left(), bounds.Top()) });
	cards.Backdrop = builder.Solid(cards.Stage, { .Extent = Extent(width, height) }, BackdropFill);

	const double side = height * CardSide;
	const double margin = width * SideMargin;
	const double upper = height * UpperRow;

	// The three across the top. Spread over the usable width rather than packed, so each has its own
	// space and a copy that grew out of its cell is doing something the others are not.
	const double usable = width - 2.0 * margin;
	const double step = (usable - side) / 2.0;

	// The still copy at its own texel size, which is the reference: at unit output scale this samples
	// one texel per pixel and nothing about it is the resampler's. It is deliberately *not* `side`.
	const Size<SurfaceSpace, float> exact = Extent(CardTexels, CardTexels);

	cards.Still = builder.Image(cards.Stage, { .Position = At(margin, upper), .Extent = exact }, card);

	// The scaling copy, anchored at its own middle so the shrink is symmetric — a copy that scaled about
	// a corner would walk across the panel and read as a translation bug on the wrong node.
	{
		const Size<SurfaceSpace, float> extent = Extent(side, side);

		cards.Scaled = builder.Image(
			cards.Stage,
			{ .Position = At(margin + step, upper),
		      .Scale = CardScaleSmall,
		      .Anchor = Middle(extent),
		      .Extent = extent },
			card
		);
	}

	{
		const Size<SurfaceSpace, float> extent = Extent(side, side);

		cards.Faded = builder.Image(
			cards.Stage, { .Position = At(margin + 2.0 * step, upper), .Extent = extent, .Opacity = CardFadeDim }, card
		);
	}

	// The travelling copy, alone on its own row so the run is the panel's width. Its ends are derived
	// from the extent that was actually authored rather than from the fraction it came from, because an
	// extent is single precision and a position is double — computing the far end out of the doubles
	// leaves the card off the end of its own travel by whatever the two roundings disagreed about.
	{
		const Size<SurfaceSpace, float> extent = Extent(side, side);

		cards.Sliding =
			builder.Image(cards.Stage, { .Position = At(margin, height * LowerRow), .Extent = extent }, card);

		cards.SlideNear = At(margin, height * LowerRow);
		cards.SlideFar = At(width - margin - static_cast<double>(extent.Width), height * LowerRow);
	}

	if (!builder.Ok())
	{
		return Failure(ENOSPC, "the store had no room for the card scene");
	}

	return cards;
}
