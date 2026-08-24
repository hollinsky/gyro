#include "Gym/Cards.h"

#include <cerrno>

#include "Core/Clock.h"
#include "Core/Texture.h"
#include "Core/Time.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Testing/Test.h"
#include "World/Node.h"

// The card scene's layout, checked where a reader would otherwise have to run it and look.
//
// Colours and fractions are taste and are not asserted, per Gym/Lanes.Test.cpp. What is asserted is
// what a wrong answer makes unreadable: a reference copy that is not at its own texel size and so
// resamples like everything else, a travelling copy that leaves the panel, and any copy that is not
// naming the image the gym adopted.

namespace
{
constexpr double PanelWidth = 1920.0;
constexpr double PanelHeight = 1080.0;

constexpr TextureId Adopted{ 3, 1 };

[[nodiscard]] SceneOutput Panel(Point<GlobalSpace> origin = {})
{
	return { .Bounds = { origin, { PanelWidth, PanelHeight } },
		     .Density = Scale::FromInteger(1),
		     .Grid = { 1920, 1080 } };
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
} // namespace

GYRO_TEST(GymCards, AnOutputSetAndAnImageAreWhatTheLayoutIsAgainst)
{
	Fixture bare;

	const Result<CardScene> none = AuthorCards(bare.Store, Adopted);

	GYRO_REQUIRE(!none);
	GYRO_CHECK_EQ(none.error().Code(), ENODEV);

	Fixture unplaced;
	unplaced.Attach({ .Bounds = {}, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } });

	GYRO_CHECK_EQ(AuthorCards(unplaced.Store, Adopted).error().Code(), EINVAL);

	// A scene of four nodes naming no image is four rectangles that draw nothing and say nothing, which
	// is the one failure Core/Texture.h makes deliberately silent — so it is refused here instead.
	Fixture imageless;
	imageless.Attach(Panel());

	GYRO_CHECK_EQ(AuthorCards(imageless.Store, TextureId{}).error().Code(), EINVAL);
}

// Four copies of one image. A copy that named a different id would be a diagnostic reporting on
// something other than the picture beside it.
GYRO_TEST(GymCards, EveryCopyDrawsTheOneImageThatWasAdopted)
{
	Fixture fixture;
	fixture.Attach(Panel());

	const Result<CardScene> cards = AuthorCards(fixture.Store, Adopted);

	GYRO_REQUIRE(cards);

	for (const EntityId copy : { cards->Still, cards->Scaled, cards->Faded, cards->Sliding })
	{
		const ImageContent* const content = Drawn(fixture.Store, copy);

		GYRO_REQUIRE(content != nullptr);
		GYRO_CHECK_EQ(content->Texture, Adopted);

		// Empty is the whole image and the whole extent, per World/Content.h. A viewport or a window
		// geometry rect appearing here would make every copy a test of that instead.
		GYRO_CHECK(content->Source.IsEmpty());
		GYRO_CHECK(content->Frame.IsEmpty());
	}
}

// The reference has to be a reference. At unit output scale a node the size of its own texture samples
// one texel per pixel, so anything wrong with the still copy is the import — and a still copy laid out
// as a fraction of the panel like the others would resample too, leaving the scene with three suspects
// and no control.
GYRO_TEST(GymCards, TheStillCopyIsItsOwnTexelSizeAndTheOthersAreNot)
{
	Fixture fixture;
	fixture.Attach(Panel());

	const Result<CardScene> cards = AuthorCards(fixture.Store, Adopted);

	GYRO_REQUIRE(cards);

	const Entity* const still = fixture.Store.Find(cards->Still);

	GYRO_REQUIRE(still != nullptr);
	GYRO_CHECK_EQ(still->Extent.Width, static_cast<float>(CardTexels));
	GYRO_CHECK_EQ(still->Extent.Height, static_cast<float>(CardTexels));

	const Entity* const scaled = fixture.Store.Find(cards->Scaled);

	GYRO_REQUIRE(scaled != nullptr);
	GYRO_CHECK(scaled->Extent.Width != static_cast<float>(CardTexels));
}

// The travelling copy runs the width of the panel and stops inside it. A far end that hung off the edge
// would make the one copy whose job is to show subpixel phase spend half its travel clipped.
GYRO_TEST(GymCards, TheSlidingCopyReachesBothEndsAndNeitherPastThePanel)
{
	Fixture fixture;
	fixture.Attach(Panel());

	const Result<CardScene> cards = AuthorCards(fixture.Store, Adopted);

	GYRO_REQUIRE(cards);

	const Entity* const sliding = fixture.Store.Find(cards->Sliding);

	GYRO_REQUIRE(sliding != nullptr);
	GYRO_CHECK_EQ(sliding->Translation.Model(), cards->SlideNear);

	GYRO_CHECK(cards->SlideFar.X > cards->SlideNear.X);
	GYRO_CHECK(cards->SlideFar.X + static_cast<double>(sliding->Extent.Width) <= PanelWidth);
	GYRO_CHECK_EQ(cards->SlideFar.Y, cards->SlideNear.Y);
}

// Decision 55 makes the sibling list the z order, so the backdrop has to be created before every copy
// or the scene is one fill over four images.
GYRO_TEST(GymCards, TheBackdropIsBehindEveryCopy)
{
	Fixture fixture;
	fixture.Attach(Panel());

	const Result<CardScene> cards = AuthorCards(fixture.Store, Adopted);

	GYRO_REQUIRE(cards);

	const Entity* const stage = fixture.Store.Find(cards->Stage);

	GYRO_REQUIRE(stage != nullptr);
	GYRO_CHECK_EQ(stage->FirstChild, cards->Backdrop);
}

// The stage is parked at the output's own origin, so every position in the scene is read against the
// panel rather than against global space — which is what makes the layout the same on a second output
// placed anywhere.
GYRO_TEST(GymCards, TheOutputsOriginIsWhereTheStageIsParked)
{
	Fixture fixture;
	fixture.Attach(Panel({ 2560.0, 140.0 }));

	const Result<CardScene> cards = AuthorCards(fixture.Store, Adopted);

	GYRO_REQUIRE(cards);

	const Entity* const stage = fixture.Store.Find(cards->Stage);

	GYRO_REQUIRE(stage != nullptr);
	GYRO_CHECK_EQ(stage->Translation.Model(), (Vector3<double>{ 2560.0, 140.0, 0.0 }));
}
