#include "Scene/Hit.h"

#include <array>
#include <optional>

#include "Core/Clock.h"
#include "Core/Handle.h"
#include "Core/Session.h"
#include "Geometry/Scale.h"
#include "Geometry/Shape.h"
#include "Geometry/Space.h"
#include "Scene/Commit.h"
#include "Scene/Output.h"
#include "Scene/Serializer.h"
#include "Scene/Store.h"
#include "Testing/Test.h"
#include "World/Node.h"

// What is under the pointer. Every claim here fails as something a person does with a mouse rather
// than as an arithmetic slip: a click that lands on the window behind the one being pointed at, a
// window that has closed still swallowing clicks, a dead strip beside a window that is not there.
//
// The pointer's own position is `Scene/Pointer.h`'s and is not exercised here — what this file is
// about is the walk and what it passes through, so every case states a global coordinate directly.

namespace
{
ManualClock Clock;

// A window as decision 141's floorplanner builds one: a container holding an image, with the image
// being the thing a client's `wl_surface` is and therefore the thing that accepts the pointer. The
// container accepts nothing, which is the arrangement the default is chosen for.
struct Window
{
	EntityId Frame;
	EntityId Surface;
};

[[nodiscard]] Window
Open(SceneStore& store, double x, double y, float width, float height, std::optional<SurfaceShape> shape = {})
{
	const EntityId frame =
		store.CreateContainer({}, { .Position = { x, y, 0.0 }, .Extent = { width, height } }).value();
	const EntityId surface = store.CreateImage(frame, { .Extent = { width, height } }, ImageContent{}).value();

	SceneCommit commit{ store, CommitAuthor::Client };
	commit.AcceptInput(surface, std::move(shape));

	return { .Frame = frame, .Surface = surface };
}

[[nodiscard]] Point<GlobalSpace> At(double x, double y)
{
	return { x, y };
}

[[nodiscard]] SurfaceShape Box(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
	SurfaceShape shape;
	shape.Add({ { x, y }, { width, height } });

	return shape;
}
} // namespace

GYRO_TEST(Hit, TheSurfaceUnderThePointerIsFoundInItsOwnCoordinates)
{
	SceneStore store{ Clock };
	const Window window = Open(store, 100.0, 50.0, 400.0F, 300.0F);

	const SceneHit hit = HitTest(store, At(150.0, 80.0));

	GYRO_CHECK(hit.Node == window.Surface);
	GYRO_CHECK(hit.Local == (Point<SurfaceSpace>{ 50.0F, 30.0F }));
}

GYRO_TEST(Hit, NothingIsUnderThePointerOverTheBackground)
{
	SceneStore store{ Clock };
	const Window window = Open(store, 100.0, 50.0, 400.0F, 300.0F);

	GYRO_CHECK(store.InputFor(window.Surface) != nullptr);

	GYRO_CHECK(!HitTest(store, At(50.0, 20.0)));
	GYRO_CHECK(!HitTest(store, At(600.0, 400.0)));
}

GYRO_TEST(Hit, TheQuadIsHalfOpenSoAnEdgeBelongsToOneWindow)
{
	// Two windows meeting exactly at x = 500. A pixel that answered for both is a click that lands on
	// whichever one the walk happened to visit second, which is the kind of bug that reproduces once a
	// week and never in front of anyone.
	SceneStore store{ Clock };
	const Window left = Open(store, 100.0, 0.0, 400.0F, 300.0F);
	const Window right = Open(store, 500.0, 0.0, 400.0F, 300.0F);

	GYRO_CHECK(HitTest(store, At(499.5, 10.0)).Node == left.Surface);
	GYRO_CHECK(HitTest(store, At(500.0, 10.0)).Node == right.Surface);
	GYRO_CHECK(!HitTest(store, At(99.5, 10.0)));
}

GYRO_TEST(Hit, TheFrontmostWindowTakesThePointer)
{
	// Decision 55 makes the sibling list the z order with the last root frontmost, and this is the whole
	// of what "the walk is the draw order" has to mean: the window a person can see is the window they
	// click on.
	SceneStore store{ Clock };
	const Window under = Open(store, 0.0, 0.0, 400.0F, 300.0F);
	const Window over = Open(store, 100.0, 100.0, 400.0F, 300.0F);

	GYRO_CHECK(HitTest(store, At(150.0, 150.0)).Node == over.Surface);

	// And where they do not overlap, the one underneath still answers.
	GYRO_CHECK(HitTest(store, At(50.0, 50.0)).Node == under.Surface);
}

