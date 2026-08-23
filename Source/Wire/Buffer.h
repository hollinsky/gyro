#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Wire/Message.h"

// The two byte queues a connection is, separated from the socket that fills and empties them.
//
// **This is what makes the codec testable with no peer at all**, which is decision 119's first
// argument for the module existing. A `MessageWriter` writes into an `OutputBuffer` and a
// `MessageReader` reads out of an `InputBuffer`, and the connection is what carries bytes from one
// process's output buffer to another's. A test that hands the first buffer's bytes straight to the
// second has exercised every framing, padding, and argument rule without a socket, a host, or a
// second thread — and can then hand them over *wrong*, which no host can be asked to do.
//
// **Each buffer owns the descriptors passing through it, and that follows from buffering rather than
// from taste.** `MessageWriter::PutFd` takes an owning `Fd` because the message it belongs to may
// sit unsent for as long as the far end is slow, and a caller that closed its descriptor on return
// would have handed over a number the kernel has already reused. A caller that needs to keep the
// descriptor dups it, which is one explicit call at the site where the sharing is decided. libwayland
// takes the opposite position and leaves the fd with the caller; it can, because it documents that
// the fd must stay open until the next flush, which is a rule with no type behind it.

namespace Wire
{
// A descriptor queued for the wire, and the point in the byte stream it is owed by.
//
// **The boundary is what keeps fds ordered against their messages across the 28-per-`sendmsg`
// limit.** `SCM_RIGHTS` attaches its descriptors to the *first byte* of the data in the same
// `sendmsg`, so descriptors can only ever arrive early, never late — unless more than 28 pile up, at
// which point the 29th message's bytes would go out with no descriptor behind them and the receiver
// would demarshal a `new_id` for a buffer it does not have. Recording where each message ends is what
// lets `Connection::Flush` clamp the write to the last message it can still carry the descriptors for.
struct PendingFd
{
	Fd Descriptor;

	// Offset into `Pending()`, one past the last byte of the message this descriptor belongs to.
	std::size_t Boundary = 0;
};

// What has been marshalled and not yet handed to the kernel.
class OutputBuffer
{
public:
	// Where a message started, so that a writer destroyed without `Send` leaves nothing behind. Both
	// halves are needed: a half-written message has bytes, and it may already have taken ownership of
	// descriptors that have to be closed rather than leaked into the next message.
	struct Mark
	{
		std::size_t Bytes = 0;
		std::size_t Fds = 0;
	};

	OutputBuffer()
	{
		// One page of messages and one `sendmsg` of descriptors, so that the ordinary frame allocates
		// nothing. Core/FrameSection.h does not cover the drain — Frame/Loop.h puts it deliberately
		// outside the guard — but this runs on a SCHED_FIFO thread and an arena lock is an arena lock
		// whether or not a check is watching.
		m_Bytes.reserve(4096);
		m_Fds.reserve(MaxFdsPerMessage);
	}

	[[nodiscard]] Mark Position() const noexcept { return Mark{ .Bytes = m_Bytes.size(), .Fds = m_Fds.size() }; }

	void PutWord(std::uint32_t word)
	{
		const std::size_t offset = m_Bytes.size();
		m_Bytes.resize(offset + sizeof word);
		StoreWord(m_Bytes, offset, word);
	}

	// Appends the bytes and then the zeros that round them up to a word. One call rather than two,
	// because a string or an array is the only thing that ever needs either and forgetting the second
	// produces a message the far end reads one argument off from.
	void PutPadded(std::span<const std::byte> bytes)
	{
		const std::size_t padded = Padded(bytes.size());

		m_Bytes.insert(m_Bytes.end(), bytes.begin(), bytes.end());
		m_Bytes.resize(m_Bytes.size() + (padded - bytes.size()), std::byte{ 0 });
	}

	// The bytes, a terminator, and the padding that rounds the pair up to a word. What a string is:
	// the NUL is part of the payload the length counts, so it is padded together with it rather than
	// appended after the padding.
	void PutTerminated(std::span<const std::byte> bytes)
	{
		const std::size_t padded = Padded(bytes.size() + 1);

		m_Bytes.insert(m_Bytes.end(), bytes.begin(), bytes.end());
		m_Bytes.resize(m_Bytes.size() + (padded - bytes.size()), std::byte{ 0 });
	}

	// The boundary is filled in by `Commit`, because a descriptor is put while its message is still
	// being written and the offset it is owed by is not known until the message ends.
	void PutFd(Fd descriptor) { m_Fds.push_back(PendingFd{ .Descriptor = std::move(descriptor), .Boundary = 0 }); }

