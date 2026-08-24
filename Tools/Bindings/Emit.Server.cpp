#include "Emit.Server.h"

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
bool IsRegistry(const Interface& interface)
{
	return interface.Name == "wl_registry";
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
			"\n"
			"\t// The implementation behind this object, or null where there is none.\n"
			"\t//\n"
			"\t// **A client names objects in its requests, and this is the only way from the name to the\n"
			"\t// thing.** `wl_surface.set_input_region` hands over a `wl_region`, `wl_surface.attach` a\n"
			"\t// `wl_buffer`, `xdg_wm_base.get_xdg_surface` a `wl_surface` — every one an id the client\n"
			"\t// chose. Reading the user data off one without checking it is not a client crash, it is a\n"
			"\t// type confusion inside the compositor, reachable by a client sending the wrong id on\n"
			"\t// purpose.\n"
			"\t//\n"
			"\t// The check is against this interface *and this dispatch table*, which is stricter than\n"
			"\t// the interface alone: a resource of the right interface that some other party created is\n"
			"\t// refused rather than reinterpreted as one of these.\n"
			"\t[[nodiscard]] Handler* Implementation() const noexcept;\n"
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
			"\tconst {4} wireObject = {4}::Create(\n"
			"\t\t*wireClient,\n"
			"\t\tstatic_cast<std::uint32_t>(wl_resource_get_version(wireResource)),\n"
			"\t\t{3},\n"
			"\t\t*wireImplementation\n"
			"\t);\n"
			"\n"
			"\tif (!wireObject.IsValid())\n"
			"\t{{\n"
			"\t\t// The allocation failed and `Create` has already ended the client. The handler the call\n"
			"\t\t// site just built was never adopted by a resource, so nothing will ever destroy it and\n"
			"\t\t// nothing will ever tell whoever is holding a pointer to it — `OnGone` is both, and its\n"
			"\t\t// contract already covers this: the object is not there, and the handler may delete\n"
			"\t\t// itself. Without it, every request that mints an object leaks one under memory\n"
			"\t\t// pressure, which is the moment it can least afford to.\n"
			"\t\twireImplementation->OnGone();\n"
			"\t}}\n",
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
			"\n"
			"{1}* {0}::Implementation() const noexcept\n"
			"{{\n"
			"\tif (m_Resource == nullptr || wl_resource_instance_of(m_Resource, &{3}, &{4}) == 0)\n"
			"\t{{\n"
			"\t\treturn nullptr;\n"
			"\t}}\n"
			"\n"
			"\treturn static_cast<{1}*>(wl_resource_get_user_data(m_Resource));\n"
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
} // namespace

bool IsLibwaylandsOwn(const Interface& interface)
{
	return IsDisplay(interface) || IsRegistry(interface);
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
		   "// **And an object argument that cannot be reinterpreted.** A client names its own objects in\n"
		   "// its requests — the `wl_region` in `set_input_region`, the `wl_buffer` in `attach` — and\n"
		   "// getting from that name back to the implementation is `Implementation()`, which checks the\n"
		   "// interface and the dispatch table before it reads any user data. Reading it unchecked is not\n"
		   "// a client crash but a type confusion inside the compositor, and it is reachable by a client\n"
		   "// passing the wrong id deliberately.\n"
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