GYRO_TEST(Hit, AContainerAcceptsNothingAndThePointerReachesItsChild)
{
	// The container is the window as far as decision 141 is concerned and it covers exactly the same
	// area as the surface inside it. If the default ran the other way this test would return the frame,
	// and `Protocol` would have no `wl_surface` to name in a `wl_pointer.enter`.
	SceneStore store{ Clock };
	const Window window = Open(store, 0.0, 0.0, 400.0F, 300.0F);

	const SceneHit hit = HitTest(store, At(10.0, 10.0));

	GYRO_CHECK(hit.Node == window.Surface);
	GYRO_CHECK(hit.Node != window.Frame);
}

GYRO_TEST(Hit, AShapeNarrowsTheExtentAndNeverWidensIt)
{
	// A rounded corner as every toolkit states one: the surface is the full rectangle and the region
	// cuts the corners out. Clicking the corner has to reach whatever is behind the window, because
	// there is nothing drawn there.
	SceneStore store{ Clock };
	const Window window = Open(store, 0.0, 0.0, 400.0F, 300.0F, Box(20, 20, 360, 260));

	GYRO_CHECK(HitTest(store, At(200.0, 150.0)).Node == window.Surface);
	GYRO_CHECK(!HitTest(store, At(5.0, 5.0)));

	// A shape reaching past the surface is ignored where it does, which is what the protocol says
	// happens rather than a policy of gyro's.
	const Window wide = Open(store, 1000.0, 0.0, 100.0F, 100.0F, Box(-500, -500, 2000, 2000));

	GYRO_CHECK(HitTest(store, At(1050.0, 50.0)).Node == wide.Surface);
	GYRO_CHECK(!HitTest(store, At(900.0, 50.0)));
}

GYRO_TEST(Hit, AnEmptyShapeIsClickThroughAndIsNotAnAbsentOne)
{
	// The distinction `Scene/Input.h` carries, as the two behaviours it produces. An overlay that states
	// an empty region wants every click to pass to what is under it; a surface that states no region at
	// all wants all of them.
	SceneStore store{ Clock };
	const Window under = Open(store, 0.0, 0.0, 400.0F, 300.0F);
	const Window overlay = Open(store, 0.0, 0.0, 400.0F, 300.0F, SurfaceShape{});

	GYRO_CHECK(HitTest(store, At(200.0, 150.0)).Node == under.Surface);

	// The same node, told to accept everything, takes the point back.
	{
		SceneCommit commit{ store, CommitAuthor::Client };
		commit.AcceptInput(overlay.Surface, std::nullopt);
	}

	GYRO_CHECK(HitTest(store, At(200.0, 150.0)).Node == overlay.Surface);
}

GYRO_TEST(Hit, AClosingWindowStopsTakingThePointerBeforeItStopsBeingDrawn)
{
	// Decision 114's two-step lifetime, on the reader `Entity.h` names. The window is still in the tree
	// and still published for as long as its exit runs; clicking the ghost of an application that has
	// exited is worse than clicking through to what is behind it.
	SceneStore store{ Clock };
	const Window under = Open(store, 0.0, 0.0, 400.0F, 300.0F);
	const Window closing = Open(store, 0.0, 0.0, 400.0F, 300.0F);

	GYRO_CHECK(HitTest(store, At(200.0, 150.0)).Node == closing.Surface);

	{
		SceneCommit commit{ store, CommitAuthor::Client };
		commit.Retire(closing.Frame);
	}

	GYRO_CHECK(store.Find(closing.Surface) != nullptr);
	GYRO_CHECK(HitTest(store, At(200.0, 150.0)).Node == under.Surface);
}

GYRO_TEST(Hit, AHiddenSubtreeTakesNothing)
{
	// `Node::Hidden` skips the subtree in the frame walk, so a window on an inactive workspace draws
	// nothing — and a hit test that disagreed would route clicks to a workspace nobody is looking at.
	SceneStore store{ Clock };
	const EntityId workspace = store.CreateContainer({}, { .Flags = Node::Hidden }).value();
	const EntityId surface = store.CreateImage(workspace, { .Extent = { 400.0F, 300.0F } }, ImageContent{}).value();

	{
		SceneCommit commit{ store, CommitAuthor::Client };
		commit.AcceptInput(surface, std::nullopt);
	}

	GYRO_CHECK(!HitTest(store, At(200.0, 150.0)));
}

