#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

#include "Animation/Solve/Ramp.h"
#include "Animation/Solve/Spring.h"
#include "Core/FrameSection.h"
#include "Core/Time.h"
#include "Publication/Publisher/Outbox.h"
#include "Publication/Reader/Reader.h"
#include "Publication/Return.h"
#include "Publication/Ring.h"
#include "Publication/Snapshot.h"
#include "Testing/Test.h"

// The crossing, run on two threads at once.
//
// The protocol tests beside Publication/Ring.h and Publication/Return.h assert what the channel does;
// this asserts that the *memory ordering* under it is real, which is the one part no single-threaded
// test can see. It is the intended home of `GYRO_SANITIZE=thread`:
//
//     cmake -S . -B build-tsan -G Ninja -D GYRO_SANITIZE=thread && ninja -C build-tsan &&
//         ctest --test-dir build-tsan -R Integration
//
// A missing release on the publish, or a slot recycled while the reader still names it, is a data race
// that ThreadSanitizer reports and that an ordinary run will pass through for months.
//
// **Bounded by iteration count rather than by wall clock**, so it takes as long as the machine takes
// and never flakes on a loaded one. The livelock guard below is the same idea: it gives up and fails
// rather than hanging a test run, and its limit is far above any real interleaving.
//
// **The content check is the half that does not need a sanitiser.** Every snapshot encodes its own
// sequence in its coefficients, so a buffer recycled under the reader shows up as a snapshot whose
// contents disagree with the sequence it was acquired under — deferred reclamation failing, observed
// directly, on a build with no instrumentation at all.

namespace
{
// Four thousand crossings is a few seconds of a real session and a few milliseconds here, and it is
// enough interleaving for ThreadSanitizer to have seen every path through both channels.
constexpr std::uint64_t kSnapshots = 4096;

// Generous past any real scheduling delay. Reaching it means the two sides have stopped making
// progress against each other, which is a failure to report rather than a run to wait out.
constexpr std::uint64_t kSpinLimit = 50'000'000;

// One instant, off the origin, so the springs and the ramp are all genuinely evaluated rather than
// answering with their initial conditions.
constexpr Instant kProbe = Monotonic::FromNanoseconds(40'000'000);

// A snapshot that says which snapshot it is, in both runs, so the frame side can cross-check its
// contents against the sequence the ring handed it.
[[nodiscard]] Spring<double> SpringFor(std::uint64_t sequence) noexcept
{
	return {
		.Origin = Monotonic::FromNanoseconds(0),
		.Parameters = { 12.0, 0.9 },
		.Target = static_cast<double>(sequence),
		.Offset = -4.0,
		.Velocity = 1.0,
	};
}

[[nodiscard]] Ramp RampFor(std::uint64_t sequence) noexcept
{
	return {
		.Origin = Monotonic::FromNanoseconds(0),
		.Horizon = Duration{ 125'000'000 },
		.Progress = static_cast<float>(sequence),
		.Rate = 0.5f,
	};
}

// What the frame thread comes back with. Counters rather than assertions, because a GYRO_CHECK formats
// its operands and that allocates, which is precisely what the frame section forbids.
struct FrameOutcome
{
	std::uint64_t Acquisitions = 0;
	std::uint64_t Held = 0;
	std::uint64_t Invalid = 0;
	std::uint64_t Disagreed = 0; // contents did not match the sequence they arrived under
	std::uint64_t WentBackwards = 0;
	double Sink = 0.0; // so the evaluation cannot be optimised away
};

void RunFrameSide(SnapshotRing& ring, ReturnChannel& reports, std::atomic<bool>& abandoned, FrameOutcome& outcome)
{
	std::span<const std::byte> bytes;
	std::uint64_t spins = 0;

	while (outcome.Held < kSnapshots && !abandoned.load(std::memory_order_relaxed))
	{
		const AcquiredSnapshot acquired = ring.Acquire(outcome.Held);

		if (!acquired.IsNewer())
		{
			if (++spins > kSpinLimit)
			{
				abandoned.store(true, std::memory_order_relaxed);
				break;
			}

			std::this_thread::yield();
			continue;
		}

		spins = 0;

		// Everything from here to the post is what the frame thread really does, and none of it may
		// allocate — the debug allocator of decision 36 aborts if it does.
		const FrameSection guard;

		if (acquired.Sequence < outcome.Held)
		{
			++outcome.WentBackwards;
		}

		outcome.Held = acquired.Sequence;
		bytes = acquired.Bytes;
		++outcome.Acquisitions;

		const SnapshotReader reader{ bytes };

		if (!reader.IsValid())
		{
			++outcome.Invalid;
		}
		else
		{
			const std::span<const Spring<double>> springs = reader.Run<Spring<double>>(SnapshotRun::Positions);
			const std::span<const Ramp> ramps = reader.Run<Ramp>(SnapshotRun::DrivenProgress);

			if (springs.size() != 1 || ramps.size() != 1 || reader.Sequence() != acquired.Sequence ||
			    springs[0].Target != static_cast<double>(acquired.Sequence) ||
			    ramps[0].Progress != static_cast<float>(acquired.Sequence))
			{
				++outcome.Disagreed;
			}
			else
			{
				outcome.Sink += springs[0].Evaluate(kProbe).Position;
				outcome.Sink += static_cast<double>(ramps[0].Evaluate(kProbe).Position);
			}
		}

		// The watermark is posted after the acquire and names what is being rendered from, which is
		// what authorises the dispatch side to reclaim everything strictly below it.
		static_cast<void>(reports.Post(outcome.Held));
	}
}
} // namespace

