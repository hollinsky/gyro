#include "Gym/Console.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Core/Clock.h"
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Core/Time.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Testing/Test.h"
#include "Text/Font.h"
#include "World/Node.h"

// The console scene's sizing, checked where a reader would otherwise have to run it and look.
//
// Colours, margins and the strings themselves are taste and are not asserted, per Gym/Lanes.Test.cpp.
// What is asserted is the one claim this gym exists to make: a specimen is drawn at its own texel size
// on the panel's own pixels. A rung that is off by the output's scale is an instrument that reports a
// resample every face fails, on the machine where the answer decides whether a person can read the
// screen at all — and it is invisible on the 1x panel a test would otherwise be written against.

namespace
{
constexpr double PanelWidth = 1920.0;
constexpr double PanelHeight = 1080.0;

[[nodiscard]] SceneOutput Panel(std::int32_t density = 1)
{
	return { .Bounds = { {}, { PanelWidth, PanelHeight } },
		     .Density = Scale::FromInteger(density),
		     .Grid = { static_cast<std::int32_t>(PanelWidth) * density,
		               static_cast<std::int32_t>(PanelHeight) * density } };
}

struct Fixture
{
	ManualClock Clock{ Monotonic::FromNanoseconds(1'000'000'000) };
	SceneStore Store{ Clock };

	void Attach(SceneOutput output)
	{
		const SceneOutput outputs[] = { output };

		Store.SetOutputs(outputs);
	}
};

// A texture space that only counts, which is all this scene asks of one: it adopts the labels and
// never retires them. Gym/Pointer.Test.cpp's, for its reason.
class CountingTextures final : public ITextures
{
public:
	using ITextures::Adopt;

	[[nodiscard]] Result<TextureId>
	Adopt(PixelSize<BufferSpace>, std::uint32_t, std::span<const std::byte> pixels, TextureAlpha alpha) override
	{
		if (pixels.empty())
		{
			return Failure(EINVAL, "an image with no pixels");
		}

		// The one property of the adopt this scene is obliged to get right, and the one whose failure is
		// silent: `Blit` refuses an item whose alpha mode is not the output's, and a refusal loses the
		// frame rather than the label.
		Premultiplied = Premultiplied && alpha == TextureAlpha::Premultiplied;

		++Adopted;

		return TextureId{ Adopted, 1 };
	}

	void Retire(TextureId) noexcept override {}

	std::uint32_t Adopted = 0;
	bool Premultiplied = true;
};

// What an image node draws, which is the store's payload rather than the entity.
[[nodiscard]] const ImageContent* Drawn(const SceneStore& scene, EntityId id)
{
	const Entity* const entity = scene.Find(id);

	if (entity == nullptr || entity->Kind != NodeKind::Image || entity->Content >= scene.Images().size())
	{
		return nullptr;
	}

	return &scene.Images()[entity->Content];
}

[[nodiscard]] Size<SurfaceSpace, float> ExtentOf(const SceneStore& scene, EntityId id)
{
	const Entity* const entity = scene.Find(id);

	return entity == nullptr ? Size<SurfaceSpace, float>{} : entity->Extent;
}
} // namespace

GYRO_TEST(GymConsole, AnOutputWithBoundsIsWhatTheSpecimensAreLaidOutAgainst)
{
	CountingTextures textures;

	Fixture bare;

	const Result<ConsoleScene> none = AuthorConsole(bare.Store, textures);

	GYRO_REQUIRE(!none);
	GYRO_CHECK_EQ(none.error().Code(), ENODEV);

	Fixture unplaced;
	unplaced.Attach({ .Bounds = {}, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } });

	GYRO_CHECK_EQ(AuthorConsole(unplaced.Store, textures).error().Code(), EINVAL);

