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

// --- Validation --------------------------------------------------------------------------------

Result<Catalog> BuildCatalog(std::span<const ProtocolSource> protocols, EmitDiagnostic* diagnostic)
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

			std::vector<std::pair<std::string, std::string>> claimed{
				{ ProxyName(interface.Name), interface.Name },
			};

			if (WantsListener(interface))
			{
				claimed.emplace_back(ListenerName(interface.Name), interface.Name);
				claimed.emplace_back(IgnoringName(interface.Name), interface.Name);
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

			// An untyped `new_id` becomes a template parameter on a request, which is a thing a
			// caller can supply and a dispatcher cannot: an event would have to name the interface
			// from the wire and instantiate a proxy for it, which C++ has no form for. No published
			// protocol does this; refusing says so at the file that starts.
			if (isEvent && argument.Interface.empty())
			{
				return Refuse(
					diagnostic,
					EmitFailure::UntypedEventArgument,
					std::format(
						"the event {}.{} creates an object of no named interface, which a dispatcher "
						"cannot construct",
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

Result<void> ValidateInterface(const Interface& interface, const Catalog& catalog, EmitDiagnostic* diagnostic)
{
	// One namespace of members per proxy: the requests, the enumeration aliases, and everything the
	// generated class declares for itself.
	std::map<std::string, std::string> members;

	auto claim = [&](std::string spelling, std::string origin) -> Result<void> {
		if (IsReservedMember(spelling))
		{
			return Refuse(
				diagnostic,
				EmitFailure::NameCollision,
				std::format("{} spells {}, which every generated proxy already declares", origin, spelling)
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

	for (const Message& request : interface.Requests)
	{
		if (Result<void> claimed =
		        claim(Pascal(request.Name), std::format("the request {}.{}", interface.Name, request.Name));
		    !claimed)
		{
			return claimed;
		}

		if (Result<void> valid = ValidateMessage(request, interface, false, catalog, diagnostic); !valid)
		{
			return valid;
		}
	}

	// Handlers live on the listener rather than the proxy and carry an `On` prefix, so they collide
	// only with each other.
	std::map<std::string, std::string> handlers;

	for (const Message& event : interface.Events)
	{
		if (const auto [existing, inserted] = handlers.try_emplace(Pascal(event.Name), event.Name); !inserted)
		{
			return Refuse(
				diagnostic,
				EmitFailure::NameCollision,
				std::format(
					"the events {} and {} on {} both spell On{}",
					existing->second,
					event.Name,
					interface.Name,
					Pascal(event.Name)
				)
			);
		}

		if (Result<void> valid = ValidateMessage(event, interface, true, catalog, diagnostic); !valid)
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

Result<std::vector<EmittedFile>> Emit(std::span<const ProtocolSource> protocols, EmitDiagnostic* diagnostic)
{
	const Result<Catalog> catalog = BuildCatalog(protocols, diagnostic);

	if (!catalog)
	{
		return std::unexpected{ catalog.error() };
	}

	for (const ProtocolSource& source : protocols)
	{
		for (const Interface& interface : source.Model.Interfaces)
		{
			if (Result<void> valid = ValidateInterface(interface, *catalog, diagnostic); !valid)
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

	for (std::size_t index = 0; index < protocols.size(); ++index)
	{
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
