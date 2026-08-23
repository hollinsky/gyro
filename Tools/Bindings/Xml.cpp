#include "Xml.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "Core/Result.h"
#include "Protocol.h"

// A recursive-descent parse of the DTD, written out rather than driven by a table.
//
// The subset is nine elements deep in three or four levels, and every one of them has a different
// set of attributes with different types behind them. A generic XML tree, walked afterwards to pull
// the model out of it, would be the same code split in half with a `std::string`-keyed map in the
// middle — and would move every "this attribute does not belong here" from the place that knows the
// answer to a second pass that has to be taught it. The DTD is small enough to be the program.
//
// The one structural idea is that attributes are *taken*: each element's parse asks for the ones it
// declares by name, and whatever is left when it stops asking is an error. That is what makes an
// unknown attribute free to detect and impossible to forget, and it is why `allow_null` for
// `allow-null` fails the build instead of quietly leaving a pointer non-null.

namespace
{
constexpr std::string_view Utf8ByteOrderMark = "\xEF\xBB\xBF";

constexpr bool IsSpace(char character) noexcept
{
	return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

constexpr bool IsNameStart(char character) noexcept
{
	return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_' ||
	       character == ':';
}

constexpr bool IsNameChar(char character) noexcept
{
	return IsNameStart(character) || (character >= '0' && character <= '9') || character == '-' || character == '.';
}

void AppendUtf8(std::string& text, std::uint32_t codepoint)
{
	if (codepoint < 0x80)
	{
		text.push_back(static_cast<char>(codepoint));
	}
	else if (codepoint < 0x800)
	{
		text.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
		text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	}
	else if (codepoint < 0x10000)
	{
		text.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
		text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
		text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	}
	else
	{
		text.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
		text.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
		text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
		text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
	}
}

// A description body as it has to read once it is a comment in a generated header: trailing
// whitespace gone, the blank lines around it gone, and the indentation the XML author used to line
// the paragraph up under its tag removed from every line at once.
//
// The common prefix is removed rather than all leading whitespace, because the protocols indent
// list items and code samples relative to the paragraph and losing that turns a bulleted list into
// one run-on sentence.
std::string Normalise(std::string_view text)
{
	std::vector<std::string_view> lines;
	for (std::size_t start = 0; start <= text.size();)
	{
		const std::size_t newline = text.find('\n', start);
		const std::size_t stop = (newline == std::string_view::npos) ? text.size() : newline;

		std::string_view line = text.substr(start, stop - start);
		while (!line.empty() && IsSpace(line.back()))
		{
			line.remove_suffix(1);
		}
		lines.push_back(line);

		if (newline == std::string_view::npos)
		{
			break;
		}
		start = newline + 1;
	}

	while (!lines.empty() && lines.front().empty())
	{
		lines.erase(lines.begin());
	}
	while (!lines.empty() && lines.back().empty())
	{
		lines.pop_back();
	}

	std::size_t indent = std::string_view::npos;
	for (const std::string_view line : lines)
	{
		if (!line.empty())
		{
			indent = std::min(indent, line.find_first_not_of(" \t"));
		}
	}
	if (indent == std::string_view::npos)
	{
		indent = 0;
	}

	std::string result;
	for (std::size_t index = 0; index < lines.size(); ++index)
	{
		if (index != 0)
		{
			result.push_back('\n');
		}
		result.append(lines[index].substr(std::min(indent, lines[index].size())));
	}
	return result;
}

// One attribute, with the offset of its name kept so a bad value points at the attribute rather
// than at the element it was written on.
struct Attribute
{
	std::string_view Name;
	std::string Value;
	std::size_t Offset = 0;
};

// A start tag, after its attributes have been read and before its children have been. `Empty` is
// the `<arg .../>` form, which is most of the corpus.
struct Tag
{
	std::string_view Name;
	std::vector<Attribute> Attributes;
	bool Empty = false;
	std::size_t Offset = 0;
};

class Parser
{
public:
	Parser(std::string_view xml, Diagnostic& diagnostic) noexcept : m_Xml{ xml }, m_Diagnostic{ diagnostic } {}

	[[nodiscard]] Result<Protocol> Run();

private:
	// Scanning. The cursor is a byte offset and carries no line number: the position is wanted once
	// per document at most, and reconstructing it from the offset costs a scan nobody pays for.
	[[nodiscard]] bool AtEnd() const noexcept { return m_Offset >= m_Xml.size(); }
	[[nodiscard]] char Peek(std::size_t ahead = 0) const noexcept
	{
		return (m_Offset + ahead < m_Xml.size()) ? m_Xml[m_Offset + ahead] : '\0';
	}
	[[nodiscard]] bool Looking(std::string_view text) const noexcept
	{
		return m_Xml.compare(m_Offset, text.size(), text) == 0;
	}
	void SkipSpace() noexcept
	{
		while (!AtEnd() && IsSpace(m_Xml[m_Offset]))
		{
			++m_Offset;
		}
	}

	[[nodiscard]] std::unexpected<Error> Fail(ParseFailure failure, std::string message, std::size_t offset);

	// A comment, from the `<!--` the caller has already looked at. Separate from SkipMisc because
	// it is also reached from inside character data, where the whitespace around the comment is
	// content and must not be eaten with it.
	[[nodiscard]] Result<void> SkipComment();

	// Whitespace and comments, which are legal anywhere an element may appear.
	[[nodiscard]] Result<void> SkipMisc();

	[[nodiscard]] Result<std::string_view> ReadName(std::string_view what);
	[[nodiscard]] Result<std::string> ReadQuoted();
	[[nodiscard]] Result<void> AppendReference(std::string& text);
	[[nodiscard]] Result<Tag> ReadStartTag();
	[[nodiscard]] Result<void> ReadEndTag(const Tag& tag);

	// Character data up to the element's end tag, entity references resolved and whitespace
	// normalised. Refuses a child element, which is what `(#PCDATA)` means.
	[[nodiscard]] Result<std::string> ReadText(const Tag& tag);

	[[nodiscard]] std::optional<Attribute> Take(Tag& tag, std::string_view name);
	[[nodiscard]] Result<Attribute> Require(Tag& tag, std::string_view name);
	[[nodiscard]] Result<void> FinishAttributes(const Tag& tag);

	[[nodiscard]] Result<int> ToVersion(const Attribute& attribute);
	[[nodiscard]] Result<int> TakeVersion(Tag& tag, std::string_view name, int fallback);
	[[nodiscard]] Result<bool> TakeFlag(Tag& tag, std::string_view name);
	[[nodiscard]] std::string TakeText(Tag& tag, std::string_view name);

	// Walks the children of an already-read start tag, handing each child's start tag to `handler`
	// and consuming the end tag itself. Every container element in the DTD is one call to this.
	template<typename Handler>
	[[nodiscard]] Result<void> ParseChildren(const Tag& tag, Handler handler);

	[[nodiscard]] Result<void> ParseDescription(Tag& tag, const Tag& parent, std::string& summary, std::string& body);
	[[nodiscard]] Result<void> ParseProtocolElement(Tag& tag, Protocol& protocol);
	[[nodiscard]] Result<void> ParseInterfaceElement(Tag& tag, Protocol& protocol);
	[[nodiscard]] Result<void> ParseMessageElement(Tag& tag, std::vector<Message>& messages);
	[[nodiscard]] Result<void> ParseArgumentElement(Tag& tag, std::vector<Argument>& arguments);
	[[nodiscard]] Result<void> ParseEnumerationElement(Tag& tag, std::vector<Enumeration>& enumerations);
	[[nodiscard]] Result<void> ParseEntryElement(Tag& tag, std::vector<EnumerationEntry>& entries);

	[[nodiscard]] std::unexpected<Error> Unexpected(const Tag& child, const Tag& parent);

	std::string_view m_Xml;
	Diagnostic& m_Diagnostic;
	std::size_t m_Offset = 0;
};

std::unexpected<Error> Parser::Fail(ParseFailure failure, std::string message, std::size_t offset)
{
	m_Diagnostic.Failure = failure;
	m_Diagnostic.Message = std::move(message);

	// Characters rather than bytes, so a column stays honest after the `©` every copyright notice
	// opens with: a UTF-8 continuation byte is not a character the author typed.
	std::size_t line = 1;
	std::size_t column = 1;
	for (std::size_t index = 0; index < std::min(offset, m_Xml.size()); ++index)
	{
		if (m_Xml[index] == '\n')
		{
			++line;
			column = 1;
		}
		else if ((static_cast<unsigned char>(m_Xml[index]) & 0xC0) != 0x80)
		{
			++column;
		}
	}
	m_Diagnostic.Line = line;
	m_Diagnostic.Column = column;

	// The context is a literal from the fixed set Name() switches over, which is the one variable
	// case Core/Result.h's static-storage obligation admits. Everything that is different every
	// time is in the diagnostic beside it.
	return Failure(EINVAL, Name(failure));
}

Result<void> Parser::SkipComment()
{
	const std::size_t start = m_Offset;
	const std::size_t end = m_Xml.find("-->", m_Offset + 4);
	if (end == std::string_view::npos)
	{
		return Fail(ParseFailure::NotWellFormed, "a comment that is never closed", start);
	}
	m_Offset = end + 3;
	return {};
}

Result<void> Parser::SkipMisc()
{
	for (;;)
	{
		SkipSpace();
		if (!Looking("<!--"))
		{
			return {};
		}
		if (Result<void> comment = SkipComment(); !comment)
		{
			return comment;
		}
	}
}

Result<std::string_view> Parser::ReadName(std::string_view what)
{
	const std::size_t start = m_Offset;
	if (AtEnd() || !IsNameStart(Peek()))
	{
		return Fail(ParseFailure::NotWellFormed, std::format("expected {}", what), start);
	}

	while (!AtEnd() && IsNameChar(Peek()))
	{
		++m_Offset;
	}
	return m_Xml.substr(start, m_Offset - start);
}

Result<void> Parser::AppendReference(std::string& text)
{
	const std::size_t start = m_Offset;
	const std::size_t semicolon = m_Xml.find(';', m_Offset);
	if (semicolon == std::string_view::npos)
	{
		return Fail(ParseFailure::NotWellFormed, "an entity reference with no closing ';'", start);
	}

	const std::string_view body = m_Xml.substr(start + 1, semicolon - start - 1);
	m_Offset = semicolon + 1;

	if (body == "amp")
	{
		text.push_back('&');
		return {};
	}
	if (body == "lt")
	{
		text.push_back('<');
		return {};
	}
	if (body == "gt")
	{
		text.push_back('>');
		return {};
	}
	if (body == "quot")
	{
		text.push_back('"');
		return {};
	}
	if (body == "apos")
	{
		text.push_back('\'');
		return {};
	}

	// The five above are the only ones XML predefines, and this parser refuses a DOCTYPE, so there
	// is nowhere a document could have declared a sixth. A numeric reference is always available.
	if (body.starts_with('#'))
	{
		const bool hexadecimal = body.size() > 1 && (body[1] == 'x' || body[1] == 'X');
		const std::string_view digits = body.substr(hexadecimal ? 2 : 1);

		std::uint32_t codepoint = 0;
		const char* const first = digits.data();
		const char* const last = first + digits.size();
		const std::from_chars_result parsed = std::from_chars(first, last, codepoint, hexadecimal ? 16 : 10);
		if (parsed.ec != std::errc{} || parsed.ptr != last || digits.empty())
		{
			return Fail(ParseFailure::NotWellFormed, std::format("'&{};' is not a number", body), start);
		}
		if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
		{
			return Fail(ParseFailure::NotWellFormed, std::format("'&{};' is not a character", body), start);
		}

		AppendUtf8(text, codepoint);
		return {};
	}

	return Fail(ParseFailure::NotWellFormed, std::format("unknown entity reference '&{};'", body), start);
}

Result<std::string> Parser::ReadQuoted()
{
	const std::size_t start = m_Offset;
	const char quote = Peek();
	if (quote != '"' && quote != '\'')
	{
		return Fail(ParseFailure::NotWellFormed, "an attribute value must be quoted", start);
	}
	++m_Offset;

	std::string value;
	for (;;)
	{
		if (AtEnd())
		{
			return Fail(ParseFailure::NotWellFormed, "an attribute value that is never closed", start);
		}

		const char character = Peek();
		if (character == quote)
		{
			++m_Offset;
			return value;
		}
		if (character == '<')
		{
			return Fail(ParseFailure::NotWellFormed, "'<' in an attribute value; write '&lt;'", m_Offset);
		}
		if (character == '&')
		{
			if (Result<void> reference = AppendReference(value); !reference)
			{
				return std::unexpected{ reference.error() };
			}
			continue;
		}

		value.push_back(character);
		++m_Offset;
	}
}

Result<Tag> Parser::ReadStartTag()
{
	const std::size_t start = m_Offset;

	// Everything `<` can introduce that is not a start tag. Each of these is well-formed XML this
	// parser deliberately does not accept, so each says what it is rather than "unexpected '<'".
	if (Looking("<?"))
	{
		return Fail(ParseFailure::UnexpectedElement, "a processing instruction outside the XML declaration", start);
	}
	if (Looking("<![CDATA["))
	{
		return Fail(ParseFailure::UnexpectedElement, "a CDATA section; text may use entity references instead", start);
	}
	if (Looking("<!"))
	{
		// A DOCTYPE is refused rather than skipped because its internal subset can declare entities,
		// and a parser that ignored the declarations while resolving the references would expand
		// them to the wrong text — or refuse a reference the document had defined perfectly well.
		return Fail(ParseFailure::UnexpectedElement, "a document type declaration", start);
	}
	if (Peek() != '<')
	{
		return Fail(ParseFailure::NotWellFormed, "expected an element", start);
	}
	++m_Offset;

	Tag tag;
	tag.Offset = start;

	Result<std::string_view> name = ReadName("an element name");
	if (!name)
	{
		return std::unexpected{ name.error() };
	}
	tag.Name = *name;

	for (;;)
	{
		const bool spaced = IsSpace(Peek());
		SkipSpace();

		if (Looking("/>"))
		{
			m_Offset += 2;
			tag.Empty = true;
			return tag;
		}
		if (Peek() == '>')
		{
			++m_Offset;
			return tag;
		}
		if (AtEnd())
		{
			return Fail(ParseFailure::NotWellFormed, std::format("<{}> is never closed", tag.Name), start);
		}
		if (!spaced)
		{
			return Fail(ParseFailure::NotWellFormed, "expected whitespace before an attribute", m_Offset);
		}

		Attribute attribute;
		attribute.Offset = m_Offset;

		Result<std::string_view> attributeName = ReadName("an attribute name");
		if (!attributeName)
		{
			return std::unexpected{ attributeName.error() };
		}
		attribute.Name = *attributeName;

		SkipSpace();
		if (Peek() != '=')
		{
			return Fail(
				ParseFailure::NotWellFormed,
				std::format("attribute '{}' has no value", attribute.Name),
				attribute.Offset
			);
		}
		++m_Offset;
		SkipSpace();

		Result<std::string> value = ReadQuoted();
		if (!value)
		{
			return std::unexpected{ value.error() };
		}
		attribute.Value = std::move(*value);

		const auto duplicate = std::ranges::find(tag.Attributes, attribute.Name, &Attribute::Name);
		if (duplicate != tag.Attributes.end())
		{
			return Fail(
				ParseFailure::NotWellFormed,
				std::format("<{}> gives '{}' twice", tag.Name, attribute.Name),
				attribute.Offset
			);
		}

		tag.Attributes.push_back(std::move(attribute));
	}
}

Result<void> Parser::ReadEndTag(const Tag& tag)
{
	const std::size_t start = m_Offset;
	m_Offset += 2;

	Result<std::string_view> name = ReadName("an element name");
	if (!name)
	{
		return std::unexpected{ name.error() };
	}
	if (*name != tag.Name)
	{
		return Fail(ParseFailure::NotWellFormed, std::format("</{}> closes <{}>", *name, tag.Name), start);
	}

	SkipSpace();
	if (Peek() != '>')
	{
		return Fail(ParseFailure::NotWellFormed, std::format("</{}> is never closed", tag.Name), start);
	}
	++m_Offset;
	return {};
}

Result<std::string> Parser::ReadText(const Tag& tag)
{
	if (tag.Empty)
	{
		return std::string{};
	}

	std::string text;
	for (;;)
	{
		if (AtEnd())
		{
			return Fail(ParseFailure::NotWellFormed, std::format("<{}> is never closed", tag.Name), tag.Offset);
		}

		const char character = Peek();
		if (character == '&')
		{
			if (Result<void> reference = AppendReference(text); !reference)
			{
				return std::unexpected{ reference.error() };
			}
			continue;
		}
		if (character != '<')
		{
			text.push_back(character);
			++m_Offset;
			continue;
		}

		if (Looking("<!--"))
		{
			// SkipComment rather than SkipMisc: a comment between two words is not whitespace, and
			// the spaces on either side of it belong to the sentence being generated.
			if (Result<void> comment = SkipComment(); !comment)
			{
				return std::unexpected{ comment.error() };
			}
			continue;
		}
		if (Looking("</"))
		{
			if (Result<void> end = ReadEndTag(tag); !end)
			{
				return std::unexpected{ end.error() };
			}
			return Normalise(text);
		}

		return Fail(ParseFailure::UnexpectedElement, std::format("<{}> holds text, not elements", tag.Name), m_Offset);
	}
}

std::optional<Attribute> Parser::Take(Tag& tag, std::string_view name)
{
	const auto found = std::ranges::find(tag.Attributes, name, &Attribute::Name);
	if (found == tag.Attributes.end())
	{
		return std::nullopt;
	}

	Attribute attribute = std::move(*found);
	// Emptied rather than erased, so the offsets of the attributes after it stay where the document
	// put them and FinishAttributes still reports the leftovers in source order.
	found->Name = {};
	return attribute;
}

Result<Attribute> Parser::Require(Tag& tag, std::string_view name)
{
	std::optional<Attribute> attribute = Take(tag, name);
	if (!attribute)
	{
		return Fail(ParseFailure::MissingAttribute, std::format("<{}> needs a '{}'", tag.Name, name), tag.Offset);
	}
	return std::move(*attribute);
}

Result<void> Parser::FinishAttributes(const Tag& tag)
{
	for (const Attribute& attribute : tag.Attributes)
	{
		if (!attribute.Name.empty())
		{
			return Fail(
				ParseFailure::UnexpectedAttribute,
				std::format("<{}> has no attribute '{}'", tag.Name, attribute.Name),
				attribute.Offset
			);
		}
	}
	return {};
}

// Every version in the document is a positive decimal count of interface revisions: `version`,
// `since`, `deprecated-since`. Zero is refused along with the negatives, because there is no
// version zero of an interface and `since="0"` is a typo that would otherwise read as "always".
Result<int> Parser::ToVersion(const Attribute& attribute)
{
	int version = 0;
	const char* const first = attribute.Value.data();
	const char* const last = first + attribute.Value.size();
	const std::from_chars_result parsed = std::from_chars(first, last, version);
	if (parsed.ec != std::errc{} || parsed.ptr != last || version < 1)
	{
		return Fail(
			ParseFailure::InvalidValue,
			std::format("{}=\"{}\" is not a version", attribute.Name, attribute.Value),
			attribute.Offset
		);
	}
	return version;
}

Result<int> Parser::TakeVersion(Tag& tag, std::string_view name, int fallback)
{
	std::optional<Attribute> attribute = Take(tag, name);
	if (!attribute)
	{
		return fallback;
	}
	return ToVersion(*attribute);
}

Result<bool> Parser::TakeFlag(Tag& tag, std::string_view name)
{
	std::optional<Attribute> attribute = Take(tag, name);
	if (!attribute)
	{
		return false;
	}
	if (attribute->Value == "true")
	{
		return true;
	}
	if (attribute->Value == "false")
	{
		return false;
	}

	return Fail(
		ParseFailure::InvalidValue,
		std::format("{}=\"{}\" is neither \"true\" nor \"false\"", name, attribute->Value),
		attribute->Offset
	);
}

std::string Parser::TakeText(Tag& tag, std::string_view name)
{
	std::optional<Attribute> attribute = Take(tag, name);
	return attribute ? std::move(attribute->Value) : std::string{};
}

std::unexpected<Error> Parser::Unexpected(const Tag& child, const Tag& parent)
{
	return Fail(
		ParseFailure::UnexpectedElement,
		std::format("<{}> may not appear in <{}>", child.Name, parent.Name),
		child.Offset
	);
}

template<typename Handler>
Result<void> Parser::ParseChildren(const Tag& tag, Handler handler)
{
	if (tag.Empty)
	{
		return {};
	}

	for (;;)
	{
		if (Result<void> misc = SkipMisc(); !misc)
		{
			return misc;
		}
		if (AtEnd())
		{
			return Fail(ParseFailure::NotWellFormed, std::format("<{}> is never closed", tag.Name), tag.Offset);
		}
		if (Looking("</"))
		{
			return ReadEndTag(tag);
		}
		if (Peek() != '<')
		{
			return Fail(ParseFailure::NotWellFormed, std::format("<{}> may only contain elements", tag.Name), m_Offset);
		}

		Result<Tag> child = ReadStartTag();
		if (!child)
		{
			return std::unexpected{ child.error() };
		}
		if (Result<void> handled = handler(*child); !handled)
		{
			return handled;
		}
	}
}

// `<description summary="...">body</description>`, which is the same shape on all six elements that
// may carry one. The summary is required by the DTD and is the line a generated comment leads with;
// the body is the paragraphs under it.
Result<void> Parser::ParseDescription(Tag& tag, const Tag& parent, std::string& summary, std::string& body)
{
	if (!summary.empty() || !body.empty())
	{
		return Fail(ParseFailure::UnexpectedElement, std::format("<{}> has two descriptions", parent.Name), tag.Offset);
	}

	Result<Attribute> attribute = Require(tag, "summary");
	if (!attribute)
	{
		return std::unexpected{ attribute.error() };
	}
	if (Result<void> finished = FinishAttributes(tag); !finished)
	{
		return finished;
	}

	Result<std::string> text = ReadText(tag);
	if (!text)
	{
		return std::unexpected{ text.error() };
	}

	summary = std::move(attribute->Value);
	body = std::move(*text);
	return {};
}

Result<void> Parser::ParseArgumentElement(Tag& tag, std::vector<Argument>& arguments)
{
	Argument argument;

	Result<Attribute> name = Require(tag, "name");
	if (!name)
	{
		return std::unexpected{ name.error() };
	}
	argument.Name = std::move(name->Value);

	Result<Attribute> type = Require(tag, "type");
	if (!type)
	{
		return std::unexpected{ type.error() };
	}

	// Matched against Protocol.h's Name() rather than against a second table of spellings here. One
	// list means the name an emitter prints and the name this accepts cannot drift, and a kind
	// added without a spelling is then a compile error in the switch rather than a `type=` that
	// silently stops being recognised.
	bool known = false;
	for (const ArgumentKind kind : { ArgumentKind::Int,
	                                 ArgumentKind::Uint,
	                                 ArgumentKind::Fixed,
	                                 ArgumentKind::String,
	                                 ArgumentKind::Object,
	                                 ArgumentKind::NewId,
	                                 ArgumentKind::Array,
	                                 ArgumentKind::Fd })
	{
		if (Name(kind) == type->Value)
		{
			argument.Kind = kind;
			known = true;
			break;
		}
	}
	if (!known)
	{
		return Fail(ParseFailure::InvalidValue, std::format("'{}' is not an argument type", type->Value), type->Offset);
	}

	argument.Interface = TakeText(tag, "interface");
	argument.Enumeration = TakeText(tag, "enum");

	Result<bool> allowNull = TakeFlag(tag, "allow-null");
	if (!allowNull)
	{
		return std::unexpected{ allowNull.error() };
	}
	argument.AllowNull = *allowNull;

	// The attribute wins over the description's summary where an argument carries both, because it
	// is the one the author wrote against this argument rather than against a paragraph.
	std::string summary = TakeText(tag, "summary");

	if (Result<void> finished = FinishAttributes(tag); !finished)
	{
		return finished;
	}

	std::string descriptionSummary;
	Result<void> children = ParseChildren(tag, [&](Tag& child) -> Result<void> {
		if (child.Name == "description")
		{
			return ParseDescription(child, tag, descriptionSummary, argument.Description);
		}
		return Unexpected(child, tag);
	});
	if (!children)
	{
		return children;
	}

	argument.Summary = summary.empty() ? std::move(descriptionSummary) : std::move(summary);
	arguments.push_back(std::move(argument));
	return {};
}

Result<void> Parser::ParseMessageElement(Tag& tag, std::vector<Message>& messages)
{
	Message message;

	Result<Attribute> name = Require(tag, "name");
	if (!name)
	{
		return std::unexpected{ name.error() };
	}
	message.Name = std::move(name->Value);

	// `type=` is #IMPLIED and "destructor" is the only thing it may say. Anything else is refused
	// rather than treated as absent, since a request that meant to destroy and does not is a leak
	// of the shadow object and a client that thinks the id is free.
	if (std::optional<Attribute> type = Take(tag, "type"); type)
	{
		if (type->Value != "destructor")
		{
			return Fail(
				ParseFailure::InvalidValue,
				std::format("'{}' is not a message type; only \"destructor\" is", type->Value),
				type->Offset
			);
		}
		message.Destructor = true;
	}

	Result<int> since = TakeVersion(tag, "since", 1);
	if (!since)
	{
		return std::unexpected{ since.error() };
	}
	message.Since = *since;

	Result<int> deprecated = TakeVersion(tag, "deprecated-since", 0);
	if (!deprecated)
	{
		return std::unexpected{ deprecated.error() };
	}
	message.DeprecatedSince = *deprecated;

	if (Result<void> finished = FinishAttributes(tag); !finished)
	{
		return finished;
	}

	Result<void> children = ParseChildren(tag, [&](Tag& child) -> Result<void> {
		if (child.Name == "description")
		{
			return ParseDescription(child, tag, message.Summary, message.Description);
		}
		if (child.Name == "arg")
		{
			return ParseArgumentElement(child, message.Arguments);
		}
		return Unexpected(child, tag);
	});
	if (!children)
	{
		return children;
	}

	messages.push_back(std::move(message));
	return {};
}

Result<void> Parser::ParseEntryElement(Tag& tag, std::vector<EnumerationEntry>& entries)
{
	EnumerationEntry entry;

	Result<Attribute> name = Require(tag, "name");
	if (!name)
	{
		return std::unexpected{ name.error() };
	}
	entry.Name = std::move(name->Value);

	Result<Attribute> value = Require(tag, "value");
	if (!value)
	{
		return std::unexpected{ value.error() };
	}
	{
		const std::string_view text = value->Value;
		const bool hexadecimal = text.starts_with("0x") || text.starts_with("0X");
		const std::string_view digits = hexadecimal ? text.substr(2) : text;

		const char* const first = digits.data();
		const char* const last = first + digits.size();
		const std::from_chars_result parsed = std::from_chars(first, last, entry.Value, hexadecimal ? 16 : 10);
		if (parsed.ec != std::errc{} || parsed.ptr != last || digits.empty())
		{
			// Unsigned and 32 bits, so `0x80000000` in a bitfield and the DRM fourccs in
			// linux-dmabuf both land, and anything wider is a range error rather than a wrap.
			return Fail(
				ParseFailure::InvalidValue,
				std::format("value=\"{}\" is not a 32-bit unsigned number", text),
				value->Offset
			);
		}
	}

	Result<int> since = TakeVersion(tag, "since", 1);
	if (!since)
	{
		return std::unexpected{ since.error() };
	}
	entry.Since = *since;

	Result<int> deprecated = TakeVersion(tag, "deprecated-since", 0);
	if (!deprecated)
	{
		return std::unexpected{ deprecated.error() };
	}
	entry.DeprecatedSince = *deprecated;

	std::string summary = TakeText(tag, "summary");

	if (Result<void> finished = FinishAttributes(tag); !finished)
	{
		return finished;
	}

	std::string descriptionSummary;
	Result<void> children = ParseChildren(tag, [&](Tag& child) -> Result<void> {
		if (child.Name == "description")
		{
			return ParseDescription(child, tag, descriptionSummary, entry.Description);
		}
		return Unexpected(child, tag);
	});
	if (!children)
	{
		return children;
	}

	entry.Summary = summary.empty() ? std::move(descriptionSummary) : std::move(summary);
	entries.push_back(std::move(entry));
	return {};
}

Result<void> Parser::ParseEnumerationElement(Tag& tag, std::vector<Enumeration>& enumerations)
{
	Enumeration enumeration;

	Result<Attribute> name = Require(tag, "name");
	if (!name)
	{
		return std::unexpected{ name.error() };
	}
	enumeration.Name = std::move(name->Value);

	Result<bool> bitfield = TakeFlag(tag, "bitfield");
	if (!bitfield)
	{
		return std::unexpected{ bitfield.error() };
	}
	enumeration.Bitfield = *bitfield;

	Result<int> since = TakeVersion(tag, "since", 1);
	if (!since)
	{
		return std::unexpected{ since.error() };
	}
	enumeration.Since = *since;

	if (Result<void> finished = FinishAttributes(tag); !finished)
	{
		return finished;
	}

	Result<void> children = ParseChildren(tag, [&](Tag& child) -> Result<void> {
		if (child.Name == "description")
		{
			return ParseDescription(child, tag, enumeration.Summary, enumeration.Description);
		}
		if (child.Name == "entry")
		{
			return ParseEntryElement(child, enumeration.Entries);
		}
		return Unexpected(child, tag);
	});
	if (!children)
	{
		return children;
	}

	enumerations.push_back(std::move(enumeration));
	return {};
}

Result<void> Parser::ParseInterfaceElement(Tag& tag, Protocol& protocol)
{
	Interface interface;

	Result<Attribute> name = Require(tag, "name");
	if (!name)
	{
		return std::unexpected{ name.error() };
	}
	interface.Name = std::move(name->Value);

	// Required by the DTD, unlike every other version in the document: an interface with no version
	// is one an emitter would have to guess a ceiling for, and guessing it low silently withholds
	// requests from clients that are entitled to them.
	Result<Attribute> version = Require(tag, "version");
	if (!version)
	{
		return std::unexpected{ version.error() };
	}

	Result<int> parsedVersion = ToVersion(*version);
	if (!parsedVersion)
	{
		return std::unexpected{ parsedVersion.error() };
	}
	interface.Version = *parsedVersion;

	Result<bool> frozen = TakeFlag(tag, "frozen");
	if (!frozen)
	{
		return std::unexpected{ frozen.error() };
	}
	interface.Frozen = *frozen;

	if (Result<void> finished = FinishAttributes(tag); !finished)
	{
		return finished;
	}

	Result<void> children = ParseChildren(tag, [&](Tag& child) -> Result<void> {
		if (child.Name == "description")
		{
			return ParseDescription(child, tag, interface.Summary, interface.Description);
		}
		if (child.Name == "request")
		{
			return ParseMessageElement(child, interface.Requests);
		}
		if (child.Name == "event")
		{
			return ParseMessageElement(child, interface.Events);
		}
		if (child.Name == "enum")
		{
			return ParseEnumerationElement(child, interface.Enumerations);
		}
		return Unexpected(child, tag);
	});
	if (!children)
	{
		return children;
	}

	protocol.Interfaces.push_back(std::move(interface));
	return {};
}

Result<void> Parser::ParseProtocolElement(Tag& tag, Protocol& protocol)
{
	Result<Attribute> name = Require(tag, "name");
	if (!name)
	{
		return std::unexpected{ name.error() };
	}
	protocol.Name = std::move(name->Value);

	if (Result<void> finished = FinishAttributes(tag); !finished)
	{
		return finished;
	}

	bool copyrighted = false;
	return ParseChildren(tag, [&](Tag& child) -> Result<void> {
		if (child.Name == "copyright")
		{
			if (copyrighted)
			{
				return Fail(ParseFailure::UnexpectedElement, "<protocol> has two copyright notices", child.Offset);
			}
			if (Result<void> finished = FinishAttributes(child); !finished)
			{
				return finished;
			}

			Result<std::string> text = ReadText(child);
			if (!text)
			{
				return std::unexpected{ text.error() };
			}
			protocol.Copyright = std::move(*text);
			copyrighted = true;
			return {};
		}
		if (child.Name == "description")
		{
			return ParseDescription(child, tag, protocol.Summary, protocol.Description);
		}
		if (child.Name == "interface")
		{
			return ParseInterfaceElement(child, protocol);
		}
		return Unexpected(child, tag);
	});
}

Result<Protocol> Parser::Run()
{
	if (m_Xml.starts_with(Utf8ByteOrderMark))
	{
		m_Offset += Utf8ByteOrderMark.size();
	}

	// The declaration, if there is one, and only where XML puts it. Its pseudo-attributes are not
	// read: the only encoding the protocols are published in is UTF-8, this parser has no decoder
	// for a second one, and pretending to honour `encoding=` would be worse than ignoring it.
	if (Looking("<?xml") && (IsSpace(Peek(5)) || Peek(5) == '?'))
	{
		const std::size_t end = m_Xml.find("?>", m_Offset);
		if (end == std::string_view::npos)
		{
			return Fail(ParseFailure::NotWellFormed, "an XML declaration that is never closed", m_Offset);
		}
		m_Offset = end + 2;
	}

	if (Result<void> misc = SkipMisc(); !misc)
	{
		return std::unexpected{ misc.error() };
	}
	if (AtEnd())
	{
		return Fail(ParseFailure::UnexpectedElement, "an empty document; expected <protocol>", m_Offset);
	}

	Result<Tag> root = ReadStartTag();
	if (!root)
	{
		return std::unexpected{ root.error() };
	}
	if (root->Name != "protocol")
	{
		return Fail(
			ParseFailure::UnexpectedElement,
			std::format("the root element is <{}>, not <protocol>", root->Name),
			root->Offset
		);
	}

	Protocol protocol;
	if (Result<void> body = ParseProtocolElement(*root, protocol); !body)
	{
		return std::unexpected{ body.error() };
	}

	if (Result<void> misc = SkipMisc(); !misc)
	{
		return std::unexpected{ misc.error() };
	}
	if (!AtEnd())
	{
		return Fail(ParseFailure::NotWellFormed, "content after </protocol>", m_Offset);
	}

	return protocol;
}
} // namespace

Result<Protocol> ParseProtocol(std::string_view xml, Diagnostic* diagnostic)
{
	Diagnostic discarded;
	Parser parser{ xml, diagnostic ? *diagnostic : discarded };
	return parser.Run();
}
