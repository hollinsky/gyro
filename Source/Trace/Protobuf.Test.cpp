#include "Trace/Protobuf.h"

#include <cstddef>
#include <cstdint>
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