GYRO_TEST(Hit, ThePointerFollowsAWindowThatHasBeenMoved)
{
	// The chain is composed rather than read off a stored rectangle, which is what makes this free: a
	// window under a spring is somewhere different every frame and nothing has to be told.
	SceneStore store{ Clock };
	const Window window = Open(store, 0.0, 0.0, 400.0F, 300.0F);

	{
		SceneCommit commit{ store, CommitAuthor::Client };
		commit.Move(window.Frame, { 600.0, 400.0, 0.0 }, Immediate());
	}

	GYRO_CHECK(!HitTest(store, At(100.0, 100.0)));
	GYRO_CHECK(HitTest(store, At(700.0, 500.0)).Node == window.Surface);
	GYRO_CHECK(HitTest(store, At(700.0, 500.0)).Local == (Point<SurfaceSpace>{ 100.0F, 100.0F }));
}

GYRO_TEST(Hit, ASlotThatComesBackDoesNotInheritWhatItAccepted)
{
	// The failure this prevents has no visible cause: a window closes, a new one is minted into its
	// index, and clicks land on it in a shape a program that has already exited chose.
	SceneStore store{ Clock };
	const Window first = Open(store, 0.0, 0.0, 400.0F, 300.0F);

	{
		SceneCommit commit{ store, CommitAuthor::Client };
		commit.Retire(first.Frame);
	}

	// The free is the serializer's, on the pass that finds the retiring subtree at rest — decision 114's
	// second step, reached the way it is actually reached rather than by a call this module does not
	// expose.
	SceneSerializer serializer;
	serializer.Serialize(store);

	GYRO_CHECK(!store.IsLive(first.Surface));

	// Both slots back, as containers that accept nothing. One of the two is the index the surface had,
	// and neither may answer for it — which is why both are checked rather than the one that happens to
	// come off the free list first.
	const EntityId reclaimedFirst = store.CreateContainer({}, { .Extent = { 400.0F, 300.0F } }).value();
	const EntityId reclaimedSecond = store.CreateContainer({}, { .Extent = { 400.0F, 300.0F } }).value();

	GYRO_CHECK(reclaimedFirst.Index == first.Frame.Index || reclaimedFirst.Index == first.Surface.Index);
	GYRO_CHECK(reclaimedSecond.Index == first.Frame.Index || reclaimedSecond.Index == first.Surface.Index);

	GYRO_CHECK(!HitTest(store, At(200.0, 150.0)));
	GYRO_CHECK(store.InputFor(reclaimedFirst) == nullptr);
	GYRO_CHECK(store.InputFor(reclaimedSecond) == nullptr);
}

// The hit is the surface and focus belongs to the window around it — decision 141's two entities, and
// a toolkit's subsurfaces below them.
GYRO_TEST(Hit, AHitResolvesUpToTheWindowThatCouldTakeFocus)
{
	SceneStore store{ Clock };
	const Window window = Open(store, 0.0, 0.0, 400.0F, 300.0F);
	const EntityId popup = store.CreateImage(window.Surface, { .Extent = { 40.0F, 20.0F } }, ImageContent{}).value();

	// The container is what a client's toplevel offers, which is what makes it the answer for everything
	// beneath it.
	store.Focus().Offer(window.Frame);

	GYRO_CHECK(FocusTargetFor(store, window.Surface) == window.Frame);
	GYRO_CHECK(FocusTargetFor(store, popup) == window.Frame);
	GYRO_CHECK(FocusTargetFor(store, window.Frame) == window.Frame);
}

// Nothing on the way up is focusable, which is every node gyro draws for itself: the splash, the
// console, a gym's lanes, and the cursor the pointer is sitting on by construction.
GYRO_TEST(Hit, AHitOnSomethingNobodyOfferedResolvesToNoWindow)
{
	SceneStore store{ Clock };
	const Window window = Open(store, 0.0, 0.0, 400.0F, 300.0F);

	GYRO_CHECK(FocusTargetFor(store, window.Surface).IsNull());
	GYRO_CHECK(FocusTargetFor(store, {}).IsNull());

	// Withdrawn rather than never offered, which is the window that is closing: decision 114 takes focus
	// off it at retirement, so a click during its exit resolves to nothing rather than to a ghost.
	store.Focus().Offer(window.Frame);
	store.Focus().Withdraw(window.Frame);

	GYRO_CHECK(FocusTargetFor(store, window.Surface).IsNull());
}

