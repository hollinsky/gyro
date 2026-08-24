#include "Emit.Shared.h"

#include <cerrno>
#include <format>
#include <iterator>

#include "Naming.h"

namespace
{
// The file a protocol came from, with any directories dropped. Not std::filesystem, which this
// module has no other use for and which would put a second spelling of "the last component" beside
// the one below it.
std::string_view FileName(std::string_view path)
{
	const std::size_t slash = path.find_last_of('/');

	return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

bool HasEvents(const Interface& interface)
{
	return !interface.Events.empty();
}

// A protocol description turned into a `//` block at the given indent. Kept because it is the only
// documentation a binding has: the alternative is a reader with the header open in one window and
// the XML in another, working out which of six `set_` requests is the one that takes surface-local
// coordinates.
void Comment(std::string& out, std::string_view indent, std::string_view text)
{
	if (text.empty())
	{
		return;
	}

	for (std::size_t start = 0; start <= text.size();)
	{
		const std::size_t newline = text.find('\n', start);
		const std::size_t stop = (newline == std::string_view::npos) ? text.size() : newline;
		const std::string_view line = text.substr(start, stop - start);

		if (line.empty())
		{
			std::format_to(std::back_inserter(out), "{}//\n", indent);
		}
		else
		{
			std::format_to(std::back_inserter(out), "{}// {}\n", indent, line);
		}

		if (newline == std::string_view::npos)
		{
			break;
		}

		start = newline + 1;
	}
}
} // namespace

std::unexpected<Error> Refuse(EmitDiagnostic* diagnostic, EmitFailure failure, std::string message)
{
	if (diagnostic != nullptr)
	{
		*diagnostic = EmitDiagnostic{ .Failure = failure, .Message = std::move(message) };
	}

	// EINVAL with a context chosen from the fixed set Name() holds, which is Core/Result.h's one
	// permitted variable case. The detail is in the diagnostic, for Tools/Bindings/Xml.h's reason.
	return std::unexpected{ Error{ EINVAL, Name(failure) } };
}

std::string Unit(std::string_view path)
{
	const std::string_view file = FileName(path);
	const std::size_t dot = file.find_last_of('.');

	return Pascal(dot == std::string_view::npos ? file : file.substr(0, dot));
}

std::string HeaderName(const ProtocolSource& source)
{
	return std::format("Wayland/{}.h", Unit(source.Path));
}

std::string ProxyName(std::string_view wireInterface)
{
	return Pascal(wireInterface);
}

std::string ListenerName(std::string_view wireInterface)
{
	return std::format("{}Listener", Pascal(wireInterface));
}

std::string IgnoringName(std::string_view wireInterface)
{
	return std::format("{}Ignoring", Pascal(wireInterface));
}

std::string EnumName(std::string_view wireInterface, std::string_view wireEnumeration)
{
	return std::format("{}{}", Pascal(wireInterface), Pascal(wireEnumeration));
}

std::pair<std::string, std::string> SplitEnumeration(const Argument& argument, const Interface& owner)
{
	const std::size_t dot = argument.Enumeration.find('.');

	if (dot == std::string::npos)
	{
		return { owner.Name, argument.Enumeration };
	}

	return { argument.Enumeration.substr(0, dot), argument.Enumeration.substr(dot + 1) };
}

const Argument* CreatedArgument(const Message& message)
{
	for (const Argument& argument : message.Arguments)
	{
		if (argument.Kind == ArgumentKind::NewId)
		{
			return &argument;
		}
	}

	return nullptr;
}

bool IsDisplay(const Interface& interface)
{
	return interface.Name == "wl_display";
}

bool WantsListener(const Interface& interface)
{
	return HasEvents(interface) && !IsDisplay(interface);
}

void Documentation(std::string& out, std::string_view indent, std::string_view summary, std::string_view description)
{
	if (!summary.empty())
	{
		Comment(out, indent, summary);

		if (!description.empty())
		{
			std::format_to(std::back_inserter(out), "{}//\n", indent);
		}
	}

	Comment(out, indent, description);
}

void EmitEnumeration(std::string& out, const Interface& interface, const Enumeration& enumeration)
{
	const std::string name = EnumName(interface.Name, enumeration.Name);

	Documentation(out, "", enumeration.Summary, enumeration.Description);

	// Always the 32-bit unsigned underlying type, including where the argument that draws on it is a
	// signed `int`. Tools/Bindings/Protocol.h records why: linux-dmabuf's format entries are DRM
	// fourccs, four packed characters that run past INT_MAX, and an enumeration whose underlying type
	// could not hold its own entries is not a thing to generate.
	std::format_to(std::back_inserter(out), "enum class {} : std::uint32_t\n{{\n", name);

	for (const EnumerationEntry& entry : enumeration.Entries)
	{
		Documentation(out, "\t", entry.Summary, entry.Description);

		if (entry.Since > 1)
		{
			std::format_to(std::back_inserter(out), "\t// Since version {}.\n", entry.Since);
		}

		if (enumeration.Bitfield)
		{
			std::format_to(std::back_inserter(out), "\t{} = {:#x},\n", Pascal(entry.Name), entry.Value);
		}
		else
		{
			std::format_to(std::back_inserter(out), "\t{} = {},\n", Pascal(entry.Name), entry.Value);
		}
	}

	out += "};\n\n";

	if (!enumeration.Bitfield)
	{
		return;
	}

	// The operators a bitfield needs and a scoped enum does not have. Generated per enumeration
	// rather than reached through one variadic `Flags<E>` template in Wire, because the operators are
	// the only thing that template would carry and it would put a header below both waists whose one
	// job is to serve generated code — which is a dependency the layering check would be right to ask
	// about and this is six lines.
	std::format_to(
		std::back_inserter(out),
		"[[nodiscard]] constexpr {0} operator|({0} left, {0} right) noexcept\n"
		"{{\n"
		"\treturn static_cast<{0}>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));\n"
		"}}\n\n"
		"[[nodiscard]] constexpr {0} operator&({0} left, {0} right) noexcept\n"
		"{{\n"
		"\treturn static_cast<{0}>(static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));\n"
		"}}\n\n"
		"[[nodiscard]] constexpr {0} operator~({0} value) noexcept\n"
		"{{\n"
		"\treturn static_cast<{0}>(~static_cast<std::uint32_t>(value));\n"
		"}}\n\n"
		"constexpr {0}& operator|=({0}& left, {0} right) noexcept\n"
		"{{\n"
		"\tleft = left | right;\n"
		"\treturn left;\n"
		"}}\n\n"
		"// Whether any bit is set. Named rather than left to a comparison against a zero the\n"
		"// enumeration may not have an entry for.\n"
		"[[nodiscard]] constexpr bool Any({0} value) noexcept\n"
		"{{\n"
		"\treturn static_cast<std::uint32_t>(value) != 0;\n"
		"}}\n\n",
		name
	);
}

std::string Joined(const std::vector<std::string>& parameters)
{
	std::string joined;

	for (const std::string& parameter : parameters)
	{
		if (!joined.empty())
		{
			joined += ", ";
		}

		joined += parameter;
	}

	return joined;
}

std::string Wrapped(const std::vector<std::string>& parameters, std::string_view indent, std::size_t lead)
{
	const std::string flat = Joined(parameters);

	// 120 is CMake/BuildFlags.cmake's neighbour, the ColumnLimit in .clang-format. One less so that
	// the closing paren and whatever follows it still fit.
	if (parameters.empty() || lead + flat.size() < 119)
	{
		return flat;
	}

	std::string wrapped = "\n";

	for (std::size_t index = 0; index < parameters.size(); ++index)
	{
		std::format_to(
			std::back_inserter(wrapped),
			"{}\t{}{}\n",
			indent,
			parameters[index],
			index + 1 == parameters.size() ? "" : ","
		);
	}

	wrapped += indent;

	return wrapped;
}

std::set<std::size_t> References(const Protocol& protocol, std::size_t self, const Catalog& catalog)
{
	std::set<std::size_t> referenced;

	auto note = [&](std::string_view interface) {
		if (interface.empty())
		{
			return;
		}

		const auto found = catalog.find(interface);

		if (found != catalog.end() && found->second.Protocol != self)
		{
			referenced.insert(found->second.Protocol);
		}
	};

	for (const Interface& interface : protocol.Interfaces)
	{
		for (const std::vector<Message>* messages : { &interface.Requests, &interface.Events })
		{
			for (const Message& message : *messages)
			{
				for (const Argument& argument : message.Arguments)
				{
					note(argument.Interface);

					if (!argument.Enumeration.empty())
					{
						note(SplitEnumeration(argument, interface).first);
					}
				}
			}
		}
	}

	return referenced;
}

void EmitPreamble(std::string& out, const ProtocolSource& source)
{
	std::format_to(
		std::back_inserter(out),
		"// {}, generated from {} by Tools/Bindings. Do not edit.\n",
		source.Model.Name,
		FileName(source.Path)
	);

	if (!source.Model.Copyright.empty())
	{
		out += "//\n";
		Comment(out, "", source.Model.Copyright);
	}
}
