#include "Protocol/Scene.h"

#include "Animation/Author/Catalog.h"
#include "Testing/Test.h"
#include "Wayland/Server/GyroSceneV1.h"

// The part of this protocol that is arithmetic rather than a conversation: two enumerations, in two
// files, that have to mean the same thing, and the one entry deliberately missing from one of them.
//
// The rest — a container that survives its shell, a window placed into one, a workspace sliding under
// a named transition — is a sequence between two processes, and Integration/ProtocolRoundTrip.Test.cpp
// is where gyro's server is put in front of a demarshaller that is not its own. Protocol/Floor.Test.cpp
// is the world's side of the same machinery with no client in front of it.

GYRO_TEST(Scene, TheWireTransitionsAreTheOnesTheCatalogHasSpringsFor)
{
	// **The failure this prevents is silent and reads as the compositor's taste.** These are two
	// independent enumerations — one written in Protocols/gyro-scene-v1.xml, one in
	// Animation/Author/Catalog.h — and nothing but this holds them together. Get the mapping wrong and a
	// shell that asks for a workspace switch gets a menu dismissal: a whole desktop moving with the
	// springs of a popup, which looks like a compositor with bad instincts rather than a bug in one.
	GYRO_CHECK(TransitionOf(Wayland::Server::GyroSceneV1Transition::WindowOpen) == Transition::WindowOpen);
	GYRO_CHECK(TransitionOf(Wayland::Server::GyroSceneV1Transition::WindowClose) == Transition::WindowClose);
	GYRO_CHECK(TransitionOf(Wayland::Server::GyroSceneV1Transition::MenuAppear) == Transition::MenuAppear);
	GYRO_CHECK(TransitionOf(Wayland::Server::GyroSceneV1Transition::MenuDismiss) == Transition::MenuDismiss);
	GYRO_CHECK(TransitionOf(Wayland::Server::GyroSceneV1Transition::WorkspaceSwitch) == Transition::WorkspaceSwitch);
	GYRO_CHECK(TransitionOf(Wayland::Server::GyroSceneV1Transition::FocusChange) == Transition::FocusChange);
	GYRO_CHECK(TransitionOf(Wayland::Server::GyroSceneV1Transition::MatchedMove) == Transition::MatchedMove);
	GYRO_CHECK(TransitionOf(Wayland::Server::GyroSceneV1Transition::BackgroundChange) == Transition::BackgroundChange);
}

GYRO_TEST(Scene, ThereIsNoWireNameForNotAnimating)
{
	// **The load-bearing absence, and the whole of decision 198.** `Transition::None` is what every
	// client commit in the tree uses and what a shell may never name: a shell able to set a position
	// with no motion sixty times a second is a shell hand-animating the desktop, which is decision 51's
	// falsifiable test failing. Zero is the value a zeroed field or an uninitialised enum lands on, so
	// it is the one a shell is likeliest to send by accident — and it is not a transition here, where in
	// the catalog it is `None`.
	GYRO_CHECK(!TransitionOf(static_cast<Wayland::Server::GyroSceneV1Transition>(0)).has_value());

	// The other end: a shell built against a newer version of this protocol than the compositor it is
	// talking to. Answered with an error rather than by falling back, because falling back would land on
	// `None` and hand that shell exactly the un-animated write this design exists to withhold.
	GYRO_CHECK(!TransitionOf(static_cast<Wayland::Server::GyroSceneV1Transition>(TransitionCount)).has_value());
	GYRO_CHECK(!TransitionOf(static_cast<Wayland::Server::GyroSceneV1Transition>(0xffffffffU)).has_value());
}

GYRO_TEST(Scene, EveryTransitionTheCatalogHasIsOneAShellCanNameExceptTheOneItMayNot)
{
	// The inverse of the first test, and the one that catches the *next* transition rather than a
	// mistake in these eight: `TransitionCount` moves when the catalog learns one, and a transition gyro
	// can draw that no shell can ask for is a motion that exists only inside gyro's own code.
	//
	// Nine and eight rather than a number each, so that the relationship is what is asserted: the wire
	// names the catalog minus `None`, which is the one entry withheld.
	GYRO_CHECK_EQ(TransitionCount, 9U);

	std::size_t nameable = 0;

	for (std::size_t value = 0; value < TransitionCount; ++value)
	{
		if (TransitionOf(static_cast<Wayland::Server::GyroSceneV1Transition>(value)))
		{
			++nameable;
		}
	}

	GYRO_CHECK_EQ(nameable, TransitionCount - 1);
}

