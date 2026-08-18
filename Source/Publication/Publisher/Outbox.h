#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>

#include "Publication/Publisher/Publisher.h"
#include "Publication/Return.h"
#include "Publication/Ring.h"

// The dispatch side of the crossing, as one object: who owns a published snapshot, and until when.
//
// Docs/Decisions.md decision 45 gives the answer and this file is it. A snapshot belongs to the
// dispatch thread from the moment it is serialised until the frame thread's watermark passes it, and
// the frame thread never owns, frees, or shares anything — "shared ownership across the boundary puts
// `free` on the frame path wearing a destructor's clothes, where the debug allocator will not catch
// it". So the retained set lives here, on the side that is permitted to allocate, and every buffer in
// it is either in flight, being read, or back in the pool.
//
// **Buffers are recycled rather than freed, and the reason is legibility rather than speed.** A
// destroyed buffer's storage cannot be asked whether it was destroyed too early; a recycled one can,
// because the address comes back and a test can watch for it. Deferred reclamation is the property
// this whole module exists to have and the one whose failure is a use-after-free on the frame thread
// under load — so it is worth being able to observe directly rather than by inference. It also does
// not prejudge Docs/Open.md's "publication pacing": reuse is *reclamation*, and the eager-versus-paced
// question and the copy-on-write arena underneath it are untouched by it.
//
// **The loop this is shaped for.** Collect until it is empty, so the watermark is fresh and everything
// below it is back in the pool; Flush, so a publish the ring had no room for last iteration goes out
// before a newer one supersedes it; then Publish whatever the scene resolved to. Nothing here blocks,
// nothing here waits on the frame thread, and a full ring costs a retained buffer rather than a lost
// snapshot — see Publication/Ring.h for why dropping is the wrong answer.

class SnapshotOutbox
{
public:
	// The retained set and the pool are sized to their bounds here, so that neither reaches the allocator
	// again for the life of the session. The bounds are the ring's: a publish of sequence S is refused
	// unless S - Depth is below the watermark, and everything below the watermark has been reclaimed, so
	// the retained set spans at most Depth sequences and at most Depth + 1 buffers exist at once — Depth
	// retained plus the one being built into. Startup is where this object is allowed to allocate.
	SnapshotOutbox(SnapshotRing& ring, ReturnChannel& reports) : m_Ring{ ring }, m_Reports{ reports }
	{
		m_Retained.reserve(SnapshotRingDepth);
		m_Pool.reserve(SnapshotRingDepth + 1);
	}

	// Neither copied nor moved, and for a stronger reason than the two channels it holds. They refuse
	// because a thread would silently get an end nobody is on the other side of; this object refuses
	// because the ring's writer *is* the bookkeeping beside it — what is still out, what is free, and
	// how far the frame thread has got. A second outbox on one ring would keep a second answer to all
	// three and reclaim buffers the first still has published. A move is worse than a copy here rather
	// than better: the buffers would follow, but the flags beside them are primitives that would be
	// copied, leaving the source certain it still had a pending snapshot over storage it had handed
	// away. A ring with two writers has to be spelled out; it must not be arrived at by an assignment.
	SnapshotOutbox(const SnapshotOutbox&) = delete;
	SnapshotOutbox& operator=(const SnapshotOutbox&) = delete;
	SnapshotOutbox(SnapshotOutbox&&) = delete;
	SnapshotOutbox& operator=(SnapshotOutbox&&) = delete;

	// Take one report from the frame thread, advance the watermark, and reclaim everything that falls
	// below it. False when the channel is empty; the caller loops to drain it.
	//
	// The report is handed back rather than consumed, because the holds in it are not this object's
	// business: a released client buffer belongs to whoever imported it, and Publication only carries
	// the identity across. What *is* this object's business is the watermark, and taking it here means
	// no caller can advance one without performing the reclamation it authorises.
	[[nodiscard]] bool Collect(FrameReport& into) noexcept
	{
		if (!m_Reports.Take(into))
		{
			return false;
		}

		// Monotone by construction on the frame side, and taken as a maximum here anyway: a watermark
		// that went backwards would hand the writer a buffer that is still being read, and this is the
		// cheapest place in the design to make that impossible rather than merely untrue.
		if (into.Watermark > m_Watermark)
		{
			m_Watermark = into.Watermark;
			Reclaim();
		}

		return true;
	}

	// Serialise the scene and publish it. False means the ring was full and the snapshot is pending —
	// it is retained here, not lost, and the next Flush or Publish carries it.
	//
	// A pending snapshot is *superseded* rather than queued behind: the newer serialisation is built
	// into the same buffer and carries the same sequence, because a sequence the ring refused was never
	// consumed. Queueing would deliver a scene the world has already moved past, at the cost of the one
	// the frame thread actually wants.
	//
	// **The serialisation happens in the pending slot itself, and never in a local moved out of it.** A
	// shape that took the buffer out, built into it, and moved it back had a window between those two
	// moves in which the deferred snapshot's only owner was a local and m_HasPending already said there
	// was nothing pending — so an out-of-memory Build unwound through it and the deferral this whole
	// path exists to provide became a silent loss of the last publish before a quiescing scene. That is
	// the same failure Publication/Ring.h refuses to accept from a full ring, arriving by another route.
	// Building in place has no such window: Build's only failure point is the buffer's own growth, which
	// leaves the buffer exactly as it was (see SnapshotBuffer::Reset), so a throw here leaves the
	// pending snapshot and its flag untouched and the ring's sequence unconsumed — the next attempt
	// carries the same snapshot under the same number.
	bool Publish(const SnapshotPublisher& publisher)
	{
		AdoptBuffer();

		// The sequence is the ring's and is not kept here. A refused publish does not consume it, so
		// asking again — on a retry, or for the newer serialisation that supersedes a deferred one —
		// yields the same number, which is Publication/Ring.h's NextSequence contract rather than an
		// agreement this object has to maintain beside it.
		publisher.Build(m_Pending, m_Ring.NextSequence());

		m_HasPending = true;

		return Flush();
	}

