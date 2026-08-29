#include "Gym/Console.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "Core/ColorState.h"
#include "Geometry/Space.h"
#include "Text/Label.h"
#include "World/Content.h"

namespace
{
// The field the specimens are drawn on. Dark but not black, for `Gym/Cards.cpp`'s reason: a label that
// failed to bake or failed to import draws nothing, and against black that is indistinguishable from a
// panel that never lit.
constexpr SolidContent BackdropFill{ .Red = 0.06F, .Green = 0.07F, .Blue = 0.10F, .Color = ColorState::Srgb() };

// The two words a label selects between. Both opaque and both with their channels at or below their
// alpha, because `Scene/Textures.h` is told these are premultiplied and a word brighter than its own
// coverage is one the compositor is entitled to draw as anything at all.
//
// Off-white rather than white: a console is read for minutes at a time, and the pair that matters for
// legibility is the contrast rather than either end of it.
constexpr std::uint32_t Ink = 0xFFE6E6E6;
constexpr std::uint32_t Clear = 0x00000000;

// The status line's own block, which is the second thing the console draws — glyphs over a fill rather
// than glyphs over whatever is behind them.
constexpr std::uint32_t ReverseInk = 0xFF101418;
constexpr std::uint32_t ReversePaper = 0xFFC8CCD2;

// The characters a bitmap face fails on, and nothing else. Every pair here is one a reader has to tell
// apart in a line they are already worried about — a device node, a hexadecimal address, a path — and
// each is a different way for a small cell to lose: the vertical group to a stem one texel wide, `rn`
// and `m` to the gap between two of them, `gq9` to a descender the cell may not have room for, and the
// brackets to a shape that is mostly its own corners.
constexpr std::string_view Confusable = "  Il1| O0 rn m gq9 {}[]()<>";

// The layout specimen. A tab, a newline, and one line materially longer than its neighbours, which is
// the whole of `Text/Raster.h` as a picture: a tab landing somewhere other than the next multiple of
// eight columns puts the second word of the first line out of the column the third is in, and a line
// that wrapped where nothing asked it to is a layout that grew a rule it does not have.
constexpr std::string_view LayoutSpecimen = "gyro\tconsole\tgym\n"
											"Il1| O0 rn m ,.;: {}[]()<>\n"
											"the quick brown fox jumps over the lazy dog\n"
											"0123456789 +-*/= #@&%$";

// The status line, in the shape the console will want one: something a person reads once, at the edge
// of a screen that has otherwise gone wrong.
constexpr std::string_view StatusSpecimen = " gyro recovery console ";

// Margins and gaps, in the panel's own pixels rather than as fractions of it. `Gym/Pointer.h`'s rule:
// this instrument is about how many device pixels a letter is made of, so the space around the letters
// is measured the same way or the two disagree on a scaled output.
constexpr double EdgeMargin = 24.0;
constexpr double RowGap = 10.0;
constexpr double BlockGap = 28.0;

[[nodiscard]] constexpr Vector3<double> At(double x, double y) noexcept
{
	return { x, y, 0.0 };
}

[[nodiscard]] constexpr Size<SurfaceSpace, float> Extent(double width, double height) noexcept
{
	return { static_cast<float>(width), static_cast<float>(height) };
}

// Gym/Lanes.cpp's latching builder, for its reason: after the first refusal nothing further is created,
// so a scene missing the rung that would have said what was wrong is an error rather than a picture.
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

// A baked string: the id its pixels were adopted under, and how many texels they are. The image itself
// is a local of `Bake` — Scene/Textures.h reads the bytes during `Adopt` and the registry holds them
// afterwards, which is `Gym/Pointer.cpp`'s note and the reason nothing here owns a `Label`.
struct Baked
{
	TextureId Texture{};
	PixelSize<BufferSpace> Texels{};
};

[[nodiscard]] Result<Baked>
Bake(ITextures& textures, const Face& face, std::string_view text, std::uint32_t ink, std::uint32_t paper)
{
	const Result<Label> label = Label::Draw(face, text, ink, paper);

	if (!label)
	{
		return std::unexpected{ label.error() };
	}

	// Premultiplied, and the two words above are what makes that true rather than a hope: coverage is
	// one bit, so every texel is one of them unchanged. A straight-alpha spelling here would be refused
	// by `Blit/Blit.cpp` against an output that is not, and one refused item loses the whole frame —
	// which would empty the backend this gym is most useful on.
	const Result<TextureId> texture =
		textures.Adopt(label->Size(), label->Stride(), label->Bytes(), TextureAlpha::Premultiplied);

	if (!texture)
	{
		return std::unexpected{ texture.error() };
	}

	return Baked{ .Texture = *texture, .Texels = label->Size() };
}

// The cell height a row count asks for on a panel of this many device pixels, which is the arithmetic
// the console will do and is deliberately the *device* grid rather than the logical extent: a row is a
// row of pixels, and dividing the logical height would give a 2x panel half the rows it has room for.
[[nodiscard]] std::int32_t CellFor(std::int32_t gridHeight, std::int32_t rows) noexcept
{
	return rows > 0 ? gridHeight / rows : gridHeight;
}
} // namespace

