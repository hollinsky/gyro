#include "Text/Utf8.h"

#include "Testing/Test.h"

GYRO_TEST(Utf8, DecodesEachLength)
{
	GYRO_CHECK(DecodeUtf8("A").Code == U'A');
	GYRO_CHECK(DecodeUtf8("A").Length == 1);
	GYRO_CHECK(DecodeUtf8("\xC3\xA9").Code == U'é');
	GYRO_CHECK(DecodeUtf8("\xC3\xA9").Length == 2);
	GYRO_CHECK(DecodeUtf8("\xE2\x94\x80").Code == U'─');
	GYRO_CHECK(DecodeUtf8("\xE2\x94\x80").Length == 3);
	GYRO_CHECK(DecodeUtf8("\xF0\x9F\x92\xA9").Code == 0x1F4A9);
	GYRO_CHECK(DecodeUtf8("\xF0\x9F\x92\xA9").Length == 4);
}

GYRO_TEST(Utf8, AlwaysAdvances)
{
	// The property the recovery console rests on: whatever it was handed — a truncated buffer, a byte
	// out of a file that is not text — the loop over it terminates.
	for (int byte = 0; byte < 256; ++byte)
	{
		const char character = static_cast<char>(byte);

		GYRO_CHECK(DecodeUtf8(std::string_view{ &character, 1 }).Length >= 1);
	}

	GYRO_CHECK(DecodeUtf8("").Length == 1);
}

GYRO_TEST(Utf8, RefusesTheThreeThingsAWellShapedSequenceCanStillBe)
{
	constexpr char32_t Replacement = 0xFFFD;

	// Overlong: 'A' spelled in two bytes, which is how a check on the shape alone gets talked into
	// producing a code point the caller then trusts.
	GYRO_CHECK(DecodeUtf8("\xC1\x81").Code == Replacement);

	// A UTF-16 surrogate half, which is not a character.
	GYRO_CHECK(DecodeUtf8("\xED\xA0\x80").Code == Replacement);

	// Past U+10FFFF.
	GYRO_CHECK(DecodeUtf8("\xF7\xBF\xBF\xBF").Code == Replacement);

	// A continuation byte with no lead, and a lead with no continuation.
	GYRO_CHECK(DecodeUtf8("\x80").Code == Replacement);
	GYRO_CHECK(DecodeUtf8("\xE2\x94").Code == Replacement);
}
