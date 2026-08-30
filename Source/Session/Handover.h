#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

#include "Core/Session.h"

// The listener handover, as a vocabulary rather than as a mechanism.
//
// Docs/Architecture.md#listener-handover inverts socket creation: a small agent running as the user
// creates the Wayland listener in that user's own runtime directory and passes the descriptor to
// gyro, which cannot `chown` one into place without a capability it refuses to hold. What crosses
// between the two is here, and nothing else is — no socket, no `SO_PEERCRED`, no validation of the
// offered descriptor. Those are gyro's side and the agent's side respectively, and both of them
// include this.
//
// **It is a separate header because the handshake is an ABI.** The same section obliges it to survive
// gyro restarting — every listener is lost, so agents re-offer — and to survive version skew across
// an upgrade, since the two binaries are updated by a package manager and started by different
// parties. A wire format that lives inside whichever `.cpp` happened to need it first is one nobody
// can diff across a release. This file is the thing to diff.
//
// **`SOCK_SEQPACKET`, and the descriptor is why.** A `SCM_RIGHTS` control message attaches to
// whatever byte the kernel chose to deliver it with, so on a stream socket a short read hands the
// receiver a descriptor belonging to a message it has not finished parsing — Wire/Wire.h carries that
// hazard for the Wayland connection, where the protocol leaves no choice. Here there is a choice: a
// sequenced-packet socket makes one datagram one message, so every function below takes a whole
// message and the framing question never arises. What is left of a header is an opcode and a length,
// and the length is checked against the datagram rather than used to find its end.
//
// **Rejected: greetd's length-prefixed JSON.** That is the *login* agent's ABI, where a third-party
// greeter has to speak it and gyro gains every existing greeter for free
// (Docs/Architecture.md#the-login-agent). Nothing here has a third party: both ends ship in one
// package, and JSON would buy a parser and an allocation on the one process whose death takes every
// session on the machine.
//
// **Native byte order and fixed-width fields, for Wire/Message.h's reason.** Both ends are processes
// on one machine, so they agree on endianness by construction. Every field below is explicitly sized
// and every message is a whole number of four-byte words with no implicit padding, so the layout does
// not depend on the compiler that built either end.

namespace Session
{
// Where an agent offers its listener when nothing says otherwise.
//
// **It is in the ABI because it is the rendezvous rather than a default either end chose.** The two
// binaries are started by different parties — gyro by the service manager, the agent by the login
// agent or by a person — with no chance to agree on a path between them, so a default that lived in
// one end's option parser would be a default the other end could drift away from silently. Absolute,
// and under `/run` rather than a user's runtime directory, because one gyro serves every user on the
// machine. Docs/Architecture.md#the-login-agent has it in the boot diagram.
inline constexpr std::string_view DefaultControlPath = "/run/gyro/control";

// What this build speaks, and the oldest it will speak to.
//
// The agent sends its own in `Hello` and gyro answers `Welcome` with the version in force, which is
// the lower of the two. Two numbers rather than one because they will diverge — an agent older than
// gyro is the ordinary state of a machine mid-upgrade, and a gyro that refused it would take the
// user's session away over a field it does not need.
inline constexpr std::uint32_t HandoverVersion = 1;
inline constexpr std::uint32_t MinimumHandoverVersion = 1;

// What a message is. Numbered explicitly and never reused, because a value that meant one thing in a
// shipped release cannot mean another in the next — which is the whole content of calling this an ABI.
enum class Opcode : std::uint16_t
{
	// Agent to gyro, first on the connection and once. Nothing else is accepted before it.
	Hello = 1,

	// Agent to gyro, carrying the listening descriptor out of band.
	Offer = 2,

	// gyro to agent, in answer to `Hello`.
	Welcome = 3,

	// gyro to agent, in answer to `Offer`. The session exists from here.
	Accepted = 4,

