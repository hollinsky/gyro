#include "Session/Handover.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Testing/Test.h"

// The handover ABI, which is the file a release is diffed at — so what is checked here is the layout
// rather than the round trip. A round trip agrees with itself no matter what either half does, and
// the failure this has to catch is a field that moved between two builds that both pass their own
// tests.
//
// **Byte literals are the machine's order, matching Wire/Message.Test.cpp.** Both ends of this are
// processes on one machine, so native order *is* the format, and a test that assembled the expected
// bytes with shifts would be checking the header against itself.

namespace
{
using namespace Session;

// `Hello`, version 1, laid out by hand.
constexpr std::array<std::byte, 8> HelloBytes{
	std::byte{ 0x01 }, std::byte{ 0x00 }, std::byte{ 0x04 }, std::byte{ 0x00 },
	std::byte{ 0x01 }, std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0x00 },
};

// `Manage` and `Managing`, laid out by hand. A header and nothing after it.
constexpr std::array<std::byte, 4> ManageBytes{
	std::byte{ 0x06 },
	std::byte{ 0x00 },
	std::byte{ 0x00 },
	std::byte{ 0x00 },
};

constexpr std::array<std::byte, 4> ManagingBytes{
	std::byte{ 0x07 },
	std::byte{ 0x00 },
	std::byte{ 0x00 },
	std::byte{ 0x00 },
};

// Somewhere for a message to be written, sized as either end sizes its receive buffer.
using Datagram = std::array<std::byte, MaxMessageBytes>;
} // namespace

GYRO_TEST(Handover, HelloPacksToItsLayout)
{
	Datagram bytes{};

	GYRO_REQUIRE_EQ(Hello{ .Version = 1 }.Encode(bytes), std::size_t{ 8 });
	GYRO_CHECK(std::equal(HelloBytes.begin(), HelloBytes.end(), bytes.begin()));
}

GYRO_TEST(Handover, HelloUnpacksFromItsLayout)
{
	const std::optional<Hello> hello = Hello::Decode(HelloBytes);

	GYRO_REQUIRE(hello.has_value());
	GYRO_CHECK_EQ(hello->Version, std::uint32_t{ 1 });
}

GYRO_TEST(Handover, HeaderReadsOpcodeAndLength)
{
	const std::optional<MessageHeader> header = ReadHeader(HelloBytes);

	GYRO_REQUIRE(header.has_value());
	GYRO_CHECK(header->Op == Opcode::Hello);
	GYRO_CHECK_EQ(header->PayloadBytes, std::uint16_t{ 4 });
}

// A datagram too short to hold a header is not an incomplete message — a sequenced-packet socket
// delivers what the sender wrote — so it is refused rather than waited for.
GYRO_TEST(Handover, ShortDatagramHasNoHeader)
{
	constexpr std::array<std::byte, 3> Runt{ std::byte{ 0x01 }, std::byte{ 0x00 }, std::byte{ 0x04 } };

	GYRO_CHECK(!ReadHeader(Runt).has_value());
	GYRO_CHECK(!ReadHeader({}).has_value());
}

GYRO_TEST(Handover, UnknownOpcodeIsRefused)
{
	std::array<std::byte, HeaderBytes> unknown{};
	unknown[0] = std::byte{ 0x63 };

	GYRO_CHECK(!ReadHeader(unknown).has_value());
	GYRO_CHECK(!IsOpcode(0));

	// One past the last one this build has a name for. **It moves every time an opcode is added**, which
	// is the point of writing it rather than a constant: a value that has shipped may never mean
	// anything else, so the boundary is the only part of this question that is allowed to move.
	GYRO_CHECK(!IsOpcode(10));
	GYRO_CHECK(IsOpcode(static_cast<std::uint16_t>(Opcode::Hello)));
	GYRO_CHECK(IsOpcode(static_cast<std::uint16_t>(Opcode::Assigned)));
}

// Longer than the buffer either end reads into. The socket would have truncated it and set MSG_TRUNC;
// this is the same answer given to a caller that somehow has the whole thing.
GYRO_TEST(Handover, OversizedDatagramIsRefused)
{
	const std::vector<std::byte> huge(MaxMessageBytes + 1, std::byte{ 0 });

	GYRO_CHECK(!ReadHeader(huge).has_value());
}