	// One message has ended. Every descriptor still without a boundary belongs to it, and everything
	// written so far is now whole enough to send.
	void Commit() noexcept
	{
		for (std::size_t index = m_Fds.size(); index > 0 && m_Fds[index - 1].Boundary == 0; --index)
		{
			m_Fds[index - 1].Boundary = m_Bytes.size();
		}

		m_Committed = m_Bytes.size();
	}

	// Discards everything written since the mark, closing any descriptors it took ownership of. What a
	// `MessageWriter` does when it is destroyed without being sent.
	void Rollback(Mark mark) noexcept
	{
		m_Bytes.resize(mark.Bytes);
		m_Fds.resize(mark.Fds);
		m_Committed = std::min(m_Committed, mark.Bytes);
		m_Open = false;
	}

	void PatchWord(std::size_t offset, std::uint32_t word) noexcept { StoreWord(m_Bytes, offset, word); }

	[[nodiscard]] std::size_t Size() const noexcept { return m_Bytes.size(); }

	// Everything written and not yet handed to the kernel, whole messages or not. What a caller asks
	// when it wants to know whether anything is still queued.
	[[nodiscard]] std::span<const std::byte> Pending() const noexcept
	{
		return std::span<const std::byte>{ m_Bytes }.subspan(m_Sent);
	}

	// **What may actually go on the wire: whole messages only.** A `MessageWriter` marshals in place,
	// so between its construction and its `Send` the buffer ends in a message with a placeholder size
	// word — and a flush that wrote it would put a header claiming zero bytes on the socket, which the
	// host answers by disconnecting. Flushing mid-message is a caller mistake, but it is one that
	// costs nothing to make harmless: the half-written message simply waits for the next flush.
	[[nodiscard]] std::span<const std::byte> Committed() const noexcept
	{
		return std::span<const std::byte>{ m_Bytes }.subspan(m_Sent, m_Committed - m_Sent);
	}

	// What one `sendmsg` may carry.
	struct Batch
	{
		std::span<const std::byte> Bytes;
		std::span<PendingFd> Fds;
	};

	// **The 28-descriptor limit, spent here rather than in the caller, because the offsets live here.**
	// `SCM_RIGHTS` attaches its descriptors to the first byte of the data in the same `sendmsg`, so a
	// descriptor can arrive early but never late — the receiver queues them and takes them in order as
	// it demarshals. That holds right up until a twenty-ninth is queued, at which point sending all the
	// bytes would put its message on the wire with nothing behind it, and the far end would demarshal
	// a `new_id` for a buffer it will never receive. So a batch stops at the end of the last message it
	// can still carry the descriptors for, and the rest goes next time round.
	[[nodiscard]] Batch NextBatch() noexcept
	{
		std::span<const std::byte> bytes = Committed();

		// Committed descriptors are a prefix: commits happen in order, so an uncommitted one belongs to
		// the message still open and there are never any before a committed one.
		std::size_t count = 0;

		while (count < m_Fds.size() && m_Fds[count].Boundary != 0 && m_Fds[count].Boundary <= m_Committed)
		{
			++count;
		}

		if (count > MaxFdsPerMessage)
		{
			const std::size_t boundary = m_Fds[MaxFdsPerMessage - 1].Boundary;

			count = MaxFdsPerMessage;

			// Saturating, and it should never saturate: a boundary at or before what has already gone
			// would be a descriptor left behind by its own message, which is precisely what this clamp
			// prevents. Written this way because the alternative on an underflow is a span of two to
			// the sixty-four bytes handed to `sendmsg`.
			bytes = bytes.first(std::min(bytes.size(), boundary - std::min(boundary, m_Sent)));
		}

		return Batch{ .Bytes = bytes, .Fds = std::span<PendingFd>{ m_Fds }.first(count) };
	}

	// Every descriptor queued, committed or not. Non-const because the caller takes them: the
	// round-trip test moves them into the peer's input buffer in place of a kernel.
	[[nodiscard]] std::span<PendingFd> Fds() noexcept { return m_Fds; }

	// The kernel accepted `bytes` of the last batch and `fds` of its descriptors. The descriptors are
	// closed here rather than by the caller, which is what the ownership rule in this file's header
	// buys: there is one place a sent descriptor stops being ours.
	//
	// **Retiring bytes moves an offset rather than the bytes, and that is correctness rather than
	// economy.** A `MessageWriter` holds a mark into this buffer for as long as its message is open, so
	// a flush that compacted underneath one would leave the mark naming the wrong word — and the size
	// field would be patched into the middle of the *next* message. Reclaiming happens only where no
	// mark can be live.
	void Sent(std::size_t bytes, std::size_t fds)
	{
		m_Fds.erase(m_Fds.begin(), m_Fds.begin() + static_cast<std::ptrdiff_t>(fds));
		m_Sent += std::min(bytes, m_Bytes.size() - m_Sent);

		Reclaim();
	}

