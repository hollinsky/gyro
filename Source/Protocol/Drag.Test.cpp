#include "Protocol/Drag.h"

#include <array>
#include <optional>

#include "Animation/Author/Bundle.h"
#include "Animation/Author/Motion.h"
#include "Core/Clock.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Scene/Commit.h"
#include "Scene/Entity.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Testing/Test.h"
#include "World/Content.h"

// The drag against the store, which is where the whole of it is: what a client sent and what the seat
// checked before starting one are `Seat.h`'s, and the wire half is
// Integration/ProtocolRoundTrip.Test.cpp's. What is here is where the window ends up.

namespace
{
constexpr double PanelWidth = 1920.0;
constexpr double PanelHeight = 1080.0;

[[nodiscard]] SceneOutput Panel()
{
	return { .Bounds = { { 0.0, 0.0 }, { PanelWidth, PanelHeight } },
		     .Density = Scale::FromInteger(1),
		     .Grid = { 1920, 1080 } };
}

// A window where the shell would have put one: a container carrying the placement with the client's
// pixels beneath it, which is decision 111's toplevel and the shape a drag moves.
[[nodiscard]] EntityId Window(SceneStore& scene, Vector3<double> at)
{
	const EntityId frame = scene.CreateContainer(EntityId{}, { .Position = at, .Extent = { 400.0F, 300.0F } }).value();

	static_cast<void>(scene.CreateImage(frame, { .Extent = { 400.0F, 300.0F } }, ImageContent{}));

	return frame;
}

[[nodiscard]] Vector3<double> Where(const SceneStore& scene, EntityId window)
{
	return scene.Find(window)->Translation.Model();
}

// Put the pointer somewhere, the way a device would. `WarpTo` clamps to the outputs, which is the
// pointer's own confinement and deliberately not the window's.
void PointAt(SceneStore& scene, Point<GlobalSpace> at, std::span<const SceneOutput> outputs)
{
	static_cast<void>(scene.Pointer().WarpTo(at, outputs));
}
} // namespace

// The claim decision 51 rests on: the window travels exactly as far as the hand did, with nothing in
// between. Two steps rather than one, because a drag that applied the same displacement twice and a
// drag that tracked an anchor read identically after a single move.
GYRO_TEST(Drag, TheWindowFollowsThePointerByTheDistanceThePointerTravelled)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	const EntityId window = Window(scene, { 300.0, 200.0, 0.0 });

	PointAt(scene, { 400.0, 250.0 }, outputs);

	WindowDrag drag;
	GYRO_REQUIRE(drag.BeginMove(scene, window));

	PointAt(scene, { 450.0, 280.0 }, outputs);
	drag.Track(scene, scene.Now());

	GYRO_CHECK_EQ(Where(scene, window).X, 350.0);
	GYRO_CHECK_EQ(Where(scene, window).Y, 230.0);

	PointAt(scene, { 500.0, 200.0 }, outputs);
	drag.Track(scene, scene.Now());

	GYRO_CHECK_EQ(Where(scene, window).X, 400.0);
	GYRO_CHECK_EQ(Where(scene, window).Y, 150.0);
}

// The anchor is what makes that true over a long gesture: an iteration where the hand did not move
// writes the position it already had, rather than accumulating whatever the last displacement was.
GYRO_TEST(Drag, AnIterationWithNoMotionLeavesTheWindowWhereItIs)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	const EntityId window = Window(scene, { 300.0, 200.0, 0.0 });

	PointAt(scene, { 400.0, 250.0 }, outputs);

	WindowDrag drag;
	GYRO_REQUIRE(drag.BeginMove(scene, window));

	PointAt(scene, { 420.0, 250.0 }, outputs);
	drag.Track(scene, scene.Now());
	drag.Track(scene, scene.Now());
	drag.Track(scene, scene.Now());

	GYRO_CHECK_EQ(Where(scene, window).X, 320.0);
	GYRO_CHECK_EQ(Where(scene, window).Y, 200.0);
}

