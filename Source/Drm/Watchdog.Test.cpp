#include "Drm/Watchdog.h"

#include "Testing/Test.h"

// The rule that decides a page flip event is never coming, driven against a clock the test owns —
// which is the whole reason it is not a member of `DrmOutput`, a class that cannot be built without a
// card in the machine.

namespace
{
using namespace std::chrono_literals;

constexpr Instant At(std::int64_t milliseconds) noexcept
{
	return Monotonic::FromNanoseconds(milliseconds * 1'000'000);
}

constexpr Duration Refresh = std::chrono::microseconds{ 16'666 };
} // namespace

// **Quiet is the ordinary answer**, and it has to be the one every branch of `DrmOutput::NextEvent`
// already gives for *nothing to do here* — an output with no commit outstanding must not put a
// deadline on the loop's books at all.
GYRO_TEST(FlipWatchdog, AnUnarmedWatchdogAsksForNothingAndNeverFires)
{
	const Drm::FlipWatchdog watchdog;

	GYRO_CHECK(!watchdog.IsArmed());
	GYRO_CHECK(watchdog.Deadline() == Instant{ Duration::max() });
	GYRO_CHECK(!watchdog.HasExpired(At(1'000'000)));
}

// The deadline is one refresh from the kernel's answer, which is what puts the output back on
// `NextEvent`'s books once the commit thread has gone idle.
GYRO_TEST(FlipWatchdog, ArmingScalesTheDeadlineToTheOutputsOwnPeriod)
{
	Drm::FlipWatchdog watchdog;

	watchdog.Arm(At(100), Refresh);

	GYRO_CHECK(watchdog.IsArmed());
	GYRO_CHECK(watchdog.Deadline() == Advanced(At(100), Refresh));

	// A 24 Hz panel is not held to a 240 Hz panel's patience.
	watchdog.Arm(At(100), 41ms);

	GYRO_CHECK(watchdog.Deadline() == Advanced(At(100), 41ms));
}

// **An unknown period must not arm a deadline of zero**, which is what a bare multiply would do before
// the first flip has been timed or on a catalog that could not read the timings — and a zero deadline
// fires on the same iteration that armed it, turning every commit into a missed frame.
GYRO_TEST(FlipWatchdog, AnOutputWithNoMeasuredPeriodFallsBackRatherThanFiringAtOnce)
{
	Drm::FlipWatchdog watchdog;

	watchdog.Arm(At(100), Duration::zero());

	GYRO_CHECK(watchdog.Deadline() == Advanced(At(100), Drm::FlipWatchdog::Fallback));
	GYRO_CHECK(!watchdog.HasExpired(At(100)));
}

// **At the deadline rather than past it.** The loop is woken *at* the instant `Deadline` named, so a
// strict comparison would find nothing due on the wake it asked for and sleep another whole period —
// which doubles every recovery and is invisible in anything but a trace.
GYRO_TEST(FlipWatchdog, ItFiresOnTheWakeItAskedForRatherThanTheOneAfter)
{
	Drm::FlipWatchdog watchdog;

	watchdog.Arm(At(100), 10ms);

	GYRO_CHECK(!watchdog.HasExpired(At(109)));
	GYRO_CHECK(watchdog.HasExpired(At(110)));
	GYRO_CHECK(watchdog.HasExpired(At(400)));
}

// The event arrived. Every path in `DrmOutput` that clears `m_Flipping` calls this, several of them
// with nothing armed, so it has to tolerate being called twice and out of turn.
GYRO_TEST(FlipWatchdog, DisarmingIsIdempotentAndPutsTheOutputBackToQuiet)
{
	Drm::FlipWatchdog watchdog;

	watchdog.Arm(At(100), 10ms);
	watchdog.Disarm();

	GYRO_CHECK(!watchdog.IsArmed());
	GYRO_CHECK(watchdog.Deadline() == Instant{ Duration::max() });
	GYRO_CHECK(!watchdog.HasExpired(At(400)));

	watchdog.Disarm();

	GYRO_CHECK(!watchdog.IsArmed());
}

// Re-arming is what a second commit does, and the deadline that matters is the new one: a watchdog
// still holding the last frame's instant would fire on a commit that is doing nothing wrong.
GYRO_TEST(FlipWatchdog, ArmingAgainReplacesTheDeadlineRatherThanKeepingTheOldest)
{
	Drm::FlipWatchdog watchdog;

	watchdog.Arm(At(100), 10ms);
	watchdog.Arm(At(200), 10ms);

	GYRO_CHECK(!watchdog.HasExpired(At(115)));
	GYRO_CHECK(watchdog.HasExpired(At(210)));
}
