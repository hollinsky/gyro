#include "Animation/Author/Retarget.h"

#include <cmath>
#include <limits>
#include <numbers>

#include "Testing/Test.h"

// Authoring is arithmetic, so most of its contract is a static_assert in the header and what remains
// here is the part that has to evaluate a spring to be checked at all.
//
// The claim under test throughout is exactness. Retargeting preserves velocity exactly rather than
// approximately, and that is not a nicety: it is the difference between a dismissed menu reversing
// smoothly out of wherever it had got to and one that visibly jumps at the moment the user changed
// their mind.

namespace
{
[[nodiscard]] Instant At(double seconds)
{
	return Monotonic::FromNanoseconds(static_cast<std::int64_t>(seconds * 1'000'000'000.0));
}

// Named once with the fields spelled out, which is what the threshold pair being a type buys: two
// adjacent scalars at a call site transpose silently, and the cost is a wrong settle instant.
constexpr SettleThresholds<double> Ordinary{ .Position = 1e-3, .Velocity = 1e-3 };
constexpr SettleThresholds<double> Tight{ .Position = 1e-9, .Velocity = 1e-9 };

[[nodiscard]] Spring<double> Moving()
{
	return Begin(ParametersFromResponse(0.4, 0.7), 100.0, -250.0, 0.0, Instant{});
}

// A minimal vector channel, declared here for the reason Solve's test declares one: authoring is
// generic over the value type, and the portable tier cannot reach a real one. This one carries a
// formatter, where Solve's deliberately does not — between them the constrained formatter in the
// header is exercised in both directions.
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
static_assert(std::formattable<SpringState<Pair>, char>, "Present when the value type is printable");

GYRO_TEST(Retarget, PreservesPositionAndVelocity)
{
	const Spring<double> spring = Moving();

	for (const double seconds : { 0.0, 0.05, 0.2, 0.6 })
	{
		const Instant at = At(seconds);
		const SpringState<double> before = spring.Evaluate(at);
		const Spring<double> retargeted = Retarget(spring, at, 400.0);
		const SpringState<double> after = retargeted.Evaluate(at);

		// Exact on the stored coefficients, which is where the claim actually lives: the new
		// spring's v0 *is* the old spring's evaluated velocity, and no arithmetic stands between
		// them. Reading either back out through Evaluate is a different question — target + offset
		// is not required to round back to the same double when the two differ by orders of
		// magnitude, and the overdamped branch reconstructs the velocity at its origin as
		// C1*r1 + C2*r2, whose two products round independently.
		GYRO_CHECK_EQ(retargeted.Velocity, before.Velocity);
		GYRO_CHECK_EQ(retargeted.Offset, before.Position - 400.0);
		GYRO_CHECK_EQ(retargeted.Origin, at);

		GYRO_CHECK(std::abs(after.Position - before.Position) <= 1e-9);
		GYRO_CHECK(std::abs(after.Velocity - before.Velocity) <= std::abs(before.Velocity) * 1e-12);
	}
}

// Retargeting onto the target a spring already has must be indistinguishable from not retargeting.
// A discontinuity here would mean the differ could not resolve a commit that touched a property
// without changing it, which it does constantly.
GYRO_TEST(Retarget, ToTheSameTargetChangesNothingVisible)
{
	const Spring<double> spring = Moving();
	const Instant at = At(0.15);
	const Spring<double> retargeted = Retarget(spring, at, spring.Target);

	for (const double seconds : { 0.0, 0.1, 0.3, 1.0 })
	{
		const Instant later = at + DurationFromSeconds(seconds);

		GYRO_CHECK(std::abs(retargeted.Evaluate(later).Position - spring.Evaluate(later).Position) <= 1e-9);
		GYRO_CHECK(std::abs(retargeted.Evaluate(later).Velocity - spring.Evaluate(later).Velocity) <= 1e-6);
	}
}

// Hot reloading the catalog is an ordinary retarget with new parameters, which is why live retuning
// produces no discontinuity even while things are moving.
GYRO_TEST(Retarget, AdoptsNewParametersWithoutADiscontinuity)
{
	const Spring<double> spring = Moving();
	const Instant at = At(0.1);
	const SpringState<double> before = spring.Evaluate(at);

	const SpringParameters<double> replacement = ParametersFromResponse(0.9, 1.4);
	const Spring<double> retargeted = Retarget(spring, at, spring.Target, replacement);

	GYRO_CHECK_EQ(retargeted.Parameters, replacement);
	GYRO_CHECK_EQ(retargeted.Velocity, before.Velocity);

	// A damping ratio of 1.4 puts the replacement in the overdamped branch, so the round trip back
	// out through Evaluate is a last-place difference rather than an identity. See above.
	GYRO_CHECK(std::abs(retargeted.Evaluate(at).Velocity - before.Velocity) <= std::abs(before.Velocity) * 1e-12);
}

GYRO_TEST(Begin, PlacesTheSpringWhereItWasAsked)
{
	const Spring<double> spring = Begin(ParametersFromResponse(0.5, 1.0), 30.0, 4.0, 10.0, At(2.0));

	GYRO_CHECK_EQ(spring.Offset, 20.0);
	GYRO_CHECK_EQ(spring.Velocity, 4.0);
	GYRO_CHECK_EQ(spring.Target, 10.0);
	GYRO_CHECK_EQ(spring.Evaluate(At(2.0)).Position, 30.0);
	GYRO_CHECK_EQ(spring.Evaluate(At(2.0)).Velocity, 4.0);
}

// Resume runs this over every spring in the system, and the snapshot atlas runs it to finish exits
// early when it is out of room. Both need it to be exact and both need it to work with no GPU at
// all, which is why it is arithmetic on coefficients and nothing else.
GYRO_TEST(HardSettle, LandsExactlyOnTheTarget)
{
	const Spring<double> settled = HardSettle(Moving());

	GYRO_CHECK(settled.IsSettled(settled.Origin, Tight));
	GYRO_CHECK_EQ(settled.SettlesAt(Tight), settled.Origin);

	for (const double seconds : { 0.0, 0.01, 5.0, 1e5 })
	{
		GYRO_CHECK_EQ(settled.Evaluate(At(seconds)).Position, settled.Target);
		GYRO_CHECK_EQ(settled.Evaluate(At(seconds)).Velocity, 0.0);
	}
}

// The clamping in the header is what makes settling a property of the type rather than of the input.
// A configuration file cannot produce a spring that never settles, which is the one nonsense value
// that would cost the idle invariant rather than merely looking wrong.
GYRO_TEST(ParametersFromResponse, ClampsNonsenseIntoSomethingThatSettles)
{
	constexpr double Nonsense[] = {
		0.0,
		-1.0,
		1e12,
		-1e12,
		std::numeric_limits<double>::infinity(),
		-std::numeric_limits<double>::infinity(),
		std::numeric_limits<double>::quiet_NaN(),
	};

	for (const double response : Nonsense)
	{
		for (const double damping : Nonsense)
		{
			const SpringParameters<double> parameters = ParametersFromResponse(response, damping);

			GYRO_REQUIRE(std::isfinite(parameters.Frequency) && parameters.Frequency > 0.0);
			GYRO_REQUIRE(std::isfinite(parameters.Damping) && parameters.Damping > 0.0);

			const Spring<double> spring = Begin(parameters, 250.0, -600.0, 0.0, Instant{});
			const Instant settled = spring.SettlesAt(Ordinary);

			GYRO_CHECK(settled != Instant{ Duration::max() });
			GYRO_CHECK(std::isfinite(spring.Evaluate(settled).Position));
		}
	}
}

// A response inside the range is passed through rather than clamped, which is the case the bounds
// exist to leave alone.
GYRO_TEST(ParametersFromResponse, LeavesOrdinaryValuesAlone)
{
	const SpringParameters<double> parameters = ParametersFromResponse(0.35, 0.82);

	GYRO_CHECK_EQ(parameters.Damping, 0.82);
	GYRO_CHECK(std::abs(parameters.Frequency - 2.0 * std::numbers::pi / 0.35) <= 1e-12);
}

// The whole authoring surface on a vector channel. Nothing here is a different code path — that is
// the claim: one spring per channel, whatever the channel's arity, so a rotation and an opacity are
// authored with the same four functions.
GYRO_TEST(Author, WorksOnAVectorChannel)
{
	constexpr Pair Start{ 100.0, -40.0 };
	constexpr Pair Speed{ -250.0, 12.0 };
	constexpr Pair Elsewhere{ 5.0, 5.0 };

	const Spring<Pair> spring = Begin(ParametersFromResponse(0.4, 0.7), Start, Speed, Pair{}, Instant{});

	GYRO_CHECK_EQ(spring.Offset, Start);
	GYRO_CHECK_EQ(spring.Velocity, Speed);

	const Instant at = At(0.1);
	const SpringState<Pair> before = spring.Evaluate(at);
	const Spring<Pair> retargeted = Retarget(spring, at, Elsewhere);

	// Exact on the stored coefficients, componentwise, exactly as in the scalar case.
	GYRO_CHECK_EQ(retargeted.Velocity, before.Velocity);
	GYRO_CHECK_EQ(retargeted.Offset, before.Position - Elsewhere);
	GYRO_CHECK_EQ(retargeted.Target, Elsewhere);

	const Spring<Pair> settled = HardSettle(spring);

	GYRO_CHECK_EQ(settled.Offset, Pair{});
	GYRO_CHECK_EQ(settled.Velocity, Pair{});
	GYRO_CHECK_EQ(settled.Evaluate(At(3.0)).Position, spring.Target);
	GYRO_CHECK(settled.IsSettled(settled.Origin, Tight));
}
