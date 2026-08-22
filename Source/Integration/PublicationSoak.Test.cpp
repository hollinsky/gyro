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
//
// **There are two runs here, and the second exists because the first cannot promise what it walks.**
// Two threads spinning flat out against each other reach the interesting branch — a ring with no free
// slot — only when the scheduler happens to put them there, and a sanitiser that never executed a path
// has nothing to say about it. That branch is the one that hid the self-move described in
// Publication/Publisher/Outbox.h, so "probably exercised" is not good enough for it. The gated run
// therefore holds the frame thread still on purpose, by a handshake rather than by a sleep, and then
// asserts that the full ring, the second refusal, the supersede, and the repeated watermark all
// actually happened — on every run, on every machine, at whatever speed the two sides happen to go.

namespace
{
// Four thousand crossings is a few seconds of a real session and a few milliseconds here. It is a
// great deal of interleaving, but interleaving is all it is: nothing here makes any particular one
// happen, which is what the gated run is for.
constexpr std::uint64_t kSnapshots = 4096;

// The gated run trades length for certainty — every stall is a rendezvous, so its crossings cost more
// than the flat-out run's and it needs far fewer of them to have proved what it proves.
constexpr std::uint64_t kGatedSnapshots = 512;

// How many snapshots go out between stalls. Comfortably more than a stall episode publishes, so the
// two never overlap and the run is a sequence of flat-out stretches punctuated by held-back ones.
constexpr std::uint64_t kStallEvery = 16;

// Generous past any real scheduling delay. Reaching it means the two sides have stopped making
// progress against each other, which is a failure to report rather than a run to wait out.
constexpr std::uint64_t kSpinLimit = 50'000'000;

// One instant, off the origin, so the springs and the ramp are all genuinely evaluated rather than
// answering with their initial conditions.
constexpr Instant kProbe = Monotonic::FromNanoseconds(40'000'000);

// Which of the serialisations under one sequence a snapshot is.
//
// A publish the ring refuses does not consume its sequence, so the next attempt carries the same
// number with different contents — that is the supersede path in Publication/Publisher/Outbox.h. Only
// the last of them can ever reach the ring, because the ones before it were never published at all, so
// the frame thread reading a revision it was not offered is a defect this marker makes visible.
enum class Revision : std::uint8_t
{
	First,
	Superseding,
};

[[nodiscard]] constexpr float RateOf(Revision revision) noexcept
{
	return revision == Revision::First ? 0.5f : 1.5f;
}

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

[[nodiscard]] Ramp RampFor(std::uint64_t sequence, Revision revision) noexcept
{
	return {
		.Origin = Monotonic::FromNanoseconds(0),
		.Horizon = Duration{ 125'000'000 },
		.Progress = static_cast<float>(sequence),
		.Rate = RateOf(revision),
	};
}

[[nodiscard]] SnapshotPublisher Serialised(std::uint64_t sequence, Revision revision)
{
	const Spring<double> spring = SpringFor(sequence);
	const Ramp ramp = RampFor(sequence, revision);

	SnapshotPublisher publisher;
	publisher.Put<Spring<double>>(SnapshotRun::Translation, { &spring, 1 });
	publisher.Put<Ramp>(SnapshotRun::DrivenProgress, { &ramp, 1 });

	return publisher;
}

// The handshake that makes the deferral path reachable on purpose rather than by luck.
//
// Dispatch closes the gate; the frame thread stops acquiring and keeps re-posting the watermark it
// already holds, which is exactly what an output waking on its own clock with no commit behind it
// does. Once it has done that twice it is provably parked, so the watermark is provably frozen and the
// next few publishes provably fill the ring. Nothing here is a sleep and nothing here is a wall clock:
// each side waits on a flag the other side sets, and the spin guard turns a handshake that broke into
// a failure rather than a hung test run.
//
// Finish is how the run ends. Dispatch knows both how many snapshots it published and how far the
// watermark has come back, so it can stop the frame thread at the one moment those agree and nothing
// is left in flight — which a count on the frame side could not do, because a frame thread is allowed
// to skip and the two numbers would not have to meet.
struct FrameGate
{
	std::atomic<bool> Closed{ false };
	std::atomic<bool> Parked{ false };
	std::atomic<bool> Finish{ false };
};

// What the frame thread comes back with. Counters rather than assertions, because a GYRO_CHECK formats
// its operands and that allocates, which is precisely what the frame section forbids.
struct FrameOutcome
{
	std::uint64_t Acquisitions = 0;
	std::uint64_t Held = 0;
	std::uint64_t Invalid = 0;
	std::uint64_t Disagreed = 0; // contents did not match the sequence they arrived under
	std::uint64_t WentBackwards = 0;
	std::uint64_t Reports = 0;
	std::uint64_t RepeatedReports = 0; // posted a watermark that had already been posted
	std::uint64_t Superseded = 0;      // read a serialisation that had replaced a refused one
	std::uint64_t LastReported = 0;    // bookkeeping for the counter above rather than an outcome
	double Sink = 0.0;                 // so the evaluation cannot be optimised away
};

// One frame's report, posted whether or not a new snapshot arrived.
//
// A frame happens on the output's clock rather than on dispatch's, so re-posting an unchanged
// watermark is the ordinary case and not an edge one — it is what the single-threaded double in
// Publication/Publisher/Outbox.Test.cpp does on every call, and a frame thread that reported only when
// it had something new would leave the return channel's merge unreachable from here.
void Report(ReturnChannel& reports, FrameOutcome& outcome) noexcept
{
	const FrameSection guard;

	if (outcome.Reports != 0 && outcome.Held == outcome.LastReported)
	{
		++outcome.RepeatedReports;
	}

	outcome.LastReported = outcome.Held;
	++outcome.Reports;

	static_cast<void>(reports.Post(outcome.Held));
}

void RunFrameSide(
	SnapshotRing& ring,
	ReturnChannel& reports,
	FrameGate& gate,
	std::atomic<bool>& abandoned,
	FrameOutcome& outcome
)
{
	std::span<const std::byte> bytes;
	std::uint64_t spins = 0;
	std::uint64_t parkedReports = 0;

	const auto stalled = [&spins, &abandoned] {
		if (++spins > kSpinLimit)
		{
			abandoned.store(true, std::memory_order_relaxed);
			return false;
		}

		std::this_thread::yield();
		return true;
	};

	while (!gate.Finish.load(std::memory_order_acquire) && !abandoned.load(std::memory_order_relaxed))
	{
		if (gate.Closed.load(std::memory_order_acquire))
		{
			// Held back on purpose, and still reporting: the sequence being rendered from has not
			// changed, so neither has the watermark. Two of those go out before the flag does, so that
			// dispatch's "it is parked" is backed by a repeated watermark that has actually crossed
			// rather than by a store on the way past.
			Report(reports, outcome);

			if (++parkedReports >= 2)
			{
				gate.Parked.store(true, std::memory_order_release);
			}

			if (!stalled())
			{
				break;
			}

			continue;
		}

		parkedReports = 0;

		const AcquiredSnapshot acquired = ring.Acquire(outcome.Held);

		if (!acquired.IsNewer())
		{
			// Nothing newer than what is already held, which is what an output waking on a timer with
			// no commit in between sees. The report still goes out, unchanged.
			Report(reports, outcome);

			if (!stalled())
			{
				break;
			}

			continue;
		}

		spins = 0;

		// Everything from here to the report is what the frame thread really does, and none of it may
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
			const std::span<const Spring<double>> springs = reader.Run<Spring<double>>(SnapshotRun::Translation);
			const std::span<const Ramp> ramps = reader.Run<Ramp>(SnapshotRun::DrivenProgress);

			const bool agrees =
				springs.size() == 1 && ramps.size() == 1 && reader.Sequence() == acquired.Sequence &&
				springs[0].Target == static_cast<double>(acquired.Sequence) &&
				ramps[0].Progress == static_cast<float>(acquired.Sequence) &&
				(ramps[0].Rate == RateOf(Revision::First) || ramps[0].Rate == RateOf(Revision::Superseding));

			if (!agrees)
			{
				++outcome.Disagreed;
			}
			else
			{
				if (ramps[0].Rate == RateOf(Revision::Superseding))
				{
					++outcome.Superseded;
				}

				outcome.Sink += springs[0].Evaluate(kProbe).Position;
				outcome.Sink += static_cast<double>(ramps[0].Evaluate(kProbe).Position);
			}
		}

		// The watermark is posted after the acquire and names what is being rendered from, which is
		// what authorises the dispatch side to reclaim everything strictly below it.
		Report(reports, outcome);
	}
}