// A trailing byte is a peer speaking a dialect this build does not have. Reading the prefix would be
// guessing at what changed, which is exactly what an ABI test exists to stop.
GYRO_TEST(Handover, TrailingBytesAreRefused)
{
	std::array<std::byte, 9> extra{};
	std::copy(HelloBytes.begin(), HelloBytes.end(), extra.begin());

	GYRO_CHECK(!Hello::Decode(extra).has_value());
}

// The length field and the datagram's own size must agree. The size is the truth; a header that
// disagrees with it is the message this check exists for.
GYRO_TEST(Handover, LengthFieldMustMatchTheDatagram)
{
	std::array<std::byte, 8> lying{};
	std::copy(HelloBytes.begin(), HelloBytes.end(), lying.begin());
	lying[2] = std::byte{ 0x00 };

	GYRO_CHECK(ReadHeader(lying).has_value());
	GYRO_CHECK(!Hello::Decode(lying).has_value());
}

// Every `Decode` re-checks the opcode, so a reader that dispatched on the header and then called the
// wrong one gets nothing rather than a field read out of the wrong message.
GYRO_TEST(Handover, DecodeRefusesAnotherMessagesOpcode)
{
	Datagram bytes{};
	const std::size_t written = Welcome{ .Version = 1 }.Encode(bytes);

	GYRO_REQUIRE_EQ(written, std::size_t{ 8 });
	GYRO_CHECK(!Hello::Decode(std::span<const std::byte>{ bytes }.first(written)).has_value());
	GYRO_CHECK(!Offer::Decode(std::span<const std::byte>{ bytes }.first(written)).has_value());
}

GYRO_TEST(Handover, WelcomeCarriesTheVersionInForce)
{
	Datagram bytes{};
	const std::size_t written = Welcome{ .Version = 3 }.Encode(bytes);
	const std::optional<Welcome> welcome = Welcome::Decode(std::span<const std::byte>{ bytes }.first(written));

	GYRO_REQUIRE(welcome.has_value());
	GYRO_CHECK_EQ(welcome->Version, std::uint32_t{ 3 });
}

// The offer's answer, and the message the agent waits for before it starts anything.
GYRO_TEST(Handover, AcceptedCarriesTheSession)
{
	Datagram bytes{};
	const std::size_t written = Accepted{ .Id = SessionId{ 7 } }.Encode(bytes);
	const std::optional<Accepted> accepted = Accepted::Decode(std::span<const std::byte>{ bytes }.first(written));

	GYRO_REQUIRE(accepted.has_value());
	GYRO_CHECK(accepted->Id == SessionId{ 7 });
}

// An offer carries which listeners are attached and nothing else, the descriptors being everything
// else gyro needs to judge it.
GYRO_TEST(Handover, OfferCarriesItsRoles)
{
	Datagram bytes{};
	const std::uint32_t both =
		static_cast<std::uint32_t>(ListenerRole::Applications) | static_cast<std::uint32_t>(ListenerRole::Shell);
	const std::size_t written = Offer{ .Roles = both }.Encode(bytes);

	GYRO_REQUIRE_EQ(written, HeaderBytes + Offer::PayloadBytes);

	const std::optional<Offer> offer = Offer::Decode(std::span<const std::byte>{ bytes }.first(written));

	GYRO_REQUIRE(offer.has_value());
	GYRO_CHECK_EQ(offer->Roles, both);
}

// A session offered by an agent that starts no shell, which is the default and is the ordinary one.
GYRO_TEST(Handover, AnOfferIsApplicationsOnlyUnlessItSaysOtherwise)
{
	GYRO_CHECK(Offers(Offer{}.Roles, ListenerRole::Applications));
	GYRO_CHECK(!Offers(Offer{}.Roles, ListenerRole::Shell));
	GYRO_CHECK_EQ(ListenersIn(Offer{}.Roles), std::size_t{ 1 });
}

