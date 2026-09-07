#include "Core/Trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Core/Clock.h"
#include "Core/FrameSection.h"
#include "Testing/Test.h"

namespace
{

// A ring and its storage, since a buffer holds a span and the storage has to outlive it.
struct Ring
{
	explicit Ring(std::size_t capacity, const IClock& clock) : Records{ std::make_unique<TraceRecord[]>(capacity) }
	{
		Buffer.Arm({ Records.get(), capacity }, clock);
	}

	std::unique_ptr<TraceRecord[]> Records;
	TraceBuffer Buffer;
};

} // namespace

GYRO_TEST(Trace, AnUnarmedBufferTakesNothing)
{
	TraceBuffer buffer;

	GYRO_CHECK(!buffer.IsArmed());

	// The verbs are still callable, because a call site does not know whether the root armed anything
	// and must not have to ask.
	buffer.Emit(TraceKind::Mark, "nothing", 0, TraceThread);

	GYRO_CHECK_EQ(buffer.Written(), std::uint64_t{ 0 });
}

GYRO_TEST(Trace, CapacityMustBeAPowerOfTwo)
{
	const ManualClock clock;
	auto records = std::make_unique<TraceRecord[]>(6);

	TraceBuffer buffer;
	buffer.Arm({ records.get(), 6 }, clock);

	// Refused rather than rounded down to four: a ring that quietly held less than it was given would
	// make the coverage figure the snapshot reports a lie.
	GYRO_CHECK(!buffer.IsArmed());
}

