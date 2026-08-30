#include "Protocol/Floor.h"

#include <array>
#include <cstdint>

#include "Animation/Author/Bundle.h"
#include "Core/Clock.h"
#include "Core/Session.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Scene/Commit.h"
#include "Scene/Entity.h"
#include "Scene/Hit.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Testing/Test.h"
#include "World/Content.h"

// The floor and the placement, against the store rather than over a socket.
//
// What is worth asserting here is the arithmetic and nothing else: the wire half — that a real client
// gets as far as being placed — is Integration/ProtocolRoundTrip.Test.cpp's, and it cannot check where
// a window landed to within a pixel without reaching into the store anyway.

namespace
{
constexpr double PanelWidth = 1920.0;
constexpr double PanelHeight = 1080.0;

[[nodiscard]] SceneOutput Panel(double left = 0.0, double top = 0.0)
{
	return { .Bounds = { { left, top }, { PanelWidth, PanelHeight } },
		     .Density = Scale::FromInteger(1),
		     .Grid = { 1920, 1080 } };
}

// A development run's session, which is no session: `HostListener::Own` binds a socket with no agent
// behind it, so a client belongs to nobody and its floor is gyro's own. Most of the arithmetic below
// is indifferent to which session it is asked about and says so by using this one.
constexpr SessionId Nobody = SessionId::None;

// Two sessions, which is the arrangement everything about the partition needs and nothing about the
// arithmetic does.
constexpr auto First = static_cast<SessionId>(1);
constexpr auto Second = static_cast<SessionId>(2);

[[nodiscard]] Size<SurfaceSpace, float> Window(float width, float height) noexcept
{
	return { width, height };
}
} // namespace

GYRO_TEST(Floor, AFloorIsOneContainerPerSessionAndOnlyEverOne)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	SessionFloors floors;
	GYRO_REQUIRE(floors.Open(scene, Nobody).has_value());
	GYRO_CHECK(!floors.Container(Nobody).IsNull());
	GYRO_CHECK_EQ(scene.Count(), std::uint32_t{ 1 });

	// A second call for the same session is a wiring mistake rather than a state to serve, and the
	// failure of letting it through is two floors with one person's windows split between them and no
	// way to tell which is on screen.
	GYRO_CHECK(!floors.Open(scene, Nobody).has_value());
	GYRO_CHECK_EQ(scene.Count(), std::uint32_t{ 1 });

	// A second *session* is two floors, which is decision 21's two people logged in at once.
	GYRO_REQUIRE(floors.Open(scene, First).has_value());
	GYRO_CHECK_EQ(scene.Count(), std::uint32_t{ 2 });
	GYRO_CHECK(floors.Container(First) != floors.Container(Nobody));

	// A session nobody has offered a listener for has no floor at all, which is also the answer a commit
	// from a client whose session has already ended gets.
	GYRO_CHECK(floors.Container(Second).IsNull());
}

// The partition as the frame thread reads it: a floor is a root and its session is on the root, never
// on the windows under it. A window asked directly answers `None`, which is why nothing asks one — its
// session is the floor's, by being under the floor.
GYRO_TEST(Floor, AFloorIsARootCarryingItsSessionAndTheWindowsUnderItCarryNone)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	SessionFloors floors;
	GYRO_REQUIRE(floors.Open(scene, First).has_value());

	const EntityId container = floors.Container(First);

	GYRO_REQUIRE(scene.Find(container) != nullptr);
	GYRO_CHECK(scene.Find(container)->Parent.IsNull());
	GYRO_CHECK(scene.Find(container)->Session == First);

	const std::optional<EntityId> window = scene.CreateContainer(container, {});
	GYRO_REQUIRE(window.has_value());
	GYRO_CHECK(scene.Find(*window)->Session == SessionId::None);

	// Refused rather than stored, so there is one answer on the machine and not two that can disagree.
	GYRO_CHECK(!scene.SetSession(*window, Second));
	GYRO_CHECK(scene.Find(*window)->Session == SessionId::None);
}

