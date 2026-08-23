#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A Wayland protocol as the XML describes it, and nothing more.
//
// This is the whole interface between the parser and the emitter, so it is deliberately a set of
// plain aggregates with no behaviour: every question the emitter asks — what does this argument
// cost on the wire, does this request destroy its object, which enum does this uint belong to — is
// answered by reading a field, and any of them that turned into a method here would be a policy
// decision made on the wrong side of the seam.
//
// It is also, deliberately, a *transcription* rather than an interpretation. Names arrive as the
// protocol author wrote them: `wl_surface`, `set_window_geometry`, `wl_output.transform`. Turning
// those into C++ spellings is the emitter's job, and doing it here would mean the parser had an
// opinion about the language being generated.
//
// Two shapes in the wire format do not follow from a field being present, and both bite:
//
//   **A `new_id` argument with an empty `Interface`.** `wl_registry.bind` is the only one in the
//   published protocols, and on the wire it is not one id but three values — an interface name
//   string, a version uint, and then the id — because the client is telling the server what it is
//   binding as well as where to put it. Nothing marks it beyond the absence of the attribute, so
//   an emitter that reads `Interface` only when it is non-empty silently generates a signature
//   short by two arguments and a demarshaller that reads the wrong number of words.
//
//   **`Enumeration` may be interface-qualified.** `enum="transform"` names an enum on the
//   interface the argument belongs to; `enum="wl_output.transform"` names one on another
//   interface. The dot is the discriminator and it is carried verbatim, because resolving it here
//   would require the parser to hold every protocol at once — a qualified name routinely points
//   outside the document it appears in, `wl_output.transform` from `xdg-output` being the case in
//   the tree today.
//
// `Interface` on an `Object` argument is optional for a different reason and means something else:
// two arguments in the published protocols take an object of a type the protocol does not name, and
// there the emitter's answer is a generic handle rather than three extra wire words.

enum class ArgumentKind : std::uint8_t
{
	Int,
	Uint,
	Fixed,
	String,
	Object,
	NewId,
	Array,
	Fd,
};

[[nodiscard]] constexpr std::string_view Name(ArgumentKind kind) noexcept
{
	switch (kind)
	{
		case ArgumentKind::Int:
			return "int";
		case ArgumentKind::Uint:
			return "uint";
		case ArgumentKind::Fixed:
			return "fixed";
		case ArgumentKind::String:
			return "string";
		case ArgumentKind::Object:
			return "object";
		case ArgumentKind::NewId:
			return "new_id";
		case ArgumentKind::Array:
			return "array";
		case ArgumentKind::Fd:
			return "fd";
	}

	return "?";
}

struct Argument
{
	std::string Name;
	ArgumentKind Kind = ArgumentKind::Int;

	// The interface this object or new_id refers to. Empty is meaningful on both — see the header
	// comment; it is the `wl_registry.bind` shape on a NewId and an untyped handle on an Object.
	std::string Interface;

	bool AllowNull = false;

	// The enum this int or uint draws its values from, verbatim, dot and all. Empty where the
	// argument is a plain number.
	std::string Enumeration;

	std::string Summary;
	std::string Description;
};

struct Message
{
	std::string Name;

	// The interface version this message first appeared in. 1 where the XML is silent, which is
	// what the absence of the attribute means rather than a value the parser invented.
	int Since = 1;

	// The version that deprecated it, or 0 for a message still in use. Deprecation does not remove
	// a message — the opcode is still spoken by older clients — so this is documentation the
	// emitter can attach rather than a reason to leave anything out.
	int DeprecatedSince = 0;

	// `type="destructor"`. The object is gone once this message is sent, which is a lifetime fact
	// and not a comment: an emitter that misses it leaves the shadow object alive.
	bool Destructor = false;

	std::vector<Argument> Arguments;
	std::string Summary;
	std::string Description;
};

struct EnumerationEntry
{
	std::string Name;

	// Unsigned because a bitfield entry reaches 0x80000000 and because the fourcc entries in
	// linux-dmabuf are DRM format codes, which are four packed characters and exceed INT_MAX.
	std::uint32_t Value = 0;

	int Since = 1;
	int DeprecatedSince = 0;
	std::string Summary;
	std::string Description;
};

struct Enumeration
{
	std::string Name;

	// The values are flags to be ORed rather than an exclusive set. It changes what the emitter may
	// generate — a scoped enum with no operators is wrong for a bitfield and right for the rest.
	bool Bitfield = false;

	int Since = 1;
	std::vector<EnumerationEntry> Entries;
	std::string Summary;
	std::string Description;
};

struct Interface
{
	std::string Name;
	int Version = 1;

	// The upstream protocol is committed to this interface never changing again. Carried because
	// dropping an attribute the parser understood is the same hole as failing to parse it: it is
	// the one fact here that says a version number will not move under a generated binding.
	bool Frozen = false;

	std::vector<Message> Requests;
	std::vector<Message> Events;
	std::vector<Enumeration> Enumerations;
	std::string Summary;
	std::string Description;
};

struct Protocol
{
	std::string Name;

	// The `<copyright>` body, whitespace-normalised. Not decoration: wayland-scanner reproduces it
	// at the top of every generated file, and a generator that dropped it would be stripping the
	// licence notice off the work it is deriving from.
	std::string Copyright;

	std::vector<Interface> Interfaces;
	std::string Summary;
	std::string Description;
};