GYRO_TEST(Trace, RecordsComeBackOldestFirst)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1'000) };
	Ring ring{ 8, clock };

	ring.Buffer.Emit(TraceKind::Begin, "step", 7, TraceThread);
	clock.Advance(std::chrono::nanoseconds{ 500 });
	ring.Buffer.Emit(TraceKind::Count, "depth", static_cast<std::uint64_t>(3), TraceOutput(2));
	clock.Advance(std::chrono::nanoseconds{ 500 });
	ring.Buffer.Emit(TraceKind::End, nullptr, 0, TraceThread);

	std::array<TraceEvent, 8> into{};
	const std::size_t count = ring.Buffer.Copy(into);

	GYRO_REQUIRE_EQ(count, std::size_t{ 3 });

	GYRO_CHECK(into[0].Kind == TraceKind::Begin);
	GYRO_CHECK_EQ(std::string_view{ into[0].Name }, std::string_view{ "step" });
	GYRO_CHECK_EQ(into[0].Payload, std::uint64_t{ 7 });
	GYRO_CHECK_EQ(into[0].Stamp, Monotonic::FromNanoseconds(1'000));

	GYRO_CHECK(into[1].Kind == TraceKind::Count);
	GYRO_CHECK_EQ(into[1].Scope, TraceOutput(2));
	GYRO_CHECK_EQ(static_cast<std::int64_t>(into[1].Payload), std::int64_t{ 3 });

	GYRO_CHECK(into[2].Kind == TraceKind::End);
	GYRO_CHECK_EQ(into[2].Stamp, Monotonic::FromNanoseconds(2'000));
}

GYRO_TEST(Trace, TheRingKeepsTheNewest)
{
	ManualClock clock{ Monotonic::FromNanoseconds(0) };
	Ring ring{ 4, clock };

	// Ten into four. What a person looking at a stutter wants is the frames around it, which are the
	// last ones — so the oldest six are the ones that go.
	for (std::uint64_t index = 0; index < 10; ++index)
	{
		clock.Set(Monotonic::FromNanoseconds(static_cast<std::int64_t>(index)));
		ring.Buffer.Emit(TraceKind::Mark, "tick", index, TraceThread);
	}

	std::array<TraceEvent, 8> into{};
	const std::size_t count = ring.Buffer.Copy(into);

	GYRO_REQUIRE_EQ(count, std::size_t{ 4 });
	GYRO_CHECK_EQ(into[0].Payload, std::uint64_t{ 6 });
	GYRO_CHECK_EQ(into[3].Payload, std::uint64_t{ 9 });
	GYRO_CHECK_EQ(ring.Buffer.Written(), std::uint64_t{ 10 });
}

GYRO_TEST(Trace, ASmallDestinationTakesTheNewest)
{
	ManualClock clock{ Monotonic::FromNanoseconds(0) };
	Ring ring{ 8, clock };

	for (std::uint64_t index = 0; index < 8; ++index)
	{
		ring.Buffer.Emit(TraceKind::Mark, "tick", index, TraceThread);
	}

	std::array<TraceEvent, 3> into{};
	const std::size_t count = ring.Buffer.Copy(into);

	GYRO_REQUIRE_EQ(count, std::size_t{ 3 });
	GYRO_CHECK_EQ(into[0].Payload, std::uint64_t{ 5 });
	GYRO_CHECK_EQ(into[2].Payload, std::uint64_t{ 7 });
}

GYRO_TEST(Trace, EmittingAllocatesNothing)
{
	// The claim the whole file rests on, made mechanically. Core/DebugAllocator.cpp aborts on a heap
	// allocation inside the guard, so a tracer that formatted, grew, or captured anything would take
	// the process down here rather than in a frame six months from now.
	const MonotonicClock clock;
	Ring ring{ 64, clock };

	EnrollTracing(&ring.Buffer);

	{
		const FrameSection guard;
		const TraceSpan iteration{ "iteration" };

		TraceCount("held", 12);
		TraceElapsed("slack", std::chrono::microseconds{ 900 }, TraceOutput(0));
		TraceMark("skipped", TraceOutput(1));
	}

	GYRO_CHECK_EQ(ring.Buffer.Written(), std::uint64_t{ 5 });

	EnrollTracing(nullptr);
}

GYRO_TEST(Trace, AnUnenrolledThreadCostsNothing)
{
	GYRO_CHECK(!IsTracing());

	TraceMark("nowhere");
	TraceCount("nowhere", 1);

	{
		const TraceSpan span{ "nowhere" };
	}

	GYRO_CHECK(!IsTracing());
}

GYRO_TEST(Trace, EnrollmentIsPerThread)
{
	const MonotonicClock clock;
	Ring frame{ 16, clock };
	Ring dispatch{ 16, clock };

	EnrollTracing(&frame.Buffer);

	std::thread other{ [&dispatch] {
		EnrollTracing(&dispatch.Buffer);
		TraceMark("authored");
	} };

	TraceMark("composited");
	other.join();

	// Two rings rather than one with a lock, which is Core/FrameSection.h's argument arriving here: the
	// dispatch thread must never be able to make the frame thread wait for it.
	GYRO_CHECK_EQ(frame.Buffer.Written(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(dispatch.Buffer.Written(), std::uint64_t{ 1 });

	EnrollTracing(nullptr);
}

GYRO_TEST(Trace, TheSnapshotRunsAgainstALiveProducer)
{
	// The property the copy exists to have: a reader that never stops the writer and never believes a
	// record the writer lapped underneath it. Under GYRO_SANITIZE=thread this is also the test that
	// says the field atomics are load-bearing rather than decorative.
	const MonotonicClock clock;
	Ring ring{ 1024, clock };

	std::atomic<bool> stop{ false };
	std::atomic<bool> producing{ false };

	std::thread producer{ [&ring, &stop, &producing] {
		EnrollTracing(&ring.Buffer);

		producing.store(true, std::memory_order_release);

		while (!stop.load(std::memory_order_relaxed))
		{
			for (std::uint64_t index = 0; index < 256; ++index)
			{
				TraceMark("tick", TraceThread, TraceTag(index));
			}
		}
	} };

	// The reader must not start before the writer has, or the race this test is about never happens:
	// two hundred copies of an empty ring take less time than std::thread takes to reach the lambda,
	// and the run ends having proved nothing.
	while (!producing.load(std::memory_order_acquire))
	{
	}

	std::vector<TraceEvent> into(1024);
	std::size_t seen = 0;

	// Rounds until something has come back rather than a fixed count, because the number of copies it
	// takes to catch a record is a scheduling outcome. Bounded, so a ring that genuinely never fills
	// fails the check below instead of hanging.
	for (int round = 0; round < 200 || (seen == 0 && round < 100000); ++round)
	{
		const std::size_t count = ring.Buffer.Copy(into);

		for (std::size_t index = 0; index < count; ++index)
		{
			// A torn record would show up as a kind or a track the producer never emits, which is the
			// cheapest assertion that what came back was written all at once.
			GYRO_REQUIRE(into[index].Kind == TraceKind::Mark);
			GYRO_REQUIRE_EQ(into[index].Scope, TraceThread);
			GYRO_REQUIRE(into[index].Payload < 256);
		}

		seen += count;
	}

	stop.store(true, std::memory_order_relaxed);
	producer.join();

	GYRO_CHECK(seen != 0);
}

// The whole point of `EmitAt`: a span whose two ends are older than the records around it, which is
// what a GPU composite read back two frames later is.
GYRO_TEST(Trace, ASuppliedStampIsWhatComesBack)
{
	const MonotonicClock clock;
	Ring ring{ 8, clock };

	EnrollTracing(&ring.Buffer);

	TraceMark("now");
	TraceSpanAt(
		"composite", Monotonic::FromNanoseconds(1'000), Monotonic::FromNanoseconds(3'000), TraceGpu(0), TraceTag(7)
	);

	EnrollTracing(nullptr);

	std::array<TraceEvent, 8> into{};
	const std::size_t count = ring.Buffer.Copy(into);

	GYRO_REQUIRE_EQ(count, std::size_t{ 3 });

	// Out of order in the ring, and deliberately so — the writer is what puts them back in time order,
	// because it is the thread that can afford to.
	GYRO_CHECK(into[0].Stamp > into[1].Stamp);

	GYRO_CHECK(into[1].Kind == TraceKind::Begin);
	GYRO_CHECK_EQ(std::string_view{ into[1].Name }, std::string_view{ "composite" });
	GYRO_CHECK_EQ(into[1].Payload, std::uint64_t{ 7 });
	GYRO_CHECK_EQ(into[1].Scope, TraceGpu(0));
	GYRO_CHECK(into[1].Stamp == Monotonic::FromNanoseconds(1'000));

	GYRO_CHECK(into[2].Kind == TraceKind::End);
	GYRO_CHECK_EQ(into[2].Scope, TraceGpu(0));
	GYRO_CHECK(into[2].Stamp == Monotonic::FromNanoseconds(3'000));
}

// A late counter goes where its sample belongs rather than where it was learned about, for the span's
// reason one test up.
GYRO_TEST(Trace, ALateCounterKeepsItsMoment)
{
	const MonotonicClock clock;
	Ring ring{ 4, clock };

	EnrollTracing(&ring.Buffer);
	TraceCountAt("fragments", Monotonic::FromNanoseconds(500), 1'234'567, TraceGpu(1));
	EnrollTracing(nullptr);

	std::array<TraceEvent, 4> into{};

	GYRO_REQUIRE_EQ(ring.Buffer.Copy(into), std::size_t{ 1 });
	GYRO_CHECK(into[0].Kind == TraceKind::Count);
	GYRO_CHECK_EQ(into[0].Payload, std::uint64_t{ 1'234'567 });
	GYRO_CHECK_EQ(into[0].Scope, TraceGpu(1));
	GYRO_CHECK(into[0].Stamp == Monotonic::FromNanoseconds(500));
}

// A late mark goes where the thing it names happened rather than where it was learned about, for
// the counter's reason one test up: the flip is at the panel's vblank, and the feedback is drained
// a moment after.
GYRO_TEST(Trace, ALateMarkKeepsItsMoment)
{
	const MonotonicClock clock;
	Ring ring{ 4, clock };

	EnrollTracing(&ring.Buffer);
	TraceMark("now");
	TraceMarkAt("presented", Monotonic::FromNanoseconds(500), TraceOutput(0), TraceTag(12));
	EnrollTracing(nullptr);

	std::array<TraceEvent, 4> into{};

	GYRO_REQUIRE_EQ(ring.Buffer.Copy(into), std::size_t{ 2 });

	// Out of order in the ring, for the `EmitAt` reason, and what comes back as the later record is
	// the one the caller stamped rather than the one the clock happened to read.
	GYRO_CHECK(into[0].Stamp > into[1].Stamp);

	GYRO_CHECK(into[1].Kind == TraceKind::Mark);
	GYRO_CHECK_EQ(std::string_view{ into[1].Name }, std::string_view{ "presented" });
	GYRO_CHECK_EQ(into[1].Payload, std::uint64_t{ 12 });
	GYRO_CHECK_EQ(into[1].Scope, TraceOutput(0));
	GYRO_CHECK(into[1].Stamp == Monotonic::FromNanoseconds(500));
}

// The output block's arithmetic is load-bearing and the rows added beside it must not disturb it: one
// screen's lanes are consecutive (144), so a row wedged between two of them would move every lane
// after it and a trace taken before the change would be read against the wrong vocabulary.
GYRO_TEST(Trace, TheRowsAddedBesideTheOutputBlockLeaveItWhereItWas)
{
	GYRO_CHECK_EQ(TraceGrid(0), std::uint16_t{ 1 });
	GYRO_CHECK_EQ(TraceOutput(0), std::uint16_t{ 2 });
	GYRO_CHECK_EQ(TraceGlass(0), static_cast<std::uint16_t>(TraceLanesPerOutput));
	GYRO_CHECK_EQ(TraceGrid(1), static_cast<std::uint16_t>(1 + TraceLanesPerOutput));

	GYRO_CHECK(TraceGlass(TracedOutputs - 1) < TraceOutputScopes);
	GYRO_CHECK_EQ(TraceClient(0), TraceOutputScopes);
	GYRO_CHECK(TraceClient(TracedClients - 1) < TraceInput());
	GYRO_CHECK(TraceInput() < TraceSession());
	GYRO_CHECK(TraceSession() < TraceLog());
	GYRO_CHECK(TraceLog() < TraceScopes);
}

// The same claim `EmittingAllocatesNothing` makes, extended to the rows whose names are runtime
// strings: naming is the dispatch thread's and is allowed to take the lock, but nothing about a named
// row may follow the name into the frame path. A record still carries a number, and the name is
// looked up on the writer thread when a snapshot is taken.
GYRO_TEST(Trace, EmittingToANamedRowAllocatesNothing)
{
	const MonotonicClock clock;
	Ring ring{ 64, clock };

	const std::uint16_t client = ClaimTraceClient();
	NameTraceScope(client, "firefox");
	NameTraceScope(TraceSession(), "paul");

	EnrollTracing(&ring.Buffer);

	{
		const FrameSection guard;

		TraceMark("attached", client);
		TraceMark("motion", TraceInput());
		TraceMark("locked", TraceSession());
		TraceMark("warning", TraceLog());
	}

	EnrollTracing(nullptr);

	GYRO_CHECK_EQ(ring.Buffer.Written(), std::uint64_t{ 4 });

	ForgetTraceScope(TraceSession());
	ReleaseTraceClient(client);
}

// **A row should mean one thing for the whole file a person opens.** Handing the last-freed row back
// first is what Core/SlotAllocator.h does and is right for entity storage; here it would put two
// clients' slices on one row inside a single ring window, and a person reading down that row would
// read one client.
GYRO_TEST(Trace, AClientRowIsNotHandedOutAgainImmediately)
{
	const std::uint16_t first = ClaimTraceClient();

	GYRO_REQUIRE(first != TraceThread);

	ReleaseTraceClient(first);

	const std::uint16_t second = ClaimTraceClient();

	GYRO_REQUIRE(second != TraceThread);
	GYRO_CHECK(second != first);

	ReleaseTraceClient(second);
}

// Exhaustion is not an error. A session with more clients than the pool has rows loses the separation,
// not the events: the extra client's records land on the row of whichever thread served it, which is
// less legible than a row of its own and strictly better than work missing from the picture.
GYRO_TEST(Trace, TheClientAfterTheLastRowSharesTheThreadsOwnRow)
{
	std::vector<std::uint16_t> claimed;

	for (std::size_t index = 0; index < TracedClients; ++index)
	{
		const std::uint16_t row = ClaimTraceClient();

		GYRO_REQUIRE(row != TraceThread);
		GYRO_REQUIRE(std::ranges::find(claimed, row) == claimed.end());

		claimed.push_back(row);
	}

	GYRO_CHECK_EQ(ClaimTraceClient(), TraceThread);

	for (const std::uint16_t row : claimed)
	{
		ReleaseTraceClient(row);
	}

	// And the pool is a pool again once the clients that filled it have gone.
	const std::uint16_t reclaimed = ClaimTraceClient();

	GYRO_CHECK(reclaimed != TraceThread);

	ReleaseTraceClient(reclaimed);
}

// A row that keeps its old name is the misreading the whole table exists to prevent: the next client
// on it would be labelled with the last one's name, and every slice under that label would be read as
// something it is not.
GYRO_TEST(Trace, AReleasedRowLosesItsName)
{
	const std::uint16_t client = ClaimTraceClient();

	GYRO_REQUIRE(client != TraceThread);

	NameTraceScope(client, "firefox");

	TraceName read{};

	GYRO_REQUIRE(ReadTraceScopeName(client, read));
	GYRO_CHECK_EQ(std::string_view{ read.data() }, std::string_view{ "firefox" });

	ReleaseTraceClient(client);

	GYRO_CHECK(!ReadTraceScopeName(client, read));

	// Cycled back to the same row, which round-robin makes take the whole pool, and it is anonymous
	// again rather than still being firefox.
	std::vector<std::uint16_t> claimed;
	bool reached = false;

	for (std::size_t index = 0; index < TracedClients && !reached; ++index)
	{
		const std::uint16_t row = ClaimTraceClient();

		GYRO_REQUIRE(row != TraceThread);

		claimed.push_back(row);
		reached = row == client;
	}

	GYRO_REQUIRE(reached);
	GYRO_CHECK(!ReadTraceScopeName(client, read));

	for (const std::uint16_t row : claimed)
	{
		ReleaseTraceClient(row);
	}
}

// A name longer than the row holds is cut rather than dropped, because `xdg-desktop-porta` still tells
// a person which row they are reading and an empty descriptor tells them nothing.
GYRO_TEST(Trace, ALongNameIsTruncatedRatherThanRefused)
{
	const std::string overlong(TraceNameLimit + 20, 'x');

	NameTraceScope(TraceLog(), overlong);

	TraceName read{};

	GYRO_REQUIRE(ReadTraceScopeName(TraceLog(), read));
	GYRO_CHECK_EQ(std::string_view{ read.data() }.size(), TraceNameLimit - 1);

	ForgetTraceScope(TraceLog());

	GYRO_CHECK(!ReadTraceScopeName(TraceLog(), read));
}

// Naming happens on the dispatch thread and reading on the writer thread, which is the arrangement
// this table is for. Under GYRO_SANITIZE=thread the mutex is what makes that a legal pair rather than
// a race everybody learns to suppress — an instrument excluded from the sanitizer is one nobody runs
// under it.
GYRO_TEST(Trace, ARowCanBeNamedWhileTheSnapshotReadsIt)
{
	const std::uint16_t client = ClaimTraceClient();

	GYRO_REQUIRE(client != TraceThread);

	std::atomic<bool> stop{ false };

	std::thread writer{ [client, &stop] {
		TraceName read{};

		while (!stop.load(std::memory_order_relaxed))
		{
			// Whatever comes back is one of the two whole names, never half of each.
			if (ReadTraceScopeName(client, read))
			{
				const std::string_view name{ read.data() };

				GYRO_REQUIRE(name == "firefox" || name == "shell");
			}
		}
	} };

	for (int round = 0; round < 10'000; ++round)
	{
		NameTraceScope(client, round % 2 == 0 ? "firefox" : "shell");
	}

	stop.store(true, std::memory_order_relaxed);
	writer.join();

	ReleaseTraceClient(client);
}