	// gyro to agent, in answer to anything gyro will not do. The connection closes after it.
	Refused = 5,
};

[[nodiscard]] constexpr bool IsOpcode(std::uint16_t value) noexcept
{
	return value >= static_cast<std::uint16_t>(Opcode::Hello) && value <= static_cast<std::uint16_t>(Opcode::Refused);
}

// Which end sends it. Checked on arrival at both ends, so that a message travelling the wrong way is
// refused by the frame rather than by whatever the handler happened to do with it — gyro receiving a
// `Welcome` is a peer that is confused or lying, and neither is a thing to act on.
[[nodiscard]] constexpr bool FromAgent(Opcode op) noexcept
{
	return op == Opcode::Hello || op == Opcode::Offer;
}

// How many descriptors ride with it. **Part of the message rather than a rule in the receiver**,
// because the count is the one thing a peer controls that costs the receiver a resource: a datagram
// carrying descriptors nobody asked for is a file table filling up on the process that must not die,
// and the check that closes it belongs where every reader can see the number.
[[nodiscard]] constexpr std::size_t DescriptorsFor(Opcode op) noexcept
{
	return op == Opcode::Offer ? 1U : 0U;
}

// An opcode and the bytes after it.
inline constexpr std::size_t HeaderBytes = 4;

// What either end will receive into, and therefore the largest message that can ever be sent.
//
// **It is in the ABI because a sequenced-packet socket truncates rather than waits.** A datagram
// longer than the receiver's buffer is delivered short with `MSG_TRUNC` set, so the two ends agreeing
// on this number is what makes an oversized message a refusal rather than a message quietly missing
// its tail. It is also the bound on what one peer can cost the other, which is why it is a small
// fixed number and not a growth policy: nothing here is variable-length yet, and the room is for the
// `Spawn` that Docs/Architecture.md#the-login-agent already owes Xwayland.
inline constexpr std::size_t MaxMessageBytes = 4096;

// The word helpers, at namespace scope rather than behind a `Detail`, which is Wire/Message.h's
// arrangement and is forced by the same thing: a test says `using namespace Session`, and
// Testing/Test.h's macros expand to `Detail::` — so a nested `Detail` here makes every check in every
// test of this module ambiguous.
[[nodiscard]] inline std::uint16_t LoadHalf(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
	std::uint16_t half = 0;
	std::memcpy(&half, bytes.data() + offset, sizeof half);

	return half;
}

inline void StoreHalf(std::span<std::byte> bytes, std::size_t offset, std::uint16_t half) noexcept
{
	std::memcpy(bytes.data() + offset, &half, sizeof half);
}

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

// Write the header and answer the whole message's size, or zero where the buffer will not hold it.
// The one place either of those numbers is computed, so a message type below states its payload and
// nothing else.
[[nodiscard]] inline std::size_t WriteHeader(std::span<std::byte> into, Opcode op, std::size_t payload) noexcept
{
	const std::size_t total = HeaderBytes + payload;

	if (into.size() < total)
	{
		return 0;
	}

	StoreHalf(into, 0, static_cast<std::uint16_t>(op));
	StoreHalf(into, 2, static_cast<std::uint16_t>(payload));

	return total;
}

// Whether the datagram is exactly the message this type expects. **Exactly, rather than at least**:
// a sequenced-packet socket delivers what the sender wrote, so a trailing byte is a peer speaking a
// dialect this build does not have, and reading the prefix of it would be guessing at what changed.
[[nodiscard]] inline bool IsMessage(std::span<const std::byte> message, Opcode op, std::size_t payload) noexcept
{
	return message.size() == HeaderBytes + payload && LoadHalf(message, 0) == static_cast<std::uint16_t>(op) &&
	       LoadHalf(message, 2) == payload;
}

// What a datagram claims to be, before anything has agreed that it is.
struct MessageHeader
{
	Opcode Op{};

	// Bytes after the header, as the sender wrote them. Not to be trusted as a length — the datagram's
	// own size is the truth, and a message type's `Decode` checks the two against each other.
	std::uint16_t PayloadBytes = 0;
};

// The header of a datagram, or nothing where there is not one.
//
// **Unlike Wire/Message.h's, this returns an optional, and the difference is the socket.** There a
// short read is an ordinary state of a stream and the caller has already waited for eight bytes; here
// a datagram is delivered whole, so one too small to hold a header is not incomplete — it is a peer
// sending something this build cannot name, which is the same answer as an opcode out of range.
[[nodiscard]] inline std::optional<MessageHeader> ReadHeader(std::span<const std::byte> message) noexcept
{
	if (message.size() < HeaderBytes || message.size() > MaxMessageBytes)
	{
		return std::nullopt;
	}

	const std::uint16_t op = LoadHalf(message, 0);

	if (!IsOpcode(op))
	{
		return std::nullopt;
	}

	return MessageHeader{ .Op = static_cast<Opcode>(op), .PayloadBytes = LoadHalf(message, 2) };
}

// The sentence a refusal carries, as bytes on the wire.
//
// **It is for a log line and never for a branch** — the errno beside it is what an agent decides on —
// which is what makes a fixed buffer with truncation the right shape rather than a length-prefixed
// string. Core/Result.h's `Subject` is the same idea one layer up and is deliberately not reused: its
// layout is a Core type's business and would become an ABI here the first time somebody made it hold
// one more character.
class Reason
{
public:
	static constexpr std::size_t Capacity = 64;

