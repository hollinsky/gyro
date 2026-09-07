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
//
// **Nothing here has shipped, and the rules below are about the day it does.** Exact lengths,
// never-reused opcodes, a message that cannot grow a field: those are what makes an ABI diffable
// across a release, and every one of them is a self-inflicted constraint before the first one — both
// binaries are in this tree and are built together. So until a package exists the shape is chosen for
// being right rather than for being additive to what is already written, and after it the rules bite.
// The one exception is the greeting: `Hello` and `Welcome` negotiate a version nothing yet branches
// on, and it is kept because a version is the single field that cannot be added later, adding it
// being the thing it would have to negotiate.

namespace Session
{
// What a listener in an offer is for.
//
// **A role rather than a trust level, and the split is which end knows what.** The agent knows *this
// is the socket my shell will reach gyro on*; gyro knows what a shell may see, which is
// Protocol/Tier.h's table and is deliberately the only copy of that policy in the tree. Putting
// `Trust` on the wire would make a second one, and an ABI would then have to change whenever a tier
// did.
//
// **Bit values, because an offer carries a set of these and not one of them.** A session is handed
// over whole — every listener it has, in one message — so what crosses is a bitmap, and the
// descriptors ride in ascending bit order beside it.
enum class ListenerRole : std::uint32_t
{
	// Where this session's applications connect: the `wayland-N` in `WAYLAND_DISPLAY`. Every offer has
	// one, because a session without it is a session no client can reach.
	Applications = 1U << 0,

	// Where this session's shell connects, and clients arriving there are granted `Trust::System`
	// (Protocol/Tier.h). Optional: an agent that starts no shell offers no such socket, and a session
	// with none is one where nothing can claim a chord or declare a surface to be chrome.
	Shell = 1U << 1,
};

// Every bit this build has a meaning for. A peer setting one outside it is speaking a dialect this
// build does not, which is a refusal rather than something to mask off — the bits *are* the
// descriptor count, so ignoring one would leave a descriptor nobody has a name for.
inline constexpr std::uint32_t KnownRoles =
	static_cast<std::uint32_t>(ListenerRole::Applications) | static_cast<std::uint32_t>(ListenerRole::Shell);

// The most listeners one offer can carry, which is every role there is.
inline constexpr std::size_t MaxListeners = 2;

[[nodiscard]] constexpr bool Offers(std::uint32_t roles, ListenerRole role) noexcept
{
	return (roles & static_cast<std::uint32_t>(role)) != 0;
}

// How many descriptors a set of roles says are attached.
[[nodiscard]] constexpr std::size_t ListenersIn(std::uint32_t roles) noexcept
{
	std::size_t count = 0;

	for (std::uint32_t bit = 1; bit != 0; bit <<= 1U)
	{
		count += (roles & bit) != 0 ? 1U : 0U;
	}

	return count;
}

// Which descriptor is this role's, given the whole set. **Ascending bit order, so the answer is how
// many roles sit below it** — which is what lets a role be added later without an ordering rule and
// without renumbering what an older agent sends.
[[nodiscard]] constexpr std::size_t IndexOf(std::uint32_t roles, ListenerRole role) noexcept
{
	return ListenersIn(roles & (static_cast<std::uint32_t>(role) - 1U));
}

// Whether a set of roles is one this build can act on: no bit it has no name for, and the
// applications listener present. **Checked before a descriptor is counted**, because the bitmap is
// what says how many arrived.
[[nodiscard]] constexpr bool RolesAreWellFormed(std::uint32_t roles) noexcept
{
	return (roles & ~KnownRoles) == 0 && Offers(roles, ListenerRole::Applications);
}
} // namespace Session

namespace Session
{
// Where an agent offers its session when nothing says otherwise.
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

	// Agent to gyro, carrying the session's listeners out of band and their roles in the payload.
	Offer = 2,

	// gyro to agent, in answer to `Hello`.
	Welcome = 3,

	// gyro to agent, in answer to `Offer`. The session exists from here.
	Accepted = 4,

	// gyro to peer, in answer to anything gyro will not do. The connection closes after it.
	Refused = 5,

