#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// UTF-8 in, code points out, and nothing else.
//
// Hand-rolled rather than reached for, because what is being decoded is a compositor's own strings —
// a log line, a label, a passphrase prompt — and the alternative is a locale-dependent facet whose
// behaviour depends on the environment of a process that has no environment worth speaking of. It is
// eighty lines and its whole surface is one function.
//
// **Malformed input decodes to U+FFFD and advances one byte.** A recovery console prints whatever it
// was handed, including a truncated buffer and a byte from a file that is not text at all, and a
// decoder that stopped or looped there would take the console down at exactly the moment somebody
// needs it. Overlong forms, surrogates and values past U+10FFFF are all malformed by that rule, which
// is what stops a crafted string from addressing a glyph run it should not reach.

struct Utf8Code
{
	char32_t Code;

	// Bytes consumed, never zero — which is what makes a loop over this terminate whatever the input.
	std::size_t Length;
};

[[nodiscard]] constexpr Utf8Code DecodeUtf8(std::string_view text) noexcept
{
	constexpr char32_t Replacement = 0xFFFD;

	if (text.empty())
	{
		return { Replacement, 1 };
	}

	const auto byte = [&text](std::size_t index) { return static_cast<std::uint8_t>(text[index]); };

	const std::uint8_t lead = byte(0);

	if (lead < 0x80)
	{
		return { static_cast<char32_t>(lead), 1 };
	}

	std::size_t length = 0;
	char32_t code = 0;

	if ((lead & 0xE0) == 0xC0)
	{
		length = 2;
		code = static_cast<char32_t>(lead & 0x1F);
	}
	else if ((lead & 0xF0) == 0xE0)
	{
		length = 3;
		code = static_cast<char32_t>(lead & 0x0F);
	}
	else if ((lead & 0xF8) == 0xF0)
	{
		length = 4;
		code = static_cast<char32_t>(lead & 0x07);
	}
	else
	{
		// A continuation byte with no lead, or one of the two lead bytes UTF-8 never uses.
		return { Replacement, 1 };
	}

	if (text.size() < length)
	{
		return { Replacement, 1 };
	}

	for (std::size_t index = 1; index < length; ++index)
	{
		if ((byte(index) & 0xC0) != 0x80)
		{
			return { Replacement, 1 };
		}

		code = static_cast<char32_t>((code << 6) | (byte(index) & 0x3F));
	}

	// The three ways a well-shaped sequence still says nothing: a value that had a shorter spelling,
	// a UTF-16 surrogate half, and a value outside Unicode. All three are how a decoder that only
	// checked the shape gets talked into producing a code point the caller then trusts.
	const bool overlong =
		(length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && code < 0x10000);

	if (overlong || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)
	{
		return { Replacement, 1 };
	}

	return { code, length };
}
