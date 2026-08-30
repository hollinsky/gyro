#include "Protocol/Shell.h"

#include "Core/Clock.h"
#include "Scene/Store.h"
#include "Testing/Test.h"

// Which window an `activated` state belongs to, against the store rather than over a socket.
//
// The wire half — that a real client is told, and told once — is
// Integration/ProtocolRoundTrip.Test.cpp's. What is worth asserting here is the reading that cannot be
// seen from there without driving a menu open: focus on a popup is focus on the window the popup came
// out of, because that is what a person means by *this application is the one I am using*.

namespace
{
struct World
{
	ManualClock Clock{ Monotonic::FromNanoseconds(1) };
	SceneStore Scene{ Clock };

	// The floor, two windows on it, and a menu hanging off the first — which is where a popup hangs
	// (141), so the walk that finds the window is the same relationship that draws the menu over it.
	EntityId Floor = Scene.CreateContainer({}, {}).value();
	EntityId First = Scene.CreateContainer(Floor, {}).value();
	EntityId Second = Scene.CreateContainer(Floor, {}).value();
	EntityId Menu = Scene.CreateContainer(First, {}).value();
};
} // namespace

GYRO_TEST(Shell, AWindowHoldsFocusWhenFocusIsOnItself)
{
	World world;

	GYRO_CHECK(FocusRestsOn(world.Scene, world.First, world.First));
	GYRO_CHECK(!FocusRestsOn(world.Scene, world.First, world.Second));
}

GYRO_TEST(Shell, AMenuLeavesTheWindowItCameOutOfHoldingFocus)
{
	World world;

	// The menu has the keyboard — a grabbing popup takes it — and the window underneath must not go
	// grey while its own menu is open, which a person would read as the application having lost focus
	// in the middle of their click.
	GYRO_CHECK(FocusRestsOn(world.Scene, world.First, world.Menu));

	// And it belongs to one window rather than to every window: the other one is not lit by somebody
	// else's menu.
	GYRO_CHECK(!FocusRestsOn(world.Scene, world.Second, world.Menu));
}

GYRO_TEST(Shell, FocusAboveAWindowIsNotFocusOnIt)
{
	World world;

	// The walk goes up and never down. Focus on the floor — which is what a shell moving focus to its
	// own chrome would look like — leaves every window on it unactivated rather than all of them lit.
	GYRO_CHECK(!FocusRestsOn(world.Scene, world.First, world.Floor));
}

GYRO_TEST(Shell, NothingHoldsFocusWhereThereIsNoneToHold)
{
	World world;

	// Both directions, because both are ordinary: a window that is not mapped has no entity, and a
	// world with the recovery console on it has focus on something no client is behind.
	GYRO_CHECK(!FocusRestsOn(world.Scene, world.First, EntityId{}));
	GYRO_CHECK(!FocusRestsOn(world.Scene, EntityId{}, world.First));
}
