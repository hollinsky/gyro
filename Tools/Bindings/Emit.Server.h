#pragma once

#include <cstddef>
#include <span>
#include <string>

#include "Emit.Shared.h"

// **What libwayland implements itself, and therefore what this arm must not.** Both are structural
// rather than a preference: `wl_display` is the connection, and `wl_registry.bind` is the one message
// in the published protocols whose `new_id` names no interface — three wire values rather than one,
// with the type chosen by the client at runtime. On the client arm that is a template a caller
// instantiates; arriving as a *request* there is nothing to instantiate, because the interface is a
// string the dispatcher reads. libwayland already owns both, so decision 2 has put them on the far
// side of the seam and this file leaves them there rather than inventing a form C++ does not have.

// Whether libwayland implements this interface itself, and so whether this arm must skip it.
[[nodiscard]] bool IsLibwaylandsOwn(const Interface& interface);

// The generated file's name on this arm, which is the client arm's under a `Server/` directory.
[[nodiscard]] std::string ServerHeaderName(const ProtocolSource& source);

// The resource, its implementation interface, and the binding a compositor hands to a global.
[[nodiscard]] std::string ResourceName(std::string_view wireInterface);
[[nodiscard]] std::string HandlerName(std::string_view wireInterface);
[[nodiscard]] std::string BindingName(std::string_view wireInterface);

// The fault sink, which is one file for the whole set rather than one per protocol.
[[nodiscard]] EmittedFile EmitFaultHeader();
[[nodiscard]] EmittedFile EmitFaultSource();

// The header for one protocol: resources, handlers, and the enumerations both sides share.
[[nodiscard]] std::string EmitServerHeader(
	const ProtocolSource& source,
	std::size_t self,
	const Catalog& catalog,
	std::span<const ProtocolSource> protocols
);

// Its translation unit: the event bodies, the wire interface tables, and the dispatch table each
// handler is invoked through.
[[nodiscard]] std::string EmitServerSource(const ProtocolSource& source, const Catalog& catalog);
