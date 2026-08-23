#include "Gym/Lanes.h"

#include <cerrno>

#include "Core/Clock.h"
#include "Core/Time.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Testing/Test.h"
#include "World/Elevation.h"
#include "World/Material.h"
#include "World/Node.h"

// The instrument's own layout, checked where a reader would otherwise have to run it and look.
//
// What is asserted here is the part a wrong answer makes *unreadable* rather than merely different: a
// marker that cannot reach the end of its track, a fixed point that is not the node's middle, a lane
// whose two nodes are in the wrong z order. Colours and fractions are taste and are not asserted —
// they change with a screen in front of somebody, and a test that pinned them would be a test that
// fails every time the instrument is improved.

namespace
{
constexpr double PanelWidth = 1920.0;
constexpr double PanelHeight = 1080.0;

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

// The lane a marker sits in, as the rectangle its track covers in the marker's own coordinates. Both
// nodes are children of the same lane container, so a marker's position is directly comparable with
// the track's extent and no composition is needed to ask whether it fits.
[[nodiscard]] bool Fits(const SceneStore& store, const Lane& lane, Vector3<double> at)
{
	const Entity* const track = store.Find(lane.Track);
	const Entity* const marker = store.Find(lane.Marker);

	if (track == nullptr || marker == nullptr)
	{
		return false;
	}

	return at.X >= 0.0 && at.Y >= 0.0 &&
	       at.X + static_cast<double>(marker->Extent.Width) <= static_cast<double>(track->Extent.Width) &&
	       at.Y + static_cast<double>(marker->Extent.Height) <= static_cast<double>(track->Extent.Height);
}
} // namespace

GYRO_TEST(GymLanes, AnOutputSetIsWhatTheLayoutIsAgainst)
{
	Fixture fixture;

	// No outputs at all: the composition root has not filled the set in yet, and laying the instrument
	// out against zero would be a black panel with nothing in the log about it.
	const Result<LaneScene> none = AuthorLanes(fixture.Store);

	GYRO_REQUIRE(!none);
	GYRO_CHECK_EQ(none.error().Code(), ENODEV);

	// An output the root gave a mode and no placement. Refused rather than laid out against an empty
	// rectangle, which is the same black panel one step further along.
	fixture.Attach({ .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } });

	const Result<LaneScene> unplaced = AuthorLanes(fixture.Store);

	GYRO_REQUIRE(!unplaced);
	GYRO_CHECK_EQ(unplaced.error().Code(), EINVAL);
}

GYRO_TEST(GymLanes, EveryLaneIsATrackAndAMarkerInThatOrder)
{
	Fixture fixture;

	fixture.Attach(Panel());

	const Result<LaneScene> lanes = AuthorLanes(fixture.Store);

	GYRO_REQUIRE(lanes);

	const Lane byChannel[] = { lanes->Slide, lanes->Grow, lanes->Fade, lanes->Turn };

	for (const Lane& lane : byChannel)
	{
		const Entity* const track = fixture.Store.Find(lane.Track);
		const Entity* const marker = fixture.Store.Find(lane.Marker);

		GYRO_REQUIRE(track != nullptr && marker != nullptr);

		// Both draw, so a lane that has lost one of them is a lane with a hole in it rather than a lane
		// whose reference is merely dim.
		GYRO_CHECK(track->Kind == NodeKind::Solid);
		GYRO_CHECK(marker->Kind == NodeKind::Solid);

		// Decision 55 makes the sibling list the z order, so the marker being the track's next sibling
		// is what puts it in front. Reversed, every marker would be behind its own reference.
		GYRO_CHECK(track->NextSibling == lane.Marker);
		GYRO_CHECK(track->Parent == marker->Parent);

		// Nothing in the instrument is dressed or lifted, which is what keeps it drawable by the CPU
		// renderer — Blit refuses either outright, and one refused item loses the whole frame.
		GYRO_CHECK(track->Dress == Material::None && track->Lift == Elevation::None);
		GYRO_CHECK(marker->Dress == Material::None && marker->Lift == Elevation::None);
	}

	// Four distinct lanes and a backdrop under all of them, so a frame that is entirely black is a
	// backend problem rather than a scene that authored nothing.
	GYRO_CHECK(fixture.Store.IsLive(lanes->Stage) && fixture.Store.IsLive(lanes->Backdrop));
	GYRO_CHECK(lanes->Slide.Marker != lanes->Grow.Marker);
	GYRO_CHECK(lanes->Fade.Marker != lanes->Turn.Marker);
}

GYRO_TEST(GymLanes, TheSlideMarkerReachesBothEndsOfItsTrackAndNeitherPastIt)
{
	Fixture fixture;

	fixture.Attach(Panel());

	const Result<LaneScene> lanes = AuthorLanes(fixture.Store);

	GYRO_REQUIRE(lanes);

	// The whole of what makes the track a reference: a marker that stopped short of the far end would
	// read as a translation that undershot, and one that hung past it as an overshoot. Neither is what
	// the endpoints say, so both readings are available to mean what they say.
	GYRO_CHECK(Fits(fixture.Store, lanes->Slide, lanes->SlideNear));
	GYRO_CHECK(Fits(fixture.Store, lanes->Slide, lanes->SlideFar));
	GYRO_CHECK(lanes->SlideFar.X > lanes->SlideNear.X);

	const Entity* const track = fixture.Store.Find(lanes->Slide.Track);
	const Entity* const marker = fixture.Store.Find(lanes->Slide.Marker);

	GYRO_REQUIRE(track != nullptr && marker != nullptr);

	// Flush, rather than merely inside — that is what makes "past the track" a thing that can be seen at
	// all. Within a global unit rather than exactly, because an extent is single precision and a
	// position is double, so the two edges meet to a fraction of a pixel and never to the bit. A pixel
	// is the right slack: it is the largest disagreement nobody can see and the smallest one that would
	// make an undershoot ambiguous.
	const double reach = lanes->SlideFar.X + static_cast<double>(marker->Extent.Width);

	GYRO_CHECK(reach <= static_cast<double>(track->Extent.Width));
	GYRO_CHECK(reach > static_cast<double>(track->Extent.Width) - 1.0);

	// And it starts where it starts, at rest. Every channel of the instrument is authored still, so the
	// first frame shows the resting state rather than a motion already under way.
	GYRO_CHECK_EQ(marker->Translation.Model(), lanes->SlideNear);
	GYRO_CHECK(marker->Translation.IsAtRest());
}

