#include "Emit.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <format>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/Result.h"
#include "Naming.h"
#include "Protocol.h"

// The emitter is a set of functions that append to a string, in the order the file reads.
//
// **The generated header is laid out in four sections, and the order is what makes cycles a
// non-problem rather than a topological sort.** Interfaces reference each other freely and in both
// directions — `xdg_surface.get_popup` takes an `xdg_surface` and `xdg_popup` is created by it — so
// any scheme that needs a definition before its first mention has to order a graph with cycles in it.
// What removes the question entirely is that a function *declaration* may use an incomplete type by
// value, both as a parameter and as a return. So:
//
//   1. every class in the file, forward-declared;
//   2. every enumeration, at namespace scope, since an enumeration depends on nothing;
//   3. every proxy, whose members are a pointer, an id and a version and whose requests are
//      declarations — so no proxy needs another to be complete;
//   4. every listener, which does need its own proxy complete, and by here they all are.
//
// The cost is that an enumeration is a namespace-scope name carrying its interface —
// `WlOutputTransform` — because a nested name cannot be forward-declared. An in-class alias was the
// obvious repair and it does not survive the corpus: `wl_shell_surface` declares a `resize` request
// beside a `resize` enumeration, so `WlShellSurface::Resize` would be two things. A listener keeps
// its alias because `Listener` and `Ignoring` are in Tools/Bindings/Naming.h's reserved set, which
// makes a request of either name a build failure rather than a silent second meaning.
//
// **`wl_display` is the one interface named in this file.** Its two events are answered by the wire
// runtime rather than by a proxy — Wire/Connection.h argues that at length — so it is emitted with
// its requests and no listener at all, because a listener for it could only be bound by a call
// `Connection::Bind` refuses. That is the same exception Wire/Message.h and Wire/Connection.h already
// carve out rather than a new one this file invents.

