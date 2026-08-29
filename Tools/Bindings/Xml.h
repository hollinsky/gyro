#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include "Core/Result.h"
#include "Protocol.h"

// The XML front end: a document in, a Protocol out, and a sentence naming the spot when neither.
//
// Decision 2 leaves the server codec to libwayland but keeps the *bindings* gyro's, generated at
// build time by a host tool with a hand-rolled parser — no scripting-language dependency, no
// third-party XML library, no generated code in the tree. This is that parser, and the subset it
// accepts is exactly /usr/share/wayland/wayland.dtd: nine elements, their attributes, and nothing
// else. The DTD is worth naming rather than paraphrasing, because it is the only place the shape of
// a protocol document is actually written down, and encoding it from the corpus instead would mean
// accepting whatever the protocols happen to contain today.
//
// **Nothing outside the subset is skipped.** An unknown element, an unknown attribute, a `type=`
// this does not recognise, a DOCTYPE that could redefine an entity — each stops the parse. The
// alternative is a generator that emits a binding with a hole in it: a request whose opcode is
// right and whose arguments are short by one, which is a protocol error the client sees as a
// disconnect and the compositor sees as nothing at all. A protocol we cannot read has to fail the
// build, at the file and the line, while somebody is looking at it.
//
// **The diagnostic is a second channel, and it has to be.** Core/Result.h's Error carries a
// std::string_view over static storage, which is what keeps it trivially copyable and what makes it
// the right type for a syscall wrapper naming a fixed operation. A parse failure is the opposite
// shape: the useful half of "unexpected element <foo> at 12:5" is the part that is different every
// time, and it cannot live behind a string_view without dangling. So the Error says which of five
// things went wrong, in a literal chosen from a fixed set — the one variable case Result.h allows —
// and the Diagnostic carries the offending name and the position. The Error alone is enough to
// branch on; the Diagnostic is what a build failure prints.

// What kind of mistake the document made. A test asserts on this rather than on the message text,
// which is prose and will be reworded; each value is a distinct thing for the protocol's author to
// have done, and a distinct thing for the reader to go and fix.
enum class ParseFailure : std::uint8_t
{
	// Not XML at all: an unterminated tag, a close that does not match its open, an attribute value
	// with no quotes, an entity reference this does not know, content after the root element.
	NotWellFormed,

	// Well-formed XML the DTD has no room for: an element that is not one of the nine, or one of the
	// nine somewhere it may not appear.
	UnexpectedElement,

	// An attribute the element does not declare. Spelling `allow_null` for `allow-null` lands here,
	// which is the reason this is an error and not a shrug.
	UnexpectedAttribute,

	// An attribute the element requires and the document omitted.
	MissingAttribute,

	// The attribute is spelled right and its text is not one of the things it may say: a `type=` no
	// argument has, a `since=` that is not a number, a value that does not fit 32 bits.
	InvalidValue,
};

[[nodiscard]] constexpr const char* Name(ParseFailure failure) noexcept
{
	switch (failure)
	{
		case ParseFailure::NotWellFormed:
			return "not well-formed XML";
		case ParseFailure::UnexpectedElement:
			return "an element outside the protocol subset";
		case ParseFailure::UnexpectedAttribute:
			return "an attribute the element does not declare";
		case ParseFailure::MissingAttribute:
			return "a required attribute is missing";
		case ParseFailure::InvalidValue:
			return "an attribute value the parser does not accept";
	}

	return "?";
}

// Where the parse gave up and what it was looking at.
//
// Line and Column are 1-based and count characters rather than bytes, so a column stays honest
// after the `©` in a copyright notice. Both are computed only when the parse has already failed,
// which is what keeps the scanner from carrying them.
struct Diagnostic
{
	ParseFailure Failure = ParseFailure::NotWellFormed;
	std::string Message;
	std::size_t Line = 1;
	std::size_t Column = 1;
};

// Prints as `12:5: unexpected element <foo>`. The file name is the caller's to prepend, since this
// module never opens one.
template<>
struct std::formatter<Diagnostic>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const Diagnostic& diagnostic, Context& context) const
	{
		return std::format_to(context.out(), "{}:{}: {}", diagnostic.Line, diagnostic.Column, diagnostic.Message);
	}
};

// Parses a whole document. The view is borrowed for the duration and nothing in the result points
// into it — every string in a Protocol is owned, because entity references and whitespace
// normalisation mean the text the emitter wants is frequently not text the document contains.
//
// `diagnostic` is filled only on failure, and is optional so that the one-argument call — the one an
// emitter writes — is still the whole interface. Pass it wherever the message is going to be read
// by a person, which for a build tool is everywhere.
[[nodiscard]] Result<Protocol> ParseProtocol(std::string_view xml, Diagnostic* diagnostic = nullptr);
