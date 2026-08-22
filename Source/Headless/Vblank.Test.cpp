#include "Headless/Vblank.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>

#include "Core/Time.h"
#include "Testing/Test.h"

// The arithmetic every headless flip lands on. What is worth testing is the two edges the rest of the
// module rests on and nothing else re-derives: which vblank a commit at a given instant makes, and
// what interval the flip reports having run at. The saturating cases are here too, because a
// speculative sequence is a real caller rather than a hypothetical one.

namespace
{
using namespace std::chrono_literals;

constexpr Instant At(std::int64_t milliseconds) noexcept
{
	return Monotonic::FromNanoseconds(milliseconds * 1'000'000);
}

VblankTimeline Panel(Duration period = 10ms, Instant epoch = At(1000))
{
	VblankTimeline timeline;
	timeline.Configure(epoch, period);

	return timeline;
}
} // namespace

GYRO_TEST(Vblank, SequenceIsEpochPlusPeriods)
{
	const VblankTimeline timeline = Panel();

	GYRO_CHECK_EQ(timeline.At(0), At(1000));
	GYRO_CHECK_EQ(timeline.At(1), At(1010));
	GYRO_CHECK_EQ(timeline.At(144), At(2440));
	GYRO_CHECK_EQ(timeline.Period(), 10ms);
}

// A commit at the exact instant of a vblank takes the next one. The alternative is a zero-latency
// flip, which would let a loop present and observe inside one iteration.
GYRO_TEST(Vblank, ACommitAtAVblankTakesTheNextOne)
{
	const VblankTimeline timeline = Panel();

	GYRO_CHECK_EQ(timeline.After(At(1000)), 1U);
	GYRO_CHECK_EQ(timeline.After(At(1009)), 1U);
	GYRO_CHECK_EQ(timeline.After(At(1010)), 2U);
}

// An epoch in the future is what a sweep constructs when it places a phase, and the answer is vblank
// zero rather than a refusal.
GYRO_TEST(Vblank, BeforeTheEpochIsSequenceZero)
{
	const VblankTimeline timeline = Panel();

	GYRO_CHECK_EQ(timeline.After(At(0)), 0U);
	GYRO_CHECK_EQ(timeline.After(At(999)), 0U);
}

// The injected miss: the commit was in time by the schedule's arithmetic and the panel took it a frame
// later regardless, which is the failure correct scheduling cannot prevent.
GYRO_TEST(Vblank, ALatchLeadPushesACommitToTheNextVblank)
{
	const VblankTimeline timeline = Panel();

	GYRO_CHECK_EQ(timeline.Latch(At(1009), Duration::zero()), 1U);
	GYRO_CHECK_EQ(timeline.Latch(At(1009), 2ms), 2U);
	GYRO_CHECK_EQ(timeline.Latch(At(1000), 10ms), 1U);
	GYRO_CHECK_EQ(timeline.Latch(At(1000), 11ms), 2U);
}

// The observed interval is what a flip reports, and under jitter it is not the nominal period. This is
// the whole reason Seam/PresentationInfo.h separates what the panel did from what gyro asked for.
GYRO_TEST(Vblank, JitterMovesTheVblankAndTheReportedInterval)
{
	VblankTimeline timeline = Panel();
	const std::array<Duration, 2> samples{ Duration::zero(), 1ms };
	timeline.SetJitter(samples);

	GYRO_CHECK_EQ(timeline.At(0), At(1000));
	GYRO_CHECK_EQ(timeline.At(1), At(1011));
	GYRO_CHECK_EQ(timeline.At(2), At(1020));

	GYRO_CHECK_EQ(timeline.IntervalBefore(1), 11ms);
	GYRO_CHECK_EQ(timeline.IntervalBefore(2), 9ms);

	// Vblank zero has no predecessor, and the nominal period is the honest first observation — a zero
	// would read to FrameClock as "the backend does not know".
	GYRO_CHECK_EQ(timeline.IntervalBefore(0), 10ms);
}

// Clamped to half a period, because a vblank arriving before its predecessor would hand FrameClock a
// negative interval and the defect would surface three layers up.
GYRO_TEST(Vblank, JitterCannotReorderVblanks)
{
	VblankTimeline timeline = Panel();
	const std::array<Duration, 2> samples{ 500ms, -500ms };
	timeline.SetJitter(samples);

	GYRO_CHECK(timeline.At(0) < timeline.At(1));
	GYRO_CHECK(timeline.At(1) < timeline.At(2));
	GYRO_CHECK(timeline.IntervalBefore(1) > Duration::zero());
	GYRO_CHECK(timeline.IntervalBefore(2) > Duration::zero());
}

// The search is bounded rather than trusted, so a jittered timeline still answers exactly.
GYRO_TEST(Vblank, SearchIsExactUnderJitter)
{
	VblankTimeline timeline = Panel();
	const std::array<Duration, 3> samples{ 4ms, -4ms, 1ms };
	timeline.SetJitter(samples);

	for (std::uint64_t sequence = 0; sequence < 30; ++sequence)
	{
		// The vblank itself is not "after" itself, and one nanosecond earlier is.
		GYRO_CHECK_EQ(timeline.After(timeline.At(sequence)), sequence + 1);
		GYRO_CHECK_EQ(timeline.After(timeline.At(sequence) - Duration{ 1 }), sequence);
	}
}

// A period nobody validated does not run the output backwards. PeriodFromHertz already answers a
// nonsense rate with Duration::max(); this is the other end of the same rule.
GYRO_TEST(Vblank, ANonPositivePeriodIsClamped)
{
	VblankTimeline timeline;
	timeline.Configure(At(1000), Duration::zero());

	GYRO_CHECK(timeline.Period() > Duration::zero());
	GYRO_CHECK(timeline.At(1) > timeline.At(0));
}

// A speculative sequence is a question to answer badly rather than a precondition to assume, which is
// Frame/FrameClock.h's treatment of the same caller.
GYRO_TEST(Vblank, AnAbsurdSequenceSaturates)
{
	const VblankTimeline timeline = Panel();

	GYRO_CHECK_EQ(timeline.At(std::numeric_limits<std::uint64_t>::max()), Instant{ Duration::max() });
}