	// A scale of zero is not among the refusals because there is no such scale: `Geometry/Scale.h` clamps
	// to a minimum numerator of one, and a default-constructed one is the identity. This is the assertion
	// standing where that check was, so a scale that grows a null state is a failure here rather than a
	// specimen authored at an infinite extent.
	GYRO_CHECK(Scale{}.ToDouble() > 0.0);
}

GYRO_TEST(GymConsole, EveryRungOfTheLadderIsSpecimened)
{
	Fixture fixture;
	fixture.Attach(Panel());

	CountingTextures textures;

	const Result<ConsoleScene> console = AuthorConsole(fixture.Store, textures);

	GYRO_REQUIRE(console);

	// The ladder is whatever the bake produced, and a rung missing from the instrument is a size nobody
	// ever looks at — which is exactly how a face that is illegible ships.
	GYRO_REQUIRE(Faces().size() <= MaxLadderRungs);
	GYRO_CHECK_EQ(console->Rungs, Faces().size());
	GYRO_CHECK(textures.Premultiplied);

	for (std::size_t rung = 0; rung < console->Rungs; ++rung)
	{
		GYRO_REQUIRE(Drawn(fixture.Store, console->Ladder[rung]) != nullptr);
	}
}

GYRO_TEST(GymConsole, ASpecimenIsDrawnAtItsOwnTexelSizeOnThePanelsOwnPixels)
{
	// Two panels of the same logical size and different scales. A rung on the second must be authored at
	// half the extent of the same rung on the first, because a texel is a device pixel in both — and an
	// implementation that ignored the scale would pass on the first panel alone.
	for (const std::int32_t density : { 1, 2 })
	{
		Fixture fixture;
		fixture.Attach(Panel(density));

		CountingTextures textures;

		const Result<ConsoleScene> console = AuthorConsole(fixture.Store, textures);

		GYRO_REQUIRE(console);
		GYRO_REQUIRE(console->Rungs == Faces().size());

		for (std::size_t rung = 0; rung < console->Rungs; ++rung)
		{
			const Size<SurfaceSpace, float> extent = ExtentOf(fixture.Store, console->Ladder[rung]);
			const std::int32_t cell = Faces()[rung].CellSize().Height;

			// The label is one line, so its texel height is the face's cell exactly. Anything else and the
			// rung is being compared against a box rather than against a letter.
			GYRO_CHECK_EQ(extent.Height, static_cast<float>(cell) / static_cast<float>(density));
		}
	}
}

GYRO_TEST(GymConsole, TheSpecimenedFaceIsTheOneTheRowCountPicks)
{
	Fixture fixture;
	fixture.Attach(Panel(2));

	CountingTextures textures;

	const Result<ConsoleScene> console = AuthorConsole(fixture.Store, textures);

	GYRO_REQUIRE(console);

	for (std::size_t choice = 0; choice < ConsoleRowChoices; ++choice)
	{
		// The device grid rather than the logical extent, which is the whole of what this row reports: a
		// pick made against the logical height gives a 2x panel half the rows it has room for, and the
		// specimen would be drawn twice the size the count asked for while claiming the count.
		const std::int32_t cell = fixture.Store.Outputs().front().Grid.Height / ConsoleRowCounts[choice];

		GYRO_REQUIRE(console->Picked[choice] != nullptr);
		GYRO_CHECK_EQ(console->Picked[choice]->Name(), Nearest(cell).Name());
		GYRO_REQUIRE(Drawn(fixture.Store, console->Picks[choice]) != nullptr);
	}
}

GYRO_TEST(GymConsole, TheScaledPairIsTheSameImageAsTheOneItIsReadAgainst)
{
	Fixture fixture;
	fixture.Attach(Panel());

	CountingTextures textures;

	const Result<ConsoleScene> console = AuthorConsole(fixture.Store, textures);

	GYRO_REQUIRE(console);

	const ImageContent* const reference = Drawn(fixture.Store, console->Layout);
	const ImageContent* const enlarged = Drawn(fixture.Store, console->Enlarged);
	const ImageContent* const reduced = Drawn(fixture.Store, console->Reduced);

	GYRO_REQUIRE(reference != nullptr && enlarged != nullptr && reduced != nullptr);

	// One bake behind all three. Two strings that merely read the same would make the comparison about
	// whichever of them the layout happened to reach, which is the failure Gym/Cards.h names.
	GYRO_CHECK_EQ(enlarged->Texture, reference->Texture);
	GYRO_CHECK_EQ(reduced->Texture, reference->Texture);

	const Size<SurfaceSpace, float> at = ExtentOf(fixture.Store, console->Layout);

	// Scaled in double and narrowed once, which is how the gym does it. Multiplying the float extent by a
	// float factor is a different number in the last bit, and a test that spelled it that way would be
	// asserting its own arithmetic rather than the layout's.
	const auto scaled = [at](double factor) { return static_cast<float>(static_cast<double>(at.Height) * factor); };

	GYRO_CHECK_EQ(ExtentOf(fixture.Store, console->Enlarged).Height, scaled(ConsoleEnlargement));
	GYRO_CHECK_EQ(ExtentOf(fixture.Store, console->Reduced).Height, scaled(ConsoleReduction));

	// Neither factor is a whole number, which is the property that makes the pair say anything: doubling
	// a one-bit glyph is exact, so a specimen at an integer is a control that always passes.
	GYRO_CHECK(ConsoleEnlargement != 1.0 && ConsoleEnlargement != 2.0);
	GYRO_CHECK(ConsoleReduction != 0.5 && ConsoleReduction != 1.0);

	// The status line is its own bake, because the pair of words is what it is for.
	const ImageContent* const status = Drawn(fixture.Store, console->Reversed);

	GYRO_REQUIRE(status != nullptr);
	GYRO_CHECK(status->Texture != reference->Texture);
}

GYRO_TEST(GymConsole, EverySpecimenLandsOnThePanel)
{
	Fixture fixture;
	fixture.Attach(Panel());

	CountingTextures textures;

	const Result<ConsoleScene> console = AuthorConsole(fixture.Store, textures);

	GYRO_REQUIRE(console);

	// A specimen off the edge is an instrument with a rung nobody can look at, and it is the failure this
	// layout is most likely to have: the strings are fixed and the largest face is eight times the cell of
	// the smallest, so the widest row is a bake away from not fitting.
	for (EntityId child = fixture.Store.Find(console->Stage)->FirstChild; !child.IsNull();)
	{
		const Entity* const entity = fixture.Store.Find(child);

		if (entity->Kind == NodeKind::Image)
		{
			GYRO_CHECK(entity->Translation.Model().X + static_cast<double>(entity->Extent.Width) <= PanelWidth);
			GYRO_CHECK(entity->Translation.Model().Y + static_cast<double>(entity->Extent.Height) <= PanelHeight);
		}

		child = entity->NextSibling;
	}
}
