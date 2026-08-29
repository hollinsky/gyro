#include "Emit.Client.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Emit.Shared.h"
#include "Naming.h"
#include "Protocol.h"

namespace
{
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
			"\t\tm_Connection->Output().RecordFault(bound.error().Code(), bound.error().Sentence());\n"
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
		"\t\t// The sentence is a literal with static storage either way, so the connection's own is\n"
		"\t\t// carried rather than replaced by a vaguer one from here.\n"
		"\t\tm_Connection->Output().RecordFault(bound.error().Code(), bound.error().Sentence());\n"
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
} // namespace

std::string EmitClientHeader(
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

std::string EmitClientSource(const ProtocolSource& source, const Catalog& catalog)
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
