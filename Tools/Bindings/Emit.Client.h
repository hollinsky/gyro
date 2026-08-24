#pragma once

#include <cstddef>
#include <span>
#include <string>

#include "Emit.Shared.h"

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

// The header for one protocol: the four sections above, in that order.
[[nodiscard]] std::string EmitClientHeader(
	const ProtocolSource& source,
	std::size_t self,
	const Catalog& catalog,
	std::span<const ProtocolSource> protocols
);

// Its translation unit: the request bodies, `Listen`, and the dispatch table each listener is
// invoked through.
[[nodiscard]] std::string EmitClientSource(const ProtocolSource& source, const Catalog& catalog);
