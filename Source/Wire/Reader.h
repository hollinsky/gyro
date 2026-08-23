#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "Core/Fd.h"
#include "Wire/Buffer.h"
#include "Wire/Message.h"

// One message, being taken apart.
//
// **A reader that has run off the end of its message is a connection error, not undefined
// behaviour**, and that is the runtime's half of decision 2's argument for generated bindings. The
// generated dispatcher is a `switch` over an opcode calling `GetUint`, `GetString`, `GetFd` in the
// order the protocol file says — it cannot have a hole, because it was generated from the same file
// the host marshalled against. What it *can* meet is a host that lied, or a version skew where an
// event grew an argument, and neither of those may be a `memcpy` past the end of a heap buffer on a
// process that holds DRM master.
//
// So every accessor is total. Running out of message sets a flag and returns a zero of the right
// type, subsequent accessors do nothing, and Wire/Connection.h checks the flag once after the
// dispatcher returns and fails the connection. The generated code stays a straight-line sequence of
// `Get` calls with no error handling in it, which is the only way it is worth generating.
//
// **The zero returned is not a value the caller should use, and it does not have to check.** An
// `ObjectId::None` reaches a proxy lookup that will not find it; an empty `string_view` is empty; an
// invalid `Fd` fails whatever syscall it is handed. The connection is already dead by then and the
// dispatcher's work is discarded — what matters is that nothing between here and that check reads
// memory it does not own.

namespace Wire
{
class MessageReader
{
public:
	// `body` is the message *after* its two header words, and `fds` is the connection's queue rather
	// than this message's: descriptors arrive attached to whichever `sendmsg` carried the first byte
	// of their message, so they are consumed in order across messages rather than counted per message.
	MessageReader(MessageHeader header, std::span<const std::byte> body, InputBuffer& fds) noexcept
		: m_Body{ body }, m_Fds{ &fds }, m_Header{ header }
	{}

	[[nodiscard]] ObjectId Target() const noexcept { return m_Header.Target; }

	[[nodiscard]] std::uint16_t Opcode() const noexcept { return m_Header.Opcode; }

	[[nodiscard]] std::int32_t GetInt() noexcept { return static_cast<std::int32_t>(GetUint()); }

	[[nodiscard]] std::uint32_t GetUint() noexcept
	{
		if (!Take(sizeof(std::uint32_t)))
		{
			return 0;
		}

		return LoadWord(m_Body, m_Read - sizeof(std::uint32_t));
	}

	[[nodiscard]] Fixed GetFixed() noexcept { return Fixed::FromRaw(GetInt()); }

	[[nodiscard]] ObjectId GetObject() noexcept { return static_cast<ObjectId>(GetUint()); }

	// The same word as `GetObject`, and the distinction is for the reader of the generated code rather
	// than for this type: an argument the host is *creating* has to be bound before the next event for
	// it arrives, and a call site that says `GetNewId` is one a reader can check that against.
	[[nodiscard]] ObjectId GetNewId() noexcept { return static_cast<ObjectId>(GetUint()); }

	// Length including the terminator, then the bytes, then padding to a word. The view excludes the
	// terminator and points into the connection's receive buffer, so it is valid for the duration of
	// the dispatcher call and no longer — a proxy that keeps a title copies it.
	//
	// A length of zero is the protocol's null string, which is distinct from the empty string only in
	// that the empty string is a length of one and a lone NUL. Both come back empty here, because a
	// caller that needs to tell them apart is reading an argument the protocol marks nullable and the
	// generated code knows which those are.
	[[nodiscard]] std::string_view GetString() noexcept
	{
		const std::uint32_t length = GetUint();

		if (m_Failed || length == 0)
		{
			return {};
		}

		if (!Take(Padded(length)))
		{
			return {};
		}

		const std::span<const std::byte> text = m_Body.subspan(m_Read - Padded(length), length);

		// The terminator is the host's obligation and it is checked rather than assumed. Without this
		// a host could send a length of 8 over eight non-zero bytes and the view would be handed to a
		// proxy that eventually calls a C function on `.data()`.
		if (text.back() != std::byte{ 0 })
		{
			m_Failed = true;
			return {};
		}

		return std::string_view{ reinterpret_cast<const char*>(text.data()), length - 1 };
	}

	// Length in bytes with no terminator, then the bytes, then padding. Points into the receive buffer
	// on the same terms as `GetString`.
	[[nodiscard]] std::span<const std::byte> GetArray() noexcept
	{
		const std::uint32_t length = GetUint();

		if (m_Failed || length == 0)
		{
			return {};
		}

		if (!Take(Padded(length)))
		{
			return {};
		}

		return m_Body.subspan(m_Read - Padded(length), length);
	}

	// Descriptors occupy no bytes on the wire, so this consumes nothing from the message. A host that
	// sent the message without the descriptor is a host whose next `new_id` would be read against the
	// wrong queue position, which is why an empty queue fails the connection rather than returning an
	// invalid descriptor the caller might ignore.
	[[nodiscard]] Fd GetFd() noexcept
	{
		if (m_Failed)
		{
			return Fd{};
		}

		Fd descriptor = m_Fds->TakeFd();

		if (!descriptor.IsValid())
		{
			m_Failed = true;
		}

		return descriptor;
	}

	// Whether anything asked for more than the message had. Checked once by the connection after the
	// dispatcher returns, rather than at every accessor by the generated code.
	[[nodiscard]] bool Failed() const noexcept { return m_Failed; }

	// What is left unread. Not a protocol error on its own — a client built against an older version
	// of an interface will legitimately ignore arguments a newer host appends — so it is offered
	// rather than checked.
	[[nodiscard]] std::size_t Remaining() const noexcept { return m_Body.size() - m_Read; }

private:
	// Advances by `bytes` if the message has them, and latches the failure if it does not.
	[[nodiscard]] bool Take(std::size_t bytes) noexcept
	{
		if (m_Failed || bytes > m_Body.size() - m_Read)
		{
			m_Failed = true;
			return false;
		}

		m_Read += bytes;

		return true;
	}

	std::span<const std::byte> m_Body;
	InputBuffer* m_Fds;
	MessageHeader m_Header;
	std::size_t m_Read = 0;
	bool m_Failed = false;
};
} // namespace Wire
