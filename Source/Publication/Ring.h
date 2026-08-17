#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

// The forward half of the crossing: dispatch publishes snapshots, the frame thread takes the newest.
//
// Docs/Architecture.md#the-publication-boundary makes four of this channel's properties requirements
// rather than optimisations, and two of them are decided here. It is **wait-free on the reader**,
// because a lock shared with a thread the frame thread outranks is the priority inversion the whole
// split exists to prevent. And **reclamation is deferred and one-way**: the frame thread publishes the
// sequence it last consumed, the dispatch thread frees below that, and nothing shared owns anything —
// shared ownership across the boundary puts `free` on the frame path wearing a destructor's clothes.
//
// **Newest wins, and the reader is allowed to skip.** A snapshot is complete scene state rather than a
// delta, so a snapshot nobody read costs nothing; the frame thread jumps straight to the highest
// sequence published and never walks. The invariant that buys this is worth stating because a future
// addition is what would break it: **nothing may be owed once per published snapshot — only once per
// rendered frame.** Frame callbacks, presentation feedback, and buffer releases are all owed per
// frame, so they survive skipping; a per-snapshot obligation would not, and would fail silently.
//
// **The slot is derived from the sequence, and that is what makes the reader wait-free.** Scanning
// the slots for the newest would read fields the writer may be writing, which is a data race, and
// re-checking after the read would make the reader lock-free rather than wait-free — bounded retries
// in practice, unbounded on paper, on the one thread that must never take an unbounded pause. Indexing
// by `sequence % Depth` reduces an acquire to one atomic load and one indexed read with no retry.
//
// It is sound because of the recycling rule, and the argument is short enough to write down. The slot
// a sequence S lands in was last occupied by S - Depth, so rewriting the slot the reader is reading
// means publishing S + Depth, which requires S to be strictly below the watermark. But the watermark
// is a sequence the frame thread itself reported, so it is at most the sequence the frame thread
// holds, which is strictly below S — the snapshot it is reaching for is one it has not held yet.
// S < watermark <= held < S is a contradiction, so the slot cannot be rewritten under the reader.
//
// **A full ring defers rather than drops, and the difference is a real defect.** When the frame thread
// has not acquired for Depth publishes there is no free slot, and dropping the new snapshot is nearly
// harmless — except when it is the last publish before the scene quiesces, in which case nothing
// republishes it and the frame thread renders stale state until something unrelated moves. So a
// refused publish is retained by the dispatch side and retried, and a newer serialisation supersedes
// the pending one rather than queueing behind it. Memory is bounded at Depth + 1 snapshots and
// dispatch never blocks on a thread it outranks. See Publication/Publisher/Outbox.h, which owns that
// pending slot, and Docs/Decisions.md decision 73.
//
// **Where the watermark comes from is part of this file's contract.** It arrives over the return
// channel (Publication/Return.h), and the acquire on that channel's queue is what orders the frame
// thread's last read of a snapshot before the dispatch thread's reuse of its bytes. Passing a *stale*
// watermark is always safe — it only defers reclamation — but passing one obtained without that
// synchronisation is not, and there is no way for this file to check it.

// One published snapshot's byte range, addressed by the sequence that names it.
//
// The bytes are borrowed: they belong to the dispatch side until the watermark passes them, which is
// exactly what makes this channel's ownership one-way. The sequence is the *ring's*, not the one
// parsed out of the header — a malformed snapshot must still be reclaimable, and a watermark derived
// from bytes the reader could not make sense of would be zero and would stall reclamation forever.
struct AcquiredSnapshot
{
	std::span<const std::byte> Bytes{};
	std::uint64_t Sequence = 0;

	// Sequences begin at one, so zero is "nothing newer than what you hold" without a second field.
	[[nodiscard]] constexpr bool IsNewer() const noexcept { return Sequence != 0; }
};

// Three is the floor — one held by the frame thread, one newest, and one for the writer to build into
// — and the fourth is a frame of slack, so an ordinary late frame does not reach the deferral path at
// all. A power of two so the index is a mask rather than a division on the frame thread.
inline constexpr std::uint64_t SnapshotRingDepth = 4;