GYRO_TEST(PublicationSoak, TheCrossingSurvivesBothThreadsRunningFlatOut)
{
	SnapshotRing ring;
	ReturnChannel reports;
	SnapshotOutbox outbox{ ring, reports };

	std::atomic<bool> abandoned{ false };
	FrameOutcome outcome;

	std::thread frame{ [&] { RunFrameSide(ring, reports, abandoned, outcome); } };

	std::uint64_t deferrals = 0;
	std::uint64_t spins = 0;

	const auto collect = [&outbox] {
		FrameReport report;
		while (outbox.Collect(report))
		{
		}
	};

	for (std::uint64_t sequence = 1; sequence <= kSnapshots && !abandoned.load(std::memory_order_relaxed); ++sequence)
	{
		collect();

		const Spring<double> spring = SpringFor(sequence);
		const Ramp ramp = RampFor(sequence);

		SnapshotPublisher publisher;
		publisher.Put<Spring<double>>(SnapshotRun::Positions, { &spring, 1 });
		publisher.Put<Ramp>(SnapshotRun::DrivenProgress, { &ramp, 1 });

		if (!outbox.Publish(publisher))
		{
			++deferrals;

			// The ring is full because the frame thread has not caught up. Dispatch retries the
			// snapshot it already built rather than dropping it or rebuilding it, and never blocks.
			while (!outbox.Flush())
			{
				if (++spins > kSpinLimit)
				{
					abandoned.store(true, std::memory_order_relaxed);
					break;
				}

				std::this_thread::yield();
				collect();
			}
		}
	}

	frame.join();
	collect();

	GYRO_REQUIRE(!abandoned.load(std::memory_order_relaxed));

	// Every snapshot was published, and the frame thread ended holding the last of them.
	GYRO_CHECK_EQ(ring.Published(), kSnapshots);
	GYRO_CHECK_EQ(outcome.Held, kSnapshots);

	// Nothing was ever read as the wrong snapshot. This is the assertion that catches a buffer being
	// recycled while the reader still names it, without needing a sanitiser to see it.
	GYRO_CHECK_EQ(outcome.Disagreed, std::uint64_t{ 0 });
	GYRO_CHECK_EQ(outcome.Invalid, std::uint64_t{ 0 });
	GYRO_CHECK_EQ(outcome.WentBackwards, std::uint64_t{ 0 });

	// Skipping is the design rather than an accident: the frame thread jumps to the newest and a
	// snapshot nobody read costs nothing. This is not a lower bound on how many it should have seen —
	// it is only the check that it saw some and not more than were published.
	GYRO_CHECK(outcome.Acquisitions > 0 && outcome.Acquisitions <= kSnapshots);
	GYRO_CHECK(outcome.Sink != 0.0);

	// Everything is back on the dispatch side, and nothing accumulated. The retained set is what the
	// frame thread still names — its last snapshot — and everything else is in the pool.
	GYRO_CHECK(!outbox.HasPending());
	GYRO_CHECK_EQ(outbox.Watermark(), kSnapshots);
	GYRO_CHECK_EQ(outbox.Retained(), std::size_t{ 1 });
	GYRO_CHECK(outbox.Retained() + outbox.Pooled() <= SnapshotRingDepth + 1);

	// Deferral is a real path rather than a documented one, but whether it is reached depends on how
	// the two threads happen to be scheduled, so it is reported rather than required.
	GYRO_CHECK(deferrals <= kSnapshots);
}