// The dispatch loop's collection step, drained to empty as the real one will be.
void CollectAll(SnapshotOutbox& outbox)
{
	FrameReport report;
	while (outbox.Collect(report))
	{
	}
}

// Spin on the dispatch side until something the frame thread is responsible for comes true, draining
// reports throughout so that a full return channel can never be what the wait is really about. False
// means the two sides stopped making progress against each other, which is reported as a failure once
// the frame thread has been joined rather than asserted here — a GYRO_REQUIRE returns from the test
// function, and returning with a thread still running ends the process rather than the test.
template<typename Ready>
[[nodiscard]] bool SpinUntil(SnapshotOutbox& outbox, std::atomic<bool>& abandoned, Ready ready)
{
	for (std::uint64_t spins = 0; !ready(); ++spins)
	{
		if (spins > kSpinLimit || abandoned.load(std::memory_order_relaxed))
		{
			abandoned.store(true, std::memory_order_relaxed);
			return false;
		}

		CollectAll(outbox);
		std::this_thread::yield();
	}

	return true;
}

// Let the frame thread finish, once every snapshot published has been reported back. Both flags are
// set unconditionally, including on the way out of a run that gave up: a frame thread left parked
// behind a closed gate would be joined by a thread waiting for it to move.
void Release(FrameGate& gate, std::thread& frame)
{
	gate.Closed.store(false, std::memory_order_release);
	gate.Finish.store(true, std::memory_order_release);
	frame.join();
}
} // namespace

