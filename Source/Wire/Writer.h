#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "Core/Fd.h"
#include "Wire/Buffer.h"
#include "Wire/Connection.h"
#include "Wire/Message.h"

// One request, being assembled.
//
// **It marshals in place rather than into a staging buffer**, which is the one design choice here
// worth an argument. The header's size word is not known until the last argument has been written, so
// the alternatives are a scratch buffer copied into the connection on `Send` or a placeholder patched
// afterwards. The placeholder wins because the scratch buffer would have to be sized for the largest
// message anything might ever send, and every request would pay a copy so that the rare one does not
// need a heap allocation. What the placeholder owes in exchange is a rollback: a writer destroyed
// without `Send` — a proxy that gave up halfway, a scope left early — must leave the buffer exactly
// as it found it, including closing any descriptors it took ownership of.
//
// **Every accessor is total, for Wire/Reader.h's reason from the other side.** Generated marshalling
// code is a straight run of `Put` calls with no error handling in it, so a message that cannot be
// written latches a failure on the buffer and the next `Flush` reports it. There is nothing at a call
// site to check and nothing it could usefully do if there were.

namespace Wire
{
class MessageWriter
{
public:
	MessageWriter(Connection& connection, ObjectId target, std::uint16_t opcode)
		: MessageWriter{ connection.Output(), target, opcode }
	{}

	// The buffer form, so the codec can be exercised with no connection and therefore no socket. Both
	// constructors are the same constructor; this is the one that does the work.
	MessageWriter(OutputBuffer& buffer, ObjectId target, std::uint16_t opcode) : m_Opcode{ opcode }
	{
		if (!buffer.Begin())
		{
			return;
		}

		m_Buffer = &buffer;
		m_Mark = buffer.Position();

		buffer.PutWord(static_cast<std::uint32_t>(target));

		// The size is not known yet. Patched by `Send`, and rolled back by the destructor if there
		// never is one, so a placeholder can never reach the wire.
		buffer.PutWord(PackSizeAndOpcode(0, opcode));
	}

	~MessageWriter()
	{
		if (m_Buffer != nullptr)
		{
			m_Buffer->Rollback(m_Mark);
		}
	}

	// Neither copied nor moved. Two objects holding one mark is the rollback running twice, and the
	// second one takes a message that was already sent.
	MessageWriter(const MessageWriter&) = delete;
	MessageWriter& operator=(const MessageWriter&) = delete;
	MessageWriter(MessageWriter&&) = delete;
	MessageWriter& operator=(MessageWriter&&) = delete;

	void PutInt(std::int32_t value) { PutUint(static_cast<std::uint32_t>(value)); }

	void PutUint(std::uint32_t value)
	{
		if (m_Buffer != nullptr)
		{
			m_Buffer->PutWord(value);
		}
	}

	void PutFixed(Fixed value) { PutInt(value.Raw()); }

	void PutObject(ObjectId id) { PutUint(static_cast<std::uint32_t>(id)); }

	// The same word as `PutObject`. Separate for the reason `GetNewId` is: a request that creates an
	// object has to have allocated the id from the connection, and a call site that says so is one a
	// reader can check.
	void PutNewId(ObjectId id) { PutUint(static_cast<std::uint32_t>(id)); }

	// Length including the terminator, the bytes, the terminator, then padding to a word. A
	// `string_view` that is empty writes a length of one and a lone NUL — the *empty* string rather
	// than the protocol's null string, which is a length of zero and is what a nullable argument sends
	// when it is absent.
	void PutString(std::string_view text)
	{
		if (m_Buffer == nullptr)
		{
			return;
		}

		m_Buffer->PutWord(static_cast<std::uint32_t>(text.size() + 1));

		// The terminator is part of the payload, so it is padded with it rather than after it.
		const std::span<const std::byte> bytes{ reinterpret_cast<const std::byte*>(text.data()), text.size() };

		m_Buffer->PutTerminated(bytes);
	}

	// The absent value of a nullable string argument. Named rather than reached by passing something
	// that means "empty", because the two are different on the wire and a caller cannot spell the
	// difference with a `string_view`.
	void PutNullString() { PutUint(0); }

	// Length in bytes, the bytes, then padding. No terminator: an array is opaque and may hold zeros.
	void PutArray(std::span<const std::byte> bytes)
	{
		if (m_Buffer == nullptr)
		{
			return;
		}

		m_Buffer->PutWord(static_cast<std::uint32_t>(bytes.size()));
		m_Buffer->PutPadded(bytes);
	}

	// Takes ownership, per Wire/Buffer.h: the message may sit unsent for as long as the host is slow,
	// and a caller that closed its copy on return would have handed over a number the kernel has
	// already given to something else. A caller that still needs the descriptor dups it.
	void PutFd(Fd descriptor)
	{
		if (m_Buffer != nullptr)
		{
			m_Buffer->PutFd(std::move(descriptor));
		}
	}

	// Patches the size word and closes the message. Nothing reaches the socket here — `Flush` is what
	// talks to the kernel, so a burst of requests is one `sendmsg` rather than one each.
	void Send()
	{
		if (m_Buffer == nullptr)
		{
			return;
		}

		const std::size_t size = m_Buffer->Size() - m_Mark.Bytes;

		if (size > MaxMessageBytes)
		{
			m_Buffer->Rollback(m_Mark);
			m_Buffer->RecordFault(EMSGSIZE, "marshalling a wayland request larger than the wire's size field");
			m_Buffer = nullptr;

			return;
		}

		m_Buffer->PatchWord(m_Mark.Bytes + 4, PackSizeAndOpcode(size, m_Opcode));
		m_Buffer->Commit();
		m_Buffer->End();
		m_Buffer = nullptr;
	}

private:
	OutputBuffer* m_Buffer = nullptr;
	OutputBuffer::Mark m_Mark;
	std::uint16_t m_Opcode = 0;
};
} // namespace Wire
