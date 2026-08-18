#include "Publication/Publisher/Outbox.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "Publication/Reader/Reader.h"
#include "Publication/Return.h"
#include "Publication/Ring.h"
#include "Publication/Snapshot.h"
#include "Testing/Test.h"

// Ownership across the boundary, driven from one thread.
//
// What is asserted here is decision 45's deferred reclamation as a *visible* property: a published
// snapshot's storage is not reusable while the frame thread is still naming it, and it is reusable the
// moment the watermark passes. Recycling rather than freeing is what makes that observable — the
// address comes back, and a test can watch for it — which is the argument Outbox.h makes for the pool.

namespace
{
// A one-field run, so two snapshots can be told apart by their contents rather than by their size.
struct Mark
{
	std::uint64_t Value = 0;

	friend bool operator==(const Mark&, const Mark&) = default;
};

// The frame thread's whole job, for these tests: take the newest snapshot and say which one it holds.
class FrameSide
{
public:
	FrameSide(SnapshotRing& ring, ReturnChannel& reports) noexcept : m_Ring{ ring }, m_Reports{ reports } {}

	// Acquire whatever is newest and report it, exactly as the frame loop will: the watermark is the
	// sequence being rendered *from*, so it is posted after the acquire and never before.
	[[nodiscard]] SnapshotReader Advance()
	{
		const AcquiredSnapshot acquired = m_Ring.Acquire(m_Held);

		if (acquired.IsNewer())
		{
			m_Held = acquired.Sequence;
			m_Bytes = acquired.Bytes;
		}

		GYRO_CHECK_EQ(m_Reports.Post(m_Held), std::size_t{ 0 });

		return SnapshotReader{ m_Bytes };
	}

	[[nodiscard]] std::uint64_t Held() const noexcept { return m_Held; }

private:
	SnapshotRing& m_Ring;
	ReturnChannel& m_Reports;
	std::span<const std::byte> m_Bytes{};
	std::uint64_t m_Held = 0;
};

// The dispatch loop's collection step, drained to empty as the real one will be.
void CollectAll(SnapshotOutbox& outbox)
{
	FrameReport report;
	while (outbox.Collect(report))
	{
	}
}

[[nodiscard]] SnapshotPublisher Marked(std::uint64_t value)
{
	const std::array<Mark, 1> marks{ Mark{ value } };

	SnapshotPublisher publisher;
	publisher.Put<Mark>(SnapshotRun::Positions, marks);

	return publisher;
}

[[nodiscard]] std::uint64_t MarkOf(const SnapshotReader& reader)
{
	const std::span<const Mark> marks = reader.Run<Mark>(SnapshotRun::Positions);

	return marks.empty() ? 0 : marks[0].Value;
}
} // namespace

