#pragma once

#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.h"
#include "Protocol.h"

// The back end: parsed protocols in, C++ text out, and a sentence naming the spot when neither.
//
// **Every protocol is emitted in one call, and that is a requirement rather than a convenience.** An
// `interface=` or an `enum=` routinely points outside the document it appears in —
// `zwp_linux_buffer_params_v1.created` hands back a `wl_buffer`, and Tools/Bindings/Protocol.h
// records `wl_output.transform` reaching in from `xdg-output`. A generator run per file would have to
// either guess the header the other name lives in or emit an untyped handle where the protocol was
// specific, and both are the same failure: a binding that compiles and says less than the XML did.
// Handed the whole set, a reference either resolves to an interface somebody asked for or it is a
// build failure naming the protocol that would have to be added, which is the only two answers worth
// having.
//
// **The emitted text is checked for name collisions rather than made unique.** A request named
// `version` would land on the accessor every proxy declares, and the repair — renaming one of them —
// puts a name in the header that no reader can find in the protocol. Refusing instead costs whoever
// adds that protocol a conversation, and Tools/Bindings/Naming.h is where the reserved set lives.

// Which side of the wire the emission is for, and it is a whole second back end rather than a flag
// on a few branches.
//
// The two arms share the parser, the catalog, and every rule about names; they share nothing about
// what a class is. A **client** proxy sends requests and is handed a listener whose handlers answer
// events. A **server** resource sends events and is handed a handler whose methods answer requests —
// and it stands on `libwayland-server` rather than on `Wire`, because decision 2 leaves the server
// codec there.
//
// What the server arm exists for is the two things that decision names as its own safety claim, and
// both are properties of the generated text rather than of the code that uses it: **a resource
// cannot exist without an implementation**, because the typed constructor is the only way to make one
// and it takes the handler; and **the dispatch table cannot have a hole**, because it is one
// aggregate initialised from the same walk over the XML that declared the pure virtuals. Those are
// the two `wl_abort()` sites in `wl_closure_invoke` a client can reach, and reaching one takes the
// whole machine down at a moment the client picks — which on a compositor with no VTs is a black
// screen with no way back in.
enum class Direction : std::uint8_t
{
	// Proxies, listeners, and gyro's own codec underneath. What the nested backend speaks to its host.
	Client,

	// Resources, handlers, and `libwayland-server` underneath. What gyro speaks to its clients.
	Server,
};

// A parsed protocol and the file it came from.
//
// **The path decides the generated file's name**, not the protocol's own `name` attribute:
// `xdg-shell.xml` produces `Wayland/XdgShell.h`, and `Wayland/Server/XdgShell.h` on the other
// direction. The two agree for every published protocol and nothing guarantees they will, and the
// build is what needs the guarantee — a rule that could not
// name its own outputs without parsing the XML first is a rule CMake cannot express. Its last
// component also goes into the generated header's first line, so a reader knows what a signature was
// derived from without leaving the file.
struct ProtocolSource
{
	Protocol Model;
	std::string Path;
};

// One file to write. The path is relative to the output root and carries its directory, so the
// generated tree is `Wayland/Wayland.h` and an include reads the same way.
struct EmittedFile
{
	std::string Path;
	std::string Text;
};

// What the emitter refused, for a test to assert on rather than the prose beside it.
enum class EmitFailure : std::uint8_t
{
	// An `interface=` or the interface half of an `enum=` names something no protocol in the set
	// defines. The fix is a protocol in the CMake list, and the message names which.
	UnknownInterface,

	// An `enum=` resolves to an interface that has no enumeration by that name.
	UnknownEnumeration,

	// A generated name would be declared twice: a request landing on a member every proxy has, two
	// interfaces of the same name across two protocols, or a request and an enumeration that
	// PascalCase to the same word.
	NameCollision,

	// A message creates an object of no named interface *in the direction being dispatched*, or
	// creates more than one in either. The dispatcher would have to build a binding for a type named
	// on the wire, which C++ has no form for, and a two-object message would have to return twice.
	//
	// Which half this catches flips with the direction, because a `new_id` a caller supplies is a
	// template parameter and one that arrives is not: on the client arm it is an *event*, and on the
	// server arm a *request*. The only untyped one in the published protocols is `wl_registry.bind`,
	// which the server arm never sees — libwayland implements the registry, so decision 2 already put
	// that interface on the other side of the seam.
	UntypedEventArgument,

	// Two protocols reference each other's interfaces, so neither header can include the other. No
	// published pair does this today; it is refused rather than emitted because the compiler error it
	// would otherwise produce is in generated code and names neither protocol.
	CyclicReference,
};

[[nodiscard]] constexpr std::string_view Name(EmitFailure failure) noexcept
{
	switch (failure)
	{
		case EmitFailure::UnknownInterface:
			return "an interface no protocol in the set defines";
		case EmitFailure::UnknownEnumeration:
			return "an enumeration the named interface does not declare";
		case EmitFailure::UntypedEventArgument:
			return "a message whose created object a binding cannot express";
		case EmitFailure::NameCollision:
			return "two protocol names that spell one C++ name";
		case EmitFailure::CyclicReference:
			return "two protocols that reference each other";
	}

	return "?";
}

// What went wrong and where, in the protocol's own vocabulary. There is no line number: the emitter
// works from a model rather than from text, and a message that names `xdg_toplevel.set_parent` sends
// the reader to a better place than a byte offset into a file they did not write.
struct EmitDiagnostic
{
	EmitFailure Failure = EmitFailure::UnknownInterface;
	std::string Message;
};

template<>
struct std::formatter<EmitDiagnostic>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const EmitDiagnostic& diagnostic, Context& context) const
	{
		return std::format_to(context.out(), "{}", diagnostic.Message);
	}
};

// Every file the set produces: a header and a source per protocol, in the order the protocols were
// given. `diagnostic` is filled only on failure, and is optional for Tools/Bindings/Xml.h's reason —
// the two-argument call is still the whole interface, and a build tool passes it because a build
// failure is read by a person.
//
// The direction has no default. Both arms are generated from the same XML by the same build, so a
// call that did not say which one it wanted would be a bug the compiler could have caught.
[[nodiscard]] Result<std::vector<EmittedFile>>
Emit(std::span<const ProtocolSource> protocols, Direction direction, EmitDiagnostic* diagnostic = nullptr);