// The session ended, which retires its floor and everything still hanging on it — the path a client
// exiting already takes (114), one level up. Retired rather than freed, so a window that was closing
// finishes closing rather than vanishing part-way through.
GYRO_TEST(Floor, ClosingASessionRetiresItsFloorAndLeavesEveryOtherAlone)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	SessionFloors floors;
	GYRO_REQUIRE(floors.Open(scene, First).has_value());
	GYRO_REQUIRE(floors.Open(scene, Second).has_value());

	const EntityId leaving = floors.Container(First);
	const EntityId staying = floors.Container(Second);

	const std::optional<EntityId> window = scene.CreateContainer(leaving, {});
	GYRO_REQUIRE(window.has_value());

	floors.Close(scene, First);

	GYRO_CHECK(scene.Find(leaving)->Retiring);
	GYRO_CHECK(scene.Find(*window)->Retiring);
	GYRO_CHECK(!scene.Find(staying)->Retiring);

	// The record leaves at once even though the nodes linger, because a commit arriving from a client
	// that has not been dropped yet must not find a floor whose session is over.
	GYRO_CHECK(floors.Container(First).IsNull());
	GYRO_CHECK(!floors.Container(Second).IsNull());

	// Closing twice is a second EOF on a connection already gone, and changes nothing.
	floors.Close(scene, First);
	GYRO_CHECK(!floors.Container(Second).IsNull());
}

// The placement's half of the same fact. A window centred on `outputs.front()` regardless of who is
// being shown there is an application a person launched, running and drawing, on a monitor they are
// not looking at and cannot bring it to.
GYRO_TEST(Floor, AWindowIsPlacedOnAnOutputShowingItsOwnSession)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	SceneOutput left = Panel();
	left.Session = First;

	SceneOutput right = Panel(PanelWidth, 0.0);
	right.Id = OutputId{ 2, 1 };
	right.Session = Second;

	const std::array outputs{ left, right };
	scene.SetOutputs(outputs);

	SessionFloors floors;
	GYRO_REQUIRE(floors.Open(scene, Second).has_value());

	const std::optional<EntityId> window = scene.CreateContainer(floors.Container(Second), {});
	GYRO_REQUIRE(window.has_value());

	{
		SceneCommit commit{ scene, CommitAuthor::Compositor, scene.Now() };

		PlaceOnFloor(commit, scene, Second, *window, Window(800.0F, 600.0F));
	}

	GYRO_CHECK_EQ(scene.Find(*window)->Translation.Model().X, PanelWidth + (1920.0 - 800.0) / 2.0);
}

// And a session on no output at all, which is decision 21's *connected and not presented*: its clients
// run and draw, and a window that opens there waits rather than landing on somebody else's screen.
GYRO_TEST(Floor, AWindowOfASessionOnNoOutputIsNotPlaced)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	SceneOutput only = Panel();
	only.Session = First;

	const std::array outputs{ only };
	scene.SetOutputs(outputs);

	SessionFloors floors;
	GYRO_REQUIRE(floors.Open(scene, Second).has_value());

	const std::optional<EntityId> window = scene.CreateContainer(floors.Container(Second), {});
	GYRO_REQUIRE(window.has_value());

	{
		SceneCommit commit{ scene, CommitAuthor::Compositor, scene.Now() };

		PlaceOnFloor(commit, scene, Second, *window, Window(800.0F, 600.0F));
	}

	GYRO_CHECK_EQ(scene.Find(*window)->Translation.Model().X, 0.0);
	GYRO_CHECK_EQ(scene.Find(*window)->Translation.Model().Y, 0.0);
}

GYRO_TEST(Floor, AWindowIsCentredOnTheOutputHoldingThePointer)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	SessionFloors floors;
	GYRO_REQUIRE(floors.Open(scene, Nobody).has_value());

	const std::optional<EntityId> window = scene.CreateContainer(floors.Container(Nobody), {});
	GYRO_REQUIRE(window.has_value());

	{
		SceneCommit commit{ scene, CommitAuthor::Compositor, scene.Now() };

		PlaceOnFloor(commit, scene, Nobody, *window, Window(800.0F, 600.0F));
	}

	const Entity* const placed = scene.Find(*window);
	GYRO_REQUIRE(placed != nullptr);

	// Half the leftover on each side. Written out rather than recomputed with the same expression the
	// implementation uses, so a sign error has somewhere to show up.
	GYRO_CHECK_EQ(placed->Translation.Model().X, (1920.0 - 800.0) / 2.0);
	GYRO_CHECK_EQ(placed->Translation.Model().Y, (1080.0 - 600.0) / 2.0);
}

GYRO_TEST(Floor, ThePlacementIsInTheOutputsOwnCornerRatherThanTheWorldsOrigin)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	// The second monitor in a left-to-right arrangement. A placement computed against the extent alone
	// would put every window on the first panel, which is what a person with two screens would report
	// as *it always opens on the wrong monitor*.
	const std::array outputs{ Panel(PanelWidth, 0.0) };
	scene.SetOutputs(outputs);

	SessionFloors floors;
	GYRO_REQUIRE(floors.Open(scene, Nobody).has_value());

	const std::optional<EntityId> window = scene.CreateContainer(floors.Container(Nobody), {});
	GYRO_REQUIRE(window.has_value());

	{
		SceneCommit commit{ scene, CommitAuthor::Compositor, scene.Now() };

		PlaceOnFloor(commit, scene, Nobody, *window, Window(800.0F, 600.0F));
	}

	GYRO_CHECK_EQ(scene.Find(*window)->Translation.Model().X, PanelWidth + (1920.0 - 800.0) / 2.0);
}

