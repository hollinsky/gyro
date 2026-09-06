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
	// **The load-bearing absence, and the whole of decision 190.** `Transition::None` is what every
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
