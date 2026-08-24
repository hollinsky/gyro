#include "Core/Trace.h"

#include <array>
#include <atomic>
#include <memory>
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

	std::thread producer{ [&ring, &stop] {
		EnrollTracing(&ring.Buffer);

		while (!stop.load(std::memory_order_relaxed))
		{
			for (std::uint64_t index = 0; index < 256; ++index)
			{
				TraceMark("tick", TraceThread, index);
			}
		}
	} };

	std::vector<TraceEvent> into(1024);
	std::size_t seen = 0;

	for (int round = 0; round < 200; ++round)
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
	TraceSpanAt("composite", Monotonic::FromNanoseconds(1'000), Monotonic::FromNanoseconds(3'000), TraceGpu(0), 7);

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
