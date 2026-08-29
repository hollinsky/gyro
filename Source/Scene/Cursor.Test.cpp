#include "Scene/Cursor.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "Core/Clock.h"
#include "Core/Time.h"
#include "Core/Transfer.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Scene/Output.h"
#include "Scene/Serializer.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"
#include "Testing/Test.h"
#include "World/Node.h"

// What a wrong answer makes *unreadable*, on Gym/Lanes.Test.cpp's rule: the proportions and the
// palette are taste and are not asserted, because a test that pinned them would fail every time the
// glyph is improved with a screen in front of somebody.
//
// What is asserted is what the construction promises. The coverage is an area, so it integrates to the
// shape's area at every density rather than to whatever the grid happened to sample. The interior is
// opaque white and the outline is opaque black, so the glyph reads against any desktop. The texel is
// *encoded*, which is the one step that looks like a no-op and is not — writing the coverage straight
// into the byte would put a dark rim around the cursor. The hotspot is the node's own origin, so a
// pointer node is placed at the position `Scene/Pointer.h` holds with nothing subtracted from it. And
// the node is snapped, which is what makes the sampling a copy instead of a filter.

namespace
{
constexpr double PanelHeight = 1080.0;

struct Fixture
{
	ManualClock Clock{ Monotonic::FromNanoseconds(1'000'000'000) };
	SceneStore Store{ Clock };

	Fixture()
	{
		const SceneOutput outputs[] = {
			{ .Bounds = { {}, { 1920.0, PanelHeight } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } }
		};

		Store.SetOutputs(outputs);
	}
};

[[nodiscard]] std::uint32_t Alpha(std::uint32_t word) noexcept
{
	return (word >> 24U) & 0xFFU;
}

[[nodiscard]] std::uint32_t Level(std::uint32_t word) noexcept
{
	return word & 0xFFU;
}

// What `Blit` will make of a texel: unpremultiply, linearise, remultiply — decision 47's composite,
// which is the arithmetic the bake is written against. The answer is the light the pixel contributes,
// which is the body's coverage if the encoding is right.
[[nodiscard]] double Light(std::uint32_t word) noexcept
{
	const double alpha = static_cast<double>(Alpha(word)) / 255.0;

	if (alpha <= 0.0)
	{
		return 0.0;
	}

	const double encoded = static_cast<double>(Level(word)) / 255.0 / alpha;

	return static_cast<double>(SrgbToLinear(static_cast<float>(encoded))) * alpha;
}
} // namespace

// The claim the word *exact* is carrying, checked as a claim about areas rather than about pixels.
//
// Coverage that is really an area integrates to the shape's area. So the light each texel contributes,
// summed, is the arrow's own area — and dividing by the square of the glyph's height makes that a
// number the *shape* has and the sampling does not. Taking it at three densities is what separates the
// two: a construction that sampled the shape rather than integrating it would answer differently at
// each, and by more the coarser the grid.
GYRO_TEST(SceneCursor, CoverageIntegratesToTheSameAreaAtEveryDensity)
{
	const auto areaOf = [](Scale density) -> double {
		const Result<CursorImage> image = CursorImage::Draw(32.0, density);

		if (!image)
		{
			return 0.0;
		}

		double covered = 0.0;

		for (std::int32_t y = 0; y < image->Size().Height; ++y)
		{
			for (std::int32_t x = 0; x < image->Size().Width; ++x)
			{
				covered += Light(image->At(x, y));
			}
		}

		// Device pixels back to output units, which is the space the height was given in and the only
		// one three densities have in common.
		const double scale = density.ToDouble();

		return covered / (scale * scale) / (32.0 * 32.0);
	};

	const double one = areaOf(Scale::FromInteger(1));
	const double two = areaOf(Scale::FromInteger(2));
	const double three = areaOf(Scale::FromInteger(3));

	GYRO_CHECK(one > 0.0);

	// A hundredth, which is a real assertion rather than a shrug: the areas are the same integral taken
	// over three different partitions of the same plane, and what is between them is the eight-bit
	// quantisation of every edge texel — which is exactly the precision an eight-bit image has.
	GYRO_CHECK(std::abs(two - one) < 0.01);
	GYRO_CHECK(std::abs(three - one) < 0.01);
}

// The encoding, which is the step that looks like a no-op and is not.
//
// A texel is eight bits in the output's own encoding and `Blit` composites in linear light, so the
// value stored has to be the one that decodes back to the coverage the sweep computed. Writing the
// coverage itself would leave a half-covered edge pixel carrying about a fifth of the light it should
// — a dark rim around the cursor, which reads as a design choice rather than as a bug. So the check is
// against the decode: every texel comes back as a light between zero and its own alpha, and at least
// one partly covered texel is far enough from its stored byte to prove the curve was applied.
GYRO_TEST(SceneCursor, TexelsAreEncodedSoThatALinearCompositeGetsTheCoverageBack)
{
	const Result<CursorImage> image = CursorImage::Draw(48.0, Scale::FromInteger(1));

	GYRO_REQUIRE(image.has_value());

	bool bent = false;

	for (std::int32_t y = 0; y < image->Size().Height; ++y)
	{
		for (std::int32_t x = 0; x < image->Size().Width; ++x)
		{
			const std::uint32_t word = image->At(x, y);

			// Premultiplied means the colour can never exceed the alpha, which is the invariant an
			// importer is entitled to assume and the one a hand-rolled bake is most likely to break.
			GYRO_CHECK(Level(word) <= Alpha(word));

			const double light = Light(word);

			GYRO_CHECK(light >= -1e-9);
			GYRO_CHECK(light <= static_cast<double>(Alpha(word)) / 255.0 + 1e-9);

			const double stored = static_cast<double>(Level(word)) / 255.0;

			bent = bent || (Alpha(word) > 0U && std::abs(light - stored) > 0.05);
		}
	}

	GYRO_CHECK(bent);
}

// The glyph is a light body inside a dark outline, and both are opaque where they are not an edge.
// That is what a cursor has to be to be readable over an arbitrary desktop — a body with no outline is
// a hole over a white window, and an outline that is not opaque is a grey smudge over a dark one.
GYRO_TEST(SceneCursor, TheBodyIsOpaqueWhiteAndTheOutlineIsOpaqueBlack)
{
	const Result<CursorImage> image = CursorImage::Draw(48.0, Scale::FromInteger(1));

	GYRO_REQUIRE(image.has_value());

	std::size_t white = 0;
	std::size_t black = 0;

	for (std::int32_t y = 0; y < image->Size().Height; ++y)
	{
		for (std::int32_t x = 0; x < image->Size().Width; ++x)
		{
			const std::uint32_t word = image->At(x, y);

			white += static_cast<std::size_t>(word == 0xFFFFFFFFU);
			black += static_cast<std::size_t>(word == 0xFF000000U);
		}
	}

	// Both in quantity rather than merely present, because one stray texel of each is what a glyph that
	// came out mostly transparent would also produce.
	GYRO_CHECK(white > 64);
	GYRO_CHECK(black > 64);
}

// The hotspot is the node's own origin, which is what lets a pointer node be placed at the position
// `Scene/Pointer.h` holds with nothing subtracted from it. The outline reaches above and left of that
// point and is meant to: a hotspot is a place on the screen rather than a corner of the drawing.
//
// It is worth checking rather than assuming because a mitred offset that turned the wrong way would
// produce a glyph entirely inside its own box, and nobody would see it until the cursor's point was in
// the wrong place.
GYRO_TEST(SceneCursor, TheHotspotIsInsideTheImageAndTheOutlineHangsOutsideIt)
{
	Fixture fixture;

	const Result<CursorImage> image = CursorImage::Draw(48.0, Scale::FromInteger(1));

	GYRO_REQUIRE(image.has_value());

	GYRO_CHECK(image->HotspotX() > 0);
	GYRO_CHECK(image->HotspotY() > 0);

	// And the glyph is about as tall as it was asked to be, which is what catches an offset grown by the
	// wrong unit — the mistake that drew a black rectangle around the first staircase.
	GYRO_CHECK(image->Size().Height > 48);
	GYRO_CHECK(image->Size().Height < 48 * 3 / 2);

	const Result<EntityId> node = AuthorCursor(fixture.Store, {}, TextureId{ 1, 1 }, *image);

	GYRO_REQUIRE(node.has_value());

	const Entity* const entity = fixture.Store.Find(*node);

	GYRO_REQUIRE(entity != nullptr);

	GYRO_CHECK(entity->Translation.Model().X < 0.0);
	GYRO_CHECK(entity->Translation.Model().Y < 0.0);
}

// The node is snapped, and it is one node.
//
// Snapped because the coverage was computed for the shape sitting on the device grid: at a fractional
// position the glyph is one alignment's answer drawn at another's, through a filter, which is soft as
// well as wrong. One node because that is the whole of what the bake bought — the construction this
// replaced authored several hundred, on the one node that is on screen in every frame gyro draws.
GYRO_TEST(SceneCursor, TheCursorIsOneSnappedNodeWithNothingUnderIt)
{
	Fixture fixture;

	const Result<CursorImage> image = CursorImage::Draw(24.0, Scale::FromInteger(1));

	GYRO_REQUIRE(image.has_value());

	const Result<EntityId> node = AuthorCursor(fixture.Store, {}, TextureId{ 1, 1 }, *image);

	GYRO_REQUIRE(node.has_value());

	const Entity* const entity = fixture.Store.Find(*node);

	GYRO_REQUIRE(entity != nullptr);

	GYRO_CHECK((entity->Flags & Node::Snap) != 0);
	GYRO_CHECK(entity->Kind == NodeKind::Image);
	GYRO_CHECK(entity->FirstChild.IsNull());
}

// A node sized in the output's own units from an image measured in device pixels, so that the texels
// land one to one on the panel — which is what makes `Blit`'s filter return the copy rather than a
// resample, and the glyph reach the screen as the bytes that were baked.
GYRO_TEST(SceneCursor, TheNodeIsSizedSoTheTexelsLandOnDevicePixels)
{
	Fixture fixture;

	const Result<CursorImage> image = CursorImage::Draw(24.0, Scale::FromInteger(2));

	GYRO_REQUIRE(image.has_value());

	const Result<EntityId> node = AuthorCursor(fixture.Store, {}, TextureId{ 1, 1 }, *image);

	GYRO_REQUIRE(node.has_value());

	const Entity* const entity = fixture.Store.Find(*node);

	GYRO_REQUIRE(entity != nullptr);

	GYRO_CHECK(
		std::abs(static_cast<double>(entity->Extent.Width) * 2.0 - static_cast<double>(image->Size().Width)) < 1e-6
	);
	GYRO_CHECK(
		std::abs(static_cast<double>(entity->Extent.Height) * 2.0 - static_cast<double>(image->Size().Height)) < 1e-6
	);
}

// The bound doing its job: a glyph authored at a panel's height is a mistake about scale, and it
// arrives as a sentence naming the glyph rather than as a megabyte allocated on the dispatch thread.
GYRO_TEST(SceneCursor, RefusesAHeightThatWillNotBake)
{
	const Result<CursorImage> image = CursorImage::Draw(PanelHeight, Scale::FromInteger(2));

	GYRO_REQUIRE(!image);
	GYRO_CHECK(image.error().Code() == E2BIG);
}

// A height of nothing is a caller that computed one rather than chose one, and the sentence is the
// only place that shows.
GYRO_TEST(SceneCursor, RefusesAGlyphWithNoHeight)
{
	const Result<CursorImage> image = CursorImage::Draw(0.0, Scale::FromInteger(1));

	GYRO_REQUIRE(!image);
	GYRO_CHECK(image.error().Code() == EINVAL);
}

namespace
{
// A texture space that counts both directions, because what the cursor promises is that it gives an
// image back — a pointer taken away or carried onto a panel of another scale that kept its old texture
// is a leak at the rate somebody uses their machine.
class CountingTextures final : public ITextures
{
public:
	using ITextures::Adopt;

