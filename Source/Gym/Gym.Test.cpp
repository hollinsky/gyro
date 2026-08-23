#include "Gym/Gym.h"

#include <cerrno>
#include <chrono>
#include <memory>
#include <string_view>

#include "Core/Clock.h"
#include "Core/Handle.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Scene/Entity.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Testing/Test.h"

// What a gym promises the loop above it: authors once, retargets what is due, and answers when it
// wants to be called again.
//
// The two claims worth a test are the two the design turns on. A perpetual gym never answers
// `Settled`, because a spring settles exactly and the only way to keep a scene moving is to keep
// writing to it — so an instrument that quietly stopped would be indistinguishable from a compositor
// that had gone idle correctly. And a settling gym answers `Never()` from its first `Advance`, because
// *doing nothing costs nothing* is a claim about a scene nobody is authoring, and there is nothing
// else in the tree that can make one.

namespace
{
constexpr Instant Start = Monotonic::FromNanoseconds(1'000'000'000);

// Longer than any lane's period, so an `Advance` this far on has every lane due at once. Written as a
// duration rather than as a multiple of a period, so the test does not restate what Gym.cpp tunes.
constexpr Duration WellPast = std::chrono::seconds{ 2 };

struct Fixture
{
	ManualClock Clock{ Start };
	SceneStore Store{ Clock };

	Fixture()
	{
		const SceneOutput outputs[] = { SceneOutput{
			.Bounds = { {}, { 1920.0, 1080.0 } }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } } };

		Store.SetOutputs(outputs);
	}

	// The clock moves with the loop, because a commit's origin is clamped to dispatch's own now — a gym
	// stamping an edge the store thinks is still in the future would have every retarget silently
	// pulled back, and every assertion about origins below would pass for the wrong reason.
	[[nodiscard]] Instant Reach(Duration after)
	{
		Clock.Set(Advanced(Start, after));

		return Clock.Now();
	}
};

// The tree, through the store's own accessors. A gym hands back no ids, so what a test can see of the
// scene is what a walk from the top level can reach — which is also what the serializer sees, and is
// therefore the right vantage for asking whether anything is moving.
template<typename Visit>
void Walk(const SceneStore& store, EntityId id, const Visit& visit)
{
	while (!id.IsNull())
	{
		const Entity* const entity = store.Find(id);

		if (entity == nullptr)
		{
			return;
		}

		visit(*entity);
		Walk(store, entity->FirstChild, visit);

		id = entity->NextSibling;
	}
}

// Whether anything in the scene owes frames, which is the frame side's question asked on the authoring
// side: a channel with a nonzero offset or velocity is one that has not finished.
[[nodiscard]] bool AnythingMoving(const SceneStore& store)
{
	bool moving = false;

	Walk(store, store.FirstRoot(), [&](const Entity& entity) {
		moving = moving || !entity.Translation.IsAtRest() || !entity.Scale.IsAtRest() || !entity.Opacity.IsAtRest() ||
		         !entity.Turn.IsAtRest();
	});

	return moving;
}
} // namespace

GYRO_TEST(Gym, EveryNameInTheVocabularyConstructs)
{
	GYRO_REQUIRE_EQ(GymNames().size(), GymCount);

	for (const std::string_view name : GymNames())
	{
		const Result<std::unique_ptr<IGym>> gym = MakeGym(name);

		GYRO_REQUIRE(gym);
		GYRO_REQUIRE(*gym != nullptr);

		// The gym answers with the name it was asked for, which is what lets a log line naming a gym be
		// trusted against the argument somebody typed.
		GYRO_CHECK_EQ((*gym)->Name(), name);
	}
}

GYRO_TEST(Gym, AnUnknownNameIsRefusedRatherThanDefaulted)
{
	const Result<std::unique_ptr<IGym>> gym = MakeGym("windows");

	GYRO_REQUIRE(!gym);
	GYRO_CHECK_EQ(gym.error().Code(), EINVAL);

	// Not the empty string either, which is what `--gym` with nothing after it parses to and is the one
	// spelling most likely to be quietly taken as *the default*.
	GYRO_CHECK(!MakeGym(""));
}

GYRO_TEST(Gym, EveryGymAuthorsAgainstAnOutputAndRefusesWithout)
{
	for (const std::string_view name : GymNames())
	{
		ManualClock clock{ Start };
		SceneStore bare{ clock };

		const Result<std::unique_ptr<IGym>> gym = MakeGym(name);

		GYRO_REQUIRE(gym);

		// The failure a gym is likeliest to meet in the field: constructed before the composition root
		// has an output set to hand it. Refused with a sentence rather than laid out against zero.
		GYRO_CHECK(!(*gym)->Open(bare));

		Fixture fixture;

		GYRO_CHECK((*gym)->Open(fixture.Store));
	}
}