	constexpr Reason() noexcept = default;

	// Truncates, and replaces anything that is not printable ASCII. **The sanitising is on the way in
	// as well as the way out**, because the two ends of this are separate binaries and only one of
	// them is gyro: an agent connecting to a control socket it did not create is reading a sentence
	// from whoever created it, and a log file is a thing terminal escapes are read out of.
	explicit Reason(std::string_view text) noexcept
	{
		const std::size_t taken = text.size() < Capacity ? text.size() : Capacity;

		for (std::size_t index = 0; index < taken; ++index)
		{
			const char letter = text[index];

			m_Text[index] = letter >= ' ' && letter <= '~' ? letter : '?';
		}
	}

	// What was said, up to the first NUL. Never longer than `Capacity`, and printable throughout.
	[[nodiscard]] constexpr std::string_view Text() const noexcept
	{
		std::size_t length = 0;

		while (length < Capacity && m_Text[length] != '\0')
		{
			++length;
		}

		return std::string_view{ m_Text.data(), length };
	}

	// The bytes, NUL-padded to `Capacity`. `Encode` writes them out whole; nothing else should need
	// them, because everything else wants `Text`.
	[[nodiscard]] constexpr std::span<const char, Capacity> Bytes() const noexcept
	{
		return std::span<const char, Capacity>{ m_Text };
	}

	friend constexpr bool operator==(const Reason&, const Reason&) noexcept = default;

private:
	std::array<char, Capacity> m_Text{};
};

// Agent to gyro: *I speak this version.* First on the connection and once.
struct Hello
{
	static constexpr Opcode Op = Opcode::Hello;
	static constexpr std::size_t PayloadBytes = 4;

	std::uint32_t Version = HandoverVersion;

	// Bytes written, or zero where the buffer will not hold the message.
	[[nodiscard]] std::size_t Encode(std::span<std::byte> into) const noexcept
	{
		const std::size_t total = WriteHeader(into, Op, PayloadBytes);

		if (total != 0)
		{
			StoreWord(into, HeaderBytes, Version);
		}

		return total;
	}

	[[nodiscard]] static std::optional<Hello> Decode(std::span<const std::byte> message) noexcept
	{
		if (!IsMessage(message, Op, PayloadBytes))
		{
			return std::nullopt;
		}

		return Hello{ .Version = LoadWord(message, HeaderBytes) };
	}
};

// gyro to agent: *I have you, and this is the version we are speaking.*
//
// **It answers `Hello` and carries no session, which is the ordering rather than an omission.** A
// session is created by an accepted offer (Docs/Architecture.md#listener-handover), so there is none
// yet — and the version has to be settled *before* the offer rather than with it, because the version
// is what says how the next message is encoded.
struct Welcome
{
	static constexpr Opcode Op = Opcode::Welcome;
	static constexpr std::size_t PayloadBytes = 4;

	// The version in force, which is the lower of the two ends'. The agent does not compute it: one
	// party deciding is what keeps the two from disagreeing about what they agreed.
	std::uint32_t Version = HandoverVersion;

	[[nodiscard]] std::size_t Encode(std::span<std::byte> into) const noexcept
	{
		const std::size_t total = WriteHeader(into, Op, PayloadBytes);

		if (total != 0)
		{
			StoreWord(into, HeaderBytes, Version);
		}

		return total;
	}