// Immediate rather than sprung. A window a person is dragging must be *at* the position on the frame
// it is written, or the thing being dragged separates from the thing doing the dragging.
GYRO_TEST(Drag, TheWindowIsAtThePointerRatherThanTravellingTowardsIt)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	const EntityId window = Window(scene, { 300.0, 200.0, 0.0 });

	PointAt(scene, { 400.0, 250.0 }, outputs);

	WindowDrag drag;
	GYRO_REQUIRE(drag.BeginMove(scene, window));

	PointAt(scene, { 900.0, 250.0 }, outputs);
	drag.Track(scene, scene.Now());

	// At rest at the target rather than heading for it, on the same instant the write happened.
	GYRO_CHECK(scene.Find(window)->Translation.IsAtRest());
	GYRO_CHECK_EQ(scene.Find(window)->Translation.Presentation(scene.Now()).X, 800.0);
}

// A window grabbed while it is still moving starts from where it *is*. Anchoring on the model value
// would make the first frame of every interrupted drag a jump to wherever the window was heading.
GYRO_TEST(Drag, AWindowGrabbedMidFlightIsAnchoredWhereItIsSeen)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	const EntityId window = Window(scene, { 0.0, 0.0, 0.0 });

	{
		SceneCommit commit{ scene, CommitAuthor::Compositor, scene.Now() };

		GYRO_REQUIRE(commit.Move(window, { 800.0, 0.0, 0.0 }, Animate(Motion::Standard)));
	}

	// A few milliseconds in, which is where a person's hand lands on a window that is still opening.
	clock.Advance(Duration{ 8'000'000 });

	const double seen = scene.Find(window)->Translation.Presentation(scene.Now()).X;

	GYRO_REQUIRE(seen > 0.0);
	GYRO_REQUIRE(seen < 800.0);

	PointAt(scene, { 400.0, 400.0 }, outputs);

	WindowDrag drag;
	GYRO_REQUIRE(drag.BeginMove(scene, window));

	// The hand has not moved yet, so the window must be exactly where it was drawn — not at 800.
	drag.Track(scene, scene.Now());

	GYRO_CHECK_EQ(Where(scene, window).X, seen);
}

// Decision 51 has the shell declaring the edges, and there is no shell — so nothing here clamps. A
// window dragged past the corner of a screen goes there, and a person gets it back the way they sent
// it. The pointer is confined and the window is not, which is the distinction being asserted.
GYRO_TEST(Drag, NothingKeepsTheWindowOnTheScreen)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	const EntityId window = Window(scene, { 0.0, 0.0, 0.0 });

	PointAt(scene, { 200.0, 200.0 }, outputs);

	WindowDrag drag;
	GYRO_REQUIRE(drag.BeginMove(scene, window));

	PointAt(scene, { 0.0, 0.0 }, outputs);
	drag.Track(scene, scene.Now());

	GYRO_CHECK_EQ(Where(scene, window).X, -200.0);
	GYRO_CHECK_EQ(Where(scene, window).Y, -200.0);
}

// The button coming up, which is the only thing that ends a drag on purpose.
GYRO_TEST(Drag, LettingGoLeavesTheWindowWhereItWasDropped)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	const EntityId window = Window(scene, { 300.0, 200.0, 0.0 });

	PointAt(scene, { 400.0, 250.0 }, outputs);

	WindowDrag drag;
	GYRO_REQUIRE(drag.BeginMove(scene, window));

	PointAt(scene, { 500.0, 250.0 }, outputs);
	drag.Track(scene, scene.Now());

	drag.End();
	GYRO_CHECK(!drag.IsActive());

	// The pointer keeps travelling and the window does not follow it, which is the whole of what
	// releasing means: no snap back and nothing to confirm, because the window was already there.
	PointAt(scene, { 900.0, 700.0 }, outputs);
	drag.Track(scene, scene.Now());

	GYRO_CHECK_EQ(Where(scene, window).X, 400.0);
	GYRO_CHECK_EQ(Where(scene, window).Y, 200.0);
}

// The window leaving under a person's hand: the client destroyed the toplevel, or exited. The drag
// ends itself, because the id it holds is the one thing that can notice.
GYRO_TEST(Drag, AWindowThatCloses)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	const EntityId window = Window(scene, { 300.0, 200.0, 0.0 });

	PointAt(scene, { 400.0, 250.0 }, outputs);

	WindowDrag drag;
	GYRO_REQUIRE(drag.BeginMove(scene, window));

	// Retired rather than destroyed, which is the path an unmap takes (114): the window is still in the
	// store, still on screen, and playing out whatever exit it is owed. Dragging it would be moving
	// something a person can no longer drop.
	{
		SceneCommit unmap{ scene, CommitAuthor::Client };

		GYRO_REQUIRE(unmap.Retire(window));
	}

	PointAt(scene, { 900.0, 700.0 }, outputs);
	drag.Track(scene, scene.Now());

	GYRO_CHECK(!drag.IsActive());
	GYRO_CHECK_EQ(Where(scene, window).X, 300.0);
}

