#include "Naming.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace
{
// The keywords a protocol could plausibly hand an argument or an enumerator, rather than all of
// them. The full list is ninety-odd words and every one that is not here is one no protocol author
// would write on an argument: a name is chosen to describe a value, so `class`, `default`, `new` and
// `delete` are the live hazards and `reinterpret_cast` is not. Wayland's own answer to the first is
// already `class_`, which is this rule arrived at independently and is the reason the spelling
// matches.
constexpr std::array Keywords{
	std::string_view{ "alignas" },  std::string_view{ "and" },       std::string_view{ "auto" },
	std::string_view{ "bitand" },   std::string_view{ "bitor" },     std::string_view{ "bool" },
	std::string_view{ "break" },    std::string_view{ "case" },      std::string_view{ "catch" },
	std::string_view{ "char" },     std::string_view{ "class" },     std::string_view{ "compl" },
	std::string_view{ "concept" },  std::string_view{ "const" },     std::string_view{ "continue" },
	std::string_view{ "decltype" }, std::string_view{ "default" },   std::string_view{ "delete" },
	std::string_view{ "do" },       std::string_view{ "double" },    std::string_view{ "else" },
	std::string_view{ "enum" },     std::string_view{ "explicit" },  std::string_view{ "export" },
	std::string_view{ "extern" },   std::string_view{ "false" },     std::string_view{ "float" },
	std::string_view{ "for" },      std::string_view{ "friend" },    std::string_view{ "goto" },
	std::string_view{ "if" },       std::string_view{ "inline" },    std::string_view{ "int" },
	std::string_view{ "long" },     std::string_view{ "mutable" },   std::string_view{ "namespace" },
	std::string_view{ "new" },      std::string_view{ "noexcept" },  std::string_view{ "not" },
	std::string_view{ "nullptr" },  std::string_view{ "operator" },  std::string_view{ "or" },
	std::string_view{ "private" },  std::string_view{ "protected" }, std::string_view{ "public" },
	std::string_view{ "register" }, std::string_view{ "requires" },  std::string_view{ "return" },
	std::string_view{ "short" },    std::string_view{ "signed" },    std::string_view{ "sizeof" },
	std::string_view{ "static" },   std::string_view{ "struct" },    std::string_view{ "switch" },
	std::string_view{ "template" }, std::string_view{ "this" },      std::string_view{ "throw" },
	std::string_view{ "true" },     std::string_view{ "try" },       std::string_view{ "typedef" },
	std::string_view{ "typeid" },   std::string_view{ "typename" },  std::string_view{ "union" },
	std::string_view{ "unsigned" }, std::string_view{ "using" },     std::string_view{ "virtual" },
	std::string_view{ "void" },     std::string_view{ "volatile" },  std::string_view{ "while" },
	std::string_view{ "xor" },
};

// Everything a generated proxy declares for itself. A request landing on one of these is a build
// failure rather than a rename, because renaming would put a name in the header that no reader can
// find in the protocol.
constexpr std::array ReservedProxy{
	std::string_view{ "Child" },    std::string_view{ "Dispatch" },    std::string_view{ "Id" },
	std::string_view{ "Ignoring" }, std::string_view{ "IsValid" },     std::string_view{ "Listen" },
	std::string_view{ "Listener" }, std::string_view{ "Object" },      std::string_view{ "Version" },
	std::string_view{ "WireName" }, std::string_view{ "WireVersion" },
};

// The same for a generated resource, which is the server arm's class and shares only four of these.
// An *event* is what lands here, since a request arrives on the handler behind an `On` prefix and can
// only collide with another request.
constexpr std::array ReservedResource{
	std::string_view{ "Advertise" },   std::string_view{ "Create" },       std::string_view{ "Factory" },
	std::string_view{ "Handler" },     std::string_view{ "Ignoring" },     std::string_view{ "IsValid" },
	std::string_view{ "PostError" },   std::string_view{ "PostNoMemory" }, std::string_view{ "Version" },
	std::string_view{ "WireClient" },  std::string_view{ "WireName" },     std::string_view{ "WireResource" },
	std::string_view{ "WireVersion" },
};

constexpr bool IsDigit(char character) noexcept
{
	return character >= '0' && character <= '9';
}

constexpr char Upper(char character) noexcept
{
	return (character >= 'a' && character <= 'z') ? static_cast<char>(character - 'a' + 'A') : character;
}
} // namespace

std::string Pascal(std::string_view wire)
{
	std::string result;
	result.reserve(wire.size());

	bool boundary = true;
	for (const char character : wire)
	{
		// A hyphen separates for the same reason an underscore does, and it is not decoration: the
		// generated file is named after the file it came from — `xdg-shell.xml` becomes `XdgShell.h` —
		// so that the build knows an output path from an input path with no XML parsed in between.
		if (character == '_' || character == '-')
		{
			boundary = true;
			continue;
		}

		result.push_back(boundary ? Upper(character) : character);
		boundary = false;
	}

	if (!result.empty() && IsDigit(result.front()))
	{
		result.insert(result.begin(), '_');
	}

	return result;
}

std::string Parameter(std::string_view wire)
{
	std::string result;
	result.reserve(wire.size());

	// The leading segment is left as the protocol wrote it, which is where the lowercase comes from —
	// there is nothing to lowercase, since every name in every published protocol is already lower.
	bool boundary = false;
	bool first = true;
	for (const char character : wire)
	{
		if (character == '_')
		{
			// A trailing underscore is the protocol's own keyword guard — `class_` — and reproducing
			// it is what makes this function idempotent over a name that has already been guarded.
			boundary = true;
			continue;
		}

		result.push_back((boundary && !first) ? Upper(character) : character);
		boundary = false;
		first = false;
	}

	if (result.empty())
	{
		return result;
	}

	if (IsDigit(result.front()))
	{
		result.insert(result.begin(), '_');
	}

	if (std::ranges::find(Keywords, std::string_view{ result }) != Keywords.end())
	{
		result.push_back('_');
	}

	return result;
}

bool IsReservedProxyMember(std::string_view identifier) noexcept
{
	return std::ranges::find(ReservedProxy, identifier) != ReservedProxy.end();
}

bool IsReservedResourceMember(std::string_view identifier) noexcept
{
	return std::ranges::find(ReservedResource, identifier) != ReservedResource.end();
}