GYRO_TEST(GymLanes, TheScaledAndTurnedMarkersAreFixedAtTheirOwnMiddles)
{
	Fixture fixture;

	fixture.Attach(Panel());

	const Result<LaneScene> lanes = AuthorLanes(fixture.Store);

	GYRO_REQUIRE(lanes);

	// A fixed point half a marker off is a marker that walks sideways as it grows or swings out of its
	// lane as it turns, and both read as a translation fault on a channel that is not moving.
	for (const EntityId id : { lanes->Grow.Marker, lanes->Turn.Marker })
	{
		const Entity* const marker = fixture.Store.Find(id);

		GYRO_REQUIRE(marker != nullptr);

		GYRO_CHECK_EQ(marker->Anchor.X, marker->Extent.Width * 0.5F);
		GYRO_CHECK_EQ(marker->Anchor.Y, marker->Extent.Height * 0.5F);
	}

	const Entity* const grow = fixture.Store.Find(lanes->Grow.Marker);

	GYRO_REQUIRE(grow != nullptr);

	// The scale lane starts small so that its first transition is a growth somebody can see, and the
	// track behind it is the marker at full size — so the pair being flush is what "settled" looks like.
	GYRO_CHECK_EQ(grow->Scale.Model(), GrowSmall);
	GYRO_CHECK(grow->Scale.IsAtRest());
}

GYRO_TEST(GymLanes, TheFadeMarkerNeverCoversTheWholeOfItsTrack)
{
	Fixture fixture;

	fixture.Attach(Panel());

	const Result<LaneScene> lanes = AuthorLanes(fixture.Store);

	GYRO_REQUIRE(lanes);

	const Entity* const track = fixture.Store.Find(lanes->Fade.Track);
	const Entity* const marker = fixture.Store.Find(lanes->Fade.Marker);

	GYRO_REQUIRE(track != nullptr && marker != nullptr);

	// The uncovered strip is the reference. Without it, an opacity that settled at the wrong value looks
	// exactly like one that settled at the right value, because there is nothing in frame at full
	// strength to compare against.
	GYRO_CHECK(marker->Extent.Width < track->Extent.Width);
	GYRO_CHECK(marker->Extent.Width > 0.0F);

	GYRO_CHECK_EQ(marker->Opacity.Model(), FadeDim);
	GYRO_CHECK(marker->Opacity.IsAtRest());
}

GYRO_TEST(GymLanes, TheOutputsOriginIsWhereTheStageIsParked)
{
	Fixture fixture;

	// A second monitor's worth of offset, which is the ordinary case the moment anything is dragged in
	// a settings panel. Every position in the instrument is relative to the stage, so getting this
	// wrong puts the whole scene off the panel rather than one lane out of place.
	fixture.Attach(Panel({ 2560.0, 120.0 }));

	const Result<LaneScene> lanes = AuthorLanes(fixture.Store);

	GYRO_REQUIRE(lanes);

	const Entity* const stage = fixture.Store.Find(lanes->Stage);

	GYRO_REQUIRE(stage != nullptr);

	GYRO_CHECK_EQ(stage->Translation.Model(), Vector3<double>(2560.0, 120.0, 0.0));
	GYRO_CHECK(stage->Kind == NodeKind::Container);
}

GYRO_TEST(GymLanes, TheMaterialOverlayIsTheOnlyPartTheCpuRendererRefuses)
{
	Fixture fixture;

	fixture.Attach(Panel());

	const Result<LaneScene> lanes = AuthorLanes(fixture.Store);

	GYRO_REQUIRE(lanes);

	const Result<MaterialOverlay> overlay = AuthorMaterialOverlay(fixture.Store, *lanes);

	GYRO_REQUIRE(overlay);

	const Entity* const glass = fixture.Store.Find(overlay->Glass);
	const Entity* const smoke = fixture.Store.Find(overlay->Smoke);

	GYRO_REQUIRE(glass != nullptr && smoke != nullptr);

	GYRO_CHECK(glass->Dress == Material::Glass && smoke->Dress == Material::Smoke);

	// Containers rather than solids: a dressed container draws its material and nothing else, which is
	// what makes a material a field on a node instead of a node kind of its own.
	GYRO_CHECK(glass->Kind == NodeKind::Container && smoke->Kind == NodeKind::Container);
	GYRO_CHECK(glass->Lift != Elevation::None && smoke->Lift != Elevation::None);

	// Over the lanes rather than beside them, so what the two are gathering is visibly moving.
	GYRO_CHECK(glass->Parent == lanes->Stage && smoke->Parent == lanes->Stage);
}

GYRO_TEST(GymLanes, AnOverlayOverNothingIsRefused)
{
	Fixture fixture;

	fixture.Attach(Panel());

	const Result<MaterialOverlay> overlay = AuthorMaterialOverlay(fixture.Store, LaneScene{});

	GYRO_REQUIRE(!overlay);
	GYRO_CHECK_EQ(overlay.error().Code(), EINVAL);
}
