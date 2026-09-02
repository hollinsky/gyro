#include "Scene/Background.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "Core/Clock.h"
#include "Core/Pam.h"
#include "Core/Time.h"
#include "Geometry/Scale.h"
#include "Scene/Output.h"
#include "Scene/Serializer.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"
#include "Testing/Test.h"

// What this file promises is four things a person would notice going wrong, and they are what is
// asserted here rather than the shape of the tree.
//
// The background is *behind* everything — a wallpaper handed over an hour into a session appearing
// over somebody's windows is the failure the container exists to prevent, and it is the one that
// would only show up on a machine somebody had logged into. It is shown only where it fits, because
// the alternative to a fit policy is showing nothing and the whole design rests on that being what
// actually happens. A replacement cross-fades rather than cutting. And the outgoing image is given up
// once, when the last node drawing it has left — not when it was replaced, which would be a panel
// sampling memory the registry had taken back, and not never, which is a leak at the rate somebody
// changes their wallpaper.

namespace
{
// A texture space that counts both directions, `Scene/Cursor.Test.cpp`'s for its reason: what this
// file promises about lifetime is a number of adopts and a number of retires.
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

		LastAlpha = alpha;
		++Adopted;

		return TextureId{ Adopted, 1 };
	}

	void Retire(TextureId) noexcept override { ++Retired; }

	std::uint32_t Adopted = 0;
	std::uint32_t Retired = 0;
	TextureAlpha LastAlpha = TextureAlpha::None;
};

// A solid image of the given extent, which is every background this file cares about the shape of.
[[nodiscard]] PamImage Wallpaper(std::int32_t width, std::int32_t height, std::uint8_t level)
{
	std::vector<std::byte> bytes;

	for (const char character : FormatPamHeader({ .Width = width, .Height = height, .Depth = 3, .MaxValue = 255 }))
	{
		bytes.push_back(static_cast<std::byte>(character));
	}

	bytes.resize(
		bytes.size() + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U,
		static_cast<std::byte>(level)
	);

	Result<PamImage> image = PamImage::Decode(bytes);

	// `value()` rather than a check: a fixture that could not build its own input has nothing to assert
	// and the throw names the line, where a returned empty image would fail every test after it.
	return std::move(image).value();
}