GYRO_TEST(PublicationSoak, TheCrossingSurvivesBothThreadsRunningFlatOut)
{
	SnapshotRing ring;
	ReturnChannel reports;
	SnapshotOutbox outbox{ ring, reports };

	FrameGate gate;
	std::atomic<bool> abandoned{ false };
	FrameOutcome outcome;

	std::thread frame{ [&] { RunFrameSide(ring, reports, gate, abandoned, outcome); } };

	for (std::uint64_t sequence = 1; sequence <= kSnapshots && !abandoned.load(std::memory_order_relaxed); ++sequence)
	{
		CollectAll(outbox);

		if (!outbox.Publish(Serialised(sequence, Revision::First)))
		{
			// The ring is full because the frame thread has not caught up. Dispatch retries the
			// snapshot it already built rather than dropping it or rebuilding it, and never blocks.
			// Whether this is reached at all is the scheduler's business, which is why nothing below
			// asserts that it was; the gated test is where that branch is walked on purpose.
			if (!SpinUntil(outbox, abandoned, [&] { return outbox.Flush(); }))
			{
				break;
			}
		}
	}

	const bool drained = SpinUntil(outbox, abandoned, [&] { return outbox.Watermark() == ring.Published(); });

	Release(gate, frame);
	CollectAll(outbox);

	GYRO_REQUIRE(!abandoned.load(std::memory_order_relaxed));
	GYRO_REQUIRE(drained);

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

	// A report per iteration rather than per acquisition, so there are always at least as many as there
	// were snapshots to acquire.
	GYRO_CHECK(outcome.Reports >= outcome.Acquisitions);

	// Everything is back on the dispatch side, and nothing accumulated. The retained set is what the
	// frame thread still names — its last snapshot — and everything else is in the pool.
	GYRO_CHECK(!outbox.HasPending());
	GYRO_CHECK_EQ(outbox.Watermark(), kSnapshots);
	GYRO_CHECK_EQ(outbox.Retained(), std::size_t{ 1 });
	GYRO_CHECK(outbox.Retained() + outbox.Pooled() <= SnapshotRingDepth + 1);
}