	// Peer to gyro: *I am the machine.* Claims the machine role for this connection, which is what
	// makes the four below reachable on it.
	Manage = 6,

	// gyro to peer, in answer to `Manage`. The connection is the machine peer from here.
	Managing = 7,

	// Machine peer to gyro: *show this user's session here.*
	Assign = 8,

	// gyro to machine peer: *it is on the screen.* Sent when the request is satisfied rather than when
	// it arrives, which may be much later or never.
	Assigned = 9,
};

[[nodiscard]] constexpr bool IsOpcode(std::uint16_t value) noexcept
{
	return value >= static_cast<std::uint16_t>(Opcode::Hello) && value <= static_cast<std::uint16_t>(Opcode::Assigned);
}

// Which end sends it. Checked on arrival at both ends, so that a message travelling the wrong way is
// refused by the frame rather than by whatever the handler happened to do with it — gyro receiving a
// `Welcome` is a peer that is confused or lying, and neither is a thing to act on.
//
// **`FromPeer` rather than `FromAgent`, because there are two kinds of peer now.** A session agent
// offers a session; the machine peer assigns one to a screen. What this question is actually about is
// the direction a message travels, and that never depended on which kind was at the far end.
[[nodiscard]] constexpr bool FromPeer(Opcode op) noexcept
{
	return op == Opcode::Hello || op == Opcode::Offer || op == Opcode::Manage || op == Opcode::Assign;
}