// And the same fact asked at the other end: a client cannot start a drag on a window that has already
// gone, which is an ordinary race rather than a misbehaving client — the request was sent before the
// window closed and arrived after.
GYRO_TEST(Drag, ADragCannotStartOnAWindowThatIsNotThere)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	const EntityId window = Window(scene, { 300.0, 200.0, 0.0 });

	WindowDrag drag;

	GYRO_CHECK(!drag.BeginMove(scene, EntityId{}));

	{
		SceneCommit unmap{ scene, CommitAuthor::Client };

		GYRO_REQUIRE(unmap.Retire(window));
	}

	GYRO_CHECK(!drag.BeginMove(scene, window));
	GYRO_CHECK(!drag.IsActive());
}

// Decision 166's first half: a resize asks and does not write. The extent belongs to the client, so
// the world does not change until it has drawn one — and a test that only checked the size asked for
// would pass just as happily on a compositor that resized the window itself.
GYRO_TEST(Drag, AResizeAsksForASizeAndChangesNothingInTheWorld)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	const EntityId window = Window(scene, { 300.0, 200.0, 0.0 });

	PointAt(scene, { 700.0, 500.0 }, outputs);

	WindowDrag drag;
	GYRO_REQUIRE(drag.BeginResize(scene, window, { .Right = true }));

	PointAt(scene, { 760.0, 500.0 }, outputs);
	drag.Track(scene, scene.Now());

	GYRO_CHECK_EQ(drag.Wanted().Width, 460.0F);
	GYRO_CHECK_EQ(drag.Wanted().Height, 300.0F);

	// The window is exactly as it was: same place, same size.
	GYRO_CHECK_EQ(Where(scene, window).X, 300.0);
	GYRO_CHECK_EQ(scene.Find(window)->Extent.Width, 400.0F);
}

// Pulling the far edges grows the window without moving its origin, which is the case that would
// still look right if `Anchored` were wrong — hence the one below it.
GYRO_TEST(Drag, PullingTheRightAndBottomEdgesHoldsTheOrigin)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	const EntityId window = Window(scene, { 300.0, 200.0, 0.0 });

	PointAt(scene, { 700.0, 500.0 }, outputs);

	WindowDrag drag;
	GYRO_REQUIRE(drag.BeginResize(scene, window, { .Right = true, .Bottom = true }));

	PointAt(scene, { 750.0, 540.0 }, outputs);
	drag.Track(scene, scene.Now());

	GYRO_CHECK_EQ(drag.Wanted().Width, 450.0F);
	GYRO_CHECK_EQ(drag.Wanted().Height, 340.0F);

	const Vector3<double> anchored = drag.Anchored({ 450.0F, 340.0F });

	GYRO_CHECK_EQ(anchored.X, 300.0);
	GYRO_CHECK_EQ(anchored.Y, 200.0);
}

// And the near edges, which grow the window in the other direction and move the origin to keep the far
// edge still. A person pulling the left edge left expects the right edge not to move.
GYRO_TEST(Drag, PullingTheLeftAndTopEdgesMovesTheOriginAndHoldsTheFarEdge)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	// Right edge at 700, bottom edge at 500.
	const EntityId window = Window(scene, { 300.0, 200.0, 0.0 });

	PointAt(scene, { 300.0, 200.0 }, outputs);

	WindowDrag drag;
	GYRO_REQUIRE(drag.BeginResize(scene, window, { .Left = true, .Top = true }));

	PointAt(scene, { 250.0, 170.0 }, outputs);
	drag.Track(scene, scene.Now());

	GYRO_CHECK_EQ(drag.Wanted().Width, 450.0F);
	GYRO_CHECK_EQ(drag.Wanted().Height, 330.0F);

	const Vector3<double> anchored = drag.Anchored(drag.Wanted());

	GYRO_CHECK_EQ(anchored.X, 250.0);
	GYRO_CHECK_EQ(anchored.Y, 170.0);

	// Stated as the invariant rather than as the coordinate, because the coordinate is the thing that
	// would drift and the invariant is what a person is looking at.
	GYRO_CHECK_EQ(anchored.X + static_cast<double>(drag.Wanted().Width), 700.0);
	GYRO_CHECK_EQ(anchored.Y + static_cast<double>(drag.Wanted().Height), 500.0);
}