namespace
{
// Where an interface is defined. The protocol index is what decides whether a reference costs an
// `#include`.
struct Located
{
	std::size_t Protocol = 0;
	const Interface* Definition = nullptr;
};

using Catalog = std::map<std::string, Located, std::less<>>;

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

// --- Names -------------------------------------------------------------------------------------

// The file a protocol came from, with any directories dropped. Not std::filesystem, which this
// module has no other use for and which would put a second spelling of "the last component" beside
// the one below it.
std::string_view FileName(std::string_view path)
{
	const std::size_t slash = path.find_last_of('/');

	return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// The generated file's base name, which is the *input file's* name PascalCased rather than the
// protocol's own `name` attribute. They agree today for every published protocol and nothing
// guarantees they will: the build has to derive an output path from the path it was handed, and a
// build that had to parse the XML to learn what file the generator would write is a build that
// cannot declare its own outputs.
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

// The hoisted spelling of an enumeration. `wl_output` + `transform` is `WlOutputTransform`, which is
// unique because an interface name is unique across the whole protocol ecosystem by convention and
// by the registry that publishes them.
std::string EnumName(std::string_view wireInterface, std::string_view wireEnumeration)
{
	return std::format("{}{}", Pascal(wireInterface), Pascal(wireEnumeration));
}

// An `enum=` split at the dot, per Tools/Bindings/Protocol.h. Unqualified means the interface the
// argument belongs to.
std::pair<std::string, std::string> SplitEnumeration(const Argument& argument, const Interface& owner)
{
	const std::size_t dot = argument.Enumeration.find('.');

	if (dot == std::string::npos)
	{
		return { owner.Name, argument.Enumeration };
	}

	return { argument.Enumeration.substr(0, dot), argument.Enumeration.substr(dot + 1) };
}

// --- Types -------------------------------------------------------------------------------------

// How an argument is spelled where a caller passes it or a handler receives it.
//
// `allowNullString` is the one place the two directions differ. A nullable string is a distinct wire
// value from an empty one — length zero against a length of one and a lone NUL — and on a request the
// caller has to be able to say which, so it takes a std::optional. On an event it cannot: Wire's
// reader returns an empty view for both, and offering an optional that is never nullopt would be a
// type claiming a distinction the codec below it threw away.
std::string ArgumentType(const Argument& argument, const Interface& owner, bool allowNullString)
{
	if (!argument.Enumeration.empty())
	{
		const auto [interface, enumeration] = SplitEnumeration(argument, owner);

		return EnumName(interface, enumeration);
	}

	switch (argument.Kind)
	{
		case ArgumentKind::Int:
			return "std::int32_t";
		case ArgumentKind::Uint:
			return "std::uint32_t";
		case ArgumentKind::Fixed:
			return "Wire::Fixed";
		case ArgumentKind::String:
			return (allowNullString && argument.AllowNull) ? "std::optional<std::string_view>" : "std::string_view";
		case ArgumentKind::Object:
		case ArgumentKind::NewId:
			return argument.Interface.empty() ? "Wire::ObjectId" : ProxyName(argument.Interface);
		case ArgumentKind::Array:
			return "std::span<const std::byte>";
		case ArgumentKind::Fd:
			return "Fd";
	}

	return "void";
}

// The `new_id` a request creates, or null where it creates nothing. Protocol.h's untyped case is a
// new_id with no interface, and it stays a new_id here — what changes is that the request becomes a
// template, which the caller of this has to ask about separately.
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

bool HasEvents(const Interface& interface)
{
	return !interface.Events.empty();
}

// Wire/Connection.h answers `wl_display` itself, so it gets no listener however many events it has.
bool IsDisplay(const Interface& interface)
{
	return interface.Name == "wl_display";
}

bool WantsListener(const Interface& interface)
{
	return HasEvents(interface) && !IsDisplay(interface);
}

// --- Comments ----------------------------------------------------------------------------------

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

// Summary before description, since the protocols put a one-liner in the attribute and the prose in
// the element and a reader wants them in that order.
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

// --- Enumerations ------------------------------------------------------------------------------

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

// --- Requests ----------------------------------------------------------------------------------

// The parameters a request takes, in order, as `Type name` pairs. The created object is not among
// them — it is the return — and where the interface it creates has events, the listener that
// implements it is appended.
std::vector<std::string> RequestParameters(const Message& message, const Interface& owner, const Catalog& catalog)
{
	std::vector<std::string> parameters;

	for (const Argument& argument : message.Arguments)
	{
		if (argument.Kind != ArgumentKind::NewId)
		{
			std::format_to(
				std::back_inserter(parameters.emplace_back()),
				"{} {}",
				ArgumentType(argument, owner, true),
				Parameter(argument.Name)
			);

			continue;
		}

		// The untyped case. Three wire values rather than one — Tools/Bindings/Protocol.h has the
		// shape — and the interface name comes from the type, so only the version is a parameter.
		if (argument.Interface.empty())
		{
			parameters.emplace_back("std::uint32_t version");
		}
	}

	const Argument* const created = CreatedArgument(message);

	if (created == nullptr)
	{
		return parameters;
	}

	if (created->Interface.empty())
	{
		parameters.emplace_back("typename T::Listener& listener");

		return parameters;
	}

	const auto found = catalog.find(created->Interface);

	if (found != catalog.end() && WantsListener(*found->second.Definition))
	{
		parameters.emplace_back(std::format("{}& listener", ListenerName(created->Interface)));
	}

	return parameters;
}

// A parameter list wrapped the way the project's own .clang-format would wrap it, because the
// generated header is the documentation for these interfaces and `zwp_linux_buffer_params_v1.add`
// takes six arguments. `lead` is what precedes the open paren, which is what decides whether the flat
// form fits.
std::string Wrapped(const std::vector<std::string>& parameters, std::string_view indent, std::size_t lead);

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

// The marshalling run for one request: every argument in wire order, with the created id put where
// the protocol says rather than appended.
void EmitMarshalling(std::string& out, std::string_view indent, const Message& message, const Interface& owner)
{
	for (const Argument& argument : message.Arguments)
	{
		const std::string name = Parameter(argument.Name);
		auto emit = [&](std::string_view line) { std::format_to(std::back_inserter(out), "{}{}\n", indent, line); };

		switch (argument.Kind)
		{
			case ArgumentKind::Int:
				emit(
					argument.Enumeration.empty() ?
						std::format("wireWriter.PutInt({});", name) :
						std::format("wireWriter.PutInt(static_cast<std::int32_t>({}));", name)
				);
				break;

			case ArgumentKind::Uint:
				emit(
					argument.Enumeration.empty() ?
						std::format("wireWriter.PutUint({});", name) :
						std::format("wireWriter.PutUint(static_cast<std::uint32_t>({}));", name)
				);
				break;

			case ArgumentKind::Fixed:
				emit(std::format("wireWriter.PutFixed({});", name));
				break;

			case ArgumentKind::String:
				if (argument.AllowNull)
				{
					// The one argument shape a caller can get wrong silently, so the generated code
					// spells both sides: an engaged optional is the string, a disengaged one is the
					// protocol's null, and an empty string is neither of those things.
					emit(std::format("if ({}.has_value())", name));
					emit("{");
					std::format_to(std::back_inserter(out), "{}\twireWriter.PutString(*{});\n", indent, name);
					emit("}");
					emit("else");
					emit("{");
					std::format_to(std::back_inserter(out), "{}\twireWriter.PutNullString();\n", indent);
					emit("}");
				}
				else
				{
					emit(std::format("wireWriter.PutString({});", name));
				}
				break;

			case ArgumentKind::Object:
				emit(
					argument.Interface.empty() ? std::format("wireWriter.PutObject({});", name) :
												 std::format("wireWriter.PutObject({}.Id());", name)
				);
				break;

			case ArgumentKind::NewId:
				if (argument.Interface.empty())
				{
					emit("wireWriter.PutString(T::WireName);");
					emit("wireWriter.PutUint(version);");
				}

				emit("wireWriter.PutNewId(wireId);");
				break;

			case ArgumentKind::Array:
				emit(std::format("wireWriter.PutArray({});", name));
				break;

			case ArgumentKind::Fd:
				emit(std::format("wireWriter.PutFd(std::move({}));", name));
				break;
		}
	}

	(void)owner;
}

// The version gate. A request the bound object is too old for is a programming error and the
// generated code is where it stops: sending it anyway is a protocol error the host answers by killing
// the connection, which for the nested backend is every window on the screen going away because a
// compositor asked for something a two-year-old host had not heard of.
void EmitSinceGuard(std::string& out, std::string_view indent, const Message& message, const Interface& owner)
{
	if (message.Since <= 1)
	{
		return;
	}

	std::format_to(
		std::back_inserter(out),
		"{0}if (m_Version < {1})\n"
		"{0}{{\n"
		"{0}\tm_Connection->Output().RecordFault(\n"
		"{0}\t\tEPROTO,\n"
		"{0}\t\t\"sending {2}.{3}, which needs version {1}, to an object bound below it\"\n"
		"{0}\t);\n"
		"\n"
		"{0}\treturn{4};\n"
		"{0}}}\n"
		"\n",
		indent,
		message.Since,
		owner.Name,
		message.Name,
		CreatedArgument(message) == nullptr ? "" : " {}"
	);
}

// --- The proxy ---------------------------------------------------------------------------------

void EmitTemplateRequest(
	std::string& out,
	const Message& message,
	const Interface& owner,
	std::size_t opcode,
	const std::vector<std::string>& parameters
)
{
	// The untyped `new_id`. Two overloads because whether a listener is a parameter depends on the
	// interface the caller names, which is not known until it does — `requires` on the presence of
	// `T::Listener` is the same question the typed path answers by looking in the catalog.
	std::vector<std::string> withoutListener = parameters;
	withoutListener.pop_back();

	for (const bool listening : { true, false })
	{
		std::format_to(
			std::back_inserter(out),
			"\ttemplate<typename T>\n"
			"\t\trequires ({0}requires {{ typename T::Listener; }})\n"
			"\t[[nodiscard]] T {1}({2}) const\n"
			"\t{{\n"
			"\t\tif (m_Connection == nullptr)\n"
			"\t\t{{\n"
			"\t\t\treturn {{}};\n"
			"\t\t}}\n"
			"\n"
			"\t\t// A version above what the generated bindings describe is the same programming error\n"
			"\t\t// a too-new request is, one step earlier: the object would be created claiming a\n"
			"\t\t// contract this binary cannot hold up.\n"
			"\t\tif (version == 0 || version > T::WireVersion)\n"
			"\t\t{{\n"
			"\t\t\tm_Connection->Output().RecordFault(\n"
			"\t\t\t\tEPROTO,\n"
			"\t\t\t\t\"binding a wayland global at a version the generated bindings do not describe\"\n"
			"\t\t\t);\n"
			"\n"
			"\t\t\treturn {{}};\n"
			"\t\t}}\n"
			"\n"
			"\t\tconst Wire::ObjectId wireId = m_Connection->Allocate();\n"
			"\n"
			"\t\tif (wireId == Wire::ObjectId::None)\n"
			"\t\t{{\n"
			"\t\t\tm_Connection->Output().RecordFault(ENOSPC, \"allocating a wayland object id for {3}.{4}\");\n"
			"\n"
			"\t\t\treturn {{}};\n"
			"\t\t}}\n"
			"\n"
			"\t\t// The version is the caller's rather than this object's, which is what an untyped\n"
			"\t\t// `new_id` is for: the global's version is negotiated against what the host\n"
			"\t\t// advertised, not inherited from the registry that named it.\n"
			"\t\tT wireCreated{{ *m_Connection, wireId, version }};\n",
			listening ? "" : "!",
			Pascal(message.Name),
			Wrapped(listening ? parameters : withoutListener, "\t\t", std::size_t{ 22 } + Pascal(message.Name).size()),
			owner.Name,
			message.Name
		);

		out += listening ? "\n\t\twireCreated.Listen(listener);\n" : "\n\t\twireCreated.Listen();\n";

		std::format_to(
			std::back_inserter(out),
			"\n"
			"\t\tWire::MessageWriter wireWriter{{ *m_Connection, m_Id, {} }};\n",
			opcode
		);

		EmitMarshalling(out, "\t\t", message, owner);

		out += "\t\twireWriter.Send();\n"
			   "\n"
			   "\t\treturn wireCreated;\n"
			   "\t}\n\n";
	}
}

void EmitProxy(std::string& out, const Interface& interface, const Catalog& catalog)
{
	const std::string proxy = ProxyName(interface.Name);

	Documentation(out, "", interface.Summary, interface.Description);

	if (IsDisplay(interface))
	{
		out += "//\n"
			   "// **This one has no listener, and that is Wire/Connection.h's exception rather than an\n"
			   "// omission here.** `wl_display`'s two events are `error` and `delete_id`, and the wire\n"
			   "// runtime answers both itself: an id may not be reused until the host says it is free, so\n"
			   "// recycling has to happen below whatever allocates, and a protocol error ends the\n"
			   "// connection rather than being something an object handles. `Connection::Bind` refuses\n"
			   "// this id outright, so a listener generated for it could never be attached to anything.\n";
	}

	if (interface.Frozen)
	{
		std::format_to(
			std::back_inserter(out),
			"//\n// Frozen upstream at version {}: this interface is committed never to change again.\n",
			interface.Version
		);
	}

	std::format_to(std::back_inserter(out), "class {}\n{{\npublic:\n", proxy);

	std::format_to(
		std::back_inserter(out),
		"\t// What `wl_registry.bind` puts on the wire ahead of the id, and the highest version these\n"
		"\t// bindings were generated against.\n"
		"\tstatic constexpr std::string_view WireName = \"{}\";\n"
		"\tstatic constexpr std::uint32_t WireVersion = {};\n\n",
		interface.Name,
		interface.Version
	);

	if (WantsListener(interface))
	{
		std::format_to(
			std::back_inserter(out),
			"\tusing Listener = {};\n\tusing Ignoring = {};\n\n",
			ListenerName(interface.Name),
			IgnoringName(interface.Name)
		);
	}

	std::format_to(
		std::back_inserter(out),
		"\t// A proxy names an object; it does not own one. Copying is how a caller keeps a second\n"
		"\t// reference and is free, and a proxy whose object has been destroyed is stale rather than\n"
		"\t// dangling — the id it holds is one the connection will refuse until the host frees it.\n"
		"\t//\n"
		"\t// **Sending a request is const and destroying the object is not.** A request changes what the\n"
		"\t// host holds, never what this three-word handle names, so `const` here means what it should:\n"
		"\t// a `const` proxy can drive the object it names and cannot take it away from anyone else\n"
		"\t// holding a copy.\n"
		"\t{0}() = default;\n"
		"\n"
		"\t{0}(Wire::Connection& connection, Wire::ObjectId id, std::uint32_t version) noexcept\n"
		"\t\t: m_Connection{{ &connection }}, m_Id{{ id }}, m_Version{{ version }}\n"
		"\t{{}}\n"
		"\n"
		"\t[[nodiscard]] Wire::ObjectId Id() const noexcept {{ return m_Id; }}\n"
		"\n"
		"\t// The version this object was bound at, which is what every `since` gate below is read\n"
		"\t// against. Not WireVersion: a host is entitled to advertise less than these bindings know.\n"
		"\t[[nodiscard]] std::uint32_t Version() const noexcept {{ return m_Version; }}\n"
		"\n"
		"\t[[nodiscard]] bool IsValid() const noexcept\n"
		"\t{{\n"
		"\t\treturn m_Connection != nullptr && m_Id != Wire::ObjectId::None;\n"
		"\t}}\n"
		"\n",
		proxy
	);

	if (!IsDisplay(interface) && !WantsListener(interface))
	{
		std::format_to(
			std::back_inserter(out),
			"\t// **Binds the id, though there is no listener to bind it to.** {} declares no events, so\n"
			"\t// nothing here is about dispatch — it is about the id's lifetime. Wire/Connection.h frees\n"
			"\t// an id that was allocated and never bound the moment the proxy goes, on the grounds that\n"
			"\t// the host has never heard of it; an id that *was* marshalled and never bound breaks that\n"
			"\t// premise, and the host's `delete_id` for it then lands on a slot nothing is using and ends\n"
			"\t// the connection. Binding makes it live, so it retires and waits like every other id.\n"
			"\t//\n"
			"\t// Called by the request that creates the object, so no call site has to know this.\n"
			"\tvoid Listen() const;\n"
			"\n"
			"\t// No events, so no cases: anything arriving for this object is refused. Registered anyway,\n"
			"\t// because `Connection::Bind` is what makes the id live and it wants a dispatcher.\n"
			"\tstatic void Dispatch(void* wireSelf, std::uint16_t wireOpcode, Wire::MessageReader& wireReader);\n"
			"\n",
			interface.Name
		);
	}

	if (WantsListener(interface))
	{
		std::format_to(
			std::back_inserter(out),
			"\t// Points the connection at `listener` and records the pairing on it, so a handler can\n"
			"\t// ask which object it was called for. One listener implements one object: binding a\n"
			"\t// second is a fault on the connection rather than a silently rerouted first.\n"
			"\t//\n"
			"\t// Nothing to check here and nothing a caller could do about it, per Wire/Writer.h — a\n"
			"\t// refusal is recorded on the output buffer and surfaces at the next `Flush`.\n"
			"\tvoid Listen({}& listener) const;\n"
			"\n"
			"\t// The generated dispatch table. Registered by `Listen` rather than by a caller, which is\n"
			"\t// the whole of decision 2's argument made structural: there is no spelling for an id\n"
			"\t// that is live and has no implementation behind it.\n"
			"\tstatic void Dispatch(void* wireSelf, std::uint16_t wireOpcode, Wire::MessageReader& wireReader);\n"
			"\n",
			ListenerName(interface.Name)
		);
	}

	for (std::size_t opcode = 0; opcode < interface.Requests.size(); ++opcode)
	{
		const Message& request = interface.Requests[opcode];
		const Argument* const created = CreatedArgument(request);
		const std::vector<std::string> parameters = RequestParameters(request, interface, catalog);

		Documentation(out, "\t", request.Summary, request.Description);

		if (request.Since > 1)
		{
			std::format_to(std::back_inserter(out), "\t// Since version {}.\n", request.Since);
		}

		if (request.DeprecatedSince != 0)
		{
			std::format_to(
				std::back_inserter(out),
				"\t// Deprecated since version {}. Still spoken, so still generated.\n",
				request.DeprecatedSince
			);
		}

		if (request.Destructor)
		{
			out += "\t// Destroys the object. This proxy is left invalid, and any copy of it is stale.\n";
		}

		if (created != nullptr && created->Interface.empty())
		{
			EmitTemplateRequest(out, request, interface, opcode, parameters);

			continue;
		}

		if (created == nullptr)
		{
			std::format_to(
				std::back_inserter(out),
				"\tvoid {}({}){};\n\n",
				Pascal(request.Name),
				Wrapped(parameters, "\t", std::size_t{ 7 } + Pascal(request.Name).size()),
				request.Destructor ? "" : " const"
			);

			continue;
		}

		std::format_to(
			std::back_inserter(out),
			"\t[[nodiscard]] {} {}({}) const;\n\n",
			ProxyName(created->Interface),
			Pascal(request.Name),
			Wrapped(
				parameters, "\t", std::size_t{ 17 } + ProxyName(created->Interface).size() + Pascal(request.Name).size()
			)
		);
	}

	out += "private:\n"
		   "\t// A proxy for another object reached through this one. The version is this object's,\n"
		   "\t// capped at what these bindings describe of the child — libwayland's rule with the cap\n"
		   "\t// added, and the cap is not decoration: `wl_surface` is at version 6 and the\n"
		   "\t// `wl_callback` it creates has only ever had one.\n"
		   "\ttemplate<typename T>\n"
		   "\t[[nodiscard]] T Child(Wire::ObjectId id) const noexcept\n"
		   "\t{\n"
		   "\t\treturn T{ *m_Connection, id, m_Version < T::WireVersion ? m_Version : T::WireVersion };\n"
		   "\t}\n"
		   "\n"
		   "\tWire::Connection* m_Connection = nullptr;\n"
		   "\tWire::ObjectId m_Id = Wire::ObjectId::None;\n"
		   "\tstd::uint32_t m_Version = 0;\n"
		   "};\n\n";
}

// --- The listener ------------------------------------------------------------------------------

// What a handler returns. An event that creates an object whose interface has events returns that
// object's implementation, and the dispatcher binds what comes back — so the only way to receive the
// object is to hand over the code that answers for it, in the same call, before any event for it can
// arrive. Everything else returns void.
std::string HandlerReturn(const Message& event, const Catalog& catalog)
{
	const Argument* const created = CreatedArgument(event);

	if (created == nullptr || created->Interface.empty())
	{
		return "void";
	}

	const auto found = catalog.find(created->Interface);

	if (found == catalog.end() || !WantsListener(*found->second.Definition))
	{
		return "void";
	}

	return std::format("{}*", ListenerName(created->Interface));
}

std::vector<std::string> HandlerParameters(const Message& event, const Interface& owner, bool named)
{
	std::vector<std::string> parameters;

	for (const Argument& argument : event.Arguments)
	{
		std::string& parameter = parameters.emplace_back(ArgumentType(argument, owner, false));

		if (named)
		{
			std::format_to(std::back_inserter(parameter), " {}", Parameter(argument.Name));
		}
	}

	return parameters;
}

void EmitListener(std::string& out, const Interface& interface, const Catalog& catalog)
{
	const std::string proxy = ProxyName(interface.Name);
	const std::string listener = ListenerName(interface.Name);

	std::format_to(
		std::back_inserter(out),
		"// What answers `{0}`'s events.\n"
		"//\n"
		"// **Every handler is pure, and that is the point rather than an oversight.** A generated table\n"
		"// cannot have a hole, so an event this binding knows about and nobody handles is a compiler\n"
		"// error at the class that forgot it — not a window that never sizes itself because\n"
		"// `xdg_surface.configure` went nowhere. Where ignoring an event is the actual intent, say so by\n"
		"// deriving from `{0}::Ignoring` instead, which is one word at the declaration and greppable.\n"
		"//\n"
		"// One listener implements one object. `{0}::Listen` records the pairing, `Object()` reads it\n"
		"// back, and binding the same listener twice is refused — so a handler never has to work out\n"
		"// which of several objects it was called for.\n"
		"class {1}\n"
		"{{\n"
		"public:\n"
		"\t{1}() = default;\n"
		"\n"
		"\tvirtual ~{1}() = default;\n"
		"\n"
		"\t// Neither copied nor moved: the connection holds the address of this object for as long as\n"
		"\t// it is bound, so a copy would be a second listener the dispatcher never reaches and a move\n"
		"\t// would leave the table pointing at the husk.\n"
		"\t{1}(const {1}&) = delete;\n"
		"\t{1}& operator=(const {1}&) = delete;\n"
		"\n"
		"\t// The object this listener was bound to, or an invalid proxy before `Listen`.\n"
		"\t[[nodiscard]] {0} Object() const noexcept {{ return m_Object; }}\n"
		"\n",
		proxy,
		listener
	);

	for (const Message& event : interface.Events)
	{
		Documentation(out, "\t", event.Summary, event.Description);

		if (event.Since > 1)
		{
			std::format_to(
				std::back_inserter(out),
				"\t// Since version {}. A host will not send it to an object bound below that, so a\n"
				"\t// handler for an older binding is dead code rather than a case to guard.\n",
				event.Since
			);
		}

		if (HandlerReturn(event, catalog) != "void")
		{
			out += "\t// Returns the implementation for the object this event creates. Returning null\n"
				   "\t// leaves the id unimplemented, which is a fault on the connection.\n";
		}

		std::format_to(
			std::back_inserter(out),
			"\tvirtual {} On{}({}) = 0;\n\n",
			HandlerReturn(event, catalog),
			Pascal(event.Name),
			Wrapped(
				HandlerParameters(event, interface, true),
				"\t",
				std::size_t{ 20 } + HandlerReturn(event, catalog).size() + Pascal(event.Name).size()
			)
		);
	}

	std::format_to(
		std::back_inserter(out),
		"private:\n"
		"\tfriend class {0};\n"
		"\n"
		"\t{0} m_Object;\n"
		"}};\n\n",
		proxy
	);

	// The opt-out. Only events that can actually be ignored get a body: one that hands over an object
	// cannot be, because declining it is the null return the dispatcher faults on, so it is left pure
	// and the class that wants the rest ignored still has to answer for that one.
	std::format_to(
		std::back_inserter(out),
		"// `{0}` with every ignorable event answered by doing nothing. Deriving from this instead of\n"
		"// `{1}` is how a call site says it has read the event list and wants none of it.\n"
		"class {2} : public {1}\n"
		"{{\n"
		"public:\n",
		proxy,
		listener,
		IgnoringName(interface.Name)
	);

	for (const Message& event : interface.Events)
	{
		if (HandlerReturn(event, catalog) != "void")
		{
			continue;
		}

		std::format_to(
			std::back_inserter(out),
			"\tvoid On{}({}) override {{}}\n",
			Pascal(event.Name),
			Wrapped(HandlerParameters(event, interface, false), "\t", std::size_t{ 21 } + Pascal(event.Name).size())
		);
	}

	out += "};\n\n";
}

// --- Definitions -------------------------------------------------------------------------------

void EmitRequestDefinition(
	std::string& out,
	const Message& request,
	const Interface& owner,
	std::size_t opcode,
	const Catalog& catalog
)
{
	const std::string proxy = ProxyName(owner.Name);
	const Argument* const created = CreatedArgument(request);
	const std::vector<std::string> parameters = RequestParameters(request, owner, catalog);

	std::format_to(
		std::back_inserter(out),
		"{} {}::{}({}){}\n"
		"{{\n"
		"\tif (m_Connection == nullptr)\n"
		"\t{{\n"
		"\t\treturn{};\n"
		"\t}}\n"
		"\n",
		created == nullptr ? "void" : ProxyName(created->Interface),
		proxy,
		Pascal(request.Name),
		Wrapped(
			parameters,
			"",
			(created == nullptr ? std::size_t{ 6 } : ProxyName(created->Interface).size() + 1) + proxy.size() + 3 +
				Pascal(request.Name).size()
		),
		request.Destructor ? "" : " const",
		created == nullptr ? "" : " {}"
	);

	EmitSinceGuard(out, "\t", request, owner);

	if (created != nullptr)
	{
		std::format_to(
			std::back_inserter(out),
			"\tconst Wire::ObjectId wireId = m_Connection->Allocate();\n"
			"\n"
			"\tif (wireId == Wire::ObjectId::None)\n"
			"\t{{\n"
			"\t\tm_Connection->Output().RecordFault(ENOSPC, \"allocating a wayland object id for {0}.{1}\");\n"
			"\n"
			"\t\treturn {{}};\n"
			"\t}}\n"
			"\n"
			"\t{2} wireCreated = Child<{2}>(wireId);\n",
			owner.Name,
			request.Name,
			ProxyName(created->Interface)
		);

		const auto found = catalog.find(created->Interface);

		if (found != catalog.end() && WantsListener(*found->second.Definition))
		{
			out += "\n"
				   "\t// Bound before the request goes out rather than after, which is the property\n"
				   "\t// decision 2 asked for: there is no instant in which the host could send an event\n"
				   "\t// for this id and find nothing behind it.\n"
				   "\twireCreated.Listen(listener);\n";
		}
		else
		{
			out += "\n"
				   "\t// No listener, and the id is bound anyway — see `Listen()`. Before the request goes\n"
				   "\t// out for the same reason, and because the rule Wire/Connection.h states is that a\n"
				   "\t// proxy is bound before its id is marshalled.\n"
				   "\twireCreated.Listen();\n";
		}

		out += "\n";
	}

	std::format_to(std::back_inserter(out), "\tWire::MessageWriter wireWriter{{ *m_Connection, m_Id, {} }};\n", opcode);

	EmitMarshalling(out, "\t", request, owner);

	out += "\twireWriter.Send();\n";

	if (request.Destructor)
	{
		out += "\n"
			   "\t// `type=\"destructor\"`: the object is gone the moment this is sent, and leaving this proxy\n"
			   "\t// invalid is why it is the one request that is not const. The id stays out\n"
			   "\t// of circulation until the host's `delete_id`, which Wire/Connection.h owns.\n"
			   "\tm_Connection->Unbind(m_Id);\n"
			   "\n"
			   "\tm_Connection = nullptr;\n"
			   "\tm_Id = Wire::ObjectId::None;\n"
			   "\tm_Version = 0;\n";
	}

	if (created != nullptr)
	{
		out += "\n\treturn wireCreated;\n";
	}

	out += "}\n\n";
}

void EmitListenDefinition(std::string& out, const Interface& interface)
{
	if (!WantsListener(interface))
	{
		std::format_to(
			std::back_inserter(out),
			"void {0}::Listen() const\n"
			"{{\n"
			"\tif (m_Connection == nullptr)\n"
			"\t{{\n"
			"\t\treturn;\n"
			"\t}}\n"
			"\n"
			"\t// `Connection::Bind` wants a non-null `self` and there is no object to give it. Nothing\n"
			"\t// ever dereferences this — the dispatcher below takes no `self` at all.\n"
			"\tif (const Result<void> bound = m_Connection->Bind(m_Id, &WireAnchor, &{0}::Dispatch); !bound)\n"
			"\t{{\n"
			"\t\tm_Connection->Output().RecordFault(bound.error().Code(), bound.error().Context());\n"
			"\t}}\n"
			"}}\n\n",
			ProxyName(interface.Name)
		);

		return;
	}

	std::format_to(
		std::back_inserter(out),
		"void {0}::Listen({1}& listener) const\n"
		"{{\n"
		"\tif (m_Connection == nullptr)\n"
		"\t{{\n"
		"\t\treturn;\n"
		"\t}}\n"
		"\n"
		"\tif (listener.m_Object.IsValid())\n"
		"\t{{\n"
		"\t\tm_Connection->Output().RecordFault(EBUSY, \"binding one {2} listener to a second object\");\n"
		"\n"
		"\t\treturn;\n"
		"\t}}\n"
		"\n"
		"\tlistener.m_Object = *this;\n"
		"\n"
		"\tif (const Result<void> bound = m_Connection->Bind(m_Id, &listener, &{0}::Dispatch); !bound)\n"
		"\t{{\n"
		"\t\t// The context is a view over static storage either way, so the connection's own sentence\n"
		"\t\t// is carried rather than replaced by a vaguer one from here.\n"
		"\t\tm_Connection->Output().RecordFault(bound.error().Code(), bound.error().Context());\n"
		"\n"
		"\t\tlistener.m_Object = {{}};\n"
		"\t}}\n"
		"}}\n\n",
		ProxyName(interface.Name),
		ListenerName(interface.Name),
		interface.Name
	);
}

void EmitDispatchDefinition(std::string& out, const Interface& interface, const Catalog& catalog)
{
	const std::string proxy = ProxyName(interface.Name);

	// An interface with no events still has a dispatcher, because it still has a *binding* — see
	// `Listen()` in the header. Anything arriving for it is a host describing an interface these
	// bindings do not, which is the unknown-opcode case with no cases at all.
	if (!WantsListener(interface))
	{
		std::format_to(
			std::back_inserter(out),
			"void {}::Dispatch(void*, std::uint16_t, Wire::MessageReader& wireReader)\n"
			"{{\n"
			"\t// {} declares no events. The bytes could be skipped and the descriptors could not, so\n"
			"\t// this is the same refusal the switch below any other interface ends with.\n"
			"\twireReader.Fail();\n"
			"}}\n\n",
			proxy,
			interface.Name
		);

		return;
	}

	std::format_to(
		std::back_inserter(out),
		"// **`wireSelf` may be null and every argument is read regardless**, which is Wire/Connection.h's\n"
		"// contract rather than defensiveness. Both ends talk at once, so a host routinely has an event\n"
		"// on the wire for an object before it has read the request destroying it — a stale event is the\n"
		"// ordinary case. It cannot be skipped: descriptors arrive on a queue shared across messages, so\n"
		"// a message whose arguments are never read leaves that queue one entry out of step and the next\n"
		"// message takes a descriptor belonging to the one thrown away. Nothing detects that; it is a\n"
		"// valid descriptor for the wrong buffer. So each case demarshals in full and only the call into\n"
		"// the listener is guarded.\n"
		"void {}::Dispatch(void* wireSelf, std::uint16_t wireOpcode, Wire::MessageReader& wireReader)\n"
		"{{\n"
		"\tauto* const wireListener = static_cast<{}*>(wireSelf);\n"
		"\n"
		"\tswitch (wireOpcode)\n"
		"\t{{\n",
		proxy,
		ListenerName(interface.Name)
	);

	for (std::size_t opcode = 0; opcode < interface.Events.size(); ++opcode)
	{
		const Message& event = interface.Events[opcode];

		std::format_to(std::back_inserter(out), "\t\tcase {}:\n\t\t{{\n", opcode);

		std::vector<std::string> arguments;

		// One statement per argument, in wire order, rather than the accessors nested in the handler
		// call. The order of evaluation of function arguments is unspecified, and these accessors are
		// a stateful read off one buffer — nesting them is a demarshaller that works until a compiler
		// upgrade reorders it and every argument of every multi-argument event comes back shuffled.
		//
		// An object or a new_id is read as the bare id it is on the wire, and becomes a proxy inside
		// the guarded call: building one needs the listener's own object for the connection to hang it
		// off, and the listener may not be there.
		for (const Argument& argument : event.Arguments)
		{
			const std::string name = Parameter(argument.Name);
			const std::string type = ArgumentType(argument, interface, false);
			const bool isProxy = (argument.Kind == ArgumentKind::Object || argument.Kind == ArgumentKind::NewId) &&
			                     !argument.Interface.empty();

			if (isProxy)
			{
				arguments.push_back(std::format("wireListener->m_Object.Child<{}>({})", type, name));
			}
			else if (argument.Kind == ArgumentKind::Fd)
			{
				arguments.push_back(std::format("std::move({})", name));
			}
			else
			{
				arguments.push_back(name);
			}

			if (!argument.Enumeration.empty())
			{
				std::format_to(
					std::back_inserter(out),
					"\t\t\tconst {0} {1} = static_cast<{0}>(wireReader.GetUint());\n",
					type,
					name
				);

				continue;
			}

			switch (argument.Kind)
			{
				case ArgumentKind::Int:
					std::format_to(std::back_inserter(out), "\t\t\tconst {} {} = wireReader.GetInt();\n", type, name);
					break;

				case ArgumentKind::Uint:
					std::format_to(std::back_inserter(out), "\t\t\tconst {} {} = wireReader.GetUint();\n", type, name);
					break;

				case ArgumentKind::Fixed:
					std::format_to(std::back_inserter(out), "\t\t\tconst {} {} = wireReader.GetFixed();\n", type, name);
					break;

				case ArgumentKind::String:
					// Points into the connection's receive buffer and is valid for this call only, per
					// Wire/Reader.h. A handler that keeps it copies it.
					std::format_to(
						std::back_inserter(out), "\t\t\tconst {} {} = wireReader.GetString();\n", type, name
					);
					break;

				case ArgumentKind::Object:
					std::format_to(
						std::back_inserter(out), "\t\t\tconst Wire::ObjectId {} = wireReader.GetObject();\n", name
					);
					break;

				case ArgumentKind::NewId:
					std::format_to(
						std::back_inserter(out), "\t\t\tconst Wire::ObjectId {} = wireReader.GetNewId();\n", name
					);
					break;

				case ArgumentKind::Array:
					// Same lifetime as a string, and the same obligation on a handler that keeps it.
					std::format_to(std::back_inserter(out), "\t\t\tconst {} {} = wireReader.GetArray();\n", type, name);
					break;

				case ArgumentKind::Fd:
					// Closed by leaving this scope where the listener is gone, which is the whole reason
					// the read happens before the guard rather than after it.
					std::format_to(std::back_inserter(out), "\t\t\t{} {} = wireReader.GetFd();\n", type, name);
					break;
			}
		}

		out += "\n"
			   "\t\t\tif (wireListener == nullptr)\n"
			   "\t\t\t{\n"
			   "\t\t\t\treturn;\n"
			   "\t\t\t}\n"
			   "\n";

		const std::string handled = HandlerReturn(event, catalog);
		const Argument* const created = CreatedArgument(event);

		if (handled == "void")
		{
			std::format_to(
				std::back_inserter(out), "\t\t\twireListener->On{}({});\n", Pascal(event.Name), Joined(arguments)
			);
		}
		else
		{
			std::format_to(
				std::back_inserter(out),
				"\t\t\t{0} wireCreated = wireListener->m_Object.Child<{0}>({1});\n"
				"\t\t\t{2}* const wireImplementation = wireListener->On{3}(wireCreated);\n"
				"\n"
				"\t\t\tif (wireImplementation == nullptr)\n"
				"\t\t\t{{\n"
				"\t\t\t\twireListener->m_Object.m_Connection->Output().RecordFault(\n"
				"\t\t\t\t\tEINVAL,\n"
				"\t\t\t\t\t\"a {4}.{5} handler declined to implement the object it was handed\"\n"
				"\t\t\t\t);\n"
				"\n"
				"\t\t\t\treturn;\n"
				"\t\t\t}}\n"
				"\n"
				"\t\t\twireCreated.Listen(*wireImplementation);\n",
				ProxyName(created->Interface),
				Parameter(created->Name),
				ListenerName(created->Interface),
				Pascal(event.Name),
				interface.Name,
				event.Name
			);
		}

		out += "\n\t\t\treturn;\n\t\t}\n\n";
	}

	out += "\t\tdefault:\n"
		   "\t\t\tbreak;\n"
		   "\t}\n"
		   "\n"
		   "\t// An opcode with no case here is an event these bindings have no signature for, and the\n"
		   "\t// answer has to be the end of the connection rather than a skip. Skipping the *bytes* is\n"
		   "\t// free — the header's size word frames the message — but the descriptors are not in the\n"
		   "\t// message, per the note above. Wire/Connection.h makes the identical call for an event\n"
		   "\t// naming an id nothing has ever been bound to, for the identical reason.\n"
		   "\t//\n"
		   "\t// A conforming host cannot get here: it may not send an event above the version this\n"
		   "\t// object was bound at, and every version these bindings can ask for came from the same XML\n"
		   "\t// this switch did.\n"
		   "\twireReader.Fail();\n"
		   "}\n\n";
}

// --- Files -------------------------------------------------------------------------------------

// Every other protocol in the set this one names, so the header includes exactly what it needs. A
// reference is either to something in the set or it was already refused by the catalog, so this loop
// never has to answer for a name it cannot find.
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

std::string EmitHeader(
	const ProtocolSource& source,
	std::size_t self,
	const Catalog& catalog,
	const std::span<const ProtocolSource> protocols
)
{
	std::string out;

	EmitPreamble(out, source);

	out += "//\n"
		   "// **A dispatch table that cannot have a hole and an object that cannot be live without\n"
		   "// one.** Both are properties of this file rather than of the code that uses it. Every event\n"
		   "// an interface declares is a pure virtual on its listener, so a handler nobody wrote is a\n"
		   "// compiler error rather than an event that goes nowhere; and every id is bound to its\n"
		   "// implementation before the request that creates it reaches the wire, because the request is\n"
		   "// what takes the listener. There is no spelling in this header for an object that exists and\n"
		   "// has nothing behind it.\n"
		   "//\n"
		   "// Read a listener's `Ignoring` sibling as the deliberate opposite: it answers every\n"
		   "// ignorable event by doing nothing, and a call site that derives from it has said so in one\n"
		   "// word that greps.\n"
		   "//\n"
		   "// The layout is four sections — forward declarations, enumerations, proxies, listeners — and\n"
		   "// it is in that order so that interfaces referring to each other in both directions need no\n"
		   "// ordering at all: a function *declaration* may take and return an incomplete type by value,\n"
		   "// so no proxy needs another to be defined first.\n"
		   "//\n"
		   "// An enumeration is a namespace-scope name carrying its interface — `WlOutputTransform`\n"
		   "// rather than `WlOutput::Transform` — because a nested name cannot be forward-declared. That\n"
		   "// is not only mechanical: `wl_shell_surface` has a `resize` request *and* a `resize`\n"
		   "// enumeration, so the nested spelling is a collision in the published protocols rather than a\n"
		   "// hypothetical one.\n"
		   "\n"
		   "#pragma once\n"
		   "\n"
		   "#include <cerrno>\n"
		   "#include <cstddef>\n"
		   "#include <cstdint>\n"
		   "#include <optional>\n"
		   "#include <span>\n"
		   "#include <string_view>\n"
		   "#include <utility>\n"
		   "\n"
		   "#include \"Core/Fd.h\"\n"
		   "#include \"Wire/Connection.h\"\n"
		   "#include \"Wire/Message.h\"\n"
		   "#include \"Wire/Reader.h\"\n"
		   "#include \"Wire/Writer.h\"\n";

	for (const std::size_t referenced : References(source.Model, self, catalog))
	{
		std::format_to(std::back_inserter(out), "#include \"{}\"\n", HeaderName(protocols[referenced]));
	}

	out += "\nnamespace Wayland\n{\n";

	for (const Interface& interface : source.Model.Interfaces)
	{
		std::format_to(std::back_inserter(out), "class {};\n", ProxyName(interface.Name));

		if (WantsListener(interface))
		{
			std::format_to(
				std::back_inserter(out),
				"class {};\nclass {};\n",
				ListenerName(interface.Name),
				IgnoringName(interface.Name)
			);
		}
	}

	out += "\n";

	for (const Interface& interface : source.Model.Interfaces)
	{
		for (const Enumeration& enumeration : interface.Enumerations)
		{
			EmitEnumeration(out, interface, enumeration);
		}
	}

	for (const Interface& interface : source.Model.Interfaces)
	{
		EmitProxy(out, interface, catalog);
	}

	for (const Interface& interface : source.Model.Interfaces)
	{
		if (WantsListener(interface))
		{
			EmitListener(out, interface, catalog);
		}
	}

	out += "} // namespace Wayland\n";

	return out;
}

std::string EmitSource(const ProtocolSource& source, const Catalog& catalog)
{
	std::string out;

	EmitPreamble(out, source);

	std::format_to(
		std::back_inserter(out),
		"\n#include \"{}\"\n"
		"\n"
		"#include <cerrno>\n"
		"#include <cstddef>\n"
		"#include <cstdint>\n"
		"#include <optional>\n"
		"#include <span>\n"
		"#include <string_view>\n"
		"#include <utility>\n"
		"\n"
		"#include \"Core/Fd.h\"\n"
		"#include \"Core/Result.h\"\n"
		"#include \"Wire/Connection.h\"\n"
		"#include \"Wire/Message.h\"\n"
		"#include \"Wire/Reader.h\"\n"
		"#include \"Wire/Writer.h\"\n"
		"\n"
		"namespace Wayland\n"
		"{{\n",
		HeaderName(source)
	);

	// Only where something in this file needs it, since an unused one is a warning and every warning
	// in this tree is an error.
	const bool anchored = std::ranges::any_of(source.Model.Interfaces, [](const Interface& interface) {
		return !IsDisplay(interface) && !WantsListener(interface);
	});

	if (anchored)
	{
		out += "namespace\n"
			   "{\n"
			   "// What an event-less proxy is bound to. `Connection::Bind` refuses a null `self` and there\n"
			   "// is no object to give it; nothing reads this, and the dispatchers that name it take no\n"
			   "// `self` at all.\n"
			   "char WireAnchor = 0;\n"
			   "} // namespace\n"
			   "\n";
	}

	for (const Interface& interface : source.Model.Interfaces)
	{
		if (!IsDisplay(interface))
		{
			EmitListenDefinition(out, interface);
			EmitDispatchDefinition(out, interface, catalog);
		}

		for (std::size_t opcode = 0; opcode < interface.Requests.size(); ++opcode)
		{
			// The untyped `new_id` is a template and is already defined in the header.
			if (const Argument* const created = CreatedArgument(interface.Requests[opcode]);
			    created != nullptr && created->Interface.empty())
			{
				continue;
			}

			EmitRequestDefinition(out, interface.Requests[opcode], interface, opcode, catalog);
		}
	}

	out += "} // namespace Wayland\n";

	return out;
}

// --- The server arm ----------------------------------------------------------------------------

// **What libwayland implements itself, and therefore what this arm must not.** Both are structural
// rather than a preference: `wl_display` is the connection, and `wl_registry.bind` is the one message
// in the published protocols whose `new_id` names no interface — three wire values rather than one,
// with the type chosen by the client at runtime. On the client arm that is a template a caller
// instantiates; arriving as a *request* there is nothing to instantiate, because the interface is a
// string the dispatcher reads. libwayland already owns both, so decision 2 has put them on the far
// side of the seam and this file leaves them there rather than inventing a form C++ does not have.
bool IsRegistry(const Interface& interface)
{
	return interface.Name == "wl_registry";
}

bool IsLibwaylandsOwn(const Interface& interface)
{
	return IsDisplay(interface) || IsRegistry(interface);
}

// Whether libwayland already defines this protocol's `wl_interface` objects, which is true of exactly
// one protocol and detectable rather than named: the core protocol is the one that declares
// `wl_display`, and it is the set `libwayland-server` is built with and exports through
// `<wayland-server-protocol.h>`. Emitting a second definition of `wl_surface_interface` would be a
// duplicate symbol at best and two tables libwayland picked between at worst.
bool IsCore(const Protocol& protocol)
{
	return std::ranges::any_of(protocol.Interfaces, [](const Interface& interface) { return IsDisplay(interface); });
}

std::string ResourceName(std::string_view wireInterface)
{
	return Pascal(wireInterface);
}

std::string HandlerName(std::string_view wireInterface)
{
	return std::format("{}Handler", Pascal(wireInterface));
}

std::string BindingName(std::string_view wireInterface)
{
	return std::format("{}Binding", Pascal(wireInterface));
}

std::string ServerHeaderName(const ProtocolSource& source)
{
	return std::format("Wayland/Server/{}.h", Unit(source.Path));
}

// The `wl_interface` object, under the name wayland-scanner gives it. Spelled the same whether this
// file defines it or libwayland does, which is what lets a types array reference an interface without
// caring which protocol it came from.
std::string WireInterfaceSymbol(std::string_view wireInterface)
{
	return std::format("{}_interface", wireInterface);
}

// The prefix every generated free function in a server source file carries, so that two protocols in
// one link never collide and a symbol in a backtrace names its interface.
std::string Trampoline(const Interface& interface, std::string_view suffix)
{
	return std::format("Wire{}{}", Pascal(interface.Name), suffix);
}

// The interface's `error` enumeration, or null. Not every interface has one, and where there is one
// `PostError` takes it rather than a `std::uint32_t` a call site would have to cast into.
const Enumeration* ErrorEnumeration(const Interface& interface)
{
	const auto found = std::ranges::find_if(interface.Enumerations, [](const Enumeration& enumeration) {
		return enumeration.Name == "error";
	});

	return found == interface.Enumerations.end() ? nullptr : &*found;
}

// How an argument is spelled on the server arm.
//
// `sending` is where the two directions genuinely differ, and in three places rather than the client
// arm's one:
//
//   **A string being sent is a `const char*`.** libwayland marshals through varargs and wants a
//   terminated string; a `std::string_view` here would have to be copied into a `std::string` inside
//   every generated event, which is an allocation the call site cannot see and cannot avoid. A string
//   being *received* is a view, and a nullable one is an optional — libwayland hands over a null
//   pointer for the protocol's null, so unlike the client arm this side really can tell the
//   difference between absent and empty.
//
//   **A descriptor being sent is borrowed and one being received is owned.** `wl_closure_marshal`
//   duplicates what it is given, which is why one keymap descriptor is sent to every client that
//   binds a seat and closed once; an arriving descriptor is the server's to close.
//
//   **An object is the resource class in both directions**, and a null `wl_resource*` becomes an
//   invalid resource rather than a crash, because a nullable object argument is a thing the protocol
//   declares and a client is entitled to send.
std::string ServerArgumentType(const Argument& argument, const Interface& owner, bool sending)
{
	if (!argument.Enumeration.empty())
	{
		const auto [interface, enumeration] = SplitEnumeration(argument, owner);

		return EnumName(interface, enumeration);
	}

	switch (argument.Kind)
	{
		case ArgumentKind::Int:
			return "std::int32_t";
		case ArgumentKind::Uint:
			return "std::uint32_t";
		case ArgumentKind::Fixed:
			return "wl_fixed_t";
		case ArgumentKind::String:
			if (sending)
			{
				return "const char*";
			}

			return argument.AllowNull ? "std::optional<std::string_view>" : "std::string_view";
		case ArgumentKind::Object:
		case ArgumentKind::NewId:
			return argument.Interface.empty() ? "wl_resource*" : ResourceName(argument.Interface);
		case ArgumentKind::Array:
			return "std::span<const std::byte>";
		case ArgumentKind::Fd:
			return sending ? "RawFd" : "Fd";
	}

	return "void";
}

// The C signature libwayland calls a request trampoline with: the demarshalled wire types, in wire
// order, with no interpretation. Every conversion into the vocabulary above happens inside the
// trampoline, where the resource that owns the handler is in scope.
std::string WireArgumentType(const Argument& argument)
{
	switch (argument.Kind)
	{
		case ArgumentKind::Int:
			return "std::int32_t";
		case ArgumentKind::Uint:
		case ArgumentKind::NewId:
			return "std::uint32_t";
		case ArgumentKind::Fixed:
			return "wl_fixed_t";
		case ArgumentKind::String:
			return "const char*";
		case ArgumentKind::Object:
			return "wl_resource*";
		case ArgumentKind::Array:
			return "wl_array*";
		case ArgumentKind::Fd:
			return "std::int32_t";
	}

	return "void";
}

// What a request handler returns: the implementation for the object the request creates, or void.
// Same rule as the client arm's events and for the same reason — the only way to receive a new object
// is to hand over the code that answers for it, in the same call, before the client can send it
// anything.
std::string RequestHandlerReturn(const Message& request)
{
	const Argument* const created = CreatedArgument(request);

	return created == nullptr ? "void" : std::format("{}*", HandlerName(created->Interface));
}

// The parameters an event takes. The created object is among them here rather than being a return,
// because a server sends a `new_id` for a resource it has already made — `WlDataOffer::Create` with
// an id of zero is how one is made without a client having asked for it.
std::vector<std::string> EventParameters(const Message& event, const Interface& owner, bool named)
{
	std::vector<std::string> parameters;

	for (const Argument& argument : event.Arguments)
	{
		std::string& parameter = parameters.emplace_back(ServerArgumentType(argument, owner, true));

		if (named)
		{
			std::format_to(std::back_inserter(parameter), " {}", Parameter(argument.Name));
		}
	}

	return parameters;
}

// The parameters a request handler takes: every argument except the one being created.
std::vector<std::string> RequestHandlerParameters(const Message& request, const Interface& owner, bool named)
{
	std::vector<std::string> parameters;

	for (const Argument& argument : request.Arguments)
	{
		if (argument.Kind == ArgumentKind::NewId)
		{
			continue;
		}

		std::string& parameter = parameters.emplace_back(ServerArgumentType(argument, owner, false));

		if (named)
		{
			std::format_to(std::back_inserter(parameter), " {}", Parameter(argument.Name));
		}
	}

	return parameters;
}

// --- The resource ------------------------------------------------------------------------------

// **`foreign` is the reduced form for what libwayland implements itself.** `wl_display` and
// `wl_registry` still have to be *nameable* — `wl_fixes.destroy_registry` takes a `wl_registry` as an
// ordinary object argument, and emitting a bare `wl_resource*` there would be the generator saying
// less than the XML did, which is the one thing Tools/Bindings/Emit.h refuses to do. So the class
// exists and the half that would be a lie does not: no `Create`, because libwayland makes these; no
// handler and no table, because libwayland answers them; and no events, because libwayland sends
// them. What is left is the half that is true of any resource — who owns it, what version it is, and
// the two ways to end it.
void EmitResource(std::string& out, const Interface& interface, bool foreign)
{
	const std::string resource = ResourceName(interface.Name);
	const Enumeration* const error = ErrorEnumeration(interface);

	Documentation(out, "", interface.Summary, interface.Description);

	if (foreign)
	{
		std::format_to(
			std::back_inserter(out),
			"//\n"
			"// **libwayland implements this one, so this class only names it.** There is no `Create` and\n"
			"// no handler: {} is answered inside the library, and what is here exists because other\n"
			"// interfaces take one as an argument and a generated signature should say which type it is\n"
			"// rather than handing over an untyped pointer.\n",
			interface.Name
		);
	}

	if (interface.Frozen)
	{
		std::format_to(
			std::back_inserter(out),
			"//\n// Frozen upstream at version {}: this interface is committed never to change again.\n",
			interface.Version
		);
	}

	std::format_to(
		std::back_inserter(out),
		"class {0}\n"
		"{{\n"
		"public:\n"
		"\t// What the global is advertised under, and the highest version these bindings describe. A\n"
		"\t// resource is never created above it, whatever a client asks for.\n"
		"\tstatic constexpr std::string_view WireName = \"{1}\";\n"
		"\tstatic constexpr std::uint32_t WireVersion = {2};\n"
		"\n",
		resource,
		interface.Name,
		interface.Version
	);

	if (!foreign)
	{
		std::format_to(
			std::back_inserter(out),
			"\tusing Handler = {};\n\tusing Ignoring = {};\n\tusing Binding = {};\n\n",
			HandlerName(interface.Name),
			IgnoringName(interface.Name),
			BindingName(interface.Name)
		);
	}

	std::format_to(
		std::back_inserter(out),
		"\t// A resource names an object; it does not own one. Copying is free and a copy of a resource\n"
		"\t// whose object has been destroyed is stale rather than dangling — every method below checks,\n"
		"\t// and the handler's own copy is cleared before `OnGone` runs.\n"
		"\t{0}() = default;\n"
		"\n"
		"\texplicit {0}(wl_resource* resource) noexcept : m_Resource{{ resource }} {{}}\n"
		"\n",
		resource
	);

	if (!foreign)
	{
		std::format_to(
			std::back_inserter(out),
			"\t// **The only way to make one of these, and it takes the implementation.** That is\n"
			"\t// decision 2's safety claim made structural rather than remembered: `wl_resource_create`\n"
			"\t// leaves a resource whose implementation is null, and a request arriving for one of those\n"
			"\t// is an abort inside libwayland that takes the whole compositor — every client of every\n"
			"\t// user — down at a moment the client chose. There is no spelling in this header that\n"
			"\t// produces the gap, because the call that creates the object is the call that fills it.\n"
			"\t//\n"
			"\t// `id` is the one the client named in the request that asked for this object. **Zero means\n"
			"\t// the server picks**, which is how an object a client never asked for — the `new_id` in an\n"
			"\t// event — comes to exist.\n"
			"\t//\n"
			"\t// The version is capped at `WireVersion`: a client cannot be handed an object claiming a\n"
			"\t// contract this binary does not implement. An invalid resource comes back where the\n"
			"\t// allocation failed, and the client has been sent `no_memory` by then — which is the\n"
			"\t// policy the rest of this compositor uses for a client it cannot serve, rather than dying\n"
			"\t// alongside it.\n"
			"\t[[nodiscard]] static {0} Create(\n"
			"\t\twl_client& client,\n"
			"\t\tstd::uint32_t version,\n"
			"\t\tstd::uint32_t id,\n"
			"\t\tHandler& handler\n"
			"\t);\n"
			"\n"
			"\t// Advertises this interface as a global. `binding` answers each client's bind with the\n"
			"\t// implementation for the resource it creates, and outlives the global.\n"
			"\t//\n"
			"\t// Null where the display refused it, which is a version above `WireVersion` or an\n"
			"\t// allocation failure at startup.\n"
			"\t[[nodiscard]] static wl_global* Advertise(wl_display& display, std::uint32_t version, Binding& "
		    "binding);\n"
			"\n",
			resource
		);
	}

	out += "\t[[nodiscard]] wl_resource* WireResource() const noexcept { return m_Resource; }\n"
		   "\n"
		   "\t[[nodiscard]] bool IsValid() const noexcept { return m_Resource != nullptr; }\n"
		   "\n"
		   "\t// The client this object belongs to, or null on a stale resource.\n"
		   "\t[[nodiscard]] wl_client* WireClient() const noexcept;\n"
		   "\n"
		   "\t// The version this object was created at, which every `since` gate below is read against.\n"
		   "\t// Not WireVersion: a client is entitled to bind below what these bindings describe, and an\n"
		   "\t// event it has never heard of would end its connection.\n"
		   "\t[[nodiscard]] std::uint32_t Version() const noexcept;\n"
		   "\n";

	if (error != nullptr)
	{
		std::format_to(
			std::back_inserter(out),
			"\t// Ends this client with a protocol error. The enumeration is {0}'s own, rather than a\n"
			"\t// std::uint32_t every call site would have to cast into.\n"
			"\t//\n"
			"\t// This is what a client that has misbehaved gets, and it is deliberately not what *gyro*\n"
			"\t// misbehaving gets: killing one client keeps the machine up, which is the whole shape of\n"
			"\t// decision 2's answer to who dies when something is wrong.\n"
			"\tvoid PostError({1} code, std::string_view message) const;\n"
			"\n",
			interface.Name,
			EnumName(interface.Name, error->Name)
		);
	}
	else
	{
		out += "\t// Ends this client with a protocol error. Untyped, because this interface declares no\n"
			   "\t// `error` enumeration for the code to come from.\n"
			   "\tvoid PostError(std::uint32_t code, std::string_view message) const;\n"
			   "\n";
	}

	out += "\t// Ends this client because the compositor could not allocate on its behalf. Kept beside\n"
		   "\t// `PostError` because it is the same policy under a different cause.\n"
		   "\tvoid PostNoMemory() const;\n"
		   "\n";

	out += foreign ? "\t// Destroys the object.\n\tvoid Destroy() const;\n\n" :
	                 "\t// Destroys the object. The handler's `OnGone` runs before this returns.\n"
	                 "\tvoid Destroy() const;\n"
	                 "\n";

	if (!foreign)
	{
		for (std::size_t opcode = 0; opcode < interface.Events.size(); ++opcode)
		{
			const Message& event = interface.Events[opcode];

			Documentation(out, "\t", event.Summary, event.Description);

			if (event.Since > 1)
			{
				std::format_to(
					std::back_inserter(out),
					"\t// Since version {}. Sending it to an object created below that is dropped rather\n"
					"\t// than marshalled, because a client answers an opcode it has never heard of by\n"
					"\t// ending the connection.\n",
					event.Since
				);
			}

			if (event.DeprecatedSince != 0)
			{
				std::format_to(
					std::back_inserter(out),
					"\t// Deprecated since version {}. Still spoken, so still generated.\n",
					event.DeprecatedSince
				);
			}

			if (event.Destructor)
			{
				out += "\t// `type=\"destructor\"`: the client releases the object on receiving this.\n";
			}

			std::format_to(
				std::back_inserter(out),
				"\tvoid {}({}) const;\n\n",
				Pascal(event.Name),
				Wrapped(EventParameters(event, interface, true), "\t", std::size_t{ 13 } + Pascal(event.Name).size())
			);
		}
	}

	out += "private:\n";

	if (!foreign)
	{
		std::format_to(
			std::back_inserter(out),
			"\tfriend class {};\n"
			"\n"
			"\t// libwayland's destroy hook. Clears the handler's copy before calling it, so `OnGone` is\n"
			"\t// the one place a handler can be sure the object is gone rather than going.\n"
			"\tstatic void WireGone(wl_resource* resource);\n"
			"\n",
			HandlerName(interface.Name)
		);
	}

	out += "\twl_resource* m_Resource = nullptr;\n};\n\n";
}

// --- The handler -------------------------------------------------------------------------------

void EmitHandler(std::string& out, const Interface& interface)
{
	const std::string resource = ResourceName(interface.Name);
	const std::string handler = HandlerName(interface.Name);

	std::format_to(
		std::back_inserter(out),
		"// What answers `{0}`'s requests.\n"
		"//\n"
		"// **Every handler is pure, and the dispatch table is built from the same list.** A request this\n"
		"// binding knows about and nobody answers is a compiler error at the class that forgot it — not\n"
		"// a null slot in the table libwayland indexes by opcode, which is the second of the two aborts\n"
		"// a client can reach and is reached by advertising a version whose request set is larger than\n"
		"// the functions supplied for it. Generating both from one walk over the XML is what makes that\n"
		"// unreachable rather than unlikely.\n"
		"//\n"
		"// One handler implements one object. `{1}::Create` records the pairing, `Object()` reads it\n"
		"// back, and a handler already bound is refused rather than silently rebound.\n"
		"class {2}\n"
		"{{\n"
		"public:\n"
		"\t{2}() = default;\n"
		"\n"
		"\tvirtual ~{2}() = default;\n"
		"\n"
		"\t// Neither copied nor moved: libwayland holds the address of this object for as long as the\n"
		"\t// resource lives, so a copy would be a second handler nothing ever calls and a move would\n"
		"\t// leave the dispatch pointing at the husk.\n"
		"\t{2}(const {2}&) = delete;\n"
		"\t{2}& operator=(const {2}&) = delete;\n"
		"\n"
		"\t// The object this handler was bound to, or an invalid resource before `Create` and after\n"
		"\t// the object is gone.\n"
		"\t[[nodiscard]] {1} Object() const noexcept {{ return m_Object; }}\n"
		"\n"
		"\t// **The object is gone.** Pure rather than optional, because a client disappearing is not an\n"
		"\t// event the protocol declares and it happens to every object eventually — a crash, a kill, a\n"
		"\t// user closing a window. A handler that has not said what it does here is the destroy-listener\n"
		"\t// use-after-free that decision 2 names as the best-known bug family in compositors, and it is\n"
		"\t// the one thing this file can turn into a compiler error.\n"
		"\t//\n"
		"\t// `Object()` is already invalid when this runs, and this handler may delete itself from it.\n"
		"\tvirtual void OnGone() = 0;\n"
		"\n",
		interface.Name,
		resource,
		handler
	);

	for (const Message& request : interface.Requests)
	{
		Documentation(out, "\t", request.Summary, request.Description);

		if (request.Since > 1)
		{
			std::format_to(
				std::back_inserter(out),
				"\t// Since version {}. A client bound below that cannot send it — libwayland checks the\n"
				"\t// opcode against the version before it reaches this table.\n",
				request.Since
			);
		}

		if (request.DeprecatedSince != 0)
		{
			std::format_to(
				std::back_inserter(out),
				"\t// Deprecated since version {}. Still spoken by older clients, so still generated.\n",
				request.DeprecatedSince
			);
		}

		if (request.Destructor)
		{
			out += "\t// `type=\"destructor\"`: the resource is destroyed the moment this returns, and\n"
				   "\t// `OnGone` runs immediately after. Doing it here rather than leaving it to the\n"
				   "\t// handler is the same argument as the rest of this file — a destructor request a\n"
				   "\t// call site forgot to honour is an object that outlives the client's idea of it.\n";
		}

		if (RequestHandlerReturn(request) != "void")
		{
			out += "\t// Returns the implementation for the object this request creates. Returning null\n"
				   "\t// ends the client with `no_memory`, because an id it believes in with nothing behind\n"
				   "\t// it is the abort this whole header exists to make unreachable.\n";
		}

		std::format_to(
			std::back_inserter(out),
			"\tvirtual {} On{}({}) = 0;\n\n",
			RequestHandlerReturn(request),
			Pascal(request.Name),
			Wrapped(
				RequestHandlerParameters(request, interface, true),
				"\t",
				std::size_t{ 20 } + RequestHandlerReturn(request).size() + Pascal(request.Name).size()
			)
		);
	}

	std::format_to(
		std::back_inserter(out),
		"private:\n"
		"\tfriend class {0};\n"
		"\n"
		"\t{0} m_Object;\n"
		"}};\n\n",
		resource
	);

	// The opt-out, on the same terms as the client arm's. A request that creates an object is left
	// pure: declining it is the null return that ends the client, so there is no doing-nothing answer
	// to inherit.
	std::format_to(
		std::back_inserter(out),
		"// `{0}` with every ignorable request answered by doing nothing. Deriving from this instead of\n"
		"// `{1}` is how a call site says it has read the request list and wants none of it.\n"
		"//\n"
		"// A request that creates an object is not among them and stays pure, because the only way to\n"
		"// ignore one is to refuse it, and refusing is what ends the client.\n"
		"class {2} : public {1}\n"
		"{{\n"
		"public:\n"
		"\tvoid OnGone() override {{}}\n"
		"\n",
		interface.Name,
		handler,
		IgnoringName(interface.Name)
	);

	for (const Message& request : interface.Requests)
	{
		if (RequestHandlerReturn(request) != "void")
		{
			continue;
		}

		std::format_to(
			std::back_inserter(out),
			"\tvoid On{}({}) override {{}}\n",
			Pascal(request.Name),
			Wrapped(
				RequestHandlerParameters(request, interface, false),
				"\t",
				std::size_t{ 21 } + Pascal(request.Name).size()
			)
		);
	}

	out += "};\n\n";

	// The bind side. A global has no resource behind it until a client asks, so the thing that answers
	// is not a handler — it is what mints one.
	std::format_to(
		std::back_inserter(out),
		"// What answers a client binding `{0}` as a global.\n"
		"//\n"
		"// Separate from the handler because a global is not an object: one of these serves every\n"
		"// client on the machine, and each bind produces a resource with a handler of its own.\n"
		"class {1}\n"
		"{{\n"
		"public:\n"
		"\t{1}() = default;\n"
		"\n"
		"\tvirtual ~{1}() = default;\n"
		"\n"
		"\t{1}(const {1}&) = delete;\n"
		"\t{1}& operator=(const {1}&) = delete;\n"
		"\n"
		"\t// The implementation for the resource this bind creates. Returning null ends the client with\n"
		"\t// `no_memory` rather than leaving it holding an id nothing answers for — see `Create`.\n"
		"\t//\n"
		"\t// `version` is what the client asked for, already capped by libwayland at what the global was\n"
		"\t// advertised at.\n"
		"\tvirtual {2}* OnBind(wl_client& client, std::uint32_t version) = 0;\n"
		"}};\n\n",
		interface.Name,
		BindingName(interface.Name),
		handler
	);
}

// --- The interface tables ----------------------------------------------------------------------

// The wire signature libwayland demarshals against: the `since` version where it is above one, then
// one character per argument, with a `?` ahead of any that the protocol allows to be null.
std::string WireSignature(const Message& message)
{
	std::string signature;

	if (message.Since > 1)
	{
		signature += std::to_string(message.Since);
	}

	for (const Argument& argument : message.Arguments)
	{
		if (argument.AllowNull)
		{
			signature += '?';
		}

		switch (argument.Kind)
		{
			case ArgumentKind::Int:
				signature += 'i';
				break;
			case ArgumentKind::Uint:
				signature += 'u';
				break;
			case ArgumentKind::Fixed:
				signature += 'f';
				break;
			case ArgumentKind::String:
				signature += 's';
				break;
			case ArgumentKind::Object:
				signature += 'o';
				break;
			case ArgumentKind::NewId:
				signature += 'n';
				break;
			case ArgumentKind::Array:
				signature += 'a';
				break;
			case ArgumentKind::Fd:
				signature += 'h';
				break;
		}
	}

	return signature;
}

std::string TypesName(const Interface& interface, std::string_view direction, const Message& message)
{
	return Trampoline(interface, std::format("{}{}Types", direction, Pascal(message.Name)));
}

// One `wl_interface` per interface, laid out the way wayland-scanner lays it out and under the same
// symbol, because that is the name every other consumer of these protocols already links against.
//
// **Emitted only where libwayland does not already have it.** The core protocol is compiled into
// `libwayland-server` and exported through `<wayland-server-protocol.h>`; a second definition of
// `wl_surface_interface` would be a duplicate symbol, and if it somehow linked it would be a second
// table libwayland might demarshal a client's request against.
void EmitWireInterface(std::string& out, const Interface& interface)
{
	for (const auto& [direction, messages] : { std::pair{ std::string_view{ "Request" }, &interface.Requests },
	                                           std::pair{ std::string_view{ "Event" }, &interface.Events } })
	{
		for (const Message& message : *messages)
		{
			if (message.Arguments.empty())
			{
				continue;
			}

			// Not const: `wl_message::types` is a `const wl_interface**`, so the array itself has to be
			// writable for the pointer to convert. libwayland never writes through it.
			std::format_to(
				std::back_inserter(out), "const wl_interface* {}[] = {{\n", TypesName(interface, direction, message)
			);

			for (const Argument& argument : message.Arguments)
			{
				const bool typed = (argument.Kind == ArgumentKind::Object || argument.Kind == ArgumentKind::NewId) &&
				                   !argument.Interface.empty();

				if (typed)
				{
					std::format_to(std::back_inserter(out), "\t&{},\n", WireInterfaceSymbol(argument.Interface));
				}
				else
				{
					out += "\tnullptr,\n";
				}
			}

			out += "};\n\n";
		}
	}

	for (const auto& [direction, messages] : { std::pair{ std::string_view{ "Request" }, &interface.Requests },
	                                           std::pair{ std::string_view{ "Event" }, &interface.Events } })
	{
		if (messages->empty())
		{
			continue;
		}

		std::format_to(std::back_inserter(out), "const wl_message {}[] = {{\n", Trampoline(interface, direction));

		for (const Message& message : *messages)
		{
			std::format_to(
				std::back_inserter(out),
				"\t{{ \"{}\", \"{}\", {} }},\n",
				message.Name,
				WireSignature(message),
				message.Arguments.empty() ? std::string{ "WireNoTypes" } : TypesName(interface, direction, message)
			);
		}

		out += "};\n\n";
	}
}

void EmitWireInterfaceObject(std::string& out, const Interface& interface)
{
	std::format_to(
		std::back_inserter(out),
		"extern \"C\" const wl_interface {0} = {{ \"{1}\", {2}, {3}, {4}, {5}, {6} }};\n\n",
		WireInterfaceSymbol(interface.Name),
		interface.Name,
		interface.Version,
		interface.Requests.size(),
		interface.Requests.empty() ? std::string{ "nullptr" } : Trampoline(interface, "Request"),
		interface.Events.size(),
		interface.Events.empty() ? std::string{ "nullptr" } : Trampoline(interface, "Event")
	);
}

// --- The dispatch table ------------------------------------------------------------------------

// One trampoline per request: libwayland's C signature on the outside, the handler's on the inside,
// and every conversion between them in one place.
void EmitTrampoline(std::string& out, const Interface& interface, const Message& request, const Catalog& catalog)
{
	const std::string handler = HandlerName(interface.Name);
	const Argument* const created = CreatedArgument(request);

	// The client is named only where it is used, which is the request that creates an object — the
	// same reason the client arm leaves a dispatcher's `self` unnamed when it has no listener to cast
	// it to. Every warning in this tree is an error, generated code included.
	std::vector<std::string> parameters{
		created == nullptr ? "wl_client*" : "wl_client* wireClient",
		"wl_resource* wireResource",
	};

	for (const Argument& argument : request.Arguments)
	{
		parameters.push_back(std::format("{} {}", WireArgumentType(argument), Parameter(argument.Name)));
	}

	const std::string name = Trampoline(interface, std::format("Request{}", Pascal(request.Name)));

	std::format_to(
		std::back_inserter(out), "void {}({})\n{{\n", name, Wrapped(parameters, "", std::size_t{ 6 } + name.size())
	);

	// The cast that cannot be wrong: `Create` is the only thing that makes a resource of this
	// interface and it always sets the user data to a handler of this type.
	std::format_to(
		std::back_inserter(out),
		"\tauto* const wireHandler = static_cast<{}*>(wl_resource_get_user_data(wireResource));\n\n",
		handler
	);

	std::vector<std::string> arguments;

	for (const Argument& argument : request.Arguments)
	{
		const std::string name_ = Parameter(argument.Name);

		if (argument.Kind == ArgumentKind::NewId)
		{
			continue;
		}

		if (!argument.Enumeration.empty())
		{
			arguments.push_back(
				std::format("static_cast<{}>({})", ServerArgumentType(argument, interface, false), name_)
			);

			continue;
		}

		switch (argument.Kind)
		{
			case ArgumentKind::String:
				if (argument.AllowNull)
				{
					// The protocol's null is libwayland's null pointer, so unlike the client arm this
					// side can carry the distinction the wire actually makes.
					arguments.push_back(
						std::format(
							"{0} == nullptr ? std::optional<std::string_view>{{}} : std::optional<std::string_view>{{ "
					        "{0} }}",
							name_
						)
					);
				}
				else
				{
					arguments.push_back(std::format("std::string_view{{ {} }}", name_));
				}
				break;

			case ArgumentKind::Object:
				arguments.push_back(
					argument.Interface.empty() ? name_ :
												 std::format("{}{{ {} }}", ResourceName(argument.Interface), name_)
				);
				break;

			case ArgumentKind::Array:
				arguments.push_back(
					std::format(
						"std::span<const std::byte>{{ static_cast<const std::byte*>({0}->data), {0}->size }}", name_
					)
				);
				break;

			case ArgumentKind::Fd:
				// Owned from here. libwayland hands the server the descriptor itself rather than a
				// duplicate, so nothing else is going to close it.
				arguments.push_back(std::format("Fd{{ {} }}", name_));
				break;

			default:
				arguments.push_back(name_);
				break;
		}
	}

	if (created == nullptr)
	{
		std::format_to(std::back_inserter(out), "\twireHandler->On{}({});\n", Pascal(request.Name), Joined(arguments));
	}
	else
	{
		std::format_to(
			std::back_inserter(out),
			"\t{0}* const wireImplementation = wireHandler->On{1}({2});\n"
			"\n"
			"\tif (wireImplementation == nullptr)\n"
			"\t{{\n"
			"\t\t// The client is holding an id it believes in and nothing answers for it. Ending the\n"
			"\t\t// client is the only answer that is not the abort this file exists to prevent.\n"
			"\t\twl_resource_post_no_memory(wireResource);\n"
			"\n"
			"\t\treturn;\n"
			"\t}}\n"
			"\n"
			"\t// The child is capped at this object's version — libwayland's rule, and the cap is not\n"
			"\t// decoration: an interface that has only ever had one version is created at one however\n"
			"\t// old the object that made it is.\n"
			"\t(void){4}::Create(\n"
			"\t\t*wireClient,\n"
			"\t\tstatic_cast<std::uint32_t>(wl_resource_get_version(wireResource)),\n"
			"\t\t{3},\n"
			"\t\t*wireImplementation\n"
			"\t);\n",
			HandlerName(created->Interface),
			Pascal(request.Name),
			Joined(arguments),
			Parameter(created->Name),
			ResourceName(created->Interface)
		);

		(void)catalog;
	}

	if (request.Destructor)
	{
		out += "\n"
			   "\t// `type=\"destructor\"`. Done here rather than left to the handler, so that a call site\n"
			   "\t// cannot forget it and leave an object alive that its client has stopped believing in.\n"
			   "\t// `OnGone` runs inside this call.\n"
			   "\twl_resource_destroy(wireResource);\n";
	}

	out += "}\n\n";
}

// The table itself. One member per request, declared from the XML and initialised from the same walk
// in the same order — so libwayland's `implementation[opcode]` cannot land on a null, which is the
// second of decision 2's two client-reachable aborts.
void EmitImplementationTable(std::string& out, const Interface& interface)
{
	std::format_to(std::back_inserter(out), "struct {}\n{{\n", Trampoline(interface, "Implementation"));

	for (const Message& request : interface.Requests)
	{
		std::vector<std::string> parameters{ "wl_client*", "wl_resource*" };

		for (const Argument& argument : request.Arguments)
		{
			parameters.push_back(WireArgumentType(argument));
		}

		std::format_to(
			std::back_inserter(out),
			"\tvoid (*{})({});\n",
			Pascal(request.Name),
			Wrapped(parameters, "\t", std::size_t{ 11 } + Pascal(request.Name).size())
		);
	}

	if (interface.Requests.empty())
	{
		out += "\t// No requests. The table is empty rather than absent, because a resource is still\n"
			   "\t// created with an implementation and libwayland still wants a pointer for it.\n";
	}

	std::format_to(
		std::back_inserter(out),
		"}};\n\nconstexpr {} {}{{\n",
		Trampoline(interface, "Implementation"),
		Trampoline(interface, "Table")
	);

	for (const Message& request : interface.Requests)
	{
		std::format_to(
			std::back_inserter(out), "\t&{},\n", Trampoline(interface, std::format("Request{}", Pascal(request.Name)))
		);
	}

	out += "};\n\n";
}

void EmitBindTrampoline(std::string& out, const Interface& interface)
{
	std::format_to(
		std::back_inserter(out),
		"void {0}(wl_client* wireClient, void* wireData, std::uint32_t wireVersion, std::uint32_t wireId)\n"
		"{{\n"
		"\tauto* const wireBinding = static_cast<{1}*>(wireData);\n"
		"\t{2}* const wireImplementation = wireBinding->OnBind(*wireClient, wireVersion);\n"
		"\n"
		"\tif (wireImplementation == nullptr)\n"
		"\t{{\n"
		"\t\twl_client_post_no_memory(wireClient);\n"
		"\n"
		"\t\treturn;\n"
		"\t}}\n"
		"\n"
		"\t(void){3}::Create(*wireClient, wireVersion, wireId, *wireImplementation);\n"
		"}}\n\n",
		Trampoline(interface, "Bind"),
		BindingName(interface.Name),
		HandlerName(interface.Name),
		ResourceName(interface.Name)
	);
}

// --- Resource definitions ----------------------------------------------------------------------

void EmitEventDefinition(std::string& out, const Interface& interface, const Message& event, std::size_t opcode)
{
	const std::string resource = ResourceName(interface.Name);
	const std::vector<std::string> parameters = EventParameters(event, interface, true);

	std::format_to(
		std::back_inserter(out),
		"void {0}::{1}({2}) const\n"
		"{{\n"
		"\tif (m_Resource == nullptr)\n"
		"\t{{\n"
		"\t\treturn;\n"
		"\t}}\n"
		"\n",
		resource,
		Pascal(event.Name),
		Wrapped(parameters, "", std::size_t{ 13 } + resource.size() + Pascal(event.Name).size())
	);

	if (event.Since > 1)
	{
		// Dropped rather than sent. libwayland does not check this, and a client handed an opcode
		// above the version it bound at answers by ending its own connection — which for a window is
		// the window vanishing while the compositor believes it is talking to it.
		std::format_to(
			std::back_inserter(out),
			"\tif (Version() < {0})\n"
			"\t{{\n"
			"\t\tRecordFault(\"sending {1}.{2}, which needs version {0}, to an object created below it\");\n"
			"\n"
			"\t\treturn;\n"
			"\t}}\n"
			"\n",
			event.Since,
			interface.Name,
			event.Name
		);
	}

	std::vector<std::string> arguments;

	for (const Argument& argument : event.Arguments)
	{
		const std::string name = Parameter(argument.Name);

		if (!argument.Enumeration.empty())
		{
			const std::string_view width = argument.Kind == ArgumentKind::Int ? "std::int32_t" : "std::uint32_t";

			arguments.push_back(std::format("static_cast<{}>({})", width, name));

			continue;
		}

		switch (argument.Kind)
		{
			case ArgumentKind::Object:
			case ArgumentKind::NewId:
				arguments.push_back(argument.Interface.empty() ? name : std::format("{}.WireResource()", name));
				break;

			case ArgumentKind::Array:
				// A borrowed view turned into the structure libwayland's varargs want. It is read and
				// copied inside the call and never kept, so pointing it at the caller's bytes is safe
				// and a copy here would be an allocation per event.
				std::format_to(
					std::back_inserter(out),
					"\twl_array wire{0}{{ .size = {1}.size(), .alloc = {1}.size(), .data = "
				    "const_cast<std::byte*>({1}.data()) }};\n"
					"\n",
					Pascal(argument.Name),
					name
				);

				arguments.push_back(std::format("&wire{}", Pascal(argument.Name)));
				break;

			case ArgumentKind::Fd:
				// Borrowed. `wl_closure_marshal` duplicates what it is handed, which is what lets one
				// keymap descriptor be sent to every client that binds a seat and closed once.
				arguments.push_back(std::format("{}.Value", name));
				break;

			default:
				arguments.push_back(name);
				break;
		}
	}

	std::vector<std::string> call{ "m_Resource", std::to_string(opcode) };
	call.insert(call.end(), arguments.begin(), arguments.end());

	std::format_to(
		std::back_inserter(out),
		"\twl_resource_post_event({});\n"
		"}}\n\n",
		Wrapped(call, "\t", std::size_t{ 24 })
	);
}

void EmitResourceDefinitions(std::string& out, const Interface& interface, bool foreign)
{
	const std::string resource = ResourceName(interface.Name);
	const Enumeration* const error = ErrorEnumeration(interface);

	if (!foreign)
	{
		std::format_to(
			std::back_inserter(out),
			"void {0}::WireGone(wl_resource* resource)\n"
			"{{\n"
			"\tauto* const wireHandler = static_cast<{1}*>(wl_resource_get_user_data(resource));\n"
			"\n"
			"\t// Cleared first, so that a handler tearing itself down inside `OnGone` cannot reach back\n"
			"\t// through `Object()` for a resource libwayland is in the middle of freeing.\n"
			"\twireHandler->m_Object = {{}};\n"
			"\twireHandler->OnGone();\n"
			"}}\n"
			"\n"
			"{0} {0}::Create(wl_client& client, std::uint32_t version, std::uint32_t id, Handler& handler)\n"
			"{{\n"
			"\tif (handler.m_Object.IsValid())\n"
			"\t{{\n"
			"\t\tRecordFault(\"binding one {2} handler to a second resource\");\n"
			"\n"
			"\t\treturn {{}};\n"
			"\t}}\n"
			"\n"
			"\tconst std::uint32_t wireVersion = version < WireVersion ? version : WireVersion;\n"
			"\n"
			"\twl_resource* const wireResource =\n"
			"\t\twl_resource_create(&client, &{3}, static_cast<int>(wireVersion), id);\n"
			"\n"
			"\tif (wireResource == nullptr)\n"
			"\t{{\n"
			"\t\twl_client_post_no_memory(&client);\n"
			"\n"
			"\t\treturn {{}};\n"
			"\t}}\n"
			"\n"
			"\t// **Before this function returns, and therefore before anything can be dispatched to it.**\n"
			"\t// `wl_resource_create` leaves the implementation null and libwayland aborts the process on\n"
			"\t// a request that finds one — the gap exists for exactly the width of these two calls, and\n"
			"\t// it is closed here because there is no other door onto a resource of this type.\n"
			"\twl_resource_set_implementation(wireResource, &{4}, &handler, &{0}::WireGone);\n"
			"\n"
			"\thandler.m_Object = {0}{{ wireResource }};\n"
			"\n"
			"\treturn handler.m_Object;\n"
			"}}\n"
			"\n"
			"wl_global* {0}::Advertise(wl_display& display, std::uint32_t version, Binding& binding)\n"
			"{{\n"
			"\tif (version == 0 || version > WireVersion)\n"
			"\t{{\n"
			"\t\tRecordFault(\"advertising {2} at a version these bindings do not describe\");\n"
			"\n"
			"\t\treturn nullptr;\n"
			"\t}}\n"
			"\n"
			"\treturn wl_global_create(&display, &{3}, static_cast<int>(version), &binding, &{5});\n"
			"}}\n"
			"\n",
			resource,
			HandlerName(interface.Name),
			interface.Name,
			WireInterfaceSymbol(interface.Name),
			Trampoline(interface, "Table"),
			Trampoline(interface, "Bind")
		);
	}

	std::format_to(
		std::back_inserter(out),
		"wl_client* {0}::WireClient() const noexcept\n"
		"{{\n"
		"\treturn m_Resource == nullptr ? nullptr : wl_resource_get_client(m_Resource);\n"
		"}}\n"
		"\n"
		"std::uint32_t {0}::Version() const noexcept\n"
		"{{\n"
		"\treturn m_Resource == nullptr ? 0 : static_cast<std::uint32_t>(wl_resource_get_version(m_Resource));\n"
		"}}\n"
		"\n"
		"void {0}::PostError({1} code, std::string_view message) const\n"
		"{{\n"
		"\tif (m_Resource == nullptr)\n"
		"\t{{\n"
		"\t\treturn;\n"
		"\t}}\n"
		"\n"
		"\t// `%.*s` because a view is not terminated and libwayland's formatter wants a C string. The\n"
		"\t// alternative is a copy into a std::string on the path that is ending the client anyway.\n"
		"\twl_resource_post_error(\n"
		"\t\tm_Resource,\n"
		"\t\tstatic_cast<std::uint32_t>(code),\n"
		"\t\t\"%.*s\",\n"
		"\t\tstatic_cast<int>(message.size()),\n"
		"\t\tmessage.data()\n"
		"\t);\n"
		"}}\n"
		"\n"
		"void {0}::PostNoMemory() const\n"
		"{{\n"
		"\tif (m_Resource != nullptr)\n"
		"\t{{\n"
		"\t\twl_resource_post_no_memory(m_Resource);\n"
		"\t}}\n"
		"}}\n"
		"\n"
		"void {0}::Destroy() const\n"
		"{{\n"
		"\tif (m_Resource != nullptr)\n"
		"\t{{\n"
		"\t\twl_resource_destroy(m_Resource);\n"
		"\t}}\n"
		"}}\n"
		"\n",
		resource,
		error == nullptr ? std::string{ "std::uint32_t" } : EnumName(interface.Name, error->Name)
	);

	if (foreign)
	{
		return;
	}

	for (std::size_t opcode = 0; opcode < interface.Events.size(); ++opcode)
	{
		EmitEventDefinition(out, interface, interface.Events[opcode], opcode);
	}
}

// --- Server files ------------------------------------------------------------------------------

// The one generated file with no protocol behind it, and it is here rather than hand-written in
// Source/ because there is no module for it to live in yet: the generated library links `Core` and
// `libwayland-server` and nothing else.
//
// What it exists for is the gap libwayland leaves. `wl_resource_post_error` is what a *client* gets
// for misbehaving, and it is deliberately not what gyro gets — killing one client keeps the machine
// up. The remaining case is gyro's own mistake, and libwayland's own logger is not exported, so the
// generated code needs somewhere to say "the compositor tried to send an event this object is too old
// for" that is neither silence nor a machine-wide abort.
EmittedFile EmitFaultHeader()
{
	return EmittedFile{
		.Path = "Wayland/Server/Fault.h",
		.Text = "// The generated server bindings' fault sink. Written by Tools/Bindings. Do not edit.\n"
				"//\n"
				"// This is the one file here with no protocol behind it. The generated code needs a way to\n"
				"// report *gyro's* protocol mistakes — an event sent to an object too old for it, a handler\n"
				"// bound twice — and the three answers libwayland offers are all wrong for that: killing the\n"
				"// client blames the wrong party, aborting takes down every client of every user on the\n"
				"// machine, and its own logger is not an exported symbol.\n"
				"//\n"
				"// So a fault is recorded and the compositor stays up. The default handler writes to stderr,\n"
				"// which is what a developer running gyro from a terminal will see; the composition root\n"
				"// replaces it with the real log once there is one.\n"
				"\n"
				"#pragma once\n"
				"\n"
				"#include <string_view>\n"
				"\n"
				"namespace Wayland::Server\n"
				"{\n"
				"// Called from the generated bindings, on the thread the fault happened on.\n"
				"using FaultHandler = void (*)(std::string_view message);\n"
				"\n"
				"// Replaces the default. Null restores it.\n"
				"void SetFaultHandler(FaultHandler handler) noexcept;\n"
				"\n"
				"// A mistake on gyro's side of the connection. Never a client's — that is `PostError`.\n"
				"void RecordFault(std::string_view message);\n"
				"} // namespace Wayland::Server\n",
	};
}

EmittedFile EmitFaultSource()
{
	return EmittedFile{
		.Path = "Wayland/Server/Fault.cpp",
		.Text =
			"// The generated server bindings' fault sink. Written by Tools/Bindings. Do not edit.\n"
			"\n"
			"#include \"Wayland/Server/Fault.h\"\n"
			"\n"
			"#include <cstdio>\n"
			"#include <string_view>\n"
			"\n"
			"namespace Wayland::Server\n"
			"{\n"
			"namespace\n"
			"{\n"
			"void ToStandardError(std::string_view message)\n"
			"{\n"
			"\tstd::fprintf(stderr, \"gyro: wayland: %.*s\\n\", static_cast<int>(message.size()), message.data());\n"
			"}\n"
			"\n"
			"FaultHandler Handler = &ToStandardError;\n"
			"} // namespace\n"
			"\n"
			"void SetFaultHandler(FaultHandler handler) noexcept\n"
			"{\n"
			"\tHandler = handler == nullptr ? &ToStandardError : handler;\n"
			"}\n"
			"\n"
			"void RecordFault(std::string_view message)\n"
			"{\n"
			"\tHandler(message);\n"
			"}\n"
			"} // namespace Wayland::Server\n",
	};
}

std::string EmitServerHeader(
	const ProtocolSource& source,
	std::size_t self,
	const Catalog& catalog,
	const std::span<const ProtocolSource> protocols
)
{
	std::string out;

	EmitPreamble(out, source);

	out += "//\n"
		   "// **A resource that cannot exist without an implementation, and a dispatch table that cannot\n"
		   "// have a hole.** Both are properties of this file rather than of the code that uses it, and\n"
		   "// they are the two things decision 2 asks the bindings for in exchange for leaving the server\n"
		   "// codec to libwayland. Every request an interface declares is a pure virtual on its handler,\n"
		   "// so a request nobody answered is a compiler error rather than a null slot libwayland indexes\n"
		   "// into; and the typed `Create` is the only way to make a resource, so there is no instant in\n"
		   "// which a client's id is live and has nothing behind it. Both failures are `wl_abort` inside\n"
		   "// libwayland, which on a compositor with no VTs is the machine going black with no way in.\n"
		   "//\n"
		   "// Read a handler's `Ignoring` sibling as the deliberate opposite: it answers every ignorable\n"
		   "// request by doing nothing, and a call site that derives from it has said so in one word that\n"
		   "// greps. A request that creates an object is never among them, because the only way to ignore\n"
		   "// one is to refuse it and refusing ends the client.\n"
		   "//\n"
		   "// The layout is the client arm's, for the client arm's reason: forward declarations,\n"
		   "// enumerations, resources, handlers — in that order, so that interfaces referring to each\n"
		   "// other in both directions need no ordering at all.\n"
		   "\n"
		   "#pragma once\n"
		   "\n"
		   "#include <cstddef>\n"
		   "#include <cstdint>\n"
		   "#include <optional>\n"
		   "#include <span>\n"
		   "#include <string_view>\n"
		   "\n"
		   "#include <wayland-server-core.h>\n";

	if (IsCore(source.Model))
	{
		out += "\n"
			   "// libwayland is built with the core protocol and exports a `wl_interface` for every\n"
			   "// interface in it, so this file describes them and does not define them.\n"
			   "#include <wayland-server-protocol.h>\n";
	}

	out += "\n"
		   "#include \"Core/Fd.h\"\n"
		   "#include \"Wayland/Server/Fault.h\"\n";

	for (const std::size_t referenced : References(source.Model, self, catalog))
	{
		std::format_to(std::back_inserter(out), "#include \"{}\"\n", ServerHeaderName(protocols[referenced]));
	}

	if (!IsCore(source.Model))
	{
		out += "\n"
			   "// The tables libwayland demarshals against, under wayland-scanner's names because that is\n"
			   "// what everything else that speaks these protocols already links. Defined in the source\n"
			   "// file beside this one.\n"
			   "extern \"C\"\n"
			   "{\n";

		for (const Interface& interface : source.Model.Interfaces)
		{
			if (IsLibwaylandsOwn(interface))
			{
				continue;
			}

			std::format_to(
				std::back_inserter(out), "extern const wl_interface {};\n", WireInterfaceSymbol(interface.Name)
			);
		}

		out += "}\n";
	}

	out += "\nnamespace Wayland::Server\n{\n";

	for (const Interface& interface : source.Model.Interfaces)
	{
		std::format_to(std::back_inserter(out), "class {};\n", ResourceName(interface.Name));

		if (IsLibwaylandsOwn(interface))
		{
			continue;
		}

		std::format_to(
			std::back_inserter(out),
			"class {};\nclass {};\nclass {};\n",
			HandlerName(interface.Name),
			IgnoringName(interface.Name),
			BindingName(interface.Name)
		);
	}

	out += "\n";

	for (const Interface& interface : source.Model.Interfaces)
	{
		for (const Enumeration& enumeration : interface.Enumerations)
		{
			EmitEnumeration(out, interface, enumeration);
		}
	}

	for (const Interface& interface : source.Model.Interfaces)
	{
		EmitResource(out, interface, IsLibwaylandsOwn(interface));
	}

	for (const Interface& interface : source.Model.Interfaces)
	{
		if (!IsLibwaylandsOwn(interface))
		{
			EmitHandler(out, interface);
		}
	}

	out += "} // namespace Wayland::Server\n";

	return out;
}

std::string EmitServerSource(const ProtocolSource& source, const Catalog& catalog)
{
	std::string out;

	EmitPreamble(out, source);

	std::format_to(
		std::back_inserter(out),
		"\n#include \"{}\"\n"
		"\n"
		"#include <cstddef>\n"
		"#include <cstdint>\n"
		"#include <optional>\n"
		"#include <span>\n"
		"#include <string_view>\n"
		"\n"
		"#include <wayland-server-core.h>\n"
		"\n"
		"#include \"Core/Fd.h\"\n"
		"#include \"Wayland/Server/Fault.h\"\n"
		"\n",
		ServerHeaderName(source)
	);

	const bool tables = !IsCore(source.Model);

	if (tables)
	{
		const bool anyEmpty = std::ranges::any_of(source.Model.Interfaces, [](const Interface& interface) {
			if (IsLibwaylandsOwn(interface))
			{
				return false;
			}

			const auto empty = [](const Message& message) { return message.Arguments.empty(); };

			return std::ranges::any_of(interface.Requests, empty) || std::ranges::any_of(interface.Events, empty);
		});

		out += "namespace\n{\n";

		if (anyEmpty)
		{
			out += "// What a message with no arguments points at. The count beside it is zero, so nothing\n"
				   "// ever reads through this — `wl_message::types` simply may not be null.\n"
				   "const wl_interface* WireNoTypes[] = { nullptr };\n\n";
		}

		for (const Interface& interface : source.Model.Interfaces)
		{
			if (!IsLibwaylandsOwn(interface))
			{
				EmitWireInterface(out, interface);
			}
		}

		out += "} // namespace\n\n";

		for (const Interface& interface : source.Model.Interfaces)
		{
			if (!IsLibwaylandsOwn(interface))
			{
				EmitWireInterfaceObject(out, interface);
			}
		}
	}

	out += "namespace Wayland::Server\n"
		   "{\n"
		   "namespace\n"
		   "{\n";

	for (const Interface& interface : source.Model.Interfaces)
	{
		if (IsLibwaylandsOwn(interface))
		{
			continue;
		}

		for (const Message& request : interface.Requests)
		{
			EmitTrampoline(out, interface, request, catalog);
		}

		EmitImplementationTable(out, interface);
		EmitBindTrampoline(out, interface);
	}

	out += "} // namespace\n\n";

	for (const Interface& interface : source.Model.Interfaces)
	{
		EmitResourceDefinitions(out, interface, IsLibwaylandsOwn(interface));
	}

	out += "} // namespace Wayland::Server\n";

	return out;
}

// --- Validation --------------------------------------------------------------------------------

Result<Catalog> BuildCatalog(std::span<const ProtocolSource> protocols, Direction direction, EmitDiagnostic* diagnostic)
{
	Catalog catalog;

	// Every name this generator puts at namespace scope, in one set. A proxy, a listener, an
	// `Ignoring` and a flattened enumeration all land there, so `wl_output` + `transform` and a
	// hypothetical interface named `wl_output_transform` are the same C++ word and one of them has to
	// lose. Refusing is what turns that into a sentence naming both rather than a redefinition error
	// in a file nobody wrote.
	std::map<std::string, std::string> spellings;

	for (std::size_t index = 0; index < protocols.size(); ++index)
	{
		for (const Interface& interface : protocols[index].Model.Interfaces)
		{
			if (const auto [_, inserted] = catalog.try_emplace(interface.Name, Located{ index, &interface }); !inserted)
			{
				return Refuse(
					diagnostic,
					EmitFailure::NameCollision,
					std::format("two protocols in the set define the interface {}", interface.Name)
				);
			}

			// The classes this direction puts at namespace scope for the interface. They differ: the
			// client arm emits a listener only where there are events to answer, and the server arm
			// emits all four for every interface it emits at all.
			std::vector<std::pair<std::string, std::string>> claimed;

			if (direction == Direction::Server)
			{
				// The two libwayland implements still get a class — `wl_fixes.destroy_registry` takes a
				// `wl_registry` — so the name is still claimed. The three that answer for an interface
				// are not, because that reduced form has none of them.
				claimed.emplace_back(ResourceName(interface.Name), interface.Name);

				if (!IsLibwaylandsOwn(interface))
				{
					claimed.emplace_back(HandlerName(interface.Name), interface.Name);
					claimed.emplace_back(IgnoringName(interface.Name), interface.Name);
					claimed.emplace_back(BindingName(interface.Name), interface.Name);
				}
			}
			else
			{
				claimed.emplace_back(ProxyName(interface.Name), interface.Name);

				if (WantsListener(interface))
				{
					claimed.emplace_back(ListenerName(interface.Name), interface.Name);
					claimed.emplace_back(IgnoringName(interface.Name), interface.Name);
				}
			}

			for (const Enumeration& enumeration : interface.Enumerations)
			{
				claimed.emplace_back(
					EnumName(interface.Name, enumeration.Name), std::format("{}.{}", interface.Name, enumeration.Name)
				);
			}

			for (auto& [spelling, origin] : claimed)
			{
				if (const auto [existing, inserted] = spellings.try_emplace(spelling, origin); !inserted)
				{
					return Refuse(
						diagnostic,
						EmitFailure::NameCollision,
						std::format("{} and {} both spell {}", existing->second, origin, spelling)
					);
				}
			}
		}
	}

	return catalog;
}

Result<void> ValidateMessage(
	const Message& message,
	const Interface& owner,
	bool isEvent,
	Direction direction,
	const Catalog& catalog,
	EmitDiagnostic* diagnostic
)
{
	std::size_t created = 0;

	for (const Argument& argument : message.Arguments)
	{
		if (argument.Kind == ArgumentKind::NewId)
		{
			++created;

			// An untyped `new_id` becomes a template parameter on the message the *sender* writes,
			// which is a thing a caller can supply and a dispatcher cannot: the receiving side would
			// have to name the interface from the wire and instantiate a binding for it, which C++
			// has no form for.
			//
			// Which half that catches flips with the direction, because what a client sends a server
			// receives. The only untyped `new_id` in the published protocols is `wl_registry.bind`,
			// and the server arm never reaches this line for it — libwayland implements the registry,
			// so the interface is skipped before validation begins.
			const bool dispatched = (direction == Direction::Client) == isEvent;

			if (dispatched && argument.Interface.empty())
			{
				return Refuse(
					diagnostic,
					EmitFailure::UntypedEventArgument,
					std::format(
						"the {} {}.{} creates an object of no named interface, which a dispatcher "
						"cannot construct",
						isEvent ? "event" : "request",
						owner.Name,
						message.Name
					)
				);
			}
		}

		if (!argument.Interface.empty() && !catalog.contains(argument.Interface))
		{
			return Refuse(
				diagnostic,
				EmitFailure::UnknownInterface,
				std::format(
					"{}.{} names the interface {}, which no protocol in the set defines; add the "
					"protocol that does",
					owner.Name,
					message.Name,
					argument.Interface
				)
			);
		}

		if (argument.Enumeration.empty())
		{
			continue;
		}

		const auto [interface, enumeration] = SplitEnumeration(argument, owner);
		const auto found = catalog.find(interface);

		if (found == catalog.end())
		{
			return Refuse(
				diagnostic,
				EmitFailure::UnknownInterface,
				std::format(
					"{0}.{1} draws its {2} argument from {3}.{4}, and no protocol in the set defines {3}",
					owner.Name,
					message.Name,
					argument.Name,
					interface,
					enumeration
				)
			);
		}

		const std::vector<Enumeration>& enumerations = found->second.Definition->Enumerations;
		const bool declared = std::ranges::any_of(enumerations, [&](const Enumeration& candidate) {
			return candidate.Name == enumeration;
		});

		if (!declared)
		{
			return Refuse(
				diagnostic,
				EmitFailure::UnknownEnumeration,
				std::format(
					"{0}.{1} draws its {2} argument from {3}.{4}, which {3} does not declare",
					owner.Name,
					message.Name,
					argument.Name,
					interface,
					enumeration
				)
			);
		}
	}

	if (created > 1)
	{
		return Refuse(
			diagnostic,
			EmitFailure::UntypedEventArgument,
			std::format(
				"{}.{} creates more than one object, which a single return value cannot carry", owner.Name, message.Name
			)
		);
	}

	return {};
}

Result<void>
ValidateInterface(const Interface& interface, Direction direction, const Catalog& catalog, EmitDiagnostic* diagnostic)
{
	// **Which half of the interface lands on the class flips with the direction.** A proxy declares a
	// method per *request* and its listener a handler per *event*; a resource declares a method per
	// *event* and its handler one per request. So the same protocol is checked against two different
	// reserved sets, and a name that is a collision on one arm can be perfectly emittable on the
	// other.
	const bool sendingIsRequests = direction == Direction::Client;
	const std::vector<Message>& sent = sendingIsRequests ? interface.Requests : interface.Events;
	const std::vector<Message>& received = sendingIsRequests ? interface.Events : interface.Requests;

	// One namespace of members per class: the messages it sends, and everything the generated class
	// declares for itself.
	std::map<std::string, std::string> members;

	auto claim = [&](std::string spelling, std::string origin) -> Result<void> {
		const bool reserved = sendingIsRequests ? IsReservedProxyMember(spelling) : IsReservedResourceMember(spelling);

		if (reserved)
		{
			return Refuse(
				diagnostic,
				EmitFailure::NameCollision,
				std::format(
					"{} spells {}, which every generated {} already declares",
					origin,
					spelling,
					sendingIsRequests ? "proxy" : "resource"
				)
			);
		}

		if (const auto [existing, inserted] = members.try_emplace(spelling, origin); !inserted)
		{
			return Refuse(
				diagnostic,
				EmitFailure::NameCollision,
				std::format("{} and {} both spell {} on {}", existing->second, origin, spelling, interface.Name)
			);
		}

		return {};
	};

	for (const Enumeration& enumeration : interface.Enumerations)
	{
		// Enumerators share one scope, so two entries that PascalCase alike are a redefinition.
		std::map<std::string, std::string> entries;

		for (const EnumerationEntry& entry : enumeration.Entries)
		{
			if (const auto [existing, inserted] = entries.try_emplace(Pascal(entry.Name), entry.Name); !inserted)
			{
				return Refuse(
					diagnostic,
					EmitFailure::NameCollision,
					std::format(
						"{} and {} in {}.{} both spell {}",
						existing->second,
						entry.Name,
						interface.Name,
						enumeration.Name,
						Pascal(entry.Name)
					)
				);
			}
		}
	}

	for (const Message& message : sent)
	{
		if (Result<void> claimed = claim(
				Pascal(message.Name),
				std::format("the {} {}.{}", sendingIsRequests ? "request" : "event", interface.Name, message.Name)
			);
		    !claimed)
		{
			return claimed;
		}

		if (Result<void> valid =
		        ValidateMessage(message, interface, !sendingIsRequests, direction, catalog, diagnostic);
		    !valid)
		{
			return valid;
		}
	}

	// The receiving half lives on the listener or the handler rather than on the class the caller
	// holds, and carries an `On` prefix, so it collides only with itself.
	std::map<std::string, std::string> handlers;

	for (const Message& message : received)
	{
		if (const auto [existing, inserted] = handlers.try_emplace(Pascal(message.Name), message.Name); !inserted)
		{
			return Refuse(
				diagnostic,
				EmitFailure::NameCollision,
				std::format(
					"the {}s {} and {} on {} both spell On{}",
					sendingIsRequests ? "event" : "request",
					existing->second,
					message.Name,
					interface.Name,
					Pascal(message.Name)
				)
			);
		}

		if (Result<void> valid = ValidateMessage(message, interface, sendingIsRequests, direction, catalog, diagnostic);
		    !valid)
		{
			return valid;
		}
	}

	return {};
}

// Two protocols that name each other's interfaces, which would be two headers each including the
// other. Reported here rather than by the compiler, which would report it as an unknown type in
// generated code and name neither protocol.
Result<void>
ValidateAcyclic(std::span<const ProtocolSource> protocols, const Catalog& catalog, EmitDiagnostic* diagnostic)
{
	for (std::size_t index = 0; index < protocols.size(); ++index)
	{
		for (const std::size_t referenced : References(protocols[index].Model, index, catalog))
		{
			if (References(protocols[referenced].Model, referenced, catalog).contains(index))
			{
				return Refuse(
					diagnostic,
					EmitFailure::CyclicReference,
					std::format(
						"{} and {} reference each other, so neither header can include the other",
						protocols[index].Model.Name,
						protocols[referenced].Model.Name
					)
				);
			}
		}
	}

	return {};
}
} // namespace

Result<std::vector<EmittedFile>>
Emit(std::span<const ProtocolSource> protocols, Direction direction, EmitDiagnostic* diagnostic)
{
	const Result<Catalog> catalog = BuildCatalog(protocols, direction, diagnostic);

	if (!catalog)
	{
		return std::unexpected{ catalog.error() };
	}

	for (const ProtocolSource& source : protocols)
	{
		for (const Interface& interface : source.Model.Interfaces)
		{
			// The two libwayland implements itself are not emitted and so are not checked. Validating
			// them anyway would refuse `wl_registry.bind` for a shape no generated line has to express.
			if (direction == Direction::Server && IsLibwaylandsOwn(interface))
			{
				continue;
			}

			if (Result<void> valid = ValidateInterface(interface, direction, *catalog, diagnostic); !valid)
			{
				return std::unexpected{ valid.error() };
			}
		}
	}

	if (Result<void> acyclic = ValidateAcyclic(protocols, *catalog, diagnostic); !acyclic)
	{
		return std::unexpected{ acyclic.error() };
	}

	std::vector<EmittedFile> files;

	if (direction == Direction::Server)
	{
		files.push_back(EmitFaultHeader());
		files.push_back(EmitFaultSource());
	}

	for (std::size_t index = 0; index < protocols.size(); ++index)
	{
		if (direction == Direction::Server)
		{
			files.push_back(
				EmittedFile{
					.Path = ServerHeaderName(protocols[index]),
					.Text = EmitServerHeader(protocols[index], index, *catalog, protocols),
				}
			);

			files.push_back(
				EmittedFile{
					.Path = std::format("Wayland/Server/{}.cpp", Unit(protocols[index].Path)),
					.Text = EmitServerSource(protocols[index], *catalog),
				}
			);

			continue;
		}

		files.push_back(
			EmittedFile{
				.Path = HeaderName(protocols[index]),
				.Text = EmitHeader(protocols[index], index, *catalog, protocols),
			}
		);

		files.push_back(
			EmittedFile{
				.Path = std::format("Wayland/{}.cpp", Unit(protocols[index].Path)),
				.Text = EmitSource(protocols[index], *catalog),
			}
		);
	}

	return files;
}
