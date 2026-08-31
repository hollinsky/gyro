#include "Protocol/Foreign.h"

#include <string>

#include "Core/Handle.h"
#include "Testing/Test.h"

// The identifier, which is the one part of this protocol that is arithmetic rather than a
// conversation.
//
// Everything else here — a window announced before the roundtrip that asked about it, a title sent
// once, a `closed` on unmap, an application refused the global entirely — is a sequence of events over
// a socket, and Integration/ProtocolRoundTrip.Test.cpp is where gyro's server is checked against a
// demarshaller that is not its own. What that test cannot see is the shape of the string, because it
// only ever compares one to another.

GYRO_TEST(Foreign, AnIdentifierNamesTheGenerationAndTheSlot)
{
	// Sixteen hex digits, generation first — see [Foreign.h](Foreign.h) for why that order and not the
	// other: two windows opened a moment apart differ in the leading digits, where a person comparing
	// two names by eye is looking.
	GYRO_CHECK(ForeignIdentifier(EntityId{ .Index = 7, .Generation = 3 }) == "0000000300000007");
	GYRO_CHECK(ForeignIdentifier(EntityId{ .Index = 0xABCDEF, .Generation = 0x1234 }) == "0000123400abcdef");
}

GYRO_TEST(Foreign, AnIdentifierIsWithinWhatTheProtocolAllows)
{
	// The protocol's bound is 32 printable ASCII bytes and not empty. The widest possible handle is
	// what has to fit, so the check is against that rather than against a plausible one.
	const std::string widest = ForeignIdentifier(EntityId{ .Index = 0xFFFFFFFF, .Generation = 0xFFFFFFFF });

	GYRO_CHECK(!widest.empty());
	GYRO_CHECK(widest.size() <= 32);

	for (const char digit : widest)
	{
		GYRO_CHECK(digit > 0x20 && digit < 0x7F);
	}
}

GYRO_TEST(Foreign, ARetiredSlotDoesNotGetItsNameBack)
{
	// **The whole of the protocol's non-reuse rule, and gyro gets it for free.** An identifier must not
	// be reused after a window unmaps, and `Core/Handle.h` already promises that a slot handed out again
	// carries a generation that has moved — so the second window in a slot cannot be mistaken for the
	// first by a client holding the old name.
	const EntityId first{ .Index = 12, .Generation = 4 };
	const EntityId reused{ .Index = 12, .Generation = 5 };

	GYRO_CHECK(ForeignIdentifier(first) != ForeignIdentifier(reused));
}
