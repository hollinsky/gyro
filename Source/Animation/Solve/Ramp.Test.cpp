#include "Animation/Solve/Ramp.h"

#include <cmath>
#include <limits>

#include "Core/Time.h"
#include "Core/Wake.h"
#include "Testing/Test.h"

// The ramp's contract, asserted exactly rather than to a tolerance.
//
// Every instant below is a power-of-two fraction of a second, so the nanoseconds-to-seconds
// conversion is exact and the assertions are about the closed form rather than about where the
// rounding of a decimal fraction happened to land. That matters more here than it would for a spring:
// exactness is the *reason* the driven regime is not a spring (Docs/Decisions.md decision 65), so a
// test written to a tolerance would be checking the one property this form exists to have with the
// one instrument that cannot see it.

namespace
{
// Quarter of a second of lead, a rate of two progress per second, starting a quarter of the way
// along. Chosen so that every probe below lands on a value with an exact binary representation.
constexpr Ramp kLeading{
	.Origin = Monotonic::FromNanoseconds(0),
	.Horizon = Duration{ 500'000'000 },
	.Progress = 0.25f,
	.Rate = 2.0f,
};

[[nodiscard]] constexpr Instant At(std::int64_t milliseconds) noexcept
{
	return Monotonic::FromNanoseconds(milliseconds * 1'000'000);
}
} // namespace

GYRO_TEST(Ramp, CarriesProgressForwardAtItsRate)
{
	GYRO_CHECK_EQ(kLeading.Evaluate(At(0)).Position, 0.25f);
	GYRO_CHECK_EQ(kLeading.Evaluate(At(125)).Position, 0.5f);
	GYRO_CHECK_EQ(kLeading.Evaluate(At(250)).Position, 0.75f);
}

GYRO_TEST(Ramp, ScrubbingIsExactWhereverInTheRangeItHappens)
{
	// The property a spring cannot have, and the whole reason the driven regime is a second closed
	// form: equal elapsed times produce equal changes in progress, at the start and near the end alike.
	const float early = kLeading.Evaluate(At(250)).Position - kLeading.Evaluate(At(125)).Position;
	const float late = kLeading.Evaluate(At(375)).Position - kLeading.Evaluate(At(250)).Position;

	GYRO_CHECK_EQ(early, 0.25f);
	GYRO_CHECK_EQ(late, early);
}