static_assert(SnapshotRingDepth >= 3, "Held, newest, and one in hand are all live at once");
static_assert((SnapshotRingDepth & (SnapshotRingDepth - 1)) == 0, "so the index is a mask");

class SnapshotRing
{
public:
	SnapshotRing() = default;

	// Shared between the two threads by reference, so copying one would silently give a thread a
	// channel nobody is on the other end of.
	SnapshotRing(const SnapshotRing&) = delete;
	SnapshotRing& operator=(const SnapshotRing&) = delete;

	// The frame side. Wait-free: one acquire load, one indexed read, no retry, no allocation, and no
	// lock the dispatch thread could be holding.
	//
	// `held` is the sequence the caller already has, and the answer is empty when nothing newer exists
	// — which is the ordinary case for an output waking on a timer with no commit in between. Passing
	// zero asks for whatever is there, which is what the frame thread does on its first iteration.
	[[nodiscard]] AcquiredSnapshot Acquire(std::uint64_t held) const noexcept
	{
		const std::uint64_t published = m_Published.load(std::memory_order_acquire);

		if (published <= held)
		{
			return {};
		}

		// Safe to read without re-checking, by the argument in this file's header: the writer cannot
		// have begun rewriting this slot, because doing so would require the watermark to be above a
		// sequence the caller has not consumed yet.
		return { m_Slots[published & (SnapshotRingDepth - 1)].Bytes, published };
	}

	// The sequence the next successful publish will carry.
	//
	// The dispatch side needs it *before* it hands the bytes over, because the sequence is both this
	// ring's ordering and the snapshot's own identity in its header, and those have to be one number.
	// A refused publish does not consume it, so a retry — or a newer serialisation that supersedes the
	// pending one — carries the same sequence and the run stays contiguous with no gaps.
	[[nodiscard]] std::uint64_t NextSequence() const noexcept
	{
		return m_Published.load(std::memory_order_relaxed) + 1;
	}

	// The dispatch side. Returns false when the ring is full, in which case nothing was published and
	// the caller retains the bytes to retry — see the deferral argument in this file's header.
	//
	// `watermark` is the highest sequence the frame thread has reported consuming, and it must have
	// been read through the return channel's acquire. Everything strictly below it is the dispatch
	// side's to reuse; the sequence equal to it is the one the frame thread is rendering from.
	[[nodiscard]] bool Publish(std::span<const std::byte> bytes, std::uint64_t watermark) noexcept
	{
		const std::uint64_t next = m_Published.load(std::memory_order_relaxed) + 1;

		// The slot this sequence lands in was last occupied by next - Depth, and that occupant is only
		// free once the frame thread has moved past it. Below Depth the slot has never been written.
		if (next > SnapshotRingDepth && next - SnapshotRingDepth >= watermark)
		{
			return false;
		}

		m_Slots[next & (SnapshotRingDepth - 1)].Bytes = bytes;

		// The release is what carries the slot write to the reader's acquire, and it is the last thing
		// this function does for that reason.
		m_Published.store(next, std::memory_order_release);

		return true;
	}

	// The highest sequence published so far, for the dispatch side's own bookkeeping and for tests.
	// Zero means nothing has been published, which is why sequences begin at one.
	[[nodiscard]] std::uint64_t Published() const noexcept { return m_Published.load(std::memory_order_relaxed); }

private:
	// Written by the dispatch thread before the release below, read by the frame thread after the
	// matching acquire. Plain rather than atomic deliberately: the ordering is the publication's, and
	// a per-field atomic would claim an independence these fields do not have.
	struct Slot
	{
		std::span<const std::byte> Bytes{};
	};

	Slot m_Slots[SnapshotRingDepth]{};
	std::atomic<std::uint64_t> m_Published{ 0 };
};

static_assert(
	std::atomic<std::uint64_t>::is_always_lock_free,
	"A sequence that could take a lock to store would put one on the frame thread"
);
