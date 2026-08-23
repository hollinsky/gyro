#include "Wire/Message.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>

#include "Testing/Test.h"

// The wire vocabulary, which is mostly arithmetic and is mostly checked at compile time in the header
// itself. What is here is the part a static_assert cannot reach — the byte-level layout, which is the
// thing an endianness assumption or an off-by-one shift would quietly get wrong.

namespace
{
using namespace Wire;

// The two header words, laid out by hand, so that the packing is checked against a literal rather
// than against the function that produced it.
constexpr std::array<std::byte, HeaderBytes> Sample{
	std::byte{ 0x07 }, std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0x00 },
	std::byte{ 0x03 }, std::byte{ 0x00 }, std::byte{ 0x0c }, std::byte{ 0x00 },
};
} // namespace

GYRO_TEST(Message, HeaderUnpacks)
{
	const MessageHeader header = ReadHeader(Sample);

	GYRO_CHECK_EQ(header.Target, ObjectId{ 7 });
	GYRO_CHECK_EQ(header.Opcode, std::uint16_t{ 3 });
	GYRO_CHECK_EQ(header.Size, std::uint16_t{ 12 });
	GYRO_CHECK(header.IsWellFormed());
}

GYRO_TEST(Message, HeaderPacks)
{
	std::array<std::byte, HeaderBytes> bytes{};

	StoreWord(bytes, 0, 7);
	StoreWord(bytes, 4, PackSizeAndOpcode(12, 3));

	GYRO_CHECK(bytes == Sample);
}

// A size that is not a whole number of words, or one that does not even cover the header, is what
// Wire/Connection.h refuses rather than waits for. Waiting is the failure mode worth the test: a
// message claiming eleven bytes never becomes complete, so a connection that treated it as short
// would stop dispatching and never say why.
GYRO_TEST(Message, MalformedSizesAreRejected)
{
	std::array<std::byte, HeaderBytes> bytes{};
	StoreWord(bytes, 0, 7);

	StoreWord(bytes, 4, PackSizeAndOpcode(11, 3));
	GYRO_CHECK(!ReadHeader(bytes).IsWellFormed());

	StoreWord(bytes, 4, PackSizeAndOpcode(4, 3));
	GYRO_CHECK(!ReadHeader(bytes).IsWellFormed());

	StoreWord(bytes, 4, PackSizeAndOpcode(0, 3));
	GYRO_CHECK(!ReadHeader(bytes).IsWellFormed());

	StoreWord(bytes, 4, PackSizeAndOpcode(8, 3));
	GYRO_CHECK(ReadHeader(bytes).IsWellFormed());
}

// The largest message the size field can describe. Worth pinning because the neighbouring value is
// not a whole number of words, so the cap and the alignment rule interact.
GYRO_TEST(Message, SizeFieldCap)
{
	GYRO_CHECK_EQ(MaxMessageBytes, std::size_t{ 65532 });
	GYRO_CHECK_EQ(PackSizeAndOpcode(MaxMessageBytes, 0xffff) >> 16, std::uint32_t{ 65532 });
	GYRO_CHECK_EQ(PackSizeAndOpcode(MaxMessageBytes, 0xffff) & 0xffffU, std::uint32_t{ 65535 });
}

// The 24.8 conversions, at the boundaries the arithmetic is actually different at. `-1` truncating to
// zero rather than to -1 is the one that matters: a pointer leaving a surface to the left produces
// exactly this, and the two answers differ by a pixel on every such event.
GYRO_TEST(Message, FixedRoundTrips)
{
	GYRO_CHECK_EQ(Fixed::FromDouble(0.0).Raw(), 0);
	GYRO_CHECK_EQ(Fixed::FromDouble(1.0 / 256.0).Raw(), 1);
	GYRO_CHECK_EQ(Fixed::FromDouble(-1.0 / 256.0).Raw(), -1);
	GYRO_CHECK_EQ(Fixed::FromRaw(1).ToDouble(), 1.0 / 256.0);

	GYRO_CHECK_EQ(Fixed::FromInt(-3).Raw(), -768);
	GYRO_CHECK_EQ(Fixed::FromRaw(-768).ToInt(), -3);
	GYRO_CHECK_EQ(Fixed::FromRaw(-255).ToInt(), 0);
	GYRO_CHECK_EQ(Fixed::FromRaw(255).ToInt(), 0);

	// Rounds to nearest rather than truncating, so a value that came from a double survives the
	// return trip.
	GYRO_CHECK_EQ(Fixed::FromDouble(12.3).ToDouble(), Fixed::FromRaw(Fixed::FromDouble(12.3).Raw()).ToDouble());
	GYRO_CHECK_EQ(Fixed::FromDouble(12.3).Raw(), 3149);
	GYRO_CHECK_EQ(Fixed::FromDouble(-12.3).Raw(), -3149);
}

GYRO_TEST(Message, IdsPrint)
{
	GYRO_CHECK_EQ(std::format("{}", ObjectId::None), "object none");
	GYRO_CHECK_EQ(std::format("{}", ObjectId::Display), "object 1");
	GYRO_CHECK_EQ(std::format("{}", Fixed::FromInt(3)), "3");
	GYRO_CHECK_EQ(std::format("{}", Fixed::FromDouble(1.5)), "1.5");
}
