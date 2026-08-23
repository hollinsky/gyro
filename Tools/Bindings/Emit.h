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

// A parsed protocol and the file it came from.
//
// **The path decides the generated file's name**, not the protocol's own `name` attribute:
// `xdg-shell.xml` produces `Wayland/XdgShell.h`. The two agree for every published protocol and
// nothing guarantees they will, and the build is what needs the guarantee — a rule that could not
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

	// An event creates an object of no named interface, or a message creates more than one. A
	// dispatcher would have to build a proxy for a type named on the wire, which C++ has no form for,
	// and a request would have to return two objects. No published protocol does either.
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
// the one-argument call is still the whole interface, and a build tool passes it because a build
// failure is read by a person.
[[nodiscard]] Result<std::vector<EmittedFile>>
Emit(std::span<const ProtocolSource> protocols, EmitDiagnostic* diagnostic = nullptr);
