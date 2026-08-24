#pragma once

#include <cstddef>
#include <expected>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/Result.h"
#include "Emit.h"
#include "Protocol.h"

// What both arms of the generator are, and it is a shorter list than the two arms make it look.
//
// A client proxy and a server resource share nothing about what a class is — Tools/Bindings/Emit.h
// says why — so what survives into here is everything that is a property of the *protocol* rather
// than of the side reading it. That is: where an interface is defined, so a reference can be resolved
// and costed as an `#include`; how a wire name is spelled once the rules in Tools/Bindings/Naming.h
// have been applied to it; how a protocol's own prose becomes the comment above the thing it
// describes; and the enumerations, which are the one emitted construct that is genuinely identical on
// both sides because an enumerator is a number the two ends have to agree on.
//
// A helper belongs here when both arms call it, and not merely because it looks general. The
// alternative — a shared file that accumulates whatever the client arm happened to need first — is
// how a reader ends up looking in three files to find out what a request is.

// Where an interface is defined. The protocol index is what decides whether a reference costs an
// `#include`.
struct Located
{
	std::size_t Protocol = 0;
	const Interface* Definition = nullptr;
};

using Catalog = std::map<std::string, Located, std::less<>>;

[[nodiscard]] std::unexpected<Error> Refuse(EmitDiagnostic* diagnostic, EmitFailure failure, std::string message);

// The generated file's base name, which is the *input file's* name PascalCased rather than the
// protocol's own `name` attribute. They agree today for every published protocol and nothing
// guarantees they will: the build has to derive an output path from the path it was handed, and a
// build that had to parse the XML to learn what file the generator would write is a build that
// cannot declare its own outputs.
[[nodiscard]] std::string Unit(std::string_view path);

[[nodiscard]] std::string HeaderName(const ProtocolSource& source);

[[nodiscard]] std::string ProxyName(std::string_view wireInterface);

[[nodiscard]] std::string ListenerName(std::string_view wireInterface);

[[nodiscard]] std::string IgnoringName(std::string_view wireInterface);

// The hoisted spelling of an enumeration. `wl_output` + `transform` is `WlOutputTransform`, which is
// unique because an interface name is unique across the whole protocol ecosystem by convention and
// by the registry that publishes them.
[[nodiscard]] std::string EnumName(std::string_view wireInterface, std::string_view wireEnumeration);

// An `enum=` split at the dot, per Tools/Bindings/Protocol.h. Unqualified means the interface the
// argument belongs to.
[[nodiscard]] std::pair<std::string, std::string> SplitEnumeration(const Argument& argument, const Interface& owner);

// The `new_id` a request creates, or null where it creates nothing. Protocol.h's untyped case is a
// new_id with no interface, and it stays a new_id here — what changes is that the request becomes a
// template, which the caller of this has to ask about separately.
[[nodiscard]] const Argument* CreatedArgument(const Message& message);

// Wire/Connection.h answers `wl_display` itself, so it gets no listener however many events it has.
[[nodiscard]] bool IsDisplay(const Interface& interface);

[[nodiscard]] bool WantsListener(const Interface& interface);

// Summary before description, since the protocols put a one-liner in the attribute and the prose in
// the element and a reader wants them in that order.
void Documentation(std::string& out, std::string_view indent, std::string_view summary, std::string_view description);

void EmitEnumeration(std::string& out, const Interface& interface, const Enumeration& enumeration);

[[nodiscard]] std::string Joined(const std::vector<std::string>& parameters);

// A parameter list wrapped the way the project's own .clang-format would wrap it, because the
// generated header is the documentation for these interfaces and `zwp_linux_buffer_params_v1.add`
// takes six arguments. `lead` is what precedes the open paren, which is what decides whether the flat
// form fits.
[[nodiscard]] std::string
Wrapped(const std::vector<std::string>& parameters, std::string_view indent, std::size_t lead);

// Every other protocol in the set this one names, so the header includes exactly what it needs. A
// reference is either to something in the set or it was already refused by the catalog, so this loop
// never has to answer for a name it cannot find.
[[nodiscard]] std::set<std::size_t> References(const Protocol& protocol, std::size_t self, const Catalog& catalog);

void EmitPreamble(std::string& out, const ProtocolSource& source);