struct Fixture
{
	ManualClock Clock{ Monotonic::FromNanoseconds(1'000'000'000) };
	SceneStore Store{ Clock };
	CountingTextures Textures;
	SceneBackground Background;
	SceneSerializer Serializer;

	explicit Fixture(std::span<const SceneOutput> outputs)
	{
		Store.SetOutputs(outputs);

		GYRO_REQUIRE(Background.Open(Store));
	}

	// One dispatch iteration as `Dispatch/Loop.h` runs it: step, then serialise, which is the pass that
	// sweeps whatever finished leaving. The wake is the step's, which is what the loop folds.
	Wake Iterate()
	{
		const Wake wake = Background.Step(Store, Textures);

		static_cast<void>(Serializer.Serialize(Store));

		return wake;
	}

	// The same, past the wait the first background holds off for. Every test that is not *about* the
	// wait starts here, because a background nobody waited for is not on screen yet.
	void Show()
	{
		static_cast<void>(Iterate());

		Clock.Advance(BackgroundDelay);

		static_cast<void>(Iterate());
	}

	// Long enough that every spring in the world has settled, which is what retires a faded node. Eight
	// seconds of it, because `Motion::Gentle` is the slowest thing in the catalog and the threshold it
	// has to cross is an eight-bit code point — a wallpaper's cross-fade is genuinely a second or two
	// of wall clock, and a loop that gave up early would be a test asserting that a fade never finishes.
	void Settle()
	{
		for (int pass = 0; pass < 40; ++pass)
		{
			Clock.Advance(Duration{ 200'000'000 });

			static_cast<void>(Iterate());
		}
	}

	[[nodiscard]] std::size_t Panels() const
	{
		std::size_t count = 0;

		for (const BackgroundSheet& sheet : Background.Sheets())
		{
			count += sheet.Panels.size();
		}

		return count;
	}
};

[[nodiscard]] SceneOutput Panel(std::int32_t width, std::int32_t height, double x = 0.0)
{
	return SceneOutput{ .Id = OutputId{ static_cast<std::uint32_t>(x) + 1U, 1 },
		                .Bounds = { { x, 0.0 }, { static_cast<double>(width), static_cast<double>(height) } },
		                .Density = Scale::FromInteger(1),
		                .Grid = { width, height } };
}
} // namespace

// The whole reason the container is authored before anything else: the top level is a list, the last
// root is the frontmost, and a background that became a root when a shell handed one over would be in
// front of every window already on the machine.
GYRO_TEST(SceneBackground, IsBehindEveryRootAuthoredAfterIt)
{
	const SceneOutput outputs[] = { Panel(1920, 1080) };

	Fixture fixture{ outputs };

	// A window, authored the way anything else authors a root, after the container exists.
	const std::optional<EntityId> window = fixture.Store.CreateContainer(EntityId{}, {});

	GYRO_REQUIRE(window.has_value());

	GYRO_REQUIRE(fixture.Background.Set(fixture.Store, fixture.Textures, Wallpaper(1920, 1080, 200)).has_value());

	fixture.Show();

	GYRO_CHECK(fixture.Store.FirstRoot() == fixture.Background.Container());
}

// The fit rule, stated as the two answers it gives: a panel it matches gets the picture and a panel it
// does not gets nothing at all. No stretch, no crop, no letterbox.
GYRO_TEST(SceneBackground, DrawsOnlyOnAnOutputItFitsExactly)
{
	const SceneOutput outputs[] = { Panel(1920, 1080), Panel(1280, 720, 1920.0) };

	Fixture fixture{ outputs };

	GYRO_REQUIRE(fixture.Background.Set(fixture.Store, fixture.Textures, Wallpaper(1920, 1080, 200)).has_value());

	fixture.Show();

	GYRO_REQUIRE(fixture.Background.Sheets().size() == 1);
	GYRO_REQUIRE(fixture.Background.Sheets().front().Panels.size() == 1);
	GYRO_CHECK(fixture.Background.Sheets().front().Panels.front().Output == outputs[0].Id);
}

// A scaled panel wants the image the *device grid* is, which is what "properly scaled" means when
// there is no resampler: 1920 logical at 2x is a 3840-pixel wallpaper and the 1920-pixel one does not
// fit it.
GYRO_TEST(SceneBackground, FitsAgainstDevicePixelsRatherThanLogicalOnes)
{
	SceneOutput panel = Panel(1920, 1080);
	panel.Density = Scale::FromInteger(2);
	panel.Grid = { 3840, 2160 };

	const SceneOutput outputs[] = { panel };

	Fixture fixture{ outputs };

	GYRO_CHECK(SceneBackground::FittingSize(panel) == PixelSize<BufferSpace>{ 3840, 2160 });

	GYRO_REQUIRE(fixture.Background.Set(fixture.Store, fixture.Textures, Wallpaper(1920, 1080, 200)).has_value());

	fixture.Show();

	GYRO_CHECK(fixture.Panels() == 0);

	GYRO_REQUIRE(fixture.Background.Set(fixture.Store, fixture.Textures, Wallpaper(3840, 2160, 200)).has_value());

	fixture.Show();

	GYRO_CHECK(fixture.Panels() == 1);
}

// The cross-fade, as the two facts that make it one: both images are in the world at once, and the
// outgoing one is still there for frames after it was replaced.
GYRO_TEST(SceneBackground, KeepsBothImagesInTheWorldWhileOneReplacesTheOther)
{
	const SceneOutput outputs[] = { Panel(1920, 1080) };

	Fixture fixture{ outputs };

	GYRO_REQUIRE(fixture.Background.Set(fixture.Store, fixture.Textures, Wallpaper(1920, 1080, 40)).has_value());

	fixture.Show();

	GYRO_REQUIRE(fixture.Background.Set(fixture.Store, fixture.Textures, Wallpaper(1920, 1080, 200)).has_value());

	// One iteration and no wait, because there is a picture to fade from.
	static_cast<void>(fixture.Iterate());

	GYRO_CHECK(fixture.Background.Sheets().size() == 2);
	GYRO_CHECK(fixture.Panels() == 2);

	// And the one gyro is holding is the new one, whatever is still on screen beside it.
	GYRO_CHECK(fixture.Background.Texture() == TextureId{ 2, 1 });
}

// The lifetime rule, which is the one a fade makes easy to get wrong in the direction that tears: the
// replaced image is given up when the last node drawing it has gone, and not before.
GYRO_TEST(SceneBackground, GivesUpTheOldImageOnlyOnceNothingIsDrawingIt)
{
	const SceneOutput outputs[] = { Panel(1920, 1080) };

	Fixture fixture{ outputs };

	GYRO_REQUIRE(fixture.Background.Set(fixture.Store, fixture.Textures, Wallpaper(1920, 1080, 40)).has_value());

	fixture.Show();

	GYRO_REQUIRE(fixture.Background.Set(fixture.Store, fixture.Textures, Wallpaper(1920, 1080, 200)).has_value());

	static_cast<void>(fixture.Iterate());

	GYRO_CHECK(fixture.Textures.Retired == 0);

	fixture.Settle();

	GYRO_CHECK(fixture.Textures.Retired == 1);
	GYRO_CHECK(fixture.Background.Sheets().size() == 1);
	GYRO_CHECK(fixture.Panels() == 1);
}

// A monitor unplugged takes its node with it and the image stays, because the other panel is still
// showing it. The same path is a mode change: what fitted no longer does.
GYRO_TEST(SceneBackground, TakesTheImageOffAnOutputThatStoppedFitting)
{
	const SceneOutput outputs[] = { Panel(1920, 1080), Panel(1920, 1080, 1920.0) };

	Fixture fixture{ outputs };

	GYRO_REQUIRE(fixture.Background.Set(fixture.Store, fixture.Textures, Wallpaper(1920, 1080, 200)).has_value());

	fixture.Show();

	GYRO_REQUIRE(fixture.Panels() == 2);

	const SceneOutput remaining[] = { Panel(1920, 1080) };

	fixture.Store.SetOutputs(remaining);

	fixture.Settle();

	GYRO_CHECK(fixture.Panels() == 1);
	GYRO_CHECK(fixture.Textures.Retired == 0);
}

// Taking the background away is a fade to black rather than a cut, and it ends holding nothing — a
// session ending on a machine whose shell has gone.
GYRO_TEST(SceneBackground, ClearsToBlackAndHoldsNothing)
{
	const SceneOutput outputs[] = { Panel(1920, 1080) };

	Fixture fixture{ outputs };

	GYRO_REQUIRE(fixture.Background.Set(fixture.Store, fixture.Textures, Wallpaper(1920, 1080, 200)).has_value());

	fixture.Show();

	fixture.Background.Clear(fixture.Store);

	GYRO_CHECK(fixture.Background.Texture().IsNull());
	GYRO_CHECK(fixture.Panels() == 1);

	fixture.Settle();

	GYRO_CHECK(fixture.Panels() == 0);
	GYRO_CHECK(fixture.Textures.Retired == 1);
}

// An image with alpha is adopted as premultiplied because the decode multiplied it, and one without is
// adopted as opaque. The wrong answer here is a wallpaper the desktop shows through in whatever
// pattern the file's unused top byte happened to hold.
GYRO_TEST(SceneBackground, TellsTheTextureSpaceWhatTheTopByteMeans)
{
	const SceneOutput outputs[] = { Panel(4, 4) };

	Fixture fixture{ outputs };

	GYRO_REQUIRE(fixture.Background.Set(fixture.Store, fixture.Textures, Wallpaper(4, 4, 200)).has_value());

	GYRO_CHECK(fixture.Textures.LastAlpha == TextureAlpha::None);
}

// The wait, which is what keeps a wallpaper from racing the boot it is meant to arrive after: nothing
// is in the world at all until it is due — not a transparent quad, which nothing culls — and the step
// answers when to come back, because a settled world would otherwise sleep through the moment.
GYRO_TEST(SceneBackground, TheFirstBackgroundWaitsBeforeItFadesIn)
{
	const SceneOutput outputs[] = { Panel(1920, 1080) };

	Fixture fixture{ outputs };

	GYRO_REQUIRE(fixture.Background.Set(fixture.Store, fixture.Textures, Wallpaper(1920, 1080, 200)).has_value());

	const Wake waiting = fixture.Iterate();

	GYRO_CHECK(fixture.Panels() == 0);
	GYRO_CHECK(waiting.Which == Wake::Kind::Timed);
	GYRO_CHECK(waiting.When == Advanced(fixture.Clock.Now(), BackgroundDelay));

	// Still nothing a moment before it is due, which is the half a wait written as a slower fade would
	// get wrong.
	fixture.Clock.Advance(BackgroundDelay - Duration{ 1 });

	static_cast<void>(fixture.Iterate());

	GYRO_CHECK(fixture.Panels() == 0);

	fixture.Clock.Advance(Duration{ 1 });

	const Wake shown = fixture.Iterate();

	GYRO_CHECK(fixture.Panels() == 1);
	GYRO_CHECK(shown.Which == Wake::Kind::Settled);
}

// And a replacement does not wait, because the picture it is fading from is already on screen — a
// person changing their wallpaper and getting two seconds of black is the failure the asymmetry
// exists to avoid.
GYRO_TEST(SceneBackground, AReplacementFadesInImmediately)
{
	const SceneOutput outputs[] = { Panel(1920, 1080) };

	Fixture fixture{ outputs };

	GYRO_REQUIRE(fixture.Background.Set(fixture.Store, fixture.Textures, Wallpaper(1920, 1080, 40)).has_value());

	fixture.Show();

	GYRO_REQUIRE(fixture.Panels() == 1);

	GYRO_REQUIRE(fixture.Background.Set(fixture.Store, fixture.Textures, Wallpaper(1920, 1080, 200)).has_value());

	static_cast<void>(fixture.Iterate());

	GYRO_CHECK(fixture.Panels() == 2);
}
