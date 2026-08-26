#include "Scene/Pointer.h"

#include <vector>

#include "Core/Clock.h"
#include "Geometry/Space.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Testing/Test.h"

// Confinement is the whole of what this class decides, and Docs/Experience.md's promise that the
// pointer reaches every part of the screen is what the awkward cases below are about.

namespace
{
ManualClock Clock;

SceneOutput Panel(std::uint32_t index, double x, double y, double width, double height)
{
	return SceneOutput{
		.Id = OutputId{ index, 1 },
		.Bounds = { { x, y }, { width, height } },
		.Density = Scale::FromInteger(1),
		.Grid = { static_cast<std::int32_t>(width), static_cast<std::int32_t>(height) },
	};
}
} // namespace

GYRO_TEST(ScenePointer, ThereIsNoCursorUntilSomethingWithOneMoves)
{
	ScenePointer pointer;

	GYRO_CHECK(!pointer.IsVisible());

	const std::vector<SceneOutput> outputs{ Panel(0, 0, 0, 1920, 1080) };
	pointer.Move({ 4.0, 4.0 }, outputs);

	GYRO_CHECK(pointer.IsVisible());
}

GYRO_TEST(ScenePointer, MotionAccumulatesBelowAPixelInsteadOfRoundingToNothing)
{
	ScenePointer pointer;
	const std::vector<SceneOutput> outputs{ Panel(0, 0, 0, 1920, 1080) };

	for (int step = 0; step < 4; ++step)
	{
		pointer.Move({ 0.25, 0.0 }, outputs);
	}

	// A pointer that rounded each step would still be at zero, which is the stick-and-jump this is here
	// to forbid.
	GYRO_CHECK_EQ(pointer.Position().X, 1.0);
}

GYRO_TEST(ScenePointer, ThePointerStopsAtTheEdgeAndStaysOnTheOutput)
{
	ScenePointer pointer;
	const std::vector<SceneOutput> outputs{ Panel(0, 0, 0, 1920, 1080) };

	pointer.Move({ 5000.0, 5000.0 }, outputs);

	GYRO_CHECK(pointer.Position().X < 1920.0);
	GYRO_CHECK(pointer.Position().Y < 1080.0);

	// Not merely near the edge: still somewhere `On` can name, which is what the exclusive-edge step in
	// `Nearest` exists for.
	GYRO_CHECK(pointer.On(outputs) == outputs.front().Id);
}

GYRO_TEST(ScenePointer, ADiagonalPushAlongAnEdgeKeepsTravelling)
{
	ScenePointer pointer;
	const std::vector<SceneOutput> outputs{ Panel(0, 0, 0, 1920, 1080) };

	pointer.WarpTo({ 100.0, 0.0 }, outputs);

	// Up and to the right along the top edge. The upward half is impossible and the rightward half is
	// not, and a pointer that refused the whole displacement would jam here.
	pointer.Move({ 50.0, -50.0 }, outputs);

	GYRO_CHECK_EQ(pointer.Position().X, 150.0);
	GYRO_CHECK_EQ(pointer.Position().Y, 0.0);
}

GYRO_TEST(ScenePointer, TheGapBesideAShorterMonitorIsNotSomewhereThePointerCanGo)
{
	ScenePointer pointer;

	// A tall panel on the left and a short one on the right, tops aligned: the union is an L and the
	// corner below the short one is empty. Clamping each axis to the bounding box independently would
	// put the pointer in that corner, on no output at all.
	const std::vector<SceneOutput> outputs{ Panel(0, 0, 0, 1920, 1200), Panel(1, 1920, 0, 1920, 1080) };

	pointer.WarpTo({ 1000.0, 1150.0 }, outputs);
	pointer.Move({ 2000.0, 0.0 }, outputs);

	GYRO_CHECK(!pointer.On(outputs).IsNull());

	// It kept its row on the tall panel rather than sliding down into the second one, because the
	// horizontal push had nowhere legal to land and the clamp is to the nearest point on the union.
	GYRO_CHECK_EQ(pointer.Position().Y, 1150.0);
	GYRO_CHECK(pointer.On(outputs) == outputs.front().Id);
}

GYRO_TEST(ScenePointer, TwoPanelsAbuttingPutTheSeamOnExactlyOneOfThem)
{
	ScenePointer pointer;
	const std::vector<SceneOutput> outputs{ Panel(0, 0, 0, 1920, 1080), Panel(1, 1920, 0, 1920, 1080) };

	pointer.WarpTo({ 1920.0, 500.0 }, outputs);

	GYRO_CHECK(pointer.On(outputs) == outputs.back().Id);
}

GYRO_TEST(ScenePointer, UnpluggingTheMonitorUnderneathBringsThePointerBack)
{
	ScenePointer pointer;
	const std::vector<SceneOutput> both{ Panel(0, 0, 0, 1920, 1080), Panel(1, 1920, 0, 1920, 1080) };

	pointer.WarpTo({ 3000.0, 500.0 }, both);
	GYRO_CHECK(pointer.On(both) == both.back().Id);

	const std::vector<SceneOutput> one{ Panel(0, 0, 0, 1920, 1080) };
	pointer.Reconfine(one);

	// Left where it was, it would be off the side of a display that is no longer plugged in, and every
	// subsequent motion would slide along a union it is not touching.
	GYRO_CHECK(pointer.On(one) == one.front().Id);
	GYRO_CHECK_EQ(pointer.Position().Y, 500.0);
}

GYRO_TEST(ScenePointer, MotionIsStillTakenWhenThereIsNoOutputToBeConfinedTo)
{
	ScenePointer pointer;
	const std::vector<SceneOutput> none;

	pointer.Move({ 10.0, 10.0 }, none);

	GYRO_CHECK_EQ(pointer.Position().X, 10.0);
	GYRO_CHECK(pointer.On(none).IsNull());
}

GYRO_TEST(ScenePointer, TwoMiceMoveOneCursorAndNotTwo)
{
	ScenePointer pointer;
	const std::vector<SceneOutput> outputs{ Panel(0, 0, 0, 1920, 1080) };

	pointer.WarpTo({ 100.0, 100.0 }, outputs);

	// One drain, two devices, a push each. A seat has one cursor, so the pushes compose rather than
	// contending — which is what makes the running total the right representation and a per-device
	// position the wrong one.
	pointer.Move({ 10.0, 0.0 }, outputs);
	pointer.Move({ 0.0, 5.0 }, outputs);

	GYRO_CHECK_EQ(pointer.Position().X, 110.0);
	GYRO_CHECK_EQ(pointer.Position().Y, 105.0);
}

GYRO_TEST(ScenePointer, TheStoreReconfinesWhenTheOutputsChangeUnderneath)
{
	SceneStore store{ Clock };

	const std::vector<SceneOutput> both{ Panel(0, 0, 0, 1920, 1080), Panel(1, 1920, 0, 1920, 1080) };
	store.SetOutputs(both);
	store.Pointer().WarpTo({ 3000.0, 500.0 }, store.Outputs());

	// Nobody has to remember to call `Reconfine`, which is the point of it being here: unplugging a
	// monitor is not a moment anyone thinks about the pointer.
	const std::vector<SceneOutput> one{ Panel(0, 0, 0, 1920, 1080) };
	store.SetOutputs(one);

	GYRO_CHECK(store.Pointer().On(store.Outputs()) == one.front().Id);
}