Result<ConsoleScene> AuthorConsole(SceneStore& scene, ITextures& textures)
{
	if (scene.Outputs().empty())
	{
		return Failure(ENODEV, "a gym is laid out against an output, and the store carries none");
	}

	const SceneOutput& output = scene.Outputs().front();
	const Rect<GlobalSpace> bounds = output.Bounds;

	if (bounds.IsEmpty())
	{
		return Failure(EINVAL, "the first output has no bounds in global space, so a gym has nowhere to lay out");
	}

	// No guard on the scale, and the omission is the type's rather than an oversight: `Geometry/Scale.h`
	// clamps to a minimum numerator of one and defaults to the identity, so there is no zero to divide by
	// and a check for one would be a branch no output can take.
	const double density = output.Density.ToDouble();

	// One texel on one device pixel, which is the whole sizing claim this gym makes. Scene/Cursor.h's
	// arithmetic: an extent is logical and a texel is not, so the two differ by exactly the scale.
	const auto units = [density](std::int32_t texels) { return static_cast<double>(texels) / density; };

	ConsoleScene console{};
	Builder builder{ scene };

	console.Stage = builder.Container({}, { .Position = At(bounds.Left(), bounds.Top()) });
	console.Backdrop =
		builder.Solid(console.Stage, { .Extent = Extent(bounds.Extent.Width, bounds.Extent.Height) }, BackdropFill);

	const double left = units(static_cast<std::int32_t>(EdgeMargin));
	double top = units(static_cast<std::int32_t>(EdgeMargin));

	// One specimen per rung, at that rung's own cell size. The row a person reads down to find the first
	// line they can make out, which is the number the console's policy has to land on or above.
	const std::span<const Face> ladder = Faces();

	for (const Face& face : ladder)
	{
		if (console.Rungs == MaxLadderRungs)
		{
			break;
		}

		const std::string text = std::string{ face.Name() } + std::string{ Confusable };
		const Result<Baked> baked = Bake(textures, face, text, Ink, Clear);

		if (!baked)
		{
			return std::unexpected{ baked.error() };
		}

		console.Ladder[console.Rungs] = builder.Image(
			console.Stage,
			{ .Position = At(left, top), .Extent = Extent(units(baked->Texels.Width), units(baked->Texels.Height)) },
			{ .Texture = baked->Texture,
		      .Source = { {}, { static_cast<float>(baked->Texels.Width), static_cast<float>(baked->Texels.Height) } },
		      .Frame = {},
		      .Color = ColorState::Srgb() }
		);

		console.Rungs += 1;
		top += units(baked->Texels.Height) + units(static_cast<std::int32_t>(RowGap));
	}

	top += units(static_cast<std::int32_t>(BlockGap));

	// The picks, each drawn in the face its own row count resolves to. The caption is the specimen: a
	// row that cannot be read is the answer to whether that count is usable here.
	for (std::size_t choice = 0; choice < ConsoleRowChoices; ++choice)
	{
		const std::int32_t rows = ConsoleRowCounts[choice];
		const Face& face = Nearest(CellFor(output.Grid.Height, rows));

		console.Picked[choice] = &face;

		const std::string text = std::to_string(rows) + " rows -> " + std::string{ face.Name() };
		const Result<Baked> baked = Bake(textures, face, text, Ink, Clear);

		if (!baked)
		{
			return std::unexpected{ baked.error() };
		}

		console.Picks[choice] = builder.Image(
			console.Stage,
			{ .Position = At(left, top), .Extent = Extent(units(baked->Texels.Width), units(baked->Texels.Height)) },
			{ .Texture = baked->Texture,
		      .Source = { {}, { static_cast<float>(baked->Texels.Width), static_cast<float>(baked->Texels.Height) } },
		      .Frame = {},
		      .Color = ColorState::Srgb() }
		);

		top += units(baked->Texels.Height) + units(static_cast<std::int32_t>(RowGap));
	}

	top += units(static_cast<std::int32_t>(BlockGap));

	// The layout block and the scaled pair, all three from one bake. The same texture drawn three ways
	// is what makes the comparison about the resample rather than about two strings that might differ:
	// `Gym/Cards.h`'s rule, and the reference is in the same frame as the suspect.
	const Face& middle = Nearest(CellFor(output.Grid.Height, ConsoleRowCounts[ConsoleRowChoices / 2]));

	const Result<Baked> layout = Bake(textures, middle, LayoutSpecimen, Ink, Clear);

	if (!layout)
	{
		return std::unexpected{ layout.error() };
	}

	const ImageContent specimen{
		.Texture = layout->Texture,
		.Source = { {}, { static_cast<float>(layout->Texels.Width), static_cast<float>(layout->Texels.Height) } },
		.Frame = {},
		.Color = ColorState::Srgb()
	};

	const double specimenWidth = units(layout->Texels.Width);
	const double specimenHeight = units(layout->Texels.Height);

	console.Layout = builder.Image(
		console.Stage, { .Position = At(left, top), .Extent = Extent(specimenWidth, specimenHeight) }, specimen
	);

	top += specimenHeight + units(static_cast<std::int32_t>(RowGap));

	// The status line, which is the other thing the console draws: glyphs over their own block rather
	// than over the field. Its own bake, because the pair of words is the point of it.
	{
		const Result<Baked> status = Bake(textures, middle, StatusSpecimen, ReverseInk, ReversePaper);

		if (!status)
		{
			return std::unexpected{ status.error() };
		}

		console.Reversed = builder.Image(
			console.Stage,
			{ .Position = At(left, top), .Extent = Extent(units(status->Texels.Width), units(status->Texels.Height)) },
			{ .Texture = status->Texture,
		      .Source = { {}, { static_cast<float>(status->Texels.Width), static_cast<float>(status->Texels.Height) } },
		      .Frame = {},
		      .Color = ColorState::Srgb() }
		);

		top += units(status->Texels.Height) + units(static_cast<std::int32_t>(BlockGap));
	}

	// The same specimen off its own size, side by side under it. Enlarged first and reduced beside it,
	// so the staircase and the dropped rows are one glance apart from the copy that is neither.
	console.Enlarged = builder.Image(
		console.Stage,
		{ .Position = At(left, top),
	      .Extent = Extent(specimenWidth * ConsoleEnlargement, specimenHeight * ConsoleEnlargement) },
		specimen
	);

	console.Reduced = builder.Image(
		console.Stage,
		{ .Position = At(left + specimenWidth * ConsoleEnlargement + units(static_cast<std::int32_t>(BlockGap)), top),
	      .Extent = Extent(specimenWidth * ConsoleReduction, specimenHeight * ConsoleReduction) },
		specimen
	);

	if (!builder.Ok())
	{
		return Failure(ENOSPC, "the store had no room for the console scene");
	}

	return console;
}