	// The sent prefix, dropped, and every offset rebased onto what is left. Only while no message is
	// open, for `Sent`'s reason.
	void Reclaim()
	{
		if (m_Open || m_Sent == 0)
		{
			return;
		}

		m_Bytes.erase(m_Bytes.begin(), m_Bytes.begin() + static_cast<std::ptrdiff_t>(m_Sent));
		m_Committed -= std::min(m_Committed, m_Sent);

		for (PendingFd& pending : m_Fds)
		{
			pending.Boundary -= std::min(pending.Boundary, m_Sent);
		}

		m_Sent = 0;
	}

	// Opens a message, refusing a second one while the first is still being written.
	//
	// **Two live writers on one buffer would silently eat each other's messages**, because a writer
	// that is destroyed without being sent rolls the buffer back to where it started — which, if
	// another writer has committed a message in the meantime, discards that one too. It is a mistake
	// nothing else would catch: the request simply never arrives, and the symptom is a surface that
	// never maps. Four lines here turn it into a refusal the next `Flush` reports.
	[[nodiscard]] bool Begin() noexcept
	{
		if (m_Open)
		{
			RecordFault(EBUSY, "marshalling two wayland requests into one buffer at once");
			return false;
		}

		m_Open = true;

		return true;
	}

	void End()
	{
		m_Open = false;

		// The moment no mark can be live, which is where a flush that ran mid-message left its sent
		// prefix behind.
		Reclaim();
	}

	// A message that could not be marshalled — one too large for the 16-bit size field, or the nested
	// writer above. Latched rather than reported, because the writer that hit it has no connection to
	// fail and the next `Flush` does.
	void RecordFault(int code, std::string_view context)
	{
		if (!m_Fault)
		{
			m_Fault = Error{ code, context };
		}
	}

	[[nodiscard]] const std::optional<Error>& Fault() const noexcept { return m_Fault; }

private:
	std::vector<std::byte> m_Bytes;
	std::vector<PendingFd> m_Fds;
	std::optional<Error> m_Fault;

	// Both are absolute offsets into `m_Bytes`, and staying absolute is what lets a `MessageWriter`
	// hold a mark across a flush.
	std::size_t m_Sent = 0;
	std::size_t m_Committed = 0;

	bool m_Open = false;
};

// What has arrived and not yet been demarshalled.
class InputBuffer
{
public:
	InputBuffer()
	{
		m_Bytes.reserve(4096);
		m_Fds.reserve(MaxFdsPerMessage);
	}

	void Append(std::span<const std::byte> bytes) { m_Bytes.insert(m_Bytes.end(), bytes.begin(), bytes.end()); }

	void PutFd(Fd descriptor) { m_Fds.push_back(std::move(descriptor)); }

	[[nodiscard]] std::span<const std::byte> Available() const noexcept
	{
		return std::span<const std::byte>{ m_Bytes }.subspan(m_Read);
	}

	// **Consuming moves an index rather than erasing**, because a `MessageReader` hands the dispatcher
	// `string_view`s and `span`s that point into this buffer and the dispatcher runs before the message
	// is retired. Compaction happens at the two points where nothing can be pointing in: when the
	// buffer empties, and when the dead prefix has grown past what is left.
	void Consume(std::size_t bytes) noexcept
	{
		m_Read += bytes;

		if (m_Read >= m_Bytes.size())
		{
			m_Bytes.clear();
			m_Read = 0;
		}
	}

	// The dead prefix, dropped. Called between reads rather than between messages, for the reason
	// above.
	void Compact()
	{
		if (m_Read == 0)
		{
			return;
		}

		m_Bytes.erase(m_Bytes.begin(), m_Bytes.begin() + static_cast<std::ptrdiff_t>(m_Read));
		m_Read = 0;
	}

	[[nodiscard]] std::size_t FdCount() const noexcept { return m_Fds.size() - m_FdRead; }

	// The oldest descriptor, or an invalid one where the queue is empty — which is a message asking
	// for a descriptor the host did not send, and Wire/Reader.h turns it into a connection error
	// rather than a silent -1 the caller would pass to `mmap`.
	[[nodiscard]] Fd TakeFd() noexcept
	{
		if (m_FdRead >= m_Fds.size())
		{
			return Fd{};
		}

		Fd descriptor = std::move(m_Fds[m_FdRead]);
		++m_FdRead;

		if (m_FdRead >= m_Fds.size())
		{
			m_Fds.clear();
			m_FdRead = 0;
		}

		return descriptor;
	}

private:
	std::vector<std::byte> m_Bytes;
	std::vector<Fd> m_Fds;
	std::size_t m_Read = 0;
	std::size_t m_FdRead = 0;
};
} // namespace Wire