// **The claim decision 166 exists for.** A client is free to come back with a size other than the one
// it was asked for — its own increment, its own minimum, or its own opinion — and the window has to be
// positioned from what arrived. Anchoring on the request instead is the shimmer a person sees on the
// edge they are *not* holding.
GYRO_TEST(Drag, TheOriginFollowsTheSizeThatArrivedRatherThanTheSizeThatWasAsked)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	const EntityId window = Window(scene, { 300.0, 200.0, 0.0 });

	PointAt(scene, { 300.0, 200.0 }, outputs);

	WindowDrag drag;
	GYRO_REQUIRE(drag.BeginResize(scene, window, { .Left = true }));

	PointAt(scene, { 250.0, 200.0 }, outputs);
	drag.Track(scene, scene.Now());

	GYRO_REQUIRE(drag.Wanted().Width == 450.0F);

	// A terminal that only grows by whole character cells and rounded down to 440.
	const Vector3<double> anchored = drag.Anchored({ 440.0F, 300.0F });

	// The right edge is still at 700 — not 690, which is what anchoring on the request would give.
	GYRO_CHECK_EQ(anchored.X, 260.0);
	GYRO_CHECK_EQ(anchored.X + 440.0, 700.0);
}

// A person who drags an edge clean past the far side of the window. Zero on the wire means *pick your
// own size*, which would hand the client back the freedom the gesture is taking away, so the ask
// bottoms out at one and stays there.
GYRO_TEST(Drag, AnEdgeDraggedPastTheOppositeOneAsksForTheSmallestWindowRatherThanForNone)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	const EntityId window = Window(scene, { 300.0, 200.0, 0.0 });

	PointAt(scene, { 700.0, 500.0 }, outputs);

	WindowDrag drag;
	GYRO_REQUIRE(drag.BeginResize(scene, window, { .Right = true, .Bottom = true }));

	PointAt(scene, { 100.0, 100.0 }, outputs);
	drag.Track(scene, scene.Now());

	GYRO_CHECK_EQ(drag.Wanted().Width, 1.0F);
	GYRO_CHECK_EQ(drag.Wanted().Height, 1.0F);
}

// `xdg_toplevel.resize` with `none`, which is legal on the wire and is a gesture with no direction to
// run in. Refused rather than started, so the pointer is not taken away from the client for nothing.
GYRO_TEST(Drag, AResizeWithNoEdgesIsRefused)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	const EntityId window = Window(scene, { 300.0, 200.0, 0.0 });

	PointAt(scene, { 700.0, 500.0 }, outputs);

	WindowDrag drag;

	GYRO_CHECK(!drag.BeginResize(scene, window, ResizeEdges{}));
	GYRO_CHECK(!drag.IsActive());
	GYRO_CHECK(!drag.IsResizing());
}

// A move is not a resize, which the shell asks once per window per iteration and would otherwise
// configure every window on the machine while somebody drags one of them by its titlebar.
GYRO_TEST(Drag, AMoveIsNotAResize)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	const EntityId window = Window(scene, { 300.0, 200.0, 0.0 });

	PointAt(scene, { 700.0, 500.0 }, outputs);

	WindowDrag drag;
	GYRO_REQUIRE(drag.BeginMove(scene, window));

	GYRO_CHECK(drag.IsActive());
	GYRO_CHECK(!drag.IsResizing());

	drag.End();
	GYRO_CHECK(!drag.IsResizing());
}

// The window leaving under a resize, which ends it exactly as it ends a move — the id is the one thing
// that can notice, and both halves look it up in the same place.
GYRO_TEST(Drag, AResizeEndsWhenTheWindowDoes)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	const EntityId window = Window(scene, { 300.0, 200.0, 0.0 });

	PointAt(scene, { 700.0, 500.0 }, outputs);

	WindowDrag drag;
	GYRO_REQUIRE(drag.BeginResize(scene, window, { .Right = true }));

	{
		SceneCommit unmap{ scene, CommitAuthor::Client };

		GYRO_REQUIRE(unmap.Retire(window));
	}

	drag.Track(scene, scene.Now());

	GYRO_CHECK(!drag.IsActive());
	GYRO_CHECK(!drag.IsResizing());
}
