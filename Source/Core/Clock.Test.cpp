#include "Core/Clock.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "Testing/Test.h"

using namespace std::chrono_literals;

GYRO_TEST(Clock, MonotonicClockNeverGoesBackwards)
{
	const MonotonicClock clock;

	Instant previous = clock.Now();
	for (int i = 0; i < 1'000; ++i)
	{
		const Instant now = clock.Now();
		GYRO_REQUIRE(now >= previous);
		previous = now;
	}
}

GYRO_TEST(Clock, MonotonicClockAdvances)
{
	// Spun rather than slept. A sleep puts a real duration into a unit test in exchange for
	// nothing; the iteration bound is here only so that a stopped clock fails instead of hanging.
	const MonotonicClock clock;
	const Instant start = clock.Now();

	bool advanced = false;
	for (int i = 0; i < 1'000'000 && !advanced; ++i)
	{
		advanced = clock.Now() > start;
	}

	GYRO_CHECK(advanced);
}

GYRO_TEST(Clock, MonotonicClockCountsFromBoot)
{
	// CLOCK_MONOTONIC counts from boot, so a reading is a large positive offset from the timebase
	// epoch. The bound is a second rather than zero because dropping tv_sec — the one plausible
	// mistake in Clock.cpp's conversion — still leaves a nonzero tv_nsec behind to hide it.
	const MonotonicClock clock;

	GYRO_CHECK(clock.Now() > Instant{} + 1s);
}

GYRO_TEST(Clock, ManualClockStartsAtTheEpochAndStaysThere)
{
	const ManualClock clock;

	GYRO_CHECK_EQ(clock.Now(), Instant{});
	GYRO_CHECK_EQ(clock.Now(), Instant{});
}

GYRO_TEST(Clock, ManualClockSetsAndAdvances)
{
	ManualClock clock{ Monotonic::FromMicroseconds(500) };
	GYRO_CHECK_EQ(clock.Now(), Monotonic::FromNanoseconds(500'000));

	clock.Advance(2ms);
	GYRO_CHECK_EQ(clock.Now(), Monotonic::FromNanoseconds(2'500'000));

	// Backwards is legal and is used: the schedulability sweep places a clock at arbitrary phase
	// relationships rather than only walking one forwards.
	clock.Advance(-1ms);
	GYRO_CHECK_EQ(clock.Now(), Monotonic::FromNanoseconds(1'500'000));

	clock.Set(Instant{});
	GYRO_CHECK_EQ(clock.Now(), Instant{});
}

GYRO_TEST(Clock, SubsystemsHoldTheInterface)
{
	// Nothing downstream names a concrete clock. This is what lets the headless backend hand the
	// frame loop something that does not advance at all, which the idle assertion needs and which
	// no real clock can be asked for.
	ManualClock manual{ Monotonic::FromNanoseconds(7) };
	const IClock& clock = manual;

	GYRO_CHECK_EQ(clock.Now(), Monotonic::FromNanoseconds(7));

	manual.Advance(Duration{ 3 });
	GYRO_CHECK_EQ(clock.Now(), Monotonic::FromNanoseconds(10));
}

GYRO_TEST(Clock, ManualClockIsReadableWhileItIsDriven)
{
	// The atomic in ManualClock is load-bearing rather than defensive: headless runs the same two
	// real threads the native backend does, so the fake clock is genuinely read by one while
	// another drives it. This test exists to be run under -DGYRO_SANITIZE=thread, where the
	// non-atomic version is a reported race rather than a value that happened to be fine.
	ManualClock clock;
	std::atomic<bool> stop{ false };
	std::atomic<bool> wentBackwards{ false };

	std::thread reader{ [&] {
		Instant previous = clock.Now();
		while (!stop.load(std::memory_order_relaxed))
		{
			const Instant now = clock.Now();
			if (now < previous)
			{
				wentBackwards.store(true, std::memory_order_relaxed);
			}
			previous = now;
		}
	} };

	for (int i = 0; i < 10'000; ++i)
	{
		clock.Advance(1us);
	}

	stop.store(true, std::memory_order_relaxed);
	reader.join();

	GYRO_CHECK(!wentBackwards.load(std::memory_order_relaxed));
	GYRO_CHECK_EQ(clock.Now(), Instant{} + 10ms);
}

// The anchor is what makes a trace readable against anything outside gyro, and each of its three
// readings buys a different kind of readability: monotonic is what the records are stamped in,
// boot-time is what a kernel trace is stamped in, and the wall clock is what a person and a journal
// line are stamped in.
GYRO_TEST(Clock, TheAnchorReadsThreeDomains)
{
	const ClockAnchor anchor = ReadClockAnchor();

	// Monotonic and boot-time both count from boot, so both are large positive offsets and boot-time
	// is never behind — it counts through suspend where the other does not.
	GYRO_CHECK(anchor.Monotonic > 1'000'000'000);
	GYRO_CHECK(anchor.Boottime >= anchor.Monotonic);

	// The wall clock counts from 1970, so it is orders of magnitude larger than an uptime and a reading
	// in the uptime range would be this field having been filled from the wrong clock. The bound is
	// 2020, which any machine with a working RTC or any network at all is past.
	GYRO_CHECK(anchor.Realtime > 1'577'836'800'000'000'000);
}

// Read together, which is the whole reason the three are one call: the relation is worth nothing if
// the readings are not of the same moment. Two anchors taken back to back must have moved by the same
// amount in every domain, which a reading taken outside the window would not.
GYRO_TEST(Clock, TheAnchorsDomainsAdvanceTogether)
{
	const ClockAnchor first = ReadClockAnchor();
	const ClockAnchor second = ReadClockAnchor();

	const std::int64_t monotonic = second.Monotonic - first.Monotonic;
	const std::int64_t realtime = second.Realtime - first.Realtime;

	GYRO_CHECK(monotonic >= 0);
	GYRO_CHECK(realtime - monotonic < 1'000'000);
	GYRO_CHECK(monotonic - realtime < 1'000'000);
}