	[[nodiscard]] static std::optional<Welcome> Decode(std::span<const std::byte> message) noexcept
	{
		if (!IsMessage(message, Op, PayloadBytes))
		{
			return std::nullopt;
		}

		return Welcome{ .Version = LoadWord(message, HeaderBytes) };
	}
};

// gyro to agent: *the listener is mine, and you are this session.*
//
// **Every message the agent sends is answered, and this is the one that matters.** The agent must not
// start the session's first client until gyro holds the listener — not because a client would fail, it
// would sit in the backlog and be served on adoption, but because *refused* and *not answered yet* are
// otherwise the same silence, and the agent would be waiting on a timeout to tell them apart.
struct Accepted
{
	static constexpr Opcode Op = Opcode::Accepted;
	static constexpr std::size_t PayloadBytes = 4;

	SessionId Id = SessionId::None;

	[[nodiscard]] std::size_t Encode(std::span<std::byte> into) const noexcept
	{
		const std::size_t total = WriteHeader(into, Op, PayloadBytes);

		if (total != 0)
		{
			StoreWord(into, HeaderBytes, static_cast<std::uint32_t>(Id));
		}

		return total;
	}

	[[nodiscard]] static std::optional<Accepted> Decode(std::span<const std::byte> message) noexcept
	{
		if (!IsMessage(message, Op, PayloadBytes))
		{
			return std::nullopt;
		}

		return Accepted{ .Id = static_cast<SessionId>(LoadWord(message, HeaderBytes)) };
	}
};

// Agent to gyro: *here is the listener.* The descriptor rides out of band and the message says
// nothing, which is the shape rather than an omission — everything gyro needs to judge the offer it
// reads off the descriptor itself, and a path or a uid stated here would be a claim it would have to
// ignore in favour of what it can verify.
struct Offer
{
	static constexpr Opcode Op = Opcode::Offer;
	static constexpr std::size_t PayloadBytes = 0;

	[[nodiscard]] std::size_t Encode(std::span<std::byte> into) const noexcept
	{
		return WriteHeader(into, Op, PayloadBytes);
	}

	[[nodiscard]] static std::optional<Offer> Decode(std::span<const std::byte> message) noexcept
	{
		if (!IsMessage(message, Op, PayloadBytes))
		{
			return std::nullopt;
		}

		return Offer{};
	}
};

// gyro to agent: *no, and this is why.* The last message on the connection.
struct Refused
{
	static constexpr Opcode Op = Opcode::Refused;
	static constexpr std::size_t PayloadBytes = 4 + Reason::Capacity;

	// An errno, because Core/Result.h's domain is errno and a refusal here is nearly always a syscall
	// having said so: `EPROTO` for a version or a message out of order, `EACCES` for a peer that is not
	// who the socket belongs to, `EBADF` for an offer that is not a listening socket, `EBUSY` for a
	// session that already has one.
	std::uint32_t Code = 0;

	Reason Text;

	[[nodiscard]] std::size_t Encode(std::span<std::byte> into) const noexcept
	{
		const std::size_t total = WriteHeader(into, Op, PayloadBytes);

		if (total != 0)
		{
			StoreWord(into, HeaderBytes, Code);
			std::memcpy(into.data() + HeaderBytes + 4, Text.Bytes().data(), Reason::Capacity);
		}

		return total;
	}

	[[nodiscard]] static std::optional<Refused> Decode(std::span<const std::byte> message) noexcept
	{
		if (!IsMessage(message, Op, PayloadBytes))
		{
			return std::nullopt;
		}

		// Up to the first NUL, and the truncation is why the whole field is not simply handed to
		// `Reason`: the padding is NUL and the constructor turns anything unprintable into `?`, so a
		// short sentence would come back with its padding spelled out.
		std::array<char, Reason::Capacity> raw{};
		std::memcpy(raw.data(), message.data() + HeaderBytes + 4, Reason::Capacity);

		const std::string_view padded{ raw.data(), Reason::Capacity };
		const std::size_t length = padded.find('\0');

		return Refused{
			.Code = LoadWord(message, HeaderBytes),
			.Text = Reason{ length == std::string_view::npos ? padded : padded.substr(0, length) },
		};
	}
};

static_assert(MinimumHandoverVersion <= HandoverVersion);
static_assert(HeaderBytes + Refused::PayloadBytes <= MaxMessageBytes);
} // namespace Session
