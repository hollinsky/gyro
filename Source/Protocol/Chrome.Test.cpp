#include "Protocol/Chrome.h"

#include "Testing/Test.h"
#include "Wayland/Server/GyroChromeV1.h"
#include "World/Material.h"

// The one piece of this protocol that is arithmetic rather than a conversation: two enumerations, in
// two files, that have to mean the same thing.
//
// Everything else here — a launcher that draws over a raised window, a panel absent from the window
// list, an alt-tab that steps over both — is a sequence of events between two processes, and
// Integration/ProtocolRoundTrip.Test.cpp is where gyro's server is put in front of a demarshaller that
// is not its own. What is checked beside those, in Scene/Focus.Test.cpp and Protocol/Floor.Test.cpp,
// is the world's side without a client in front of it.

GYRO_TEST(Chrome, TheWireMaterialsAreTheOnesTheWorldDraws)
{
	// **The failure this prevents is silent and looks like a design opinion.** These are two independent
	// enumerations — one written in Protocols/gyro-chrome-v1.xml, one in World/Material.h — and nothing
	// but this holds them together. Get the mapping wrong and a shell that asks for glass gets smoke: a
	// dark panel where a person expected a light one, which reads as the compositor's taste rather than
	// as a bug in it.
	GYRO_CHECK(MaterialOf(Wayland::Server::GyroChromeV1Material::None) == Material::None);
	GYRO_CHECK(MaterialOf(Wayland::Server::GyroChromeV1Material::Glass) == Material::Glass);
	GYRO_CHECK(MaterialOf(Wayland::Server::GyroChromeV1Material::Smoke) == Material::Smoke);
}

GYRO_TEST(Chrome, AMaterialGyroHasNoNameForIsRefusedRatherThanDrawnAsNothing)
{
	// A shell built against a newer version of this protocol than the compositor it is talking to. The
	// two answers available are *draw nothing* and *say so*, and the protocol chooses the second: a
	// launcher silently rendered with no material looks correct to the shell that asked and wrong on
	// screen, with nothing anywhere naming the disagreement.
	GYRO_CHECK(!MaterialOf(static_cast<Wayland::Server::GyroChromeV1Material>(3)).has_value());

	// The value a client is most likely to send by accident, which is whatever a zeroed field or an
	// uninitialised enum lands on. Zero is `none` and is a real material, so this is the *other* end.
	GYRO_CHECK(!MaterialOf(static_cast<Wayland::Server::GyroChromeV1Material>(0xffffffffU)).has_value());
}

GYRO_TEST(Chrome, EveryMaterialTheWorldDrawsHasAWireNameToAskForIt)
{
	// The inverse of the first test and the one that catches the *next* material rather than a mistake
	// in these three: `MaterialCount` moves when `World/Material.h` learns one, and a material gyro can
	// draw that no shell can ask for is a feature that exists only in the gym.
	GYRO_CHECK(MaterialCount == 3);
}