// The bitmap is what says how many descriptors arrived and which is which, so both answers are read
// off it rather than off an order somebody has to remember.
GYRO_TEST(Handover, RolesSayHowManyListenersAndWhichIsWhich)
{
	const std::uint32_t applications = static_cast<std::uint32_t>(ListenerRole::Applications);
	const std::uint32_t both = applications | static_cast<std::uint32_t>(ListenerRole::Shell);

	GYRO_CHECK_EQ(ListenersIn(both), std::size_t{ 2 });
	GYRO_CHECK_EQ(IndexOf(both, ListenerRole::Applications), std::size_t{ 0 });
	GYRO_CHECK_EQ(IndexOf(both, ListenerRole::Shell), std::size_t{ 1 });

	// Ascending bit order rather than a fixed slot per role: with the shell absent, the applications
	// listener is still the first descriptor, and a role added later takes the position its bit gives it
	// without moving anything below it.
	GYRO_CHECK_EQ(IndexOf(applications, ListenerRole::Applications), std::size_t{ 0 });
}

// A bit this build has no name for is a peer speaking a dialect it does not, and the reason it cannot
// be masked off is that the bits *are* the descriptor count.
GYRO_TEST(Handover, RolesWithNoApplicationsListenerOrAnUnknownBitAreNotWellFormed)
{
	GYRO_CHECK(RolesAreWellFormed(static_cast<std::uint32_t>(ListenerRole::Applications)));
	GYRO_CHECK(RolesAreWellFormed(KnownRoles));

	GYRO_CHECK(!RolesAreWellFormed(0));
	GYRO_CHECK(!RolesAreWellFormed(static_cast<std::uint32_t>(ListenerRole::Shell)));
	GYRO_CHECK(!RolesAreWellFormed(KnownRoles | 1U << 8U));
}

GYRO_TEST(Handover, RefusedCarriesCodeAndSentence)
{
	Datagram bytes{};
	const Refused sent{ .Code = 13, .Text = Reason{ "the offered descriptor is not a listening socket" } };
	const std::size_t written = sent.Encode(bytes);
	const std::optional<Refused> refused = Refused::Decode(std::span<const std::byte>{ bytes }.first(written));

	GYRO_REQUIRE_EQ(written, HeaderBytes + Refused::PayloadBytes);
	GYRO_REQUIRE(refused.has_value());
	GYRO_CHECK_EQ(refused->Code, std::uint32_t{ 13 });
	GYRO_CHECK(refused->Text.Text() == "the offered descriptor is not a listening socket");
}

// The padding is NUL and is not part of the sentence. Worth its own test because the obvious
// implementation hands the whole fixed field to `Reason`, which prints the padding as `?`.
GYRO_TEST(Handover, RefusedDoesNotDecodeItsOwnPadding)
{
	Datagram bytes{};
	const std::size_t written = Refused{ .Code = 1, .Text = Reason{ "no" } }.Encode(bytes);
	const std::optional<Refused> refused = Refused::Decode(std::span<const std::byte>{ bytes }.first(written));

	GYRO_REQUIRE(refused.has_value());
	GYRO_CHECK_EQ(refused->Text.Text().size(), std::size_t{ 2 });
}

GYRO_TEST(Handover, ReasonTruncatesAndFillsItsField)
{
	const std::string overlong(Reason::Capacity + 20, 'x');
	const Reason reason{ overlong };

	GYRO_CHECK_EQ(reason.Text().size(), Reason::Capacity);
}

// A sentence arriving from the other end reaches a log file, and a log file is a thing terminal
// escapes are read out of.
GYRO_TEST(Handover, ReasonKeepsOnlyPrintableAscii)
{
	const Reason reason{ std::string_view{ "clear\x1b[2Jhere" } };

	GYRO_CHECK(reason.Text() == "clear?[2Jhere");
}

// The direction and the descriptor count are part of the message rather than rules in the receiver,
// so they are checked here beside the layout.
GYRO_TEST(Handover, DirectionIsPartOfTheMessage)
{
	GYRO_CHECK(FromPeer(Opcode::Hello));
	GYRO_CHECK(FromPeer(Opcode::Offer));
	GYRO_CHECK(FromPeer(Opcode::Manage));
	GYRO_CHECK(FromPeer(Opcode::Assign));
	GYRO_CHECK(!FromPeer(Opcode::Welcome));
	GYRO_CHECK(!FromPeer(Opcode::Accepted));
	GYRO_CHECK(!FromPeer(Opcode::Refused));
	GYRO_CHECK(!FromPeer(Opcode::Managing));
	GYRO_CHECK(!FromPeer(Opcode::Assigned));
}