GYRO_TEST(Floor, AWindowLargerThanTheScreenHangsOffBothEdgesEqually)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	SessionFloors floors;
	GYRO_REQUIRE(floors.Open(scene, Nobody).has_value());

	const std::optional<EntityId> window = scene.CreateContainer(floors.Container(Nobody), {});
	GYRO_REQUIRE(window.has_value());

	{
		SceneCommit commit{ scene, CommitAuthor::Compositor, scene.Now() };

		PlaceOnFloor(commit, scene, Nobody, *window, Window(2400.0F, 1400.0F));
	}

	// Negative, and deliberately not clamped: a window too big for the screen is centred on it, which
	// puts its middle where a person is looking. Clamping to the corner would hide the far half of a
	// dialog whose buttons are at the bottom.
	GYRO_CHECK_EQ(scene.Find(*window)->Translation.Model().X, (1920.0 - 2400.0) / 2.0);
	GYRO_CHECK_EQ(scene.Find(*window)->Translation.Model().Y, (1080.0 - 1400.0) / 2.0);
}

// The screen a window is on, which is what `xdg_toplevel.configure_bounds` tells a client and is the
// half of that answer worth asserting: the event itself carries these two numbers and nothing else.
//
// **The extent is the output's logical rectangle rather than its device grid**, which is the whole
// point of sending it. A client that is told nothing derives the screen from `wl_output` — the grid
// over the integer scale — and decision 164 runs panels at scales that are not integers, so on the
// 1.37x panel below the two answers differ by a third and a toolkit refuses to grow a window past the
// smaller one.
GYRO_TEST(Floor, AWindowWithNoPlaceYetIsToldAboutTheScreenItIsAboutToOpenOn)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	// A laptop panel: 1920 device pixels at 164/120, which is what 309 mm at arm's length derives.
	SceneOutput panel{ .Bounds = { { 0.0, 0.0 }, { 1405.0, 790.0 } },
		               .Density = Scale::FromNumerator(164),
		               .Grid = { 1920, 1080 } };
	panel.Session = First;

	const std::array outputs{ panel };
	scene.SetOutputs(outputs);

	// Null, because the window does not exist yet: the first configure goes out before anything is
	// mapped, and it is the one a toolkit sizes its opening frame against.
	const SceneOutput* const shown = OutputFor(scene, First, EntityId{});

	GYRO_REQUIRE(shown != nullptr);
	GYRO_CHECK_EQ(shown->Bounds.Extent.Width, 1405.0);

	// What `wl_output` alone would have said, and the reason this function exists.
	GYRO_CHECK(shown->Bounds.Extent.Width != panel.Grid.Width / panel.Density.CeilToInteger());
}

GYRO_TEST(Floor, AWindowOfASessionOnNoOutputIsToldNothing)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	SceneOutput panel = Panel();
	panel.Session = First;

	const std::array outputs{ panel };
	scene.SetOutputs(outputs);

	// Not the other session's screen, which would have a window sized for a monitor it will never
	// appear on. The caller sends zero, which the protocol reads as *bounds unknown*.
	GYRO_CHECK(OutputFor(scene, Second, EntityId{}) == nullptr);
}

// Two panels, one session, and a window dragged across the seam between them.
GYRO_TEST(Floor, AWindowIsToldAboutTheScreenItIsMostlyOn)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	SceneOutput left = Panel();
	left.Session = First;

	SceneOutput right = Panel(PanelWidth, 0.0);
	right.Id = OutputId{ 2, 1 };
	right.Session = First;

	const std::array outputs{ left, right };
	scene.SetOutputs(outputs);

	SessionFloors floors;
	GYRO_REQUIRE(floors.Open(scene, First).has_value());

	const std::optional<EntityId> window = scene.CreateContainer(floors.Container(First), {});
	GYRO_REQUIRE(window.has_value());

	const auto put = [&](double x) {
		SceneCommit commit{ scene, CommitAuthor::Compositor, scene.Now() };

		static_cast<void>(commit.Move(*window, { x, 0.0, 0.0 }, Immediate()));
		static_cast<void>(commit.Resize(*window, Window(600.0F, 400.0F)));
	};

	put(100.0);
	GYRO_CHECK(OutputFor(scene, First, *window) == &scene.Outputs()[0]);

	// Two thirds of the way over the seam, which is a corner still on the left panel and most of the
	// window on the right. **The corner is what a naive answer would follow**, and following it would
	// change what a window may be at the instant a sliver of it crosses.
	put(PanelWidth - 200.0);
	GYRO_CHECK(OutputFor(scene, First, *window) == &scene.Outputs()[1]);

	// Dragged off the end of the desk entirely. There is nothing true to say, so it keeps the session's
	// first screen rather than being told its bounds went away underneath it.
	put(PanelWidth * 4.0);
	GYRO_CHECK(OutputFor(scene, First, *window) == &scene.Outputs()[0]);
}