// The nearest one, so a window inside a group somebody offered separately does not hand focus to the
// group. Reading the stack rather than a kind is what makes this come out right without a rule about
// what containers mean.
GYRO_TEST(Hit, AHitStopsAtTheNearestFocusableAncestorRatherThanTheOutermost)
{
	SceneStore store{ Clock };
	const EntityId group = store.CreateContainer({}, { .Extent = { 400.0F, 300.0F } }).value();
	const EntityId frame = store.CreateContainer(group, { .Extent = { 400.0F, 300.0F } }).value();
	const EntityId surface = store.CreateImage(frame, { .Extent = { 400.0F, 300.0F } }, ImageContent{}).value();

	store.Focus().Offer(group);
	store.Focus().Offer(frame);

	GYRO_CHECK(FocusTargetFor(store, surface) == frame);
}

// Two sessions, two screens, and what the pointer is allowed to reach. Every claim here fails as a
// person clicking on somebody else's window: the lock screen up on the panel and the keystroke going
// into the mail client behind it.
namespace
{
constexpr auto Mine = static_cast<SessionId>(1);
constexpr auto Theirs = static_cast<SessionId>(2);

// Two 1000x1000 panels side by side, the left showing one session and the right the other.
[[nodiscard]] std::array<SceneOutput, 2> Panels(SessionId left, SessionId right)
{
	SceneOutput first{ .Bounds = { { 0.0, 0.0 }, { 1000.0, 1000.0 } },
		               .Density = Scale::FromInteger(1),
		               .Grid = { 1000, 1000 } };
	SceneOutput second = first;

	first.Session = left;
	second.Session = right;
	second.Bounds.Origin.X = 1000.0;

	return { first, second };
}
} // namespace

GYRO_TEST(Hit, AWindowOfASessionThisScreenIsNotShowingIsNotUnderThePointer)
{
	SceneStore store{ Clock };
	const std::array outputs = Panels(Mine, Theirs);
	store.SetOutputs(outputs);

	// Both windows are in exactly the same place on the left-hand panel, so nothing about the geometry
	// separates them and the session is the only thing that can.
	const Window mine = Open(store, 100.0, 100.0, 400.0F, 300.0F);
	const Window theirs = Open(store, 100.0, 100.0, 400.0F, 300.0F);

	GYRO_REQUIRE(store.SetSession(mine.Frame, Mine));
	GYRO_REQUIRE(store.SetSession(theirs.Frame, Theirs));

	// The other session's window is the later root and therefore the frontmost (55), so without the gate
	// this is the one the last-hit-wins walk would answer with.
	GYRO_CHECK(HitTest(store, At(200.0, 200.0)).Node == mine.Surface);

	// And on the right-hand panel, where their session is the one being shown, the answer is theirs —
	// which is what says the gate is the output's assignment rather than an ordering accident.
	const Window alsoTheirs = Open(store, 1100.0, 100.0, 400.0F, 300.0F);
	GYRO_REQUIRE(store.SetSession(alsoTheirs.Frame, Theirs));

	GYRO_CHECK(HitTest(store, At(1200.0, 200.0)).Node == alsoTheirs.Surface);
}

GYRO_TEST(Hit, GyrosOwnRootsAreUnderThePointerOnEveryScreen)
{
	SceneStore store{ Clock };
	const std::array outputs = Panels(Mine, Theirs);
	store.SetOutputs(outputs);

	// `SessionId::None` and left that way, which is the pointer glyph, the background, the splash and
	// the recovery console. It has to be hit on a screen showing somebody's session.
	const Window own = Open(store, 1100.0, 100.0, 400.0F, 300.0F);

	GYRO_CHECK(HitTest(store, At(1200.0, 200.0)).Node == own.Surface);
}

GYRO_TEST(Hit, APointOnNoScreenAtAllReachesOnlyGyrosOwn)
{
	SceneStore store{ Clock };
	const std::array outputs = Panels(Mine, Theirs);
	store.SetOutputs(outputs);

	// Off the side of every panel, which is where a grab's coordinates go when a person drags a
	// scrollbar past the frame. Their window is there and is not reachable; gyro's own is.
	const Window theirs = Open(store, 3000.0, 100.0, 400.0F, 300.0F);
	GYRO_REQUIRE(store.SetSession(theirs.Frame, Theirs));

	GYRO_CHECK(!HitTest(store, At(3100.0, 200.0)));

	const Window own = Open(store, 3000.0, 100.0, 400.0F, 300.0F);

	GYRO_CHECK(HitTest(store, At(3100.0, 200.0)).Node == own.Surface);
}