// The machine peer's half of the ABI, checked the same way and for the same reason: these are the
// values a release is diffed at, and never reusing one is the whole content of calling this an ABI.
GYRO_TEST(Handover, TheMachineOpcodesAreTheValuesTheyShipped)
{
	GYRO_CHECK_EQ(static_cast<std::uint16_t>(Opcode::Manage), std::uint16_t{ 6 });
	GYRO_CHECK_EQ(static_cast<std::uint16_t>(Opcode::Managing), std::uint16_t{ 7 });
	GYRO_CHECK_EQ(static_cast<std::uint16_t>(Opcode::Assign), std::uint16_t{ 8 });
	GYRO_CHECK_EQ(static_cast<std::uint16_t>(Opcode::Assigned), std::uint16_t{ 9 });

	GYRO_CHECK(IsOpcode(9));
	GYRO_CHECK(!IsOpcode(10));
	GYRO_CHECK(!IsOpcode(0));
}

GYRO_TEST(Handover, ClaimingTheMachinePacksToItsLayout)
{
	Datagram bytes{};

	// An opcode and a length, and nothing after them: what the peer may do is gyro's table rather than
	// anything negotiated here.
	GYRO_REQUIRE_EQ(Manage{}.Encode(bytes), std::size_t{ 4 });
	GYRO_CHECK_EQ(bytes[0], std::byte{ 0x06 });
	GYRO_CHECK_EQ(bytes[2], std::byte{ 0x00 });

	GYRO_REQUIRE_EQ(Managing{}.Encode(bytes), std::size_t{ 4 });
	GYRO_CHECK_EQ(bytes[0], std::byte{ 0x07 });

	GYRO_CHECK(Manage::Decode(std::span<const std::byte>{ ManageBytes }).has_value());
	GYRO_CHECK(Managing::Decode(std::span<const std::byte>{ ManagingBytes }).has_value());

	// A payload where the message has none is a peer speaking a dialect this build does not.
	GYRO_CHECK(!Manage::Decode(HelloBytes));
}

GYRO_TEST(Handover, AnAssignmentPacksToItsLayout)
{
	Datagram bytes{};
	const std::optional<ConnectorName> connector = ConnectorName::From("eDP-1");

	GYRO_REQUIRE(connector.has_value());

	const Assign assign{ .Uid = 1000, .Connector = *connector };

	GYRO_REQUIRE_EQ(assign.Encode(bytes), std::size_t{ 4 + 4 + ConnectorName::Capacity });
	GYRO_CHECK_EQ(bytes[0], std::byte{ 0x08 });
	GYRO_CHECK_EQ(bytes[2], std::byte{ 0x24 });
	GYRO_CHECK_EQ(bytes[4], std::byte{ 0xE8 });
	GYRO_CHECK_EQ(bytes[5], std::byte{ 0x03 });
	GYRO_CHECK_EQ(bytes[8], std::byte{ 'e' });

	// **Padding rather than a length**, which is what makes the field diffable: every byte past the
	// name is NUL, so two builds cannot disagree about where the name ends.
	GYRO_CHECK_EQ(bytes[8 + 5], std::byte{ 0x00 });
	GYRO_CHECK_EQ(bytes[8 + ConnectorName::Capacity - 1], std::byte{ 0x00 });

	const std::optional<Assign> read = Assign::Decode(std::span<const std::byte>{ bytes }.first(40));

	GYRO_REQUIRE(read.has_value());
	GYRO_CHECK_EQ(read->Uid, std::uint32_t{ 1000 });
	GYRO_CHECK(read->Connector.Text() == "eDP-1");
}

GYRO_TEST(Handover, AnAnsweredAssignmentEchoesTheRequest)
{
	Datagram bytes{};
	const std::optional<ConnectorName> connector = ConnectorName::From("DP-7");

	GYRO_REQUIRE(connector.has_value());
	GYRO_REQUIRE_EQ(Assigned{ .Uid = 42, .Connector = *connector }.Encode(bytes), std::size_t{ 40 });
	GYRO_CHECK_EQ(bytes[0], std::byte{ 0x09 });

	const std::optional<Assigned> read = Assigned::Decode(std::span<const std::byte>{ bytes }.first(40));

	GYRO_REQUIRE(read.has_value());
	GYRO_CHECK_EQ(read->Uid, std::uint32_t{ 42 });
	GYRO_CHECK(read->Connector.Text() == "DP-7");
}