GYRO_TEST(Scene, TheWireStatesAreTheOnesAShellDecidesAndNoneOfTheOnesGyroDoes)
{
	using State = Wayland::Server::GyroSceneV1State;

	// Each bit on its own, because the failure a combined check would miss is two entries mapped to one
	// field — a shell tiling a window to the left and getting a window told it is maximised.
	GYRO_CHECK(StatesOf(State::Maximized) == WindowStates{ .Maximized = true });
	GYRO_CHECK(StatesOf(State::Fullscreen) == WindowStates{ .Fullscreen = true });
	GYRO_CHECK(StatesOf(State::TiledLeft) == WindowStates{ .TiledLeft = true });
	GYRO_CHECK(StatesOf(State::TiledRight) == WindowStates{ .TiledRight = true });
	GYRO_CHECK(StatesOf(State::TiledTop) == WindowStates{ .TiledTop = true });
	GYRO_CHECK(StatesOf(State::TiledBottom) == WindowStates{ .TiledBottom = true });

	// A set rather than one bit, which is what a shell actually sends: a window filling a screen it is
	// also tiled against is both, and the two must not be exclusive.
	GYRO_CHECK(
		StatesOf(State::Maximized | State::TiledTop | State::TiledLeft) ==
		WindowStates{ .Maximized = true, .TiledLeft = true, .TiledTop = true }
	);

	// **Nothing is what a shell that says nothing means**, and it is the value a restored window is set
	// back to rather than a state the protocol has no name for.
	GYRO_CHECK(StatesOf(State{}) == WindowStates{});

	// **The bits gyro keeps are unreachable from here, and that is the assertion rather than a
	// coincidence of the numbering.** `activated`, `resizing` and `suspended` are gyro's own answers
	// about a window — who has the keyboard (149), whose edge the pointer is holding (166), and whether
	// anybody is looking — and a shell that could write one could light the titlebar of a window a
	// person is not typing into. Every bit outside the six is dropped, which is what makes a shell built
	// against a newer protocol lose the surplus rather than the session.
	GYRO_CHECK(
		StatesOf(static_cast<State>(0xffffffffU)) == WindowStates{ .Maximized = true,
	                                                               .Fullscreen = true,
	                                                               .TiledLeft = true,
	                                                               .TiledRight = true,
	                                                               .TiledTop = true,
	                                                               .TiledBottom = true }
	);

	GYRO_CHECK(StatesOf(static_cast<State>(64)) == WindowStates{});
}

GYRO_TEST(Scene, TheWireCapabilitiesAreTheThreeAShellCanBeAskedFor)
{
	using Capability = Wayland::Server::GyroSceneV1Capability;

	GYRO_CHECK(CapabilitiesOf(Capability::Maximize) == WindowCapabilities{ .Maximize = true });
	GYRO_CHECK(CapabilitiesOf(Capability::Minimize) == WindowCapabilities{ .Minimize = true });
	GYRO_CHECK(CapabilitiesOf(Capability::Fullscreen) == WindowCapabilities{ .Fullscreen = true });

	// **The empty set is a value rather than an absence**, which is the whole of why `xdg_wm_base` can
	// be advertised at version 5: a client told 5 and sent nothing assumes it has all four controls, so
	// *none* has to be a thing a shell can say and gyro can send.
	GYRO_CHECK(CapabilitiesOf(Capability{}) == WindowCapabilities{});

	// **There is no window-menu capability and this is where that is asserted.** `xdg_toplevel` has
	// four and this has three, because nothing forwards the request a client sends after being offered
	// the fourth — so offering it would produce exactly the dead control the event exists to prevent.
	// The day `window_request` learns to carry it, this number moves with it.
	GYRO_CHECK(
		CapabilitiesOf(static_cast<Capability>(0xffffffffU)) ==
		WindowCapabilities{ .Maximize = true, .Minimize = true, .Fullscreen = true }
	);
}