GYRO_TEST(Gym, ThePerpetualGymsNeverAnswerSettled)
{
	for (const GymKind kind : { GymKind::Lanes, GymKind::Turn, GymKind::Materials })
	{
		Fixture fixture;

		const Result<std::unique_ptr<IGym>> gym = MakeGym(Name(kind));

		GYRO_REQUIRE(gym);
		GYRO_REQUIRE((*gym)->Open(fixture.Store));

		// Before the first edge there is nothing to do, and the answer is still a demand: the scene as
		// authored is what the first frame shows, and the gym is owed a wake at the instant it moves.
		const Wake first = (*gym)->Advance(fixture.Store, fixture.Clock.Now());

		GYRO_REQUIRE(first.Which == Wake::Kind::Timed);
		GYRO_CHECK(first.When > fixture.Clock.Now());
		GYRO_CHECK(!AnythingMoving(fixture.Store));

		// Past every lane's first edge. Something is now moving, and the gym is owed another wake — the
		// only way a scene keeps moving is that something keeps retargeting it.
		Instant now = fixture.Reach(WellPast);
		Wake next = (*gym)->Advance(fixture.Store, now);

		GYRO_REQUIRE(next.Which == Wake::Kind::Timed);
		GYRO_CHECK(next.When > now);
		GYRO_CHECK(AnythingMoving(fixture.Store));

		// And it keeps being owed one, for as long as anybody keeps asking. Core/Wake.h requires a timed
		// instant strictly after the instant it was computed for, or the loop above spins at full rate
		// on a wake it has already served — which is what this sweep is really for, since serving every
		// wake exactly on time is the schedule that produces the edge case.
		for (int served = 0; served < 32; ++served)
		{
			now = next.When;

			fixture.Clock.Set(now);

			next = (*gym)->Advance(fixture.Store, now);

			GYRO_REQUIRE(next.Which == Wake::Kind::Timed);
			GYRO_REQUIRE(next.When > now);
		}
	}
}

GYRO_TEST(Gym, TheSettlingGymAuthorsOnceAndThenAsksForNothing)
{
	Fixture fixture;

	const Result<std::unique_ptr<IGym>> gym = MakeGym(Name(GymKind::Settle));

	GYRO_REQUIRE(gym);
	GYRO_REQUIRE((*gym)->Open(fixture.Store));

	// The whole of what this gym ever writes happens in `Open`, so the scene is already in motion before
	// the loop has called it once. A gym that waited for its first `Advance` would make the settle
	// instant a function of when the loop got round to it.
	GYRO_CHECK(AnythingMoving(fixture.Store));

	// And nothing is ever owed. That is what the frame side's fold is read against: once the springs
	// have run down there is no contributor left, so no timer is armed and the frame thread blocks —
	// which is the promise that doing nothing costs nothing, and the only instrument here for it.
	GYRO_CHECK_EQ((*gym)->Advance(fixture.Store, fixture.Clock.Now()), Wake::Never());
	GYRO_CHECK_EQ((*gym)->Advance(fixture.Store, fixture.Reach(WellPast)), Wake::Never());
}

GYRO_TEST(Gym, ARetargetIsStampedWithTheInstantItFellDueRatherThanTheWakeItWasServedAt)
{
	Fixture fixture;

	const Result<std::unique_ptr<IGym>> gym = MakeGym(Name(GymKind::Lanes));

	GYRO_REQUIRE(gym);
	GYRO_REQUIRE((*gym)->Open(fixture.Store));

	// Served a whole second late, which is what a busy dispatch thread or a coarse timer produces.
	const Instant late = fixture.Reach(WellPast);

	GYRO_REQUIRE((*gym)->Advance(fixture.Store, late).Which == Wake::Kind::Timed);

	bool stamped = false;

	Walk(fixture.Store, fixture.Store.FirstRoot(), [&](const Entity& entity) {
		if (entity.Translation.IsAtRest())
		{
			return;
		}

		// The edge, not the wake. Decision 89's lateness argument read from the authoring side: a motion
		// stamped at the instant its event happened renders already in progress by exactly the elapsed
		// amount, so being served late costs the first frame or two and never the shape — and a gym that
		// stamped `now` would hide precisely the lateness it exists to expose.
		GYRO_CHECK(entity.Translation.Coefficients().Origin < late);

		stamped = true;
	});

	GYRO_CHECK(stamped);
}

GYRO_TEST(Gym, TheGymsThatWantAGpuSaySo)
{
	// Not a preference: `Blit::Classify` refuses a material, an elevation, and a quad that is not
	// axis-aligned, and a single refusal fails the whole `Record` — so under a CPU renderer these two
	// write no frames at all rather than frames with a node missing. A caller that has one bound owes
	// the person a sentence before the directory it is writing into stays empty.
	GYRO_CHECK(DrawsOnCpu(GymKind::Lanes));
	GYRO_CHECK(DrawsOnCpu(GymKind::Settle));
	GYRO_CHECK(!DrawsOnCpu(GymKind::Turn));
	GYRO_CHECK(!DrawsOnCpu(GymKind::Materials));

	// The name lookup and the vocabulary agree in both directions, which is what keeps the table above
	// reachable from an argument somebody typed.
	for (const GymKind kind : AllGyms)
	{
		GYRO_CHECK(GymNamed(Name(kind)) == kind);
	}

	GYRO_CHECK(!GymNamed("lane").has_value());
}
