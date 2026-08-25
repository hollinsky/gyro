#include "Trace/Protobuf.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Testing/Test.h"

namespace
{

[[nodiscard]] std::string Hex(std::span<const std::byte> bytes)
{
	constexpr char Digits[] = "0123456789abcdef";
	std::string text;

	for (const std::byte value : bytes)
	{
		text.push_back(Digits[(static_cast<unsigned>(value) >> 4) & 0xFU]);
		text.push_back(Digits[static_cast<unsigned>(value) & 0xFU]);
	}

	return text;
}

} // namespace

GYRO_TEST(Protobuf, TheCanonicalVarint)
{
	// protobuf's own worked example: field 1, varint, 150. If this is wrong nothing downstream can be
	// right, and it is the one case a reader can check against the specification by eye.
	ProtoWriter writer;
	writer.Varint(1, 150);

	GYRO_CHECK_EQ(Hex(writer.View()), std::string{ "089601" });
}

GYRO_TEST(Protobuf, ASmallValueIsOneByte)
{
	ProtoWriter writer;
	writer.Varint(9, 1);

	GYRO_CHECK_EQ(Hex(writer.View()), std::string{ "4801" });
}

GYRO_TEST(Protobuf, AFieldNumberPastFifteenTakesTwoTagBytes)
{
	// Perfetto's field numbers run into the hundreds — `track_descriptor` is 60 — so the tag itself is
	// a varint and a writer that assumed one byte would encode the wrong field rather than fail.
	ProtoWriter writer;
	writer.Varint(60, 0);

	GYRO_CHECK_EQ(Hex(writer.View()), std::string{ "e00300" });
}

GYRO_TEST(Protobuf, ANegativeSignedFieldIsTenBytes)
{
	ProtoWriter writer;
	writer.Signed(1, -1);

	GYRO_CHECK_EQ(Hex(writer.View()), std::string{ "08ffffffffffffffffff01" });
}

GYRO_TEST(Protobuf, AFixedSixtyFourIsLittleEndian)
{
	ProtoWriter writer;
	writer.Fixed64(47, 0x0102030405060708ULL);

	GYRO_CHECK_EQ(Hex(writer.View()), std::string{ "f9020807060504030201" });
}

GYRO_TEST(Protobuf, TextCarriesItsLength)
{
	ProtoWriter writer;
	writer.Text(2, "gyro");

	GYRO_CHECK_EQ(Hex(writer.View()), std::string{ "12046779726f" });
}

GYRO_TEST(Protobuf, ANestedMessageCarriesItsLength)
{
	ProtoWriter inner;
	inner.Varint(1, 150);

	ProtoWriter outer;
	outer.Nested(3, inner);

	GYRO_CHECK_EQ(Hex(outer.View()), std::string{ "1a03089601" });
}

GYRO_TEST(Protobuf, AnEmptyNestedMessageIsStillPresent)
{
	// Field presence is the whole reason: `track_event { }` with nothing in it is what says an event
	// exists, and a writer that dropped an empty submessage would silently drop those.
	ProtoWriter inner;
	ProtoWriter outer;
	outer.Nested(11, inner);

	GYRO_CHECK_EQ(Hex(outer.View()), std::string{ "5a00" });
}

GYRO_TEST(Protobuf, ClearReusesTheWriter)
{
	ProtoWriter writer;
	writer.Varint(1, 150);
	writer.Clear();

	GYRO_CHECK_EQ(writer.Size(), std::size_t{ 0 });

	writer.Varint(1, 150);

	GYRO_CHECK_EQ(Hex(writer.View()), std::string{ "089601" });
}

// The reader's own tests, which are round trips against the writer above wherever a round trip can
// say it — and hand-built bytes wherever the interesting case is one the writer cannot produce. Both
// halves matter: the round trips are the guarantee the pair agrees, and the malformed inputs are the
// guarantee that a file somebody else wrote cannot walk this off the end of a buffer.

