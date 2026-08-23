#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>

// The Wayland wire format, as a vocabulary rather than as a protocol.
//
// Everything here is what a message *is* — two header words, an argument sequence, word alignment,
// and the id space split between the two ends. What any particular id or opcode *means* is not here
// and never will be: decision 119 sites this module below both waists precisely so that the codec is
// exercised by a peer the test wrote, and a codec that knew `wl_surface.attach` takes a buffer could
// only be exercised by something that agreed. The one exception is `wl_display`, whose two events the
// runtime answers itself, and that exception is argued in Wire/Connection.h where it is spent.
//
// **Native byte order, and that is the protocol rather than an optimisation.** A Wayland connection
// is an `AF_UNIX` socket between two processes on one machine, so both ends agree on endianness by
// construction and the protocol never says otherwise. The words below are therefore `memcpy`ed
// rather than assembled byte by byte, which is also what makes a 4 KiB read one copy instead of a
// thousand shifts.
//
// **Everything in this module is in `namespace Wire`, and the reason is Core/Signal.h.** That header
// already owns `Connection` in the global namespace as a class *template* — the observer's half of a
// signal — and a non-template class of the same name in the same scope is not a shadowing hazard but
// a hard redeclaration error. The composition root and the nested backend will both include a signal
// and a wire connection in one translation unit, so the collision is certain rather than theoretical.
// A namespace is the cheap answer and it costs the call sites one qualifier; renaming a type with
// dozens of members across the tree is not this module's to spend.

namespace Wire
{
// An object's identity on the wire. A distinct type rather than a `std::uint32_t`, because the same
// word is spelled three ways in one message — an id, an argument that names an existing object, and
// an argument that creates one — and a plain integer would let a length be passed where an object is
// wanted with nothing to say about it.
enum class ObjectId : std::uint32_t
{
	// The null object. Legal as an argument where the protocol marks one nullable, never as a target.
	None = 0,

	// `wl_display`, which a connection has before it has asked for anything. It is not allocated and
	// not bindable; Wire/Connection.h answers its two events itself.
	Display = 1,
};

// The first id the *host* allocates. The id space is split so that neither end has to ask the other
// before naming something new: a client counts up from 1, a server counts up from here, and the two
// runs never meet. It matters to this module for one reason — an id from the top half was never
// handed out by `Allocate`, so it is not recycled by `delete_id` either.
inline constexpr std::uint32_t FirstServerId = 0xff000000U;

[[nodiscard]] constexpr bool IsServerId(ObjectId id) noexcept
{
	return static_cast<std::uint32_t>(id) >= FirstServerId;
}

// Target id, then opcode in the low half of the second word and byte size in the high half.
inline constexpr std::size_t HeaderBytes = 8;

// What the size field can say, rounded down to a word. The 16-bit field is the only cap the wire
// format imposes; libwayland's own 4 KiB buffer is a buffer rather than a protocol limit, and
// clamping to it here would refuse a message a conforming host is entitled to send.
inline constexpr std::size_t MaxMessageBytes = 0xfffcU;

// What one `sendmsg` may carry. `SCM_RIGHTS` is bounded by `SCM_MAX_FD` in the kernel, which is 253
// on Linux; 28 is libwayland's figure and it is the one to match rather than the kernel's, because
// the number that matters is what the *receiver* is willing to accept in one `recvmsg` and every
// Wayland peer in existence sizes that buffer at 28.
inline constexpr std::size_t MaxFdsPerMessage = 28;

// Every argument starts on a word boundary. Strings and arrays are the only ones that do not end on
// one by themselves, and this is what they round up to.
[[nodiscard]] constexpr std::size_t Padded(std::size_t bytes) noexcept
{
	return (bytes + 3) & ~std::size_t{ 3 };
}

// A signed 24.8 fixed-point number, which is how the protocol spells every coordinate and every
// scroll delta.
//
// **A type rather than the `std::int32_t` it is, because the conversion is the whole content.** A
// raw 256 and a logical 256 are both `int32_t` and differ by a factor of 256, and the bug that
// confuses them is a pointer landing 256 pixels away — visible, but only if someone is looking at
// that surface. Naming the two constructions `FromRaw` and `FromInt` is what makes the wire form
// unwritable by accident.
//
// Geometry/Scale.h is not what this is. That is an exact rational because an output scale must
// divide evenly; this is a fixed binary fraction because the protocol says so, and the two would be
// a lossy conversion apart if they were the same type.
class Fixed
{
public:
	Fixed() = default;

	[[nodiscard]] static constexpr Fixed FromRaw(std::int32_t raw) noexcept { return Fixed{ raw }; }

	[[nodiscard]] static constexpr Fixed FromInt(std::int32_t value) noexcept { return Fixed{ value * 256 }; }

	// Rounds to nearest rather than truncating, which is the direction libwayland takes and the one
	// that matters: a pointer position arriving as 3.99609375 and leaving as 3.99609375 is a round
	// trip, and one that leaves as 3.99218750 has drifted by a step nothing recovers.
	[[nodiscard]] static constexpr Fixed FromDouble(double value) noexcept
	{
		const double scaled = value * 256.0;

		return Fixed{ static_cast<std::int32_t>(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5) };
	}