// Whether descriptors ride with it, which is `Offer` and nothing else.
//
// **How many is the message's own business rather than the opcode's**, because a session is offered
// whole and the number of listeners it has is in the payload. That leaves the receiver two checks
// instead of one and both are load-bearing: nothing but an `Offer` may carry a descriptor at all —
// which is what stops a peer filling the file table of the process that must not die — and an
// `Offer`'s bitmap has to account for exactly the descriptors that arrived, which is what stops a
// socket being adopted at a role nobody named. The bound on what one datagram can cost is
// Session/Transport.h's `MaxAttached`, where it has to be: a control buffer is sized before `recvmsg`
// can tell anybody what the payload says.
[[nodiscard]] constexpr bool CarriesListeners(Opcode op) noexcept
{
	return op == Opcode::Offer;
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

// Which outputs a machine peer means, as the kernel's name for a connector — `eDP-1`, `DP-7`,
// `HDMI-A-1` — or empty for every output there is.
//
// **A connector name rather than an output identity, because an identity gyro minted could not be
// spoken.** `Core/Handle.h`'s `OutputId` is generational and is deliberately never named outside gyro
// (44): it changes when a monitor is unplugged and the next one takes the slot, so a peer holding one
// would be addressing a display that is physically no longer there. A connector name is the kernel's,
// it is what `--output=eDP-1:1920x1080` already spells, and it survives gyro restarting — which the
// machine peer must, since it reconnects and re-states what it wanted.
//
// **Empty means every output, and it is the ordinary case rather than a wildcard.** A machine with one
// panel is the machine gyro boots on, and a login agent that had to name its connector could not place
// a greeter without first discovering one — which is a mechanism that does not exist
// (Docs/Open.md, *the machine peer cannot enumerate outputs*). So the request that needs no knowledge
// is the one that costs nothing to write.
//
// **Constructed through a factory that refuses, rather than truncating like `Reason`.** A truncated
// sentence is still the sentence; a truncated connector name is a *different screen*, or more usually
// none, and the difference between refusing to send and sending something that will not match is
// which end reports it. Nothing on Linux names a connector anywhere near this long, so the refusal is
// a bug in the caller rather than a limit anybody meets.
class ConnectorName
{
public:
	static constexpr std::size_t Capacity = 32;

	constexpr ConnectorName() noexcept = default;

	// The name, or nothing where it will not fit or holds a byte a connector name never does. Empty is
	// accepted and is *every output*.
	[[nodiscard]] static constexpr std::optional<ConnectorName> From(std::string_view text) noexcept
	{
		if (text.size() > Capacity)
		{
			return std::nullopt;
		}

		ConnectorName name;

		for (std::size_t index = 0; index < text.size(); ++index)
		{
			const char letter = text[index];

			// Printable and not a space, which is every connector name the kernel produces and is
			// checked because this arrives from another process: a log line is a thing terminal escapes
			// are read out of, and Reason above sanitises for the same reason one field over.
			if (letter <= ' ' || letter > '~')
			{
				return std::nullopt;
			}

			name.m_Text[index] = letter;
		}

		return name;
	}

	// The name, up to the first NUL. Empty is every output.
	[[nodiscard]] constexpr std::string_view Text() const noexcept
	{
		std::size_t length = 0;

		while (length < Capacity && m_Text[length] != '\0')
		{
			++length;
		}

		return std::string_view{ m_Text.data(), length };
	}

	// Whether this names every output rather than one of them.
	[[nodiscard]] constexpr bool IsEveryOutput() const noexcept { return m_Text[0] == '\0'; }

	// The bytes, NUL-padded to `Capacity`. `Encode` writes them whole; everything else wants `Text`.
	[[nodiscard]] constexpr std::span<const char, Capacity> Bytes() const noexcept
	{
		return std::span<const char, Capacity>{ m_Text };
	}

	friend constexpr bool operator==(const ConnectorName&, const ConnectorName&) noexcept = default;

private:
	std::array<char, Capacity> m_Text{};
};

// A connector name as it arrived, or nothing where the bytes are not one.
//
// **Decoding is stricter than `Reason`'s and the asymmetry is deliberate.** A sentence is read by a
// person, so anything unprintable becomes a `?` and the message still reads; a name is compared
// against the connectors gyro has, so bytes that could never be one are a peer speaking a dialect
// this build does not have. Interior NULs are refused for the same reason: a field that decoded to
// one name and encoded back as another would make the echo in `Assigned` a lie.
[[nodiscard]] inline std::optional<ConnectorName> DecodeConnector(std::span<const std::byte> field) noexcept
{
	std::array<char, ConnectorName::Capacity> raw{};
	std::memcpy(raw.data(), field.data(), ConnectorName::Capacity);

	const std::string_view padded{ raw.data(), ConnectorName::Capacity };
	const std::size_t length = padded.find('\0');
	const std::string_view text = length == std::string_view::npos ? padded : padded.substr(0, length);

	// Everything after the name must be padding. A byte beyond the terminator is a sender that packed
	// the field differently, and taking the prefix would be guessing at what it meant.
	for (std::size_t index = text.size(); index < ConnectorName::Capacity; ++index)
	{
		if (raw[index] != '\0')
		{
			return std::nullopt;
		}
	}

	return ConnectorName::From(text);
}

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

// Agent to gyro: *here is my session.* The listeners ride out of band and the payload says what each
// of them is for.
//
// **A session rather than a listener is the unit, and it is what makes the handshake atomic.** An
// agent knows every socket its session has before it connects — it created them — so offering them
// one at a time would invent states that should not exist: gyro holding a privileged listener for a
// session that is not established, a first offer taken and a second refused, and an ordering rule
// between the two for somebody to remember. Offered whole, a session either exists with everything
// it has or does not exist.
//
// **The roles are the only thing stated, and everything else is read off the descriptors.** A path
// or a uid in this message would be a claim gyro would have to ignore in favour of what it can
// verify; a role is the one fact that is not discoverable from a socket, because two listeners a
// user bound in their own runtime directory are indistinguishable and only the agent knows which one
// it will point its shell at.
struct Offer
{
	static constexpr Opcode Op = Opcode::Offer;
	static constexpr std::size_t PayloadBytes = 4;

	// Which listeners are attached, as `ListenerRole` bits. The descriptors are in ascending bit
	// order, so `IndexOf` answers which is which.
	std::uint32_t Roles = static_cast<std::uint32_t>(ListenerRole::Applications);

	[[nodiscard]] std::size_t Encode(std::span<std::byte> into) const noexcept
	{
		const std::size_t total = WriteHeader(into, Op, PayloadBytes);

		if (total != 0)
		{
			StoreWord(into, HeaderBytes, Roles);
		}

		return total;
	}

	// **Decodes a bitmap it has no name for rather than refusing it**, which is `Hello`'s arrangement
	// with the version: what a peer said is separate from whether gyro will act on it, and only the
	// second is a refusal a sentence can be written for. `RolesAreWellFormed` is that question.
	[[nodiscard]] static std::optional<Offer> Decode(std::span<const std::byte> message) noexcept
	{
		if (!IsMessage(message, Op, PayloadBytes))
		{
			return std::nullopt;
		}

		return Offer{ .Roles = LoadWord(message, HeaderBytes) };
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

// Peer to gyro: *I am the machine.* Claims the machine role for this connection.
//
// **An explicit claim rather than a uid gyro could have inferred, and it closes a race that would be
// visible on the screen.** gyro places sessions itself while nobody is entitled to
// (`Compositor.cpp`'s `ShowSession`), and it must stop the moment somebody is — otherwise the greeter
// lands on a panel by gyro's stand-in rule a moment before the login agent places it deliberately,
// which is a flash at the one seam Docs/Experience.md#one-continuous-image promises there is not one
// at. The claim is where gyro learns to stop, and inferring the role from the first `Assign` would
// leave exactly that window open.
//
// It also keeps the two roles apart for a peer that could hold either. root has a session like anybody
// else, and *root offering a session* must not silently become *root running the machine*.
//
// **No payload.** What the peer may do is decided by gyro's table rather than negotiated, so there is
// nothing here to ask for; the version that governs the message was settled by `Hello`.
struct Manage
{
	static constexpr Opcode Op = Opcode::Manage;
	static constexpr std::size_t PayloadBytes = 0;

	[[nodiscard]] std::size_t Encode(std::span<std::byte> into) const noexcept
	{
		return WriteHeader(into, Op, PayloadBytes);
	}

	[[nodiscard]] static std::optional<Manage> Decode(std::span<const std::byte> message) noexcept
	{
		if (!IsMessage(message, Op, PayloadBytes))
		{
			return std::nullopt;
		}

		return Manage{};
	}
};

// gyro to peer: *you are the machine.*
//
// **It carries no list of outputs, which is the one thing a peer will want and cannot have yet.**
// Telling it would mean an enumeration that stays true — a message now and an event on every hotplug
// — and that is a channel with an ordering problem rather than a field. Until it exists a peer places
// by `ConnectorName`'s empty name, which needs no knowledge, or by a name it got from somewhere that
// is not gyro. Docs/Open.md carries it.
struct Managing
{
	static constexpr Opcode Op = Opcode::Managing;
	static constexpr std::size_t PayloadBytes = 0;

	[[nodiscard]] std::size_t Encode(std::span<std::byte> into) const noexcept
	{
		return WriteHeader(into, Op, PayloadBytes);
	}

	[[nodiscard]] static std::optional<Managing> Decode(std::span<const std::byte> message) noexcept
	{
		if (!IsMessage(message, Op, PayloadBytes))
		{
			return std::nullopt;
		}

		return Managing{};
	}
};

// What a machine peer asked for: a user's session, on some outputs or on all of them.
//
// **The pair both messages below carry, named once because it is one fact.** `Assign` states it and
// `Assigned` echoes it, and the two ends each hand it to something internal — gyro to the composition
// root, the peer to whatever is waiting for a login to land. A struct per message would make those
// four names for one thing.
//
// **Deliberately not resolved to a session or an output.** Neither resolution can be made where this
// is decoded: gyro's sessions are keyed by uid and its outputs belong to the world, and only the
// composition root sees both.
struct MachineRequest
{
	std::uint32_t Uid = 0;

	ConnectorName Connector;

	friend constexpr bool operator==(const MachineRequest&, const MachineRequest&) noexcept = default;
};

// Machine peer to gyro: *show this user's session on these outputs.*
//
// **A uid rather than a session id, which is decision 44 spent rather than worked around.** A user has
// at most one session, so a uid names it completely — and the party that would otherwise need a
// session id is the login agent, which authenticated a person and forked an agent for them but never
// saw the `Accepted` that named the session, because that answer went to the agent. Naming the user
// removes a correlation step that would have needed a channel of its own between two processes that
// have no reason to have one.
//
// It also makes the request expressible *before* the session exists, which is what a login agent
// actually does: it forks an agent and says where that person goes, and the two race by construction.
// A message that named a session could only be sent after the race was won.
//
// **A request rather than a standing policy, and it is consumed when it is satisfied.** gyro holds one
// per output — the uid it is waiting for — applies it when that user has a session, answers
// `Assigned`, and forgets. The alternative, a rule gyro keeps enforcing, would put a screen back on
// somebody's session when they logged in again hours later because of a sentence the login agent said
// at boot, which is a machine acting on an intention nobody still holds.
struct Assign
{
	static constexpr Opcode Op = Opcode::Assign;
	static constexpr std::size_t PayloadBytes = 4 + ConnectorName::Capacity;

	// The user whose session is wanted. **Never `SessionId`**: see above.
	std::uint32_t Uid = 0;

	// Which outputs, or every one of them.
	ConnectorName Connector;

	[[nodiscard]] std::size_t Encode(std::span<std::byte> into) const noexcept
	{
		const std::size_t total = WriteHeader(into, Op, PayloadBytes);

		if (total != 0)
		{
			StoreWord(into, HeaderBytes, Uid);
			std::memcpy(into.data() + HeaderBytes + 4, Connector.Bytes().data(), ConnectorName::Capacity);
		}

		return total;
	}

	[[nodiscard]] static std::optional<Assign> Decode(std::span<const std::byte> message) noexcept
	{
		if (!IsMessage(message, Op, PayloadBytes))
		{
			return std::nullopt;
		}

		const std::optional<ConnectorName> connector =
			DecodeConnector(message.subspan(HeaderBytes + 4, ConnectorName::Capacity));

		if (!connector)
		{
			return std::nullopt;
		}

		return Assign{ .Uid = LoadWord(message, HeaderBytes), .Connector = *connector };
	}
};

// gyro to machine peer: *that user's session is on those screens.*
//
// **Sent when the request is satisfied rather than when it is received, and that is the whole value of
// the message.** A request names a user who may have no session yet, so *accepted* and *done* are far
// apart in time and only the second is a fact the peer can act on — it is what says the greeter may be
// told the login went through, and it is the only thing that distinguishes a session that arrived from
// one that never will.
//
// **It echoes the request rather than describing the screen**, so that a peer with several outstanding
// can tell which one landed. Naming what actually happened would mean naming outputs gyro resolved an
// empty connector into, which is the enumeration `Managing` does not have.
struct Assigned
{
	static constexpr Opcode Op = Opcode::Assigned;
	static constexpr std::size_t PayloadBytes = 4 + ConnectorName::Capacity;

	std::uint32_t Uid = 0;

	ConnectorName Connector;

	[[nodiscard]] std::size_t Encode(std::span<std::byte> into) const noexcept
	{
		const std::size_t total = WriteHeader(into, Op, PayloadBytes);

		if (total != 0)
		{
			StoreWord(into, HeaderBytes, Uid);
			std::memcpy(into.data() + HeaderBytes + 4, Connector.Bytes().data(), ConnectorName::Capacity);
		}

		return total;
	}

	[[nodiscard]] static std::optional<Assigned> Decode(std::span<const std::byte> message) noexcept
	{
		if (!IsMessage(message, Op, PayloadBytes))
		{
			return std::nullopt;
		}

		const std::optional<ConnectorName> connector =
			DecodeConnector(message.subspan(HeaderBytes + 4, ConnectorName::Capacity));

		if (!connector)
		{
			return std::nullopt;
		}

		return Assigned{ .Uid = LoadWord(message, HeaderBytes), .Connector = *connector };
	}
};

static_assert(MinimumHandoverVersion <= HandoverVersion);
static_assert(HeaderBytes + Refused::PayloadBytes <= MaxMessageBytes);
static_assert(HeaderBytes + Assign::PayloadBytes <= MaxMessageBytes);
} // namespace Session