GYRO_TEST(Protobuf, EveryWireTypeSurvivesTheRoundTrip)
{
	ProtoWriter inner;
	inner.Varint(1, 7);

	ProtoWriter writer;
	writer.Varint(1, 150);
	writer.Fixed64(2, 0xDEADBEEFCAFEBABEULL);
	writer.Text(3, "frame");
	writer.Nested(4, inner);

	ProtoReader reader{ writer.View() };

	const std::optional<ProtoField> varint = reader.Next();
	GYRO_REQUIRE(varint.has_value());
	GYRO_CHECK_EQ(varint->Number, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(varint->Value, std::uint64_t{ 150 });

	const std::optional<ProtoField> fixed = reader.Next();
	GYRO_REQUIRE(fixed.has_value());
	GYRO_CHECK_EQ(fixed->Wire, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(fixed->Value, std::uint64_t{ 0xDEADBEEFCAFEBABEULL });

	const std::optional<ProtoField> text = reader.Next();
	GYRO_REQUIRE(text.has_value());
	GYRO_CHECK_EQ(std::string{ ProtoText(*text) }, std::string{ "frame" });

	const std::optional<ProtoField> nested = reader.Next();
	GYRO_REQUIRE(nested.has_value());

	const std::optional<ProtoField> within = ProtoFind(nested->Bytes, 1);
	GYRO_REQUIRE(within.has_value());
	GYRO_CHECK_EQ(within->Value, std::uint64_t{ 7 });

	GYRO_CHECK(reader.Done());
}

GYRO_TEST(Protobuf, FindAllKeepsTheOrderRepeatedMeans)
{
	ProtoWriter writer;
	writer.Fixed64(47, 11);
	writer.Varint(9, 1);
	writer.Fixed64(47, 22);
	writer.Fixed64(47, 33);

	const std::vector<ProtoField> flows = ProtoFindAll(writer.View(), 47);

	GYRO_REQUIRE(flows.size() == std::size_t{ 3 });
	GYRO_CHECK_EQ(flows[0].Value, std::uint64_t{ 11 });
	GYRO_CHECK_EQ(flows[1].Value, std::uint64_t{ 22 });
	GYRO_CHECK_EQ(flows[2].Value, std::uint64_t{ 33 });
}

GYRO_TEST(Protobuf, AFixed32IsSkippedByItsOwnWidth)
{
	// Nothing gyro writes is `fixed32`, and a system trace concatenated in front of one of these is
	// full of them. Skipping it as eight bytes would not lose that field, it would lose every field
	// after it — which is a merged trace where gyro's half is simply absent.
	const std::vector<std::byte> bytes{
		std::byte{ 0x0D }, std::byte{ 0x01 }, std::byte{ 0x02 }, std::byte{ 0x03 },
		std::byte{ 0x04 }, std::byte{ 0x10 }, std::byte{ 0x2A },
	};

	ProtoReader reader{ bytes };

	const std::optional<ProtoField> fixed = reader.Next();
	GYRO_REQUIRE(fixed.has_value());
	GYRO_CHECK_EQ(fixed->Wire, std::uint32_t{ 5 });
	GYRO_CHECK_EQ(fixed->Value, std::uint64_t{ 0x04030201 });

	const std::optional<ProtoField> after = reader.Next();
	GYRO_REQUIRE(after.has_value());
	GYRO_CHECK_EQ(after->Number, std::uint32_t{ 2 });
	GYRO_CHECK_EQ(after->Value, std::uint64_t{ 42 });
}

GYRO_TEST(Protobuf, ALengthPastTheEndIsRefused)
{
	// The shape a truncated file takes, and the one that matters most: `m_At + length` on a length
	// near the top of the range wraps, and the subspan then reads whatever follows the buffer.
	const std::vector<std::byte> bytes{ std::byte{ 0x1A }, std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0xFF },
		                                std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0xFF },
		                                std::byte{ 0xFF }, std::byte{ 0x7F } };

	ProtoReader reader{ bytes };

	GYRO_CHECK(!reader.Next().has_value());
}

GYRO_TEST(Protobuf, ARunOfZeroesEndsTheWalk)
{
	// Field zero is not legal, and zero padding decodes as an endless run of empty varints. A reader
	// that accepted it would never finish on a buffer that was zero filled.
	const std::vector<std::byte> bytes{ std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0x00 } };

	ProtoReader reader{ bytes };

	GYRO_CHECK(!reader.Next().has_value());
}

GYRO_TEST(Protobuf, AGroupIsRefusedRatherThanSkipped)
{
	// Wire types three and four are proto2's groups, which carry no length and can only be skipped by
	// understanding them. Treating one as anything else loses the framing, not just the field.
	const std::vector<std::byte> bytes{ std::byte{ 0x0B }, std::byte{ 0x08 }, std::byte{ 0x01 } };

	ProtoReader reader{ bytes };

	GYRO_CHECK(!reader.Next().has_value());
}

GYRO_TEST(Protobuf, AVarintWithNoEndIsRefused)
{
	const std::vector<std::byte> bytes{ std::byte{ 0x08 }, std::byte{ 0x80 }, std::byte{ 0x80 } };

	ProtoReader reader{ bytes };

	GYRO_CHECK(!reader.Next().has_value());
}

GYRO_TEST(Protobuf, AMissingFieldIsAbsentRatherThanZero)
{
	ProtoWriter writer;
	writer.Varint(1, 0);

	GYRO_CHECK(ProtoFind(writer.View(), 1).has_value());
	GYRO_CHECK(!ProtoFind(writer.View(), 2).has_value());
	GYRO_CHECK(ProtoFindAll(writer.View(), 2).empty());
}
