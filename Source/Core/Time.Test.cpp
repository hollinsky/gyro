#include "Core/Time.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <string>

#include "Testing/Test.h"

// The runtime half of Time.h's contract. The compile-time half is the static_assert block at the
// foot of that header and is not repeated here — what is left for a test is the arithmetic that is
// awkward to spell in a constant expression, and the values that are only interesting because real
// hardware produces them.

using namespace std::chrono_literals;

GYRO_TEST(Time, PeriodsAtRealRefreshRates)
{
	// The rates in the two-output examples throughout Architecture.md, and the .94 variants that
	// make the point in Phase drift is not something to track: a panel does not run at its nominal
	// rate, so nothing may key on a period being a round number.
	GYRO_CHECK_EQ(PeriodFromHertz(60.0), Duration{ 16'666'666 });
	GYRO_CHECK_EQ(PeriodFromHertz(144.0), Duration{ 6'944'444 });
	GYRO_CHECK_EQ(PeriodFromHertz(59.94), Duration{ 16'683'350 });
	GYRO_CHECK_EQ(PeriodFromHertz(143.98), Duration{ 6'945'409 });
	GYRO_CHECK_EQ(PeriodFromHertz(240.0), Duration{ 4'166'666 });
	GYRO_CHECK_EQ(PeriodFromHertz(30.0), Duration{ 33'333'333 });
}

GYRO_TEST(Time, PeriodOfANonRateIsNeverOwed)
{
	// Duration::max() rather than zero, so a bad mode line yields an output that never owes a frame
	// instead of one that owes infinitely many.
	GYRO_CHECK_EQ(PeriodFromHertz(0.0), Duration::max());
	GYRO_CHECK_EQ(PeriodFromHertz(-60.0), Duration::max());
	GYRO_CHECK_EQ(PeriodFromHertz(std::numeric_limits<double>::quiet_NaN()), Duration::max());
}

GYRO_TEST(Time, SecondsTruncateTowardZero)
{
	GYRO_CHECK_EQ(DurationFromSeconds(0.4), 400ms);
	GYRO_CHECK_EQ(DurationFromSeconds(-0.25), -250ms);
	GYRO_CHECK_EQ(DurationFromSeconds(1e-9), Duration{ 1 });

	// Below the timebase's resolution. Configuration authored in seconds can express a value the
	// timebase cannot hold, and losing it is the correct outcome rather than an error.
	GYRO_CHECK_EQ(DurationFromSeconds(1e-10), Duration::zero());
	GYRO_CHECK_EQ(DurationFromSeconds(-1e-10), Duration::zero());
}

GYRO_TEST(Time, SecondsSaturateRatherThanWrap)
{
	// The finite cases are the ones the static_asserts do not reach, and they are the ones a
	// configuration file can actually contain. A bare cast here would be undefined behaviour.
	GYRO_CHECK_EQ(DurationFromSeconds(1e10), Duration::max());
	GYRO_CHECK_EQ(DurationFromSeconds(-1e10), Duration::min());
	GYRO_CHECK_EQ(DurationFromSeconds(std::numeric_limits<double>::max()), Duration::max());
	GYRO_CHECK_EQ(DurationFromSeconds(std::numeric_limits<double>::lowest()), Duration::min());
	GYRO_CHECK_EQ(DurationFromSeconds(std::numeric_limits<double>::quiet_NaN()), Duration::zero());
}

GYRO_TEST(Time, SecondsRoundTripWithinTheTimebase)
{
	// The catalog authors response times in seconds, so this is the path every spring parameter
	// takes. Half a nanosecond is the most that can be lost, and the tolerance says so.
	constexpr double Tolerance = 1e-9;

	for (const double seconds : { 0.4, 0.35, 0.001, 3.0, -0.25 })
	{
		const double roundTripped = ToSeconds(DurationFromSeconds(seconds));
		GYRO_CHECK(std::abs(roundTripped - seconds) < Tolerance);
	}
}

GYRO_TEST(Time, EgressAgreesWithTheDurationItDescribes)
{
	GYRO_CHECK(std::abs(ToMilliseconds(16'666'666ns) - 16.666666) < 1e-6);
	GYRO_CHECK(std::abs(ToSeconds(1s) - 1.0) < 1e-12);
	GYRO_CHECK_EQ(ToSeconds(Duration::zero()), 0.0);
}

GYRO_TEST(Time, DeadlineArithmeticGoesNegative)
{
	// Why Duration is signed. A frame loop asking "how long until this deadline" after the deadline
	// has passed is the ordinary case, not the error case — Architecture.md#the-frame-loop turns
	// exactly this subtraction into the decision to fall to the floor tier or skip the output.
	const Instant deadline = Monotonic::FromNanoseconds(1'000);
	const Instant now = Monotonic::FromNanoseconds(1'500);

	GYRO_CHECK_EQ(deadline - now, Duration{ -500 });
	GYRO_CHECK((deadline - now).count() < 0);
}

GYRO_TEST(Time, ArithmeticSaturatesRatherThanWrapping)
{
	// The compile-time half is the static_assert block in Time.h, and for this property a constant
	// expression is the stronger instrument: signed overflow is not merely undefined there but
	// ill-formed, so those assertions cannot pass while the arithmetic they cover is undefined. What is
	// left for a test is the runtime path, which is the one GYRO_SANITIZE=undefined can see.
	const Instant early = Monotonic::FromNanoseconds(std::numeric_limits<std::int64_t>::min());
	const Instant late = Monotonic::FromNanoseconds(std::numeric_limits<std::int64_t>::max());

	GYRO_CHECK_EQ(Elapsed(early, late), Duration::max());
	GYRO_CHECK_EQ(Elapsed(late, early), Duration::min());
	GYRO_CHECK_EQ(Advanced(late, 1ns), Instant{ Duration::max() });
	GYRO_CHECK_EQ(Advanced(early, -1ns), Instant{ Duration::min() });

	// Saturating is the behaviour at the bounds and nowhere else: everywhere the operators are defined
	// these agree with them exactly, which is what lets a caller reach for the total form without
	// having to think about whether it costs anything.
	const Instant deadline = Monotonic::FromNanoseconds(1'000);
	const Instant now = Monotonic::FromNanoseconds(1'500);

	GYRO_CHECK_EQ(Elapsed(deadline, now), now - deadline);
	GYRO_CHECK_EQ(Elapsed(now, deadline), deadline - now);
	GYRO_CHECK_EQ(Advanced(deadline, 500ns), deadline + 500ns);
	GYRO_CHECK_EQ(Advanced(now, -500ns), deadline);
}

GYRO_TEST(Time, InstantFormatsAsTimeSinceEpoch)
{
	// The formatter in Time.h exists because the standard supplies none for a time_point over a
	// private tag, and an Instant that printed as a raw integer would be unreadable in a log line
	// beside a Duration that prints its unit.
	const Instant instant = Monotonic::FromMicroseconds(1'500);

	GYRO_CHECK_EQ(std::format("{}", instant), std::format("{}", 1'500'000ns));
	GYRO_CHECK_EQ(std::format("{}", instant), std::string{ "1500000ns" });
}