	[[nodiscard]] constexpr std::int32_t Raw() const noexcept { return m_Raw; }

	// Truncates toward zero, matching `wl_fixed_to_int`. Stated rather than left to the reader,
	// because the other plausible answer — floor — differs on exactly the negative coordinates a
	// pointer produces when it leaves a surface to the left.
	[[nodiscard]] constexpr std::int32_t ToInt() const noexcept { return m_Raw / 256; }

	[[nodiscard]] constexpr double ToDouble() const noexcept { return static_cast<double>(m_Raw) / 256.0; }

	friend constexpr bool operator==(Fixed, Fixed) noexcept = default;

private:
	explicit constexpr Fixed(std::int32_t raw) noexcept : m_Raw{ raw } {}

	std::int32_t m_Raw = 0;
};

// What the two header words say, unpacked.
struct MessageHeader
{
	ObjectId Target = ObjectId::None;
	std::uint16_t Opcode = 0;

	// In bytes and including the header, which is what the wire carries. A size below `HeaderBytes`
	// or not a multiple of four is a malformed message rather than a short one, and Wire/Connection.h
	// treats it as a connection error rather than waiting for more bytes that would never help.
	std::uint16_t Size = 0;

	[[nodiscard]] constexpr bool IsWellFormed() const noexcept { return Size >= HeaderBytes && (Size % 4) == 0; }
};

// The word accessors. `memcpy` rather than a reinterpret_cast, because a `std::byte` buffer read
// through a `std::uint32_t*` is the strict-aliasing violation that compiles for years and then
// reorders under -O2, and because the compiler emits the same single load either way.
[[nodiscard]] inline std::uint32_t LoadWord(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
	std::uint32_t word = 0;
	std::memcpy(&word, bytes.data() + offset, sizeof word);

	return word;
}

inline void StoreWord(std::span<std::byte> bytes, std::size_t offset, std::uint32_t word) noexcept
{
	std::memcpy(bytes.data() + offset, &word, sizeof word);
}

[[nodiscard]] constexpr std::uint32_t PackSizeAndOpcode(std::size_t size, std::uint16_t opcode) noexcept
{
	return (static_cast<std::uint32_t>(size) << 16) | opcode;
}

// The header of the message the span begins with. The caller has already checked that at least
// `HeaderBytes` are present; there is no optional here because "too short to have a header" and
// "has a header that is nonsense" are different answers and only the second is a connection error.
[[nodiscard]] inline MessageHeader ReadHeader(std::span<const std::byte> bytes) noexcept
{
	const std::uint32_t second = LoadWord(bytes, 4);

	return MessageHeader{
		.Target = static_cast<ObjectId>(LoadWord(bytes, 0)),
		.Opcode = static_cast<std::uint16_t>(second & 0xffffU),
		.Size = static_cast<std::uint16_t>(second >> 16),
	};
}
} // namespace Wire

// Prints as `object 7`, or `object none`. No format spec is accepted.
//
// The context is a template parameter for the reason recorded at length in Core/Time.h: naming
// std::format_context leaves std::format working and std::formattable false, so the value goes
// missing from a test failure at exactly the moment it was wanted.
template<>
struct std::formatter<Wire::ObjectId>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(Wire::ObjectId id, Context& context) const
	{
		if (id == Wire::ObjectId::None)
		{
			return std::format_to(context.out(), "object none");
		}

		return std::format_to(context.out(), "object {}", static_cast<std::uint32_t>(id));
	}
};

// Prints as the number it denotes rather than the number it stores, which is the whole reason the
// type exists. `{}` alone; a spec would have to be forwarded to a double formatter this does not own.
template<>
struct std::formatter<Wire::Fixed>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(Wire::Fixed value, Context& context) const
	{
		return std::format_to(context.out(), "{}", value.ToDouble());
	}
};

static_assert(Wire::Padded(0) == 0 && Wire::Padded(1) == 4 && Wire::Padded(4) == 4 && Wire::Padded(5) == 8);
static_assert(Wire::Fixed::FromInt(3).Raw() == 768);
static_assert(Wire::Fixed::FromRaw(768).ToInt() == 3);
static_assert(Wire::Fixed::FromRaw(-1).ToInt() == 0, "Truncation is toward zero, not toward negative infinity");
static_assert(Wire::Fixed::FromDouble(1.5).Raw() == 384);
static_assert(Wire::Fixed::FromDouble(-1.5).Raw() == -384);
static_assert(Wire::PackSizeAndOpcode(12, 3) == 0x000c0003U);
static_assert(!Wire::IsServerId(Wire::ObjectId{ 1 }) && Wire::IsServerId(Wire::ObjectId{ 0xff000000U }));
static_assert(std::formattable<Wire::ObjectId, char> && std::formattable<Wire::Fixed, char>);