	// Retry a publish the ring had no room for. True when there was nothing pending, which is the
	// ordinary case and is not worth a second name at the call site.
	//
	// **Everything goes out through here, including a first attempt**, so that a snapshot the ring
	// refuses stays exactly where it already is rather than being moved out and moved back. That is
	// not tidiness: an earlier shape passed the buffer through a helper by rvalue reference and put it
	// back on refusal, which on the *second* refusal was `m_Pending = std::move(m_Pending)`. A
	// self-move leaves a std::vector valid but unspecified — empty, in practice — while the byte size
	// beside it self-assigns intact, so the next successful publish handed the frame thread a
	// correctly-sized span over a freed pointer. It was found by the soak in Source/Integration rather
	// than by any of the single-threaded tests, because it needed the ring to be full twice running.
	bool Flush()
	{
		if (!m_HasPending)
		{
			return true;
		}

		// **Capacity before the publish, and the order is the correctness argument rather than a
		// micro-optimisation.** Growing the retained set is the only allocation on this path, and an
		// allocation that throws *after* the ring is holding a span into m_Pending's storage unwinds
		// through a temporary Held that has already taken that storage — freeing, on the way out, the
		// bytes the frame thread is about to read. Asking for the capacity here cannot cost anything,
		// because nothing has been published yet and the pending snapshot is still exactly where it was;
		// in steady state it is a comparison, because the constructor sized the set to its bound.
		m_Retained.reserve(m_Retained.size() + 1);

		if (!m_Ring.Publish(m_Pending.Bytes(), m_Watermark))
		{
			return false;
		}

		// Moved into the retained set *after* the ring has been told about the bytes, which is sound
		// rather than lucky: moving a std::vector transfers the allocation itself, so the address the
		// ring is holding does not move with the object that owns it. The same is true when the
		// retained set reallocates around it.
		//
		// And this push_back cannot throw: the capacity is reserved above, and Held's move is noexcept,
		// which the static_assert below the type holds it to.
		//
		// The sequence is read back from the ring rather than remembered from the build, so what the
		// retained set is keyed on is by construction the number the ring just assigned to these bytes.
		// Ring.h makes that the authoritative one — reclamation must work for a snapshot whose header
		// nobody could parse — and reading it here means the two cannot drift apart.
		m_Retained.push_back({ m_Ring.Published(), std::move(m_Pending) });
		m_HasPending = false;

		return true;
	}

	// The highest sequence the frame thread has reported consuming. Everything strictly below it has
	// been reclaimed.
	[[nodiscard]] std::uint64_t Watermark() const noexcept { return m_Watermark; }

	[[nodiscard]] bool HasPending() const noexcept { return m_HasPending; }

	// How many published snapshots the frame thread has not yet released, and how many buffers are
	// waiting to be filled again. Neither is read by the design; both are here so a test can assert
	// that reclamation happened rather than that nothing crashed.
	[[nodiscard]] std::size_t Retained() const noexcept { return m_Retained.size(); }
	[[nodiscard]] std::size_t Pooled() const noexcept { return m_Pool.size(); }

private:
	struct Held
	{
		std::uint64_t Sequence = 0;
		SnapshotBuffer Buffer;
	};

	static_assert(
		std::is_nothrow_move_constructible_v<Held>,
		"Flush publishes before it retains, so the retain step must not be able to throw"
	);

	// Make sure the pending slot holds storage to build into, without disturbing storage that is already
	// there. The pending buffer stays, because superseding it in place is what keeps a deferred publish
	// from becoming a queue; so does a buffer left in the slot by an earlier build that ran out of
	// memory, which is storage the pool has already given up and would otherwise be dropped. Only an
	// empty slot draws on the pool, and only an empty pool reaches the allocator — which after the first
	// few frames of a session it never does again.
	//
	// Nothing here can throw, which is what lets Publish call it before the build rather than after: the
	// slot is never left empty by a failure, and the invariant that m_HasPending implies real content in
	// m_Pending holds on every path out of this object.
	void AdoptBuffer() noexcept
	{
		if (m_HasPending || m_Pending.Capacity() != 0 || m_Pool.empty())
		{
			return;
		}

		m_Pending = std::move(m_Pool.back());
		m_Pool.pop_back();
	}

	// Everything the frame thread has moved past goes back to the pool. The retained set is in
	// ascending sequence order by construction, so what is reclaimable is always a prefix of it, and
	// the sequence *equal* to the watermark stays — that one is being rendered from.
	void Reclaim()
	{
		std::size_t reclaimed = 0;

		while (reclaimed < m_Retained.size() && m_Retained[reclaimed].Sequence < m_Watermark)
		{
			m_Pool.push_back(std::move(m_Retained[reclaimed].Buffer));
			++reclaimed;
		}

		m_Retained.erase(m_Retained.begin(), std::next(m_Retained.begin(), static_cast<std::ptrdiff_t>(reclaimed)));
	}

	SnapshotRing& m_Ring;
	ReturnChannel& m_Reports;

	// Bounded by the ring's depth: a publish beyond it is refused, so nothing can accumulate here.
	std::vector<Held> m_Retained;
	std::vector<SnapshotBuffer> m_Pool;

	SnapshotBuffer m_Pending;
	bool m_HasPending = false;

	std::uint64_t m_Watermark = 0;
};
