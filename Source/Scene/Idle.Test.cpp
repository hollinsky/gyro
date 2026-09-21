#include "Scene/Idle.h"

#include <chrono>
#include <cstdint>

#include "Core/Clock.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Geometry/Scale.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Testing/Test.h"

// What a person notices about the display-off rung is three things, and they are what is asserted: the
// screens go dark after the time they were told and not before, touching anything before then pushes it
// back, and the first touch afterwards lights every screen and is the only event that says it did. The
// fourth is what nobody notices and the rung exists for — a dark machine arms nothing.

namespace
{
using namespace std::chrono_literals;

constexpr Instant At(std::int64_t seconds) noexcept
{
	return Monotonic::FromNanoseconds(seconds * 1'000'000'000);
}

ManualClock Clock;

// Two panels, because *every output goes dark together* is a claim about more than one.
void TwoPanels(SceneStore& store)
{
	const SceneOutput outputs[] = {
		SceneOutput{
			.Id = OutputId{ 1, 1 }, .Generation = 1, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } },
		SceneOutput{
			.Id = OutputId{ 2, 1 }, .Generation = 1, .Density = Scale::FromInteger(1), .Grid = { 2560, 1440 } },
	};

	store.SetOutputs(outputs);
}
} // namespace

GYRO_TEST(IdleLadder, ArmsTheInstantThePanelsWouldGoDarkAndNothingOnceTheyHave)
{
	SceneStore store{ Clock };
	TwoPanels(store);
	IdleLadder ladder;

	ladder.DisplayOffAfter(600s, At(100));

	// One instant, not a poll: a machine somebody is using costs exactly the one alarm it would go dark at.
	GYRO_CHECK(ladder.Step(store, At(100)) == Wake::At(At(700)));
	GYRO_CHECK(ladder.Step(store, At(699)) == Wake::At(At(700)));
	GYRO_CHECK(store.Outputs()[0].Powered);

	// And at that instant both go dark, and nothing is armed afterwards — Architecture.md's invariant
	// reached from the ladder's side.
	GYRO_CHECK(ladder.Step(store, At(700)) == Wake::Never());
	GYRO_CHECK(ladder.IsDark());
	GYRO_CHECK(!store.Outputs()[0].Powered);
	GYRO_CHECK(!store.Outputs()[1].Powered);
	GYRO_CHECK(ladder.Step(store, At(5000)) == Wake::Never());
}

GYRO_TEST(IdleLadder, TouchingAnythingPushesTheInstantBack)
{
	SceneStore store{ Clock };
	TwoPanels(store);
	IdleLadder ladder;

	ladder.DisplayOffAfter(600s, At(100));

	// Not a return from dark, so nothing to swallow.
	GYRO_CHECK(!ladder.Touch(store, At(650)));
	GYRO_CHECK(ladder.Step(store, At(700)) == Wake::At(At(1250)));
	GYRO_CHECK(store.Outputs()[0].Powered);

	// An event stamped before the latest one does not pull the instant forward: the seat's devices are
	// not promised to arrive in the order they happened.
	GYRO_CHECK(!ladder.Touch(store, At(400)));
	GYRO_CHECK(ladder.Step(store, At(700)) == Wake::At(At(1250)));
}

GYRO_TEST(IdleLadder, TheFirstTouchInTheDarkLightsEveryPanelAndIsTheOnlyOneThatSaysSo)
{
	SceneStore store{ Clock };
	TwoPanels(store);
	IdleLadder ladder;

	ladder.DisplayOffAfter(600s, At(0));
	GYRO_REQUIRE(ladder.Step(store, At(600)) == Wake::Never());

	GYRO_CHECK(ladder.Touch(store, At(900)));
	GYRO_CHECK(store.Outputs()[0].Powered);
	GYRO_CHECK(store.Outputs()[1].Powered);

	// The release of the key that woke them, a second later, is ordinary activity: it is the root that
	// pairs it with its press, and the ladder only reports the return.
	GYRO_CHECK(!ladder.Touch(store, At(901)));

	// And the count starts again from the last touch.
	GYRO_CHECK(ladder.Step(store, At(902)) == Wake::At(At(1501)));
}

// The generation is the request, so it moves once per change and never for a repeat. Four moves for
// off, on, off, on — and none for being asked for what an output already is.
GYRO_TEST(IdleLadder, EachChangeOfPowerIsOneGenerationAndARepeatIsNone)
{
	SceneStore store{ Clock };
	TwoPanels(store);
	const OutputId first = store.Outputs()[0].Id;

	store.SetOutputPower(first, true);
	GYRO_CHECK_EQ(store.Outputs()[0].Generation, std::uint64_t{ 1 });

	store.SetOutputPower(first, false);
	store.SetOutputPower(first, false);
	GYRO_CHECK_EQ(store.Outputs()[0].Generation, std::uint64_t{ 2 });

	store.SetOutputPower(first, true);
	GYRO_CHECK_EQ(store.Outputs()[0].Generation, std::uint64_t{ 3 });

	// The other panel was not asked, and an output that is not here is not an error.
	GYRO_CHECK_EQ(store.Outputs()[1].Generation, std::uint64_t{ 1 });
	store.SetOutputPower(OutputId{ 9, 1 }, false);
}

GYRO_TEST(IdleLadder, AMachineThatWasNotAskedNeverGoesDark)
{
	SceneStore store{ Clock };
	TwoPanels(store);
	IdleLadder ladder;

	GYRO_CHECK(ladder.Step(store, At(1'000'000)) == Wake::Never());
	GYRO_CHECK(!ladder.Touch(store, At(1'000'001)));
	GYRO_CHECK(store.Outputs()[0].Powered);
	GYRO_CHECK_EQ(store.Outputs()[0].Generation, std::uint64_t{ 1 });
}
