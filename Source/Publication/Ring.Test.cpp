#include "Publication/Ring.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "Testing/Test.h"

// The ring's protocol, driven from one thread.
//
// Everything this file asserts is a property of the *protocol* — which sequence lands in which slot,
// when a slot may be reused, what a full ring does — and none of it needs two threads to be wrong.
// The concurrent half is proven where it can actually be observed: Source/Integration holds an
// iteration-bounded soak that runs the whole crossing on two threads under ThreadSanitizer, which is
// the only instrument that can see a missing release. Splitting them this way keeps the contract
// asserted deterministically and leaves the soak to prove the memory ordering rather than the logic.

namespace
{
// Distinguishable byte ranges, so an acquire that returned the wrong slot is visible as the wrong
// address rather than as equal-looking bytes.
struct Payloads
{
	std::array<std::array<std::byte, 8>, 8> Store{};

	[[nodiscard]] std::span<const std::byte> operator[](std::size_t which) const noexcept
	{
		return { Store[which].data(), Store[which].size() };
	}
};

constexpr std::uint64_t kNothingConsumed = 0;
} // namespace

GYRO_TEST(SnapshotRing, EmptyRingOffersNothing)
{
	const SnapshotRing ring;

	GYRO_CHECK(!ring.Acquire(kNothingConsumed).IsNewer());
	GYRO_CHECK_EQ(ring.Published(), std::uint64_t{ 0 });
	GYRO_CHECK_EQ(ring.NextSequence(), std::uint64_t{ 1 });
}

GYRO_TEST(SnapshotRing, SequencesBeginAtOneAndAreContiguous)
{
	// Zero has to mean "nothing", both in Published() and in an acquisition, so the first snapshot
	// cannot be allowed to carry it.
	SnapshotRing ring;
	const Payloads payloads;

	GYRO_CHECK(ring.Publish(payloads[0], kNothingConsumed));
	GYRO_CHECK_EQ(ring.Published(), std::uint64_t{ 1 });
	GYRO_CHECK(ring.Publish(payloads[1], kNothingConsumed));
	GYRO_CHECK_EQ(ring.Published(), std::uint64_t{ 2 });
	GYRO_CHECK_EQ(ring.NextSequence(), std::uint64_t{ 3 });
}

GYRO_TEST(SnapshotRing, AcquireTakesTheNewestAndSkipsTheRest)
{
	// The property the whole channel is built on: a snapshot is complete scene state, so one nobody
	// read costs nothing and the frame thread jumps rather than walks.
	SnapshotRing ring;
	const Payloads payloads;

	GYRO_REQUIRE(ring.Publish(payloads[0], kNothingConsumed));
	GYRO_REQUIRE(ring.Publish(payloads[1], kNothingConsumed));
	GYRO_REQUIRE(ring.Publish(payloads[2], kNothingConsumed));

	const AcquiredSnapshot acquired = ring.Acquire(kNothingConsumed);

	GYRO_REQUIRE(acquired.IsNewer());
	GYRO_CHECK_EQ(acquired.Sequence, std::uint64_t{ 3 });
	GYRO_CHECK(acquired.Bytes.data() == payloads[2].data());
}

GYRO_TEST(SnapshotRing, NothingNewerIsAnEmptyAcquisition)
{
	// The ordinary case for an output waking on a timer with no commit in between: the frame thread
	// keeps rendering from what it already holds rather than being handed the same bytes again.
	SnapshotRing ring;
	const Payloads payloads;

	GYRO_REQUIRE(ring.Publish(payloads[0], kNothingConsumed));

	const AcquiredSnapshot first = ring.Acquire(kNothingConsumed);
	GYRO_REQUIRE(first.IsNewer());

	GYRO_CHECK(!ring.Acquire(first.Sequence).IsNewer());
}

GYRO_TEST(SnapshotRing, AFullRingRefusesRatherThanOverwriting)
{
	// With nothing consumed, every slot holds a snapshot the frame thread may still reach for, so the
	// depth-plus-one publish has nowhere to go. Refusing is what keeps the reader's single indexed
	// read sound without a re-check.
	SnapshotRing ring;
	const Payloads payloads;

	for (std::uint64_t index = 0; index < SnapshotRingDepth; ++index)
	{
		GYRO_CHECK(ring.Publish(payloads[index], kNothingConsumed));
	}

	GYRO_CHECK(!ring.Publish(payloads[SnapshotRingDepth], kNothingConsumed));
}

GYRO_TEST(SnapshotRing, ARefusedPublishDoesNotConsumeItsSequence)
{
	// The dispatch side stamps the sequence into the snapshot header before offering the bytes, so a
	// refusal that burned the number would leave a gap the watermark could never close.
	SnapshotRing ring;
	const Payloads payloads;

	for (std::uint64_t index = 0; index < SnapshotRingDepth; ++index)
	{
		GYRO_REQUIRE(ring.Publish(payloads[index], kNothingConsumed));
	}

	const std::uint64_t pending = ring.NextSequence();
	GYRO_REQUIRE(!ring.Publish(payloads[SnapshotRingDepth], kNothingConsumed));

	GYRO_CHECK_EQ(ring.NextSequence(), pending);
	GYRO_CHECK_EQ(ring.Published(), SnapshotRingDepth);
}

GYRO_TEST(SnapshotRing, TheWatermarkFreesWhatIsStrictlyBelowIt)
{
	// The frame thread reports the sequence it is *rendering from*, so that one is still live and only
	// what precedes it may be reused. An off-by-one here hands the writer the buffer under the reader.
	SnapshotRing ring;
	const Payloads payloads;

	for (std::uint64_t index = 0; index < SnapshotRingDepth; ++index)
	{
		GYRO_REQUIRE(ring.Publish(payloads[index], kNothingConsumed));
	}

	// The next publish reuses the slot holding sequence one. A frame thread that is holding sequence
	// one has not finished with it.
	GYRO_CHECK(!ring.Publish(payloads[SnapshotRingDepth], 1));

	// Once it has moved on to sequence two, sequence one's slot is the dispatch side's again.
	GYRO_CHECK(ring.Publish(payloads[SnapshotRingDepth], 2));
}

GYRO_TEST(SnapshotRing, ADrainedRingKeepsAcceptingForever)
{
	// A frame thread that acquires every publish never reaches the deferral path, however long the
	// session runs — the sequence grows without bound while the slots cycle.
	SnapshotRing ring;
	const Payloads payloads;

	std::uint64_t held = 0;

	for (std::uint64_t iteration = 0; iteration < 4 * SnapshotRingDepth; ++iteration)
	{
		GYRO_REQUIRE(ring.Publish(payloads[iteration % payloads.Store.size()], held));

		const AcquiredSnapshot acquired = ring.Acquire(held);
		GYRO_REQUIRE(acquired.IsNewer());
		held = acquired.Sequence;
	}

	GYRO_CHECK_EQ(held, 4 * SnapshotRingDepth);
}