GYRO_TEST(Ramp, HoldsItsValuePastTheHorizon)
{
	// The lead stops where the evidence does. Past the horizon the value is whatever the ramp reached,
	// however long the frame thread goes on evaluating it — which is what lets the wake settle.
	const RampState atHorizon = kLeading.Evaluate(At(500));
	const RampState wellPast = kLeading.Evaluate(At(5'000));

	GYRO_CHECK_EQ(atHorizon.Position, 1.25f);
	GYRO_CHECK_EQ(wellPast.Position, atHorizon.Position);
}

GYRO_TEST(Ramp, VelocityIsTheRateInsideTheHorizonAndNothingOutsideIt)
{
	// A held value that still reported its rate would hand the release spring a velocity the content
	// no longer has, and the transition would leave the finger's last position moving.
	GYRO_CHECK_EQ(kLeading.Evaluate(At(250)).Velocity, 2.0f);
	GYRO_CHECK_EQ(kLeading.Evaluate(At(500)).Velocity, 0.0f);
	GYRO_CHECK_EQ(kLeading.Evaluate(At(5'000)).Velocity, 0.0f);
}

GYRO_TEST(Ramp, AReversalCarriesBackwards)
{
	constexpr Ramp reversing{
		.Origin = Monotonic::FromNanoseconds(0),
		.Horizon = Duration{ 500'000'000 },
		.Progress = 0.75f,
		.Rate = -2.0f,
	};

	GYRO_CHECK_EQ(reversing.Evaluate(At(250)).Position, 0.25f);
	GYRO_CHECK_EQ(reversing.Evaluate(At(250)).Velocity, -2.0f);
}

GYRO_TEST(Ramp, BeforeItsOriginItIsAtItsInitialValue)
{
	// Evaluation before t0 is a caller error rather than a state to represent — t0 is an input event's
	// timestamp and evaluation happens at predicted presentation time — so it yields the initial state,
	// which is bounded and is the same answer Spring gives to the same mistake.
	GYRO_CHECK_EQ(kLeading.Evaluate(At(-100)).Position, 0.25f);
}

GYRO_TEST(Ramp, SettlesAtTheHorizonAndTheWakeFollowsIt)
{
	GYRO_CHECK_EQ(kLeading.SettlesAt(), At(500));

	GYRO_CHECK(!kLeading.IsSettled(At(499)));
	GYRO_CHECK(kLeading.IsSettled(At(500)));

	GYRO_CHECK(kLeading.WakeAt(At(250)) == Wake::EveryFrame(At(250)));
	GYRO_CHECK(kLeading.WakeAt(At(500)) == Wake::Never());
}

GYRO_TEST(Ramp, AZeroHorizonIsAValueThatWasNeverLeading)
{
	// What dispatch republishes when a finger lifts: the progress it reached, going nowhere. It settles
	// at its own origin, so it contributes nothing to the fold and the compositor is free to idle.
	constexpr Ramp held{ .Origin = At(100), .Horizon = Duration::zero(), .Progress = 0.6f, .Rate = 0.0f };

	GYRO_CHECK_EQ(held.SettlesAt(), At(100));
	GYRO_CHECK_EQ(held.Evaluate(At(9'000)).Position, 0.6f);
	GYRO_CHECK_EQ(held.Evaluate(At(9'000)).Velocity, 0.0f);
	GYRO_CHECK(held.WakeAt(At(100)) == Wake::Never());
}

GYRO_TEST(Ramp, ADefaultRampAsksForNothing)
{
	// It settles at the epoch, so it contributes nothing to the fold rather than a standing
	// commitment — the same direction Wake{} takes, and what lets the driven run be absent from a
	// snapshot without a special case at the frame side.
	GYRO_CHECK_EQ(Ramp{}.SettlesAt(), Instant{});
	GYRO_CHECK(Ramp{}.WakeAt(Instant{}) == Wake::Never());
	GYRO_CHECK_EQ(Ramp{}.Evaluate(At(1'000)).Position, 0.0f);
}

GYRO_TEST(Ramp, AnUnreachableHorizonSaturatesRatherThanWrapping)
{
	// The same failure Spring::SettlesAt saturates against, arriving from an authored number instead of
	// from an undamped oscillator: a horizon that overflows the instant would land in the deep past and
	// read as settled, which is an animation freezing mid-gesture.
	constexpr Ramp unbounded{ .Origin = At(1'000), .Horizon = Duration::max(), .Progress = 0.0f, .Rate = 1.0f };

	constexpr Instant justShort = Monotonic::FromNanoseconds(std::numeric_limits<std::int64_t>::max() - 1);

	GYRO_CHECK_EQ(unbounded.SettlesAt(), Instant{ Duration::max() });
	GYRO_CHECK(!unbounded.IsSettled(justShort));
	GYRO_CHECK(unbounded.WakeAt(At(2'000)) == Wake::EveryFrame(At(2'000)));

	// Evaluating is asserted here and not left to the cases above, because a saturated horizon is
	// exactly where the clamp inside Evaluate could be inverted without any of them noticing. The
	// minimum has to pick the evaluation instant rather than the horizon: taken the wrong way round this
	// reads a second of lead as nine billion, and SettlesAt, IsSettled and WakeAt all still agree.
	GYRO_CHECK_EQ(unbounded.Evaluate(At(2'000)).Position, 1.0f);
	GYRO_CHECK_EQ(unbounded.Evaluate(At(2'000)).Velocity, 1.0f);

	// And the widest elapsed the clamp can hand the seconds conversion stays finite and still inside
	// the horizon, which is the arithmetic Spring's own extreme-elapsed test covers from the other side.
	const RampState atTheEnd = unbounded.Evaluate(justShort);

	GYRO_CHECK(std::isfinite(atTheEnd.Position) && atTheEnd.Position > 0.0f);
	GYRO_CHECK_EQ(atTheEnd.Velocity, 1.0f);
}