GYRO_TEST(Floor, APlacementWithNoOutputsChangesNothing)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	SessionFloors floors;
	GYRO_REQUIRE(floors.Open(scene, Nobody).has_value());

	const std::optional<EntityId> window = scene.CreateContainer(floors.Container(Nobody), {});
	GYRO_REQUIRE(window.has_value());

	{
		SceneCommit commit{ scene, CommitAuthor::Compositor, scene.Now() };

		PlaceOnFloor(commit, scene, Nobody, *window, Window(800.0F, 600.0F));
	}

	// There is no honest centre of nothing, and the window stays at the origin rather than being moved
	// to a coordinate invented here. Nothing is drawing this scene in any case — a world with no
	// outputs publishes a wake schedule nothing can read.
	GYRO_CHECK_EQ(scene.Find(*window)->Translation.Model().X, 0.0);
}

namespace
{
// A window as the shell builds one: a container on the floor, the client's surface beneath it, and the
// container being what takes focus. Two of these is the arrangement click-to-focus is for.
struct Placed
{
	EntityId Frame;
	EntityId Surface;
};

[[nodiscard]] Placed Add(SceneStore& scene, const SessionFloors& floors)
{
	const EntityId frame = scene.CreateContainer(floors.Container(Nobody), { .Extent = { 400.0F, 300.0F } }).value();
	const EntityId surface = scene.CreateImage(frame, { .Extent = { 400.0F, 300.0F } }, ImageContent{}).value();

	scene.Focus().Offer(frame);

	return { .Frame = frame, .Surface = surface };
}
} // namespace

// Decision 162: the click focuses the window under it and brings it forward, and it does both because
// with nothing drawing a focus ring the raise is the only half a person can see.
GYRO_TEST(Floor, AClickFocusesTheWindowUnderItAndBringsItToTheFront)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	SessionFloors floors;
	GYRO_REQUIRE(floors.Open(scene, Nobody).has_value());

	const Placed below = Add(scene, floors);
	const Placed above = Add(scene, floors);

	// The newest window has both, which is the stack policy (141) and the append order (55) agreeing.
	GYRO_REQUIRE(scene.Focus().Focused() == above.Frame);
	GYRO_REQUIRE(scene.Find(floors.Container(Nobody))->LastChild == above.Frame);

	// The hit is the surface and never the container: the click is on the pixels.
	FocusByClick(scene, below.Surface);

	GYRO_CHECK(scene.Focus().Focused() == below.Frame);
	GYRO_CHECK(scene.Find(floors.Container(Nobody))->LastChild == below.Frame);
	GYRO_CHECK(scene.Find(floors.Container(Nobody))->FirstChild == above.Frame);
}

// A click on the background, on the floor, or on anything gyro drew for itself. Focus stays where it
// was: there is nothing else on this machine to type into, and a person who clicks empty space and
// then types means the window they were already using.
GYRO_TEST(Floor, AClickOnNothingLeavesFocusAndTheOrderAlone)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1) };
	SceneStore scene{ clock };

	const std::array outputs{ Panel() };
	scene.SetOutputs(outputs);

	SessionFloors floors;
	GYRO_REQUIRE(floors.Open(scene, Nobody).has_value());

	const Placed below = Add(scene, floors);
	const Placed above = Add(scene, floors);

	FocusByClick(scene, {});
	FocusByClick(scene, floors.Container(Nobody));

	// A node gyro authored that is not under any window either — the cursor stands exactly here.
	const EntityId glyph = scene.CreateContainer({}, {}).value();

	FocusByClick(scene, glyph);

	GYRO_CHECK(scene.Focus().Focused() == above.Frame);
	GYRO_CHECK(scene.Find(floors.Container(Nobody))->LastChild == above.Frame);
	GYRO_CHECK(scene.Find(floors.Container(Nobody))->FirstChild == below.Frame);
}