GYRO_TEST(SnapshotOutbox, APublishedSnapshotCarriesTheRingsSequence)
{
	// The snapshot's identity in its header and the ring's ordering are one number, which is why the
	// builder takes the sequence rather than keeping one.
	SnapshotRing ring;
	ReturnChannel reports;
	SnapshotOutbox outbox{ ring, reports };
	FrameSide frame{ ring, reports };

	GYRO_REQUIRE(outbox.Publish(Marked(7)));

	const SnapshotReader reader = frame.Advance();
	GYRO_REQUIRE(reader.IsValid());
	GYRO_CHECK_EQ(reader.Sequence(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(frame.Held(), reader.Sequence());
	GYRO_CHECK_EQ(MarkOf(reader), std::uint64_t{ 7 });
}

GYRO_TEST(SnapshotOutbox, TheSnapshotBeingRenderedFromIsNotReclaimed)
{
	// The watermark names what the frame thread *holds*, so reclamation is strictly below it. Reclaiming
	// at the watermark would hand the writer the buffer under the reader, and this is the assertion that
	// would catch it.
	SnapshotRing ring;
	ReturnChannel reports;
	SnapshotOutbox outbox{ ring, reports };
	FrameSide frame{ ring, reports };

	GYRO_REQUIRE(outbox.Publish(Marked(1)));
	static_cast<void>(frame.Advance());
	CollectAll(outbox);

	GYRO_CHECK_EQ(outbox.Watermark(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(outbox.Retained(), std::size_t{ 1 });
	GYRO_CHECK_EQ(outbox.Pooled(), std::size_t{ 0 });

	// Once the frame thread has moved to the second snapshot, the first is the dispatch side's again.
	GYRO_REQUIRE(outbox.Publish(Marked(2)));
	static_cast<void>(frame.Advance());
	CollectAll(outbox);

	GYRO_CHECK_EQ(outbox.Watermark(), std::uint64_t{ 2 });
	GYRO_CHECK_EQ(outbox.Retained(), std::size_t{ 1 });
	GYRO_CHECK_EQ(outbox.Pooled(), std::size_t{ 1 });
}

GYRO_TEST(SnapshotOutbox, AReclaimedBufferIsFilledAgainRatherThanReallocated)
{
	SnapshotRing ring;
	ReturnChannel reports;
	SnapshotOutbox outbox{ ring, reports };
	FrameSide frame{ ring, reports };

	GYRO_REQUIRE(outbox.Publish(Marked(1)));
	const AcquiredSnapshot first = ring.Acquire(0);
	GYRO_REQUIRE(first.IsNewer());
	const void* const firstAddress = first.Bytes.data();

	// Two more frames, so that the first snapshot falls below the watermark and comes back to the pool.
	static_cast<void>(frame.Advance());
	GYRO_REQUIRE(outbox.Publish(Marked(2)));
	static_cast<void>(frame.Advance());
	CollectAll(outbox);
	GYRO_REQUIRE_EQ(outbox.Pooled(), std::size_t{ 1 });

	GYRO_REQUIRE(outbox.Publish(Marked(3)));
	GYRO_CHECK_EQ(outbox.Pooled(), std::size_t{ 0 });

	const AcquiredSnapshot third = ring.Acquire(2);
	GYRO_REQUIRE(third.IsNewer());
	GYRO_CHECK(third.Bytes.data() == firstAddress);
	GYRO_CHECK_EQ(MarkOf(SnapshotReader{ third.Bytes }), std::uint64_t{ 3 });
}

GYRO_TEST(SnapshotOutbox, AFullRingDefersTheSnapshotRatherThanDroppingIt)
{
	// A frame thread that has not acquired for a whole ring's worth of publishes. The snapshot that
	// finds no slot is kept, because the one case where dropping is not harmless is the one where it is
	// the last publish before the scene quiesces and nothing ever republishes it.
	SnapshotRing ring;
	ReturnChannel reports;
	SnapshotOutbox outbox{ ring, reports };
	FrameSide frame{ ring, reports };

	for (std::uint64_t index = 0; index < SnapshotRingDepth; ++index)
	{
		GYRO_REQUIRE(outbox.Publish(Marked(index)));
	}

	GYRO_CHECK(!outbox.Publish(Marked(99)));
	GYRO_CHECK(outbox.HasPending());
	GYRO_CHECK_EQ(outbox.Retained(), std::size_t{ SnapshotRingDepth });

	// The frame thread catches up, and the deferred snapshot goes out on the next flush with the
	// sequence it was refused with — nothing skipped, nothing renumbered.
	static_cast<void>(frame.Advance());
	static_cast<void>(frame.Advance());
	CollectAll(outbox);

	GYRO_REQUIRE(outbox.Flush());
	GYRO_CHECK(!outbox.HasPending());

	const SnapshotReader reader = frame.Advance();
	GYRO_REQUIRE(reader.IsValid());
	GYRO_CHECK_EQ(reader.Sequence(), SnapshotRingDepth + 1);
	GYRO_CHECK_EQ(MarkOf(reader), std::uint64_t{ 99 });
}

GYRO_TEST(SnapshotOutbox, ANewerSnapshotSupersedesAPendingOne)
{
	// Queueing behind a deferred publish would deliver a scene the world has already moved past. The
	// pending buffer is refilled instead, and the sequence is unchanged because a refused one was never
	// consumed.
	SnapshotRing ring;
	ReturnChannel reports;
	SnapshotOutbox outbox{ ring, reports };
	FrameSide frame{ ring, reports };

	for (std::uint64_t index = 0; index < SnapshotRingDepth; ++index)
	{
		GYRO_REQUIRE(outbox.Publish(Marked(index)));
	}

	GYRO_REQUIRE(!outbox.Publish(Marked(50)));
	GYRO_REQUIRE(!outbox.Publish(Marked(51)));
	GYRO_CHECK(outbox.HasPending());

	static_cast<void>(frame.Advance());
	static_cast<void>(frame.Advance());
	CollectAll(outbox);
	GYRO_REQUIRE(outbox.Flush());

	const SnapshotReader reader = frame.Advance();
	GYRO_REQUIRE(reader.IsValid());
	GYRO_CHECK_EQ(reader.Sequence(), SnapshotRingDepth + 1);
	GYRO_CHECK_EQ(MarkOf(reader), std::uint64_t{ 51 });
}

GYRO_TEST(SnapshotOutbox, APendingSnapshotSurvivesBeingRefusedAgain)
{
	// A regression, and one the two-thread soak found before any single-threaded test did, because it
	// needs the ring to be full on two consecutive attempts. A shape that moved the pending buffer out
	// to offer it and moved it back on refusal was a self-move the second time around, which empties the
	// storage while leaving the byte size beside it intact — so the next successful publish handed the
	// frame thread a correctly-sized span over a freed pointer. Refusal must leave the snapshot exactly
	// where it already is.
	SnapshotRing ring;
	ReturnChannel reports;
	SnapshotOutbox outbox{ ring, reports };
	FrameSide frame{ ring, reports };

	for (std::uint64_t index = 0; index < SnapshotRingDepth; ++index)
	{
		GYRO_REQUIRE(outbox.Publish(Marked(index)));
	}

	GYRO_REQUIRE(!outbox.Publish(Marked(77)));
	GYRO_CHECK(!outbox.Flush());
	GYRO_CHECK(!outbox.Flush());
	GYRO_CHECK(outbox.HasPending());

	static_cast<void>(frame.Advance());
	static_cast<void>(frame.Advance());
	CollectAll(outbox);
	GYRO_REQUIRE(outbox.Flush());

	const SnapshotReader reader = frame.Advance();
	GYRO_REQUIRE(reader.IsValid());
	GYRO_CHECK_EQ(reader.Sequence(), SnapshotRingDepth + 1);
	GYRO_CHECK_EQ(MarkOf(reader), std::uint64_t{ 77 });
}

GYRO_TEST(SnapshotOutbox, AWatermarkThatWentBackwardsReclaimsNothing)
{
	// The frame side's watermark is monotone by construction, so this is defence rather than a case that
	// arises — but the failure it would cause is a buffer handed back while it is being read, which is
	// the one failure in this module worth being impossible rather than merely absent.
	SnapshotRing ring;
	ReturnChannel reports;
	SnapshotOutbox outbox{ ring, reports };

	GYRO_REQUIRE(outbox.Publish(Marked(1)));
	GYRO_REQUIRE(outbox.Publish(Marked(2)));
	GYRO_REQUIRE(outbox.Publish(Marked(3)));

	GYRO_REQUIRE_EQ(reports.Post(3), std::size_t{ 0 });
	GYRO_REQUIRE_EQ(reports.Post(1), std::size_t{ 0 });

	FrameReport report;
	while (outbox.Collect(report))
	{
	}

	GYRO_CHECK_EQ(outbox.Watermark(), std::uint64_t{ 3 });
	GYRO_CHECK_EQ(outbox.Retained(), std::size_t{ 1 });
	GYRO_CHECK_EQ(outbox.Pooled(), std::size_t{ 2 });
}

GYRO_TEST(SnapshotOutbox, TheRetainedSetStaysInsideTheBoundItWasSizedFor)
{
	// The constructor reserves the retained set and the pool to the ring's bounds, so that Flush's
	// push_back never has to grow — a growth that threw *after* the ring had been told about the bytes
	// would unwind through the buffer it had just taken and free it under the frame thread. Flush asks
	// for the capacity before it publishes, so the safety does not rest on this bound; what rests on it
	// is the promise that a running session never reaches the allocator for either set. This is the
	// worst case that bound is claimed to cover: the frame thread lagging a whole ring, then catching up
	// one frame at a time, with a deferred snapshot in hand throughout.
	SnapshotRing ring;
	ReturnChannel reports;
	SnapshotOutbox outbox{ ring, reports };
	FrameSide frame{ ring, reports };

	for (std::uint64_t iteration = 0; iteration < 64; ++iteration)
	{
		static_cast<void>(outbox.Publish(Marked(iteration)));

		if (iteration % SnapshotRingDepth == 0)
		{
			static_cast<void>(frame.Advance());
			CollectAll(outbox);
		}

		GYRO_REQUIRE(outbox.Retained() <= SnapshotRingDepth);
		GYRO_REQUIRE(outbox.Retained() + outbox.Pooled() + (outbox.HasPending() ? 1u : 0u) <= SnapshotRingDepth + 1);
	}
}

GYRO_TEST(SnapshotOutbox, SteadyStateNeitherGrowsNorReachesTheAllocator)
{
	// The shape of a session: a commit, a frame, a report, forever. Nothing accumulates, the deferral
	// path is never reached, and after the first few frames every snapshot is built into storage that
	// has already been used and released.
	SnapshotRing ring;
	ReturnChannel reports;
	SnapshotOutbox outbox{ ring, reports };
	FrameSide frame{ ring, reports };

	for (std::uint64_t iteration = 0; iteration < 64; ++iteration)
	{
		CollectAll(outbox);
		GYRO_REQUIRE(outbox.Flush());
		GYRO_REQUIRE(outbox.Publish(Marked(iteration)));

		const SnapshotReader reader = frame.Advance();
		GYRO_REQUIRE(reader.IsValid());
		GYRO_CHECK_EQ(MarkOf(reader), iteration);

		GYRO_REQUIRE(outbox.Retained() + outbox.Pooled() <= SnapshotRingDepth);
	}

	GYRO_CHECK(!outbox.HasPending());
	GYRO_CHECK_EQ(frame.Held(), std::uint64_t{ 64 });
}