GYRO_TEST(PublicationSoak, TheDeferralPathIsWalkedWithTheFrameThreadHeldBack)
{
	// What the run above cannot promise, promised. A ring with no free slot is reached only when the
	// frame thread is a whole ring behind, and two threads going flat out are there when the scheduler
	// puts them there and not otherwise — so on the machine where they happen to keep pace, a green
	// sanitiser run means the branch was clean or means it never ran, and the report does not say
	// which. Here dispatch parks the frame thread first and the branch is a certainty: the ring fills,
	// the pending snapshot is refused a second time with the watermark provably unmoved, a newer
	// serialisation supersedes it in place while the frame thread is running, and the frame thread then
	// reads that snapshot and checks it. Every one of those is asserted below.
	SnapshotRing ring;
	ReturnChannel reports;
	SnapshotOutbox outbox{ ring, reports };

	FrameGate gate;
	std::atomic<bool> abandoned{ false };
	FrameOutcome outcome;

	std::thread frame{ [&] { RunFrameSide(ring, reports, gate, abandoned, outcome); } };

	std::uint64_t stalls = 0;
	std::uint64_t refusals = 0;
	std::uint64_t secondRefusals = 0;
	std::uint64_t supersedes = 0;
	std::uint64_t nextStall = kStallEvery;

	while (ring.Published() < kGatedSnapshots && !abandoned.load(std::memory_order_relaxed))
	{
		CollectAll(outbox);

		if (ring.Published() < nextStall)
		{
			// Between stalls, the same loop as the flat-out run: publish, and retry rather than drop if
			// the two sides happen to reach a full ring on their own.
			if (!outbox.Publish(Serialised(ring.NextSequence(), Revision::First)))
			{
				++refusals;

				if (!SpinUntil(outbox, abandoned, [&] { return outbox.Flush(); }))
				{
					break;
				}
			}

			continue;
		}

		nextStall = ring.Published() + kStallEvery;

		// Close the gate and wait until the frame thread says it has stopped acquiring. From here until
		// the gate opens again the watermark cannot move, and that is what makes every step below a
		// certainty rather than a likelihood. The flag is cleared before the gate closes, so what is
		// waited on is a park from this stall rather than one left over from the last.
		gate.Parked.store(false, std::memory_order_relaxed);
		gate.Closed.store(true, std::memory_order_release);

		if (!SpinUntil(outbox, abandoned, [&] { return gate.Parked.load(std::memory_order_acquire); }))
		{
			break;
		}

		++stalls;

		// Publish into a ring nobody is draining. A slot is free only once the frame thread has moved
		// past its last occupant, and it cannot, so a refusal arrives within the ring's depth.
		bool refused = false;

		for (std::uint64_t attempt = 0; attempt < SnapshotRingDepth; ++attempt)
		{
			if (!outbox.Publish(Serialised(ring.NextSequence(), Revision::First)))
			{
				refused = true;
				break;
			}
		}

		if (!refused)
		{
			abandoned.store(true, std::memory_order_relaxed);
			break;
		}

		++refusals;

		// Refused again, with nothing collected in between, so the watermark this attempt sees is the
		// one the first refusal saw and the answer cannot be anything else. This is the pair that the
		// self-move needed to show itself: the second refusal must leave the pending snapshot exactly
		// where it already is, and not a self-move's worth of empty under an intact byte count.
		if (!outbox.Flush())
		{
			++secondRefusals;
		}

		// And superseded rather than queued behind, under the sequence the ring never consumed. This is
		// the build that happens into a buffer the dispatch side still owns while the frame thread is
		// running, which is the part only a two-thread test can say anything about.
		if (!outbox.Publish(Serialised(ring.NextSequence(), Revision::Superseding)))
		{
			++supersedes;
		}

		gate.Closed.store(false, std::memory_order_release);

		if (!SpinUntil(outbox, abandoned, [&] { return outbox.Flush(); }))
		{
			break;
		}

		// The deferred snapshot is out. Nothing newer is published until the watermark reaches it, so
		// the next thing the frame thread acquires is provably that snapshot — the one built in place
		// under a running reader — and its contents are checked there rather than inferred here.
		const std::uint64_t deferred = ring.Published();

		if (!SpinUntil(outbox, abandoned, [&] { return outbox.Watermark() >= deferred; }))
		{
			break;
		}
	}

	const bool drained = SpinUntil(outbox, abandoned, [&] { return outbox.Watermark() == ring.Published(); });

	Release(gate, frame);
	CollectAll(outbox);

	GYRO_REQUIRE(!abandoned.load(std::memory_order_relaxed));
	GYRO_REQUIRE(drained);

	GYRO_CHECK(ring.Published() >= kGatedSnapshots);
	GYRO_CHECK_EQ(outcome.Held, ring.Published());

	// The same content check as the flat-out run, and it carries more here: a supersede rebuilds a
	// buffer the dispatch side owns while the frame thread is reading the ring around it.
	GYRO_CHECK_EQ(outcome.Disagreed, std::uint64_t{ 0 });
	GYRO_CHECK_EQ(outcome.Invalid, std::uint64_t{ 0 });
	GYRO_CHECK_EQ(outcome.WentBackwards, std::uint64_t{ 0 });
	GYRO_CHECK(outcome.Sink != 0.0);

	// The point of the whole test. Each of these counts a refusal the outbox actually returned, under a
	// frame thread that was provably parked at the time — so a run that reports them is a run in which
	// ThreadSanitizer walked the full-ring path, and the deferral, the second refusal and the supersede
	// were each reached once per stall rather than once per lucky schedule.
	GYRO_CHECK(stalls > 0);
	GYRO_CHECK(refusals >= stalls);
	GYRO_CHECK_EQ(secondRefusals, stalls);
	GYRO_CHECK_EQ(supersedes, stalls);

	// And the superseding serialisation is what came back, once per stall: a snapshot carrying the
	// revision that replaced a refused one, read by the frame thread and cross-checked against the
	// sequence it arrived under.
	GYRO_CHECK(outcome.Superseded >= stalls);

	// A parked frame thread reports the sequence it already holds, over and over. That is the repeated
	// watermark, and it is a path the forward channel cannot produce on its own — Collect must take a
	// report that advances nothing, and the return channel must merge rather than grow.
	GYRO_CHECK(outcome.RepeatedReports >= stalls);

	// Nothing accumulated, and the bound held across every deferral.
	GYRO_CHECK(!outbox.HasPending());
	GYRO_CHECK_EQ(outbox.Watermark(), ring.Published());
	GYRO_CHECK_EQ(outbox.Retained(), std::size_t{ 1 });
	GYRO_CHECK(outbox.Retained() + outbox.Pooled() <= SnapshotRingDepth + 1);
}