// The empty name is every output, which is the request a login agent can make without first having
// discovered anything about the machine.
GYRO_TEST(Handover, AnEmptyConnectorIsEveryOutput)
{
	const std::optional<ConnectorName> empty = ConnectorName::From("");

	GYRO_REQUIRE(empty.has_value());
	GYRO_CHECK(empty->IsEveryOutput());
	GYRO_CHECK(empty->Text().empty());
	GYRO_CHECK(ConnectorName{}.IsEveryOutput());

	const std::optional<ConnectorName> named = ConnectorName::From("eDP-1");

	GYRO_REQUIRE(named.has_value());
	GYRO_CHECK(!named->IsEveryOutput());
}

// **Refused rather than truncated, which is where this differs from `Reason` on purpose.** A cut
// sentence is still the sentence; a cut connector name is a different screen or none, and the sender
// is the end that can still tell the difference.
GYRO_TEST(Handover, AConnectorNameThatWillNotFitIsRefused)
{
	GYRO_CHECK(ConnectorName::From(std::string(ConnectorName::Capacity, 'x')).has_value());
	GYRO_CHECK(!ConnectorName::From(std::string(ConnectorName::Capacity + 1, 'x')).has_value());
}

// Bytes no connector name has are a peer speaking something else, and a log line is a thing terminal
// escapes are read out of.
GYRO_TEST(Handover, AConnectorNameHoldsOnlyWhatAConnectorIsCalled)
{
	GYRO_CHECK(!ConnectorName::From("eDP 1").has_value());
	GYRO_CHECK(!ConnectorName::From(std::string_view{ "clear\x1b[2J" }).has_value());
	GYRO_CHECK(!ConnectorName::From(std::string_view{ "eDP\0001", 5 }).has_value());
}

// A field whose padding is not padding would make the echo in `Assigned` a lie: it would decode to one
// name and encode back as another.
GYRO_TEST(Handover, AConnectorFieldIsRefusedWhereItsPaddingIsNot)
{
	std::array<std::byte, ConnectorName::Capacity> field{};
	field[0] = std::byte{ 'e' };
	field[2] = std::byte{ 'x' };

	GYRO_CHECK(!DecodeConnector(field).has_value());

	field[2] = std::byte{ 0x00 };

	const std::optional<ConnectorName> read = DecodeConnector(field);

	GYRO_REQUIRE(read.has_value());
	GYRO_CHECK(read->Text() == "e");
}

GYRO_TEST(Handover, OnlyAnOfferCarriesADescriptor)
{
	GYRO_CHECK(CarriesListeners(Opcode::Offer));
	GYRO_CHECK(!CarriesListeners(Opcode::Hello));
	GYRO_CHECK(!CarriesListeners(Opcode::Welcome));
	GYRO_CHECK(!CarriesListeners(Opcode::Accepted));
	GYRO_CHECK(!CarriesListeners(Opcode::Refused));
}

// A buffer that will not hold the message is answered with zero rather than a partial write, since a
// short datagram is a message the far end will refuse and the sender is the one that can still tell
// the difference.
GYRO_TEST(Handover, EncodeRefusesABufferItWouldOverrun)
{
	std::array<std::byte, 7> cramped{};

	GYRO_CHECK_EQ(Hello{}.Encode(cramped), std::size_t{ 0 });
	GYRO_CHECK_EQ(Welcome{}.Encode(cramped), std::size_t{ 0 });
	GYRO_CHECK_EQ(Refused{}.Encode(cramped), std::size_t{ 0 });
	GYRO_CHECK_EQ(Offer{}.Encode(cramped), std::size_t{ 0 });
	GYRO_CHECK_EQ(Assign{}.Encode(cramped), std::size_t{ 0 });
	GYRO_CHECK_EQ(Assigned{}.Encode(cramped), std::size_t{ 0 });

	std::array<std::byte, HeaderBytes - 1> tiny{};

	GYRO_CHECK_EQ(Offer{}.Encode(tiny), std::size_t{ 0 });
	GYRO_CHECK_EQ(Manage{}.Encode(tiny), std::size_t{ 0 });
	GYRO_CHECK_EQ(Managing{}.Encode(tiny), std::size_t{ 0 });
}