	[[nodiscard]] Result<TextureId>
	Adopt(PixelSize<BufferSpace>, std::uint32_t, std::span<const std::byte> pixels, TextureAlpha) override
	{
		if (pixels.empty())
		{
			return Failure(EINVAL, "an image with no pixels");
		}

		++Adopted;

		return TextureId{ Adopted, 1 };
	}

	void Retire(TextureId) noexcept override { ++Retired; }

	std::uint32_t Adopted = 0;
	std::uint32_t Retired = 0;
};

// A texture space with nothing behind it that can sample, which is a machine with no renderer and is
// what `--gym=card` refuses to open against.
class RefusingTextures final : public ITextures
{
public:
	using ITextures::Adopt;

	[[nodiscard]] Result<TextureId>
	Adopt(PixelSize<BufferSpace>, std::uint32_t, std::span<const std::byte>, TextureAlpha) override
	{
		++Asked;

		return Failure(ENODEV, "no renderer can sample this");
	}

	void Retire(TextureId) noexcept override {}

	std::uint32_t Asked = 0;
};

// Two panels of different scales side by side, which is the desk the glyph has to be re-baked on.
struct MixedFixture
{
	ManualClock Clock{ Monotonic::FromNanoseconds(1'000'000'000) };
	SceneStore Store{ Clock };

	MixedFixture()
	{
		const SceneOutput outputs[] = { { .Id = OutputId{ 1, 1 },
			                              .Bounds = { {}, { 1920.0, PanelHeight } },
			                              .Density = Scale::FromInteger(1),
			                              .Grid = { 1920, 1080 } },
			                            { .Id = OutputId{ 2, 1 },
			                              .Bounds = { { 1920.0, 0.0 }, { 1920.0, PanelHeight } },
			                              .Density = Scale::FromInteger(2),
			                              .Grid = { 3840, 2160 } } };

		Store.SetOutputs(outputs);
	}
};

// The model translation of an entity, which is where the cursor logically is rather than where any one
// output would draw it.
[[nodiscard]] Vector3<double> Where(const SceneStore& store, EntityId id) noexcept
{
	return store.Find(id)->Translation.Model();
}
} // namespace

// Nothing is drawn until a device moves the pointer, which is what makes the cursor the last root and
// therefore the frontmost node: every author that creates a root does so before the first event.
GYRO_TEST(SceneCursor, DrawsNothingUntilThePointerIsVisible)
{
	Fixture fixture;
	CountingTextures textures;
	SceneCursor cursor;

	cursor.Step(fixture.Store, textures);

	GYRO_CHECK(cursor.Container().IsNull());
	GYRO_CHECK(textures.Adopted == 0);
	GYRO_CHECK(fixture.Store.Count() == 0);
}

// One motion and there is a pointer on screen, at the position the device put it and nowhere else.
GYRO_TEST(SceneCursor, AuthorsTheGlyphAtThePointer)
{
	Fixture fixture;
	CountingTextures textures;
	SceneCursor cursor;

	static_cast<void>(fixture.Store.Pointer().Move({ 400.0, 300.0 }, fixture.Store.Outputs()));

	cursor.Step(fixture.Store, textures);

	GYRO_REQUIRE(!cursor.Container().IsNull());
	GYRO_CHECK(textures.Adopted == 1);
	GYRO_CHECK(fixture.Store.FirstRoot() == cursor.Container());

	const Vector3<double> at = Where(fixture.Store, cursor.Container());

	GYRO_CHECK(std::abs(at.X - 400.0) < 1e-9);
	GYRO_CHECK(std::abs(at.Y - 300.0) < 1e-9);

	// The glyph hangs under the container rather than being it, so what moves at input rate is one
	// translation and the hotspot offset is stated once.
	GYRO_CHECK(!fixture.Store.Find(cursor.Container())->FirstChild.IsNull());
}

// The position is written every iteration and the image is not, which is the difference between a
// pointer that keeps up with a hand and one that re-bakes an arrow at a thousand hertz.
GYRO_TEST(SceneCursor, FollowsThePointerWithoutRebaking)
{
	Fixture fixture;
	CountingTextures textures;
	SceneCursor cursor;

	static_cast<void>(fixture.Store.Pointer().Move({ 100.0, 100.0 }, fixture.Store.Outputs()));

	cursor.Step(fixture.Store, textures);

	const EntityId container = cursor.Container();

	for (int step = 0; step < 8; ++step)
	{
		static_cast<void>(fixture.Store.Pointer().Move({ 1.5, 0.5 }, fixture.Store.Outputs()));

		cursor.Step(fixture.Store, textures);
	}

	GYRO_CHECK(cursor.Container() == container);
	GYRO_CHECK(textures.Adopted == 1);
	GYRO_CHECK(textures.Retired == 0);

	const Vector3<double> at = Where(fixture.Store, container);

	GYRO_CHECK(std::abs(at.X - 112.0) < 1e-9);
	GYRO_CHECK(std::abs(at.Y - 104.0) < 1e-9);
}

// A finger on the screen and the cursor is gone from the world rather than faded out in it — the
// serialisation that follows the step is what sweeps it, so the scene published on that pass is the
// first without it.
GYRO_TEST(SceneCursor, WithdrawsTheGlyphWhenATouchHidesThePointer)
{
	Fixture fixture;
	CountingTextures textures;
	SceneCursor cursor;
	SceneSerializer serializer;

	static_cast<void>(fixture.Store.Pointer().Move({ 400.0, 300.0 }, fixture.Store.Outputs()));

	cursor.Step(fixture.Store, textures);

	GYRO_REQUIRE(fixture.Store.Count() == 2);

	fixture.Store.Pointer().Hide();

	cursor.Step(fixture.Store, textures);

	GYRO_CHECK(cursor.Container().IsNull());
	GYRO_CHECK(textures.Retired == 1);

	static_cast<void>(serializer.Serialize(fixture.Store));

	GYRO_CHECK(fixture.Store.Count() == 0);

	// And it comes back on the next motion, which is a person reaching for the mouse again.
	static_cast<void>(fixture.Store.Pointer().Move({ 1.0, 0.0 }, fixture.Store.Outputs()));

	cursor.Step(fixture.Store, textures);

	GYRO_CHECK(!cursor.Container().IsNull());
	GYRO_CHECK(textures.Adopted == 2);
}

// Coverage is taken against one device grid, so a pointer dragged onto a panel of another scale gets a
// glyph baked for that one. Without it the arrow is half the size it should be on the second monitor,
// and soft, because the sampling stops being a copy.
GYRO_TEST(SceneCursor, RebakesTheGlyphOnAPanelOfAnotherDensity)
{
	MixedFixture fixture;
	CountingTextures textures;
	SceneCursor cursor;

	static_cast<void>(fixture.Store.Pointer().Move({ 400.0, 300.0 }, fixture.Store.Outputs()));

	cursor.Step(fixture.Store, textures);

	const float width = fixture.Store.Find(fixture.Store.Find(cursor.Container())->FirstChild)->Extent.Width;

	static_cast<void>(fixture.Store.Pointer().Move({ 2000.0, 0.0 }, fixture.Store.Outputs()));

	cursor.Step(fixture.Store, textures);

	GYRO_CHECK(textures.Adopted == 2);
	GYRO_CHECK(textures.Retired == 1);

	// The glyph is the same size in the output's own units on both panels — twice the texels on the
	// scaled one, and a pointer that does not change size as a person drags it across the gap. The
	// tolerance is one unit because the polygon is bounded outward onto two different grids, which is a
	// texel of the coarser panel and is the whole of the difference a rebake can make.
	const float second = fixture.Store.Find(fixture.Store.Find(cursor.Container())->FirstChild)->Extent.Width;

	GYRO_CHECK(std::abs(static_cast<double>(second - width)) < 1.0);
}

// A texture space with no renderer behind it is a compositor that keeps running without a pointer drawn
// on it, and it asks once rather than once per motion.
GYRO_TEST(SceneCursor, AsksOnceWhereTheTextureSpaceRefuses)
{
	Fixture fixture;
	RefusingTextures textures;
	SceneCursor cursor;

	for (int step = 0; step < 8; ++step)
	{
		static_cast<void>(fixture.Store.Pointer().Move({ 1.0, 1.0 }, fixture.Store.Outputs()));

		cursor.Step(fixture.Store, textures);
	}

	GYRO_CHECK(cursor.Container().IsNull());
	GYRO_CHECK(textures.Asked == 1);
	GYRO_CHECK(fixture.Store.Count() == 0);
}
