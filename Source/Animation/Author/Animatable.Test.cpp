#include "Animation/Author/Animatable.h"

#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>

#include "Testing/Test.h"

// Animatable is a thin wrapper, so the tests worth writing are the ones about the seams rather than
// about the trajectory — Animation/Solve's tests already own the mathematics and Author's own tests
// own the exactness of retargeting. What is left here is three claims:
//
//   The model and the presentation are two values and stay two. The model is the target from the
//   instant it is set, the presentation is where the property has got to, and neither is ever
//   written back into the other.
//
//   Activity is derived from the coefficients rather than tracked beside them, so the two cannot
//   disagree. The branch in AnimateTo is the one place that could make them, and it is checked
//   against the arm it did not take rather than against a golden number.
//
//   Settling answers with a wake. The three cases have to survive the wrapper, and the case that
//   would go missing quietly is the standing commitment of a spring that never settles.

namespace
{
// Underdamped, so the trajectory overshoots and the presentation is somewhere the model is not for
// long enough to be asked about.
const SpringParameters<double> Motion = ParametersFromResponse(0.4, 0.7);

// Stand-ins rather than policy. Docs/Open.md leaves the thresholds for non-geometric properties
// open, and the whole reason they are an argument is that neither this file nor the header is
// entitled to answer it — these two numbers say only that a test had to pass something.
constexpr SettleThresholds<double> Thresholds{ .Position = 1e-3, .Velocity = 1e-3 };

[[nodiscard]] Instant At(double seconds)
{
	return Monotonic::FromNanoseconds(static_cast<std::int64_t>(seconds * 1'000'000'000.0));
}

// A minimal vector channel, declared here for the reason both of the other Animation tests declare
// one: the portable tier cannot reach a real one, and the requirement is algebraic rather than
// geometric.
struct Pair
{
	using Scalar = double;

	double X{};
	double Y{};

	friend constexpr Pair operator+(Pair left, Pair right) noexcept { return { left.X + right.X, left.Y + right.Y }; }
	friend constexpr Pair operator-(Pair left, Pair right) noexcept { return { left.X - right.X, left.Y - right.Y }; }
	friend constexpr Pair operator*(Pair value, double factor) noexcept
	{
		return { value.X * factor, value.Y * factor };
	}
	friend constexpr Pair operator/(Pair value, double divisor) noexcept
	{
		return { value.X / divisor, value.Y / divisor };
	}

	friend constexpr bool operator==(Pair, Pair) noexcept = default;
};

[[nodiscard]] inline double Magnitude(Pair value) noexcept
{
	return std::hypot(value.X, value.Y);
}
} // namespace

template<>
struct std::formatter<Pair>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(Pair value, Context& context) const
	{
		return std::format_to(context.out(), "({}, {})", value.X, value.Y);
	}
};

static_assert(SpringValue<Pair>);

GYRO_TEST(Animatable, StartsAtRestAndOwesNothing)
{
	const Animatable<double> value{ 7.5 };

	GYRO_CHECK(value.IsAtRest());
	GYRO_CHECK_EQ(value.Model(), 7.5);

	// Including an instant far enough out that a spring evaluated there would have underflowed its
	// envelope. A property that was never animated has no envelope to underflow.
	for (const double seconds : { 0.0, 0.25, 60.0, 86'400.0 })
	{
		GYRO_CHECK_EQ(value.Presentation(At(seconds)), 7.5);
		GYRO_CHECK_EQ(value.NextWake(At(seconds), Thresholds), Wake::Never());
	}
}

GYRO_TEST(Animatable, SetsTheModelAtOnceAndLetsThePresentationCatchUp)
{
	Animatable<double> value{ 0.0 };

	value.AnimateTo(100.0, Motion, At(1.0));

	// The model is the target immediately, which is what layout and hit-testing read. Anything that
	// eased the model towards the target would put window management on the animation's timeline.
	GYRO_CHECK_EQ(value.Model(), 100.0);
	GYRO_CHECK(!value.IsAtRest());

	// The presentation is exactly where the property was, at the origin.
	GYRO_CHECK_EQ(value.Presentation(At(1.0)), 0.0);
	GYRO_CHECK(value.Presentation(At(1.05)) > 0.0);
	GYRO_CHECK(value.Presentation(At(1.05)) < 100.0);

	// And the model is unmoved by having been evaluated. Neither result is ever written back.
	GYRO_CHECK_EQ(value.Model(), 100.0);
	GYRO_CHECK(std::abs(value.Presentation(At(11.0)) - 100.0) < 1e-9);
}

GYRO_TEST(Animatable, EvaluatesAtAnInstantRatherThanSteppingToOne)
{
	Animatable<double> value{ 20.0 };

	value.AnimateTo(-5.0, Motion, At(0.5));

	// Two outputs asking in either order get the same answers, which is the property that lets one
	// scene serve panels at different rates. Asked backwards on purpose.
	const double late = value.Presentation(At(0.9));
	const double early = value.Presentation(At(0.6));

	GYRO_CHECK_EQ(value.Presentation(At(0.6)), early);
	GYRO_CHECK_EQ(value.Presentation(At(0.9)), late);
	GYRO_CHECK(early != late);
}

GYRO_TEST(Animatable, ReportsWhereItIsAndHowFastAtOnce)
{
	Animatable<double> value{ 0.0 };

	value.AnimateTo(100.0, Motion, Instant{});

	// The position halves agree by construction — Presentation is defined through the state — and
	// the velocity half is the one a gesture taking over a transition in flight reads. Reaching it
	// through Coefficients().Evaluate would be a dispatch-side read through the publication
	// accessor, and would pay the transcendentals twice.
	for (const double seconds : { 0.0, 0.08, 0.3 })
	{
		const SpringState<double> state = value.PresentationState(At(seconds));

		GYRO_CHECK_EQ(state.Position, value.Presentation(At(seconds)));
		GYRO_CHECK_EQ(state.Position, value.Coefficients().Evaluate(At(seconds)).Position);
		GYRO_CHECK_EQ(state.Velocity, value.Coefficients().Evaluate(At(seconds)).Velocity);
	}

	GYRO_CHECK(value.PresentationState(At(0.08)).Velocity > 0.0);

	// At rest there is nothing to evaluate and the answer is the model, at zero, exactly.
	value.Settle();
	GYRO_CHECK_EQ(value.PresentationState(At(0.08)).Position, 100.0);
	GYRO_CHECK_EQ(value.PresentationState(At(0.08)).Velocity, 0.0);
}

GYRO_TEST(Animatable, CarriesAFlickThroughRelease)
{
	// The failure this exists to prevent is one level below the input side's "release velocity is a
	// fit over a short window, not the last delta": an estimator can be perfect and the flick still
	// dies if the property it is handed to starts from rest.
	Animatable<double> released{ 0.0 };
	Animatable<double> fromRest{ 0.0 };

	released.AnimateTo(100.0, Motion, Instant{}, 400.0);
	fromRest.AnimateTo(100.0, Motion, Instant{});

	GYRO_CHECK_EQ(released.Coefficients().Velocity, 400.0);
	GYRO_CHECK_EQ(released.Model(), 100.0);

	// Same origin, same target, same motion — and the released one is ahead from the first instant
	// after the origin and stays ahead through the approach.
	GYRO_CHECK_EQ(released.Presentation(Instant{}), fromRest.Presentation(Instant{}));

	for (const double seconds : { 0.01, 0.05, 0.15 })
	{
		GYRO_CHECK(released.Presentation(At(seconds)) > fromRest.Presentation(At(seconds)));
	}
}

GYRO_TEST(Animatable, ImposesAVelocityRatherThanPreservingOne)
{
	Animatable<double> value{ 0.0 };

	value.AnimateTo(100.0, Motion, Instant{}, 400.0);

	// A gesture taking over what is already in flight: read the state, hand a velocity of its own
	// back in. The position is continuous and the velocity is whatever was imposed, not what the
	// property was doing — which is the difference between this overload and the one that retargets.
	const Instant takeover = At(0.12);
	const SpringState<double> flight = value.PresentationState(takeover);

	GYRO_REQUIRE(flight.Velocity != -75.0);

	value.AnimateTo(0.0, Motion, takeover, -75.0);

	GYRO_CHECK_EQ(value.Coefficients().Velocity, -75.0);
	GYRO_CHECK_EQ(value.Model(), 0.0);
	GYRO_CHECK(std::abs(value.Presentation(takeover) - flight.Position) < 1e-12);
}

GYRO_TEST(Animatable, SettlesOntoTheTargetRatherThanWhereItHadGot)
{
	Animatable<double> value{ 0.0 };

	value.AnimateTo(100.0, Motion, Instant{}, 400.0);
	GYRO_REQUIRE(value.Presentation(At(0.1)) != 100.0);

	// Resume runs this over everything so that nobody wakes into the middle of yesterday's
	// transition, and the atlas runs it to finish an exit early when it is out of room. Both want
	// the transition *completed*, so the model is the survivor.
	value.Settle();

	GYRO_CHECK(value.IsAtRest());
	GYRO_CHECK_EQ(value.Model(), 100.0);
	GYRO_CHECK_EQ(value.Presentation(At(0.1)), 100.0);
	GYRO_CHECK_EQ(value.NextWake(At(0.1), Thresholds), Wake::Never());
}

GYRO_TEST(Animatable, StartingFromRestIsExactlyARetargetFromRest)
{
	Animatable<double> value{ 20.0 };

	value.AnimateTo(-5.0, Motion, At(0.5));

	// The arm AnimateTo did not take, spelled out. The branch exists only to keep the commonest case
	// off the transcendentals, so a divergence would be invisible except as a first frame starting
	// from slightly the wrong place.
	const Spring<double> retargeted = Retarget(Spring<double>{ .Target = 20.0 }, At(0.5), -5.0, Motion);
	const Spring<double>& taken = value.Coefficients();

	GYRO_CHECK_EQ(taken.Origin, retargeted.Origin);
	GYRO_CHECK(taken.Parameters == retargeted.Parameters);
	GYRO_CHECK_EQ(taken.Target, retargeted.Target);
	GYRO_CHECK_EQ(taken.Offset, retargeted.Offset);
	GYRO_CHECK_EQ(taken.Velocity, retargeted.Velocity);
}

GYRO_TEST(Animatable, RetargetsWithoutADiscontinuity)
{
	Animatable<double> value{ 0.0 };

	value.AnimateTo(100.0, Motion, Instant{});

	const Instant interrupt = At(0.15);
	const SpringState<double> flight = value.Coefficients().Evaluate(interrupt);

	value.AnimateTo(-40.0, Motion, interrupt);

	GYRO_CHECK_EQ(value.Model(), -40.0);

	// Position is continuous to within the rounding of re-expressing it against a new target, and
	// velocity is preserved exactly because it is stored rather than recomputed. Exactness on the
	// second is the whole point of the closed form: it is the difference between a dismissed menu
	// reversing out of wherever it had got to and one that visibly jumps.
	GYRO_CHECK(std::abs(value.Presentation(interrupt) - flight.Position) < 1e-12);
	GYRO_CHECK_EQ(value.Coefficients().Velocity, flight.Velocity);
}

GYRO_TEST(Animatable, ContributesEveryFrameUntilItSettlesAndNothingAfterwards)
{
	Animatable<double> value{ 0.0 };

	value.AnimateTo(100.0, Motion, Instant{});

	const Instant settles = value.Coefficients().SettlesAt(Thresholds);
	GYRO_REQUIRE(settles > Instant{});

	// In flight the answer is a standing commitment rather than an instant, because every instant
	// between now and settling is one the property wants a frame at.
	GYRO_CHECK_EQ(value.NextWake(Instant{}, Thresholds), Wake::EveryFrame(Instant{}));

	const Instant justBefore = settles - Duration{ 1 };
	GYRO_CHECK_EQ(value.NextWake(justBefore, Thresholds), Wake::EveryFrame(justBefore));

	GYRO_CHECK_EQ(value.NextWake(settles, Thresholds), Wake::Never());
	GYRO_CHECK_EQ(value.NextWake(At(30.0), Thresholds), Wake::Never());
}

GYRO_TEST(Animatable, IsWithinItsThresholdOfTheModelOnceItHasSettled)
{
	Animatable<double> value{ 0.0 };

	value.AnimateTo(100.0, Motion, Instant{});

	const Instant settles = value.Coefficients().SettlesAt(Thresholds);

	// Settled is a claim about the schedule, not about the coefficients: the property owes no more
	// frames, and it still carries the residual offset that makes it not at rest. The two are
	// different questions and the wrapper answers each on its own terms — which is what lets the
	// output snap to its device grid here without anything being written back.
	GYRO_CHECK(std::abs(value.Presentation(settles) - value.Model()) <= Thresholds.Position);
	GYRO_CHECK(!value.IsAtRest());
	GYRO_CHECK_EQ(value.NextWake(settles, Thresholds), Wake::Never());
}

GYRO_TEST(Animatable, SetImmediateStopsTheMotionDead)
{
	Animatable<double> value{ 0.0 };

	value.AnimateTo(100.0, Motion, Instant{});
	value.SetImmediate(42.0);

	GYRO_CHECK(value.IsAtRest());
	GYRO_CHECK_EQ(value.Model(), 42.0);

	// Not a retarget to 42: nothing is in flight afterwards, at any instant, at any threshold.
	for (const double seconds : { 0.0, 0.05, 3.0 })
	{
		GYRO_CHECK_EQ(value.Presentation(At(seconds)), 42.0);
		GYRO_CHECK_EQ(value.NextWake(At(seconds), {}), Wake::Never());
	}
}

GYRO_TEST(Animatable, NeverSettlingIsAStandingCommitmentRatherThanIdle)
{
	// Undamped, which ParametersFromResponse clamps away — constructed directly, because the hazard
	// is precisely that a wrapper collapses the case the solver was careful about. SettlesAt
	// saturates rather than returning a sentinel, so the comparison against it is false forever and
	// the answer stays continuous; a representation whose "never again" could be spelled from a
	// "never settles" would drop the compositor to idle with this still moving.
	Animatable<double> value{ 0.0 };

	value.AnimateTo(100.0, SpringParameters<double>{ 2.0 * std::numbers::pi / 0.4, 0.0 }, Instant{});

	for (const double seconds : { 0.0, 1.0, 3'600.0, 1e6 })
	{
		GYRO_CHECK_EQ(value.NextWake(At(seconds), Thresholds), Wake::EveryFrame(At(seconds)));
	}

	GYRO_CHECK(!value.IsAtRest());
}

GYRO_TEST(Animatable, WorksOnAVectorChannel)
{
	Animatable<Pair> value{ Pair{ 3.0, -4.0 } };

	GYRO_CHECK(value.IsAtRest());
	GYRO_CHECK_EQ(value.Presentation(At(2.0)), (Pair{ 3.0, -4.0 }));

	value.AnimateTo(Pair{ 60.0, 80.0 }, Motion, Instant{});

	GYRO_CHECK_EQ(value.Model(), (Pair{ 60.0, 80.0 }));
	GYRO_CHECK_EQ(value.Presentation(Instant{}), (Pair{ 3.0, -4.0 }));

	// One spring with one settling criterion, on the magnitude. A channel cannot be two-thirds
	// settled, which is what three per-component springs would allow.
	const Instant settles = value.Coefficients().SettlesAt(Thresholds);

	GYRO_CHECK_EQ(value.NextWake(Instant{}, Thresholds), Wake::EveryFrame(Instant{}));
	GYRO_CHECK_EQ(value.NextWake(settles, Thresholds), Wake::Never());
	GYRO_CHECK(Magnitude(value.Presentation(settles) - value.Model()) <= Thresholds.Position);

	value.SetImmediate(Pair{ 1.0, 2.0 });

	GYRO_CHECK(value.IsAtRest());
	GYRO_CHECK_EQ(value.Presentation(At(5.0)), (Pair{ 1.0, 2.0 }));
}
