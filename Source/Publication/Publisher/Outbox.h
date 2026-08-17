#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
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
	SnapshotOutbox(SnapshotRing& ring, ReturnChannel& reports) noexcept : m_Ring{ ring }, m_Reports{ reports } {}

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
	bool Publish(const SnapshotPublisher& publisher)
	{
		SnapshotBuffer buffer = TakeBuffer();

		m_PendingSequence = m_Ring.NextSequence();
		publisher.Build(buffer, m_PendingSequence);

		m_Pending = std::move(buffer);
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

		if (!m_Ring.Publish(m_Pending.Bytes(), m_Watermark))
		{
			return false;
		}

		// Moved into the retained set *after* the ring has been told about the bytes, which is sound
		// rather than lucky: moving a std::vector transfers the allocation itself, so the address the
		// ring is holding does not move with the object that owns it. The same is true when the
		// retained set reallocates around it.
		m_Retained.push_back({ m_PendingSequence, std::move(m_Pending) });
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

	// Where the next snapshot is built. The pending buffer first, because superseding it is what keeps
	// a deferred publish from becoming a queue; then a reclaimed one; and only then the allocator,
	// which after the first few frames of a session it never reaches again.
	[[nodiscard]] SnapshotBuffer TakeBuffer()
	{
		if (m_HasPending)
		{
			m_HasPending = false;

			return std::move(m_Pending);
		}

		if (!m_Pool.empty())
		{
			SnapshotBuffer buffer = std::move(m_Pool.back());
			m_Pool.pop_back();

			return buffer;
		}

		return {};
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
	std::uint64_t m_PendingSequence = 0;
	bool m_HasPending = false;

	std::uint64_t m_Watermark = 0;
};
