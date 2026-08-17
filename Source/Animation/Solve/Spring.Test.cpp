#include "Animation/Solve/Spring.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <string>

#include "Testing/Test.h"

// The interesting tests here do not compare against golden numbers, and that is deliberate: a table
// of expected outputs tests a transcription of the solution, while the ODE it claims to solve tests
// the solution. A sign error in one coefficient passes the first and fails the second.
//
// Every assertion is swept rather than sampled, because the failures this file exists to catch are
// all regime-dependent — a formula that is right for the ordinary damping ratio and wrong beside the
// critical band, or right for a small displacement and wrong when the initial velocity throws the
// trajectory well past its starting amplitude.
//
// Springs are constructed as aggregates rather than through Animation/Author. Solve does not depend
// on Author and neither does its test.

namespace
{
constexpr double Target = 12.0;

// Ordinary, fast, and two either side of the critical band — including one close enough to it that
// the overdamped form's root separation is nearly degenerate.
constexpr double Frequencies[] = { 0.8, 6.283, 25.0, 120.0 };
constexpr double Dampings[] = { 0.15, 0.5, 0.85, 0.9999999, 1.0, 1.0000001, 1.2, 3.0, 12.0 };
constexpr double Offsets[] = { -240.0, -1.0, 0.25, 60.0 };

// A large opposing velocity is the case a naive amplitude bound gets wrong: it sends the trajectory
// well past the initial displacement before the envelope takes hold.
constexpr double Velocities[] = { -900.0, 0.0, 7.0, 450.0 };

template<typename Body>
void Sweep(Body&& body)
{
	for (const double frequency : Frequencies)
	{
		for (const double damping : Dampings)
		{
			for (const double offset : Offsets)
			{
				for (const double velocity : Velocities)
				{
					body(Spring<double>{ Instant{}, { frequency, damping }, Target, offset, velocity });
				}
			}
		}
	}
}

// The scale the spring's own values are measured against, so a tolerance means the same thing for a
// quarter-pixel nudge and a 900-unit-per-second fling.
[[nodiscard]] double Amplitude(const Spring<double>& spring)
{
	return std::abs(spring.Offset) + std::abs(spring.Velocity) / spring.Parameters.Frequency;
}

// Long enough for the slowest member of the sweep to have decayed, expressed in the spring's own
// timescale rather than in seconds.
[[nodiscard]] double Span(const Spring<double>& spring)
{
	const double frequency = spring.Parameters.Frequency;
	const double constant = 1.0 / (spring.Parameters.Damping * frequency);
	const double period = 2.0 * std::numbers::pi / frequency;

	return 5.0 * std::max(constant, period);
}

[[nodiscard]] Instant At(double seconds)
{
	return Monotonic::FromNanoseconds(static_cast<std::int64_t>(seconds * 1'000'000'000.0));
}

// Tracks the worst violation across a sweep and reports it once. Sixteen hundred failing assertions
// say less than one that names the parameters it failed at.
class Worst
{
public:
	void Observe(double error, const Spring<double>& spring)
	{
		if (!(error > m_Error))
		{
			return;
		}

		m_Error = error;
		m_Where = std::format(
			"w={} z={} u0={} v0={}",
			spring.Parameters.Frequency,
			spring.Parameters.Damping,
			spring.Offset,
			spring.Velocity
		);
	}

	void Require(double tolerance, std::string_view what) const
	{
		if (m_Error > tolerance)
		{
			GYRO_FAIL(std::format("{}: {} exceeds {} at {}", what, m_Error, tolerance, m_Where));
		}
	}

private:
	double m_Error = 0.0;
	std::string m_Where;
};

// Stands in for Geometry's rotation vector, which Animation deliberately cannot reach: Solve depends
// on Core alone, and the requirement it places on a value type is algebraic rather than geometric.
// Anything satisfying this satisfies the solver, and the point of declaring it here rather than
// borrowing a real one is that the concept is checked against its own terms.
//
// Note what it does *not* have: a formatter. That is deliberate too — the constrained formatter in
// the header has to be absent rather than ill-formed for such a type, or the test harness turns a
// failure report into a compile error at the moment the value was wanted.
struct Triple
{
	using Scalar = double;

	double X{};
	double Y{};
	double Z{};

	friend constexpr Triple operator+(Triple left, Triple right) noexcept
	{
		return { left.X + right.X, left.Y + right.Y, left.Z + right.Z };
	}

	friend constexpr Triple operator-(Triple left, Triple right) noexcept
	{
		return { left.X - right.X, left.Y - right.Y, left.Z - right.Z };
	}

	friend constexpr Triple operator*(Triple value, double factor) noexcept
	{
		return { value.X * factor, value.Y * factor, value.Z * factor };
	}

	friend constexpr Triple operator/(Triple value, double divisor) noexcept
	{
		return { value.X / divisor, value.Y / divisor, value.Z / divisor };
	}

	friend constexpr bool operator==(Triple, Triple) noexcept = default;
};

[[nodiscard]] inline double Magnitude(Triple value) noexcept
{
	return std::sqrt(value.X * value.X + value.Y * value.Y + value.Z * value.Z);
}

static_assert(SpringValue<Triple>, "A vector space with a norm is all the solver asks for");
static_assert(std::is_same_v<SpringScalar<Triple>, double>, "The scalar comes from the value type's own typedef");
static_assert(!std::formattable<SpringState<Triple>, char>, "Absent rather than ill-formed");
} // namespace

// The one test that checks the mathematics rather than a transcription of it. Differentiating the
// closed form twice and substituting into u'' + 2*z*w*u' + w^2*u = 0 is what catches a sign error in
// a coefficient, and it is regime-blind: all three branches claim to solve the same equation.
GYRO_TEST(Spring, SatisfiesTheEquationOfMotion)
{
	Worst worst;

	Sweep([&](const Spring<double>& spring) {
		const double frequency = spring.Parameters.Frequency;
		const double damping = spring.Parameters.Damping;
		const double scale = frequency * frequency * Amplitude(spring);

		// Small against the spring's own period, and taken back out of the instants rather than
		// assumed, since the timebase is integer nanoseconds and the step is rounded into it.
		const Duration step = DurationFromSeconds(0.001 / frequency);
		const double h = static_cast<double>(step.count()) / 1'000'000'000.0;

		for (int sample = 1; sample <= 8; ++sample)
		{
			const Instant centre = At(Span(spring) * sample / 8.0);

			const double before = spring.Evaluate(centre - step).Position - Target;
			const double here = spring.Evaluate(centre).Position - Target;
			const double after = spring.Evaluate(centre + step).Position - Target;

			const double first = (after - before) / (2.0 * h);
			const double second = (after - 2.0 * here + before) / (h * h);

			const double residual = second + 2.0 * damping * frequency * first + frequency * frequency * here;

			worst.Observe(std::abs(residual) / scale, spring);
		}
	});

	worst.Require(1e-5, "ODE residual");
}

// Velocity is returned from its own closed form rather than differenced, so it is an independent
// claim and gets an independent check.
GYRO_TEST(Spring, VelocityIsTheDerivativeOfPosition)
{
	Worst worst;

	Sweep([&](const Spring<double>& spring) {
		const double scale = spring.Parameters.Frequency * Amplitude(spring);
		const Duration step = DurationFromSeconds(0.001 / spring.Parameters.Frequency);
		const double h = static_cast<double>(step.count()) / 1'000'000'000.0;

		for (int sample = 1; sample <= 8; ++sample)
		{
			const Instant centre = At(Span(spring) * sample / 8.0);

			const double before = spring.Evaluate(centre - step).Position;
			const double after = spring.Evaluate(centre + step).Position;
			const double differenced = (after - before) / (2.0 * h);

			worst.Observe(std::abs(spring.Evaluate(centre).Velocity - differenced) / scale, spring);
		}
	});

	worst.Require(1e-6, "analytic velocity against a central difference");
}

// At the origin every regime must reproduce exactly what it was handed. This is where a swapped
// coefficient in the overdamped constants shows up immediately, since C1 + C2 has to recover u0.
GYRO_TEST(Spring, StartsWhereItWasPut)
{
	Worst worst;

	Sweep([&](const Spring<double>& spring) {
		const SpringState<double> state = spring.Evaluate(spring.Origin);
		const double scale = Amplitude(spring);

		worst.Observe(std::abs(state.Position - (Target + spring.Offset)) / scale, spring);
		worst.Observe(std::abs(state.Velocity - spring.Velocity) / (scale * spring.Parameters.Frequency), spring);
	});

	worst.Require(1e-12, "initial conditions");
}

// What pins the band's value. The critical form is an approximation either side of unity damping,
// and the band is only correct if crossing it is invisible — from below, where the underdamped form
// is what it replaces, and from above, where the overdamped form is.
GYRO_TEST(Spring, IsContinuousAcrossTheCriticalBand)
{
	constexpr double Band = CriticalBand<double>;
	Worst worst;

	for (const double frequency : Frequencies)
	{
		for (const double offset : Offsets)
		{
			for (const double velocity : Velocities)
			{
				const auto make = [&](double damping) {
					return Spring<double>{ Instant{}, { frequency, damping }, Target, offset, velocity };
				};

				// Just inside the band against just outside it, on each side.
				const Spring<double> pairs[][2] = {
					{ make(1.0 - Band * 0.5), make(1.0 - Band * 1.5) },
					{ make(1.0 + Band * 0.5), make(1.0 + Band * 1.5) },
				};

				for (const auto& pair : pairs)
				{
					GYRO_REQUIRE(pair[0].Regime() == SpringRegime::Critical);
					GYRO_REQUIRE(pair[1].Regime() != SpringRegime::Critical);

					const double scale = Amplitude(pair[0]);

					for (int sample = 1; sample <= 8; ++sample)
					{
						const Instant at = At(Span(pair[0]) * sample / 8.0);
						const double inside = pair[0].Evaluate(at).Position;
						const double outside = pair[1].Evaluate(at).Position;

						worst.Observe(std::abs(inside - outside) / scale, pair[0]);
					}
				}
			}
		}
	}

	worst.Require(1e-7, "trajectory continuity across the band");
}

// The property decision 11 was bought for. Trivially true of a stateless form, which is exactly why
// it is worth an assertion: it is the invariant that would be silently lost the day somebody caches
// anything inside Evaluate.
GYRO_TEST(Spring, AMissedFrameCostsNothing)
{
	Sweep([](const Spring<double>& spring) {
		const Instant first = At(Span(spring) * 0.25);
		const Instant second = At(Span(spring) * 0.75);

		const SpringState<double> direct = spring.Evaluate(second);

		[[maybe_unused]] const SpringState<double> skipped = spring.Evaluate(first);
		const SpringState<double> after = spring.Evaluate(second);

		GYRO_CHECK(direct.Position == after.Position && direct.Velocity == after.Velocity);
	});
}

// The conservatism claim, stated as a property rather than compared against a reference. Evaluating
// at the instant the solver nominates and finding the spring still outside its thresholds is the
// failure that matters, because it is the one the user sees: the compositor drops to idle and an
// animation freezes mid-flight.
GYRO_TEST(Spring, SettlesNoEarlierThanItClaims)
{
	constexpr double PositionEpsilon = 1e-3;
	constexpr double VelocityEpsilon = 1e-3;

	Sweep([&](const Spring<double>& spring) {
		const Instant settled = spring.SettlesAt(PositionEpsilon, VelocityEpsilon);

		GYRO_REQUIRE(settled != Instant{ Duration::max() });
		GYRO_REQUIRE(settled >= spring.Origin);

		// At the nominated instant and everywhere after it, both thresholds hold. Sampled by
		// adding to the settle instant rather than by scaling the elapsed time and rebuilding
		// it, which would round-trip through double seconds and can land a nanosecond short of
		// the instant under test.
		for (const double multiple : { 0.0, 0.25, 1.0, 7.0 })
		{
			const Duration elapsed = settled - spring.Origin;
			const Instant at = settled + DurationFromSeconds(ToSeconds(elapsed) * multiple);
			const SpringState<double> state = spring.Evaluate(at);

			GYRO_CHECK(std::abs(state.Position - Target) <= PositionEpsilon);
			GYRO_CHECK(std::abs(state.Velocity) <= VelocityEpsilon);
			GYRO_CHECK(spring.IsSettled(at, PositionEpsilon, VelocityEpsilon));
		}
	});
}

// A spring already inside both thresholds is settled at its origin rather than at some later
// instant, which is what keeps a no-op commit from arming a timer.
GYRO_TEST(Spring, SettlesImmediatelyWhenItHasNowhereToGo)
{
	const Spring<double> spring{ Instant{}, { 6.283, 1.0 }, Target, 0.0, 0.0 };

	GYRO_CHECK_EQ(spring.SettlesAt(1e-3, 1e-3), spring.Origin);
	GYRO_CHECK(spring.IsSettled(spring.Origin, 1e-3, 1e-3));
	GYRO_CHECK_EQ(spring.Evaluate(At(10.0)).Position, Target);
}

// An undamped oscillator never settles, and saying so is the conservative answer. Returning any
// finite instant here would licence the frame thread to stop drawing something still in motion.
GYRO_TEST(Spring, NeverSettlesWithoutDamping)
{
	const Spring<double> spring{ Instant{}, { 6.283, 0.0 }, Target, 5.0, 0.0 };

	GYRO_CHECK_EQ(spring.SettlesAt(1e-3, 1e-3), Instant{ Duration::max() });
	GYRO_CHECK(!spring.IsSettled(At(1e6), 1e-3, 1e-3));
}

// What the spring hands the scheduler, swept for the one property the fold depends on: a spring that
// is not settled must contribute something the reduction cannot fold away.
GYRO_TEST(Spring, ContributesEveryFrameUntilItSettlesAndNothingAfterwards)
{
	constexpr double PositionEpsilon = 1e-3;
	constexpr double VelocityEpsilon = 1e-3;

	Sweep([&](const Spring<double>& spring) {
		const Instant settled = spring.SettlesAt(PositionEpsilon, VelocityEpsilon);

		GYRO_REQUIRE(settled > spring.Origin);

		const Instant before = settled - Duration{ 1 };
		const Wake inFlight = spring.WakeAt(before, PositionEpsilon, VelocityEpsilon);

		GYRO_CHECK_EQ(inFlight, Wake::EveryFrame(before));
		GYRO_CHECK(inFlight.IsDue(before));
		GYRO_CHECK_EQ(Sooner(inFlight, Wake::Never()), inFlight);

		GYRO_CHECK_EQ(spring.WakeAt(settled, PositionEpsilon, VelocityEpsilon), Wake::Never());
		GYRO_CHECK_EQ(spring.WakeAt(settled + Duration{ 1 }, PositionEpsilon, VelocityEpsilon), Wake::Never());
	});
}

// The composition the third case exists for. SettlesAt saturates for an undamped oscillator, and the
// comparison against a saturated instant is false forever — so the one motion in the design that
// genuinely never stops contributes a standing commitment rather than an absence of one, and folding
// it against an otherwise idle scene cannot produce idle.
GYRO_TEST(Spring, NeverSettlingContributesAStandingCommitmentRatherThanIdle)
{
	const Spring<double> spring{ Instant{}, { 6.283, 0.0 }, Target, 5.0, 0.0 };

	for (const double seconds : { 0.0, 1.0, 1e6 })
	{
		const Wake wake = spring.WakeAt(At(seconds), 1e-3, 1e-3);

		GYRO_CHECK_EQ(wake.Which, Wake::Kind::Continuous);
		GYRO_CHECK(wake.IsDue(At(seconds)));
		GYRO_CHECK_EQ(Sooner(Wake::Never(), wake).Which, Wake::Kind::Continuous);
	}
}

// Evaluating before the origin runs the exponential backwards into an overflow, so it yields the
// initial state instead. The ordering holds by construction — t0 is an input event's timestamp and
// evaluation happens at predicted presentation time — which is what makes this a guard rather than
// a case.
GYRO_TEST(Spring, DoesNotRunBackwards)
{
	const Spring<double> spring{ At(100.0), { 6.283, 0.7 }, Target, 5.0, -3.0 };
	const SpringState<double> before = spring.Evaluate(At(90.0));

	GYRO_CHECK_EQ(before.Position, Target + spring.Offset);
	GYRO_CHECK_EQ(before.Velocity, spring.Velocity);
}

// The envelope SettlesAt divides by has to actually bound the trajectory. Checking it directly is
// what separates "the bound is conservative" from "the bound happened to be conservative for the
// cases in the sweep".
GYRO_TEST(Spring, StaysInsideItsEnvelope)
{
	Worst worst;

	Sweep([&](const Spring<double>& spring) {
		if (spring.Regime() != SpringRegime::Underdamped)
		{
			return;
		}

		const double frequency = spring.Parameters.Frequency;
		const double damping = spring.Parameters.Damping;
		const double damped = frequency * std::sqrt(1.0 - damping * damping);
		const double b = (spring.Velocity + damping * frequency * spring.Offset) / damped;
		const double amplitude = std::sqrt(spring.Offset * spring.Offset + b * b);

		for (int sample = 0; sample <= 16; ++sample)
		{
			const double seconds = Span(spring) * sample / 16.0;
			const SpringState<double> state = spring.Evaluate(At(seconds));
			const double envelope = amplitude * std::exp(-damping * frequency * seconds);

			worst.Observe((std::abs(state.Position - Target) - envelope) / Amplitude(spring), spring);
			worst.Observe((std::abs(state.Velocity) - envelope * frequency) / Amplitude(spring), spring);
		}
	});

	worst.Require(1e-12, "trajectory outside its own envelope");
}

// Nothing in the sweep, at any instant, produces a value the renderer cannot use. Infinities count:
// a position of infinity crossing the publication boundary is a geometry bug that arrives as a
// blank screen rather than as a wrong one.
GYRO_TEST(Spring, ProducesNoNonFiniteValues)
{
	Sweep([](const Spring<double>& spring) {
		for (const double seconds : { 0.0, 1e-9, 0.5, 30.0, 1e4 })
		{
			const SpringState<double> state = spring.Evaluate(At(seconds));

			GYRO_CHECK(std::isfinite(state.Position) && std::isfinite(state.Velocity));
		}

		GYRO_CHECK(spring.SettlesAt(1e-3, 1e-3) >= spring.Origin);
		GYRO_CHECK(spring.SettlesAt(0.0, 0.0) == Instant{ Duration::max() });
	});
}

// The precision the position channel does not use. Agreement between the two instantiations is what
// catches a formula that is only well conditioned at double — which is the specific risk taken on by
// templating rather than fixing the type.
//
// The tolerance is the band's, not the epsilon's: float's critical band is some five orders of
// magnitude wider than double's, so near unity damping the two instantiations are legitimately
// solving with different forms, and their disagreement is bounded by that width rather than by
// rounding.
GYRO_TEST(Spring, AgreesBetweenPrecisions)
{
	Worst worst;

	Sweep([&](const Spring<double>& wide) {
		const Spring<float> narrow{
			wide.Origin,
			{ static_cast<float>(wide.Parameters.Frequency), static_cast<float>(wide.Parameters.Damping) },
			static_cast<float>(wide.Target),
			static_cast<float>(wide.Offset),
			static_cast<float>(wide.Velocity),
		};

		const double scale = Amplitude(wide);

		for (int sample = 0; sample <= 8; ++sample)
		{
			const Instant at = At(Span(wide) * sample / 16.0);
			const double narrowed = static_cast<double>(narrow.Evaluate(at).Position);

			worst.Observe(std::abs(wide.Evaluate(at).Position - narrowed) / scale, wide);
		}
	});

	worst.Require(1e-3, "float against double");
}

// One spring per channel, not one per component. The trajectories are identical because the equation
// is decoupled and a shared (omega, zeta, t0) gives every component the same solution — bitwise
// identical, in fact, since the vector form evaluates exactly the scalar expressions componentwise.
//
// This is the half of the vector case that three scalar springs would have got right, and asserting
// it is what makes the next test's disagreement mean something.
GYRO_TEST(SpringVector, MatchesThreeScalarSpringsSharingOneMotion)
{
	constexpr SpringParameters<double> Parameters{ 9.0, 0.55 };
	constexpr Triple Origin{ 0.4, -1.2, 2.5 };
	constexpr Triple Speed{ -14.0, 3.0, 0.0 };

	const Spring<Triple> vector{ Instant{}, Parameters, Triple{}, Origin, Speed };
	const Spring<double> components[] = {
		{ Instant{}, Parameters, 0.0, Origin.X, Speed.X },
		{ Instant{}, Parameters, 0.0, Origin.Y, Speed.Y },
		{ Instant{}, Parameters, 0.0, Origin.Z, Speed.Z },
	};

	for (const double seconds : { 0.0, 0.01, 0.2, 0.75, 3.0 })
	{
		const Instant at = At(seconds);
		const SpringState<Triple> state = vector.Evaluate(at);

		GYRO_CHECK_EQ(state.Position.X, components[0].Evaluate(at).Position);
		GYRO_CHECK_EQ(state.Position.Y, components[1].Evaluate(at).Position);
		GYRO_CHECK_EQ(state.Position.Z, components[2].Evaluate(at).Position);

		GYRO_CHECK_EQ(state.Velocity.X, components[0].Evaluate(at).Velocity);
		GYRO_CHECK_EQ(state.Velocity.Y, components[1].Evaluate(at).Velocity);
		GYRO_CHECK_EQ(state.Velocity.Z, components[2].Evaluate(at).Velocity);
	}
}

// And the half they would have got wrong, which is the whole argument for generalizing the solver
// rather than instantiating it three times.
//
// A component is smaller than the vector it belongs to, so a per-component threshold is crossed
// first — by a factor of root three when the displacement is evenly spread, and by an axis-dependent
// amount otherwise. Settling per component would therefore call a rotation finished while it was
// still visibly moving, and would do it at a different moment depending on which way the axis
// pointed. The device-grid snap happens at that moment.
GYRO_TEST(SpringVector, SettlesOnTheMagnitudeAndNotOnAComponent)
{
	constexpr double Epsilon = 1e-3;
	constexpr SpringParameters<double> Parameters{ 9.0, 0.55 };

	// Same magnitude, different axis. The magnitude criterion cannot tell these apart, and that is
	// the property being asserted: a per-component rule sees a largest component of four in one and
	// five in the other, and would finish them at different moments for no reason but where the axis
	// happened to point.
	const Spring<Triple> spread{ Instant{}, Parameters, Triple{}, Triple{ 3.0, 4.0, 0.0 }, Triple{} };
	const Spring<Triple> along{ Instant{}, Parameters, Triple{}, Triple{ 0.0, 0.0, 5.0 }, Triple{} };

	// To within the norm's own rounding, since sqrt(9 + 16) and sqrt(25) are not obliged to be the
	// same double even though both are five.
	GYRO_CHECK(std::abs(ToSeconds(spread.SettlesAt(Epsilon, Epsilon) - along.SettlesAt(Epsilon, Epsilon))) <= 1e-6);

	// And an evenly spread displacement crosses the threshold as a vector strictly later than any one
	// of its components does alone — by exactly the root three between their amplitudes, since the
	// bound is logarithmic in the amplitude and the two share a decay rate. A per-component rule
	// would stop the compositor that much before the channel had stopped moving.
	const double component = 5.0 / std::sqrt(3.0);
	const Spring<Triple> vector{ Instant{}, Parameters, Triple{}, Triple{ component, component, component }, Triple{} };
	const Spring<double> single{ Instant{}, Parameters, 0.0, component, 0.0 };

	const Instant whole = vector.SettlesAt(Epsilon, Epsilon);
	const Instant part = single.SettlesAt(Epsilon, Epsilon);
	const double gap = std::log(std::sqrt(3.0)) / (Parameters.Damping * Parameters.Frequency);

	GYRO_CHECK(whole > part);
	GYRO_CHECK(!vector.IsSettled(part, Epsilon, Epsilon));
	GYRO_CHECK(std::abs(ToSeconds(whole - part) - gap) <= 1e-6);

	// And the criterion the solver actually applies holds, at the instant it nominates and after it.
	for (const double multiple : { 0.0, 1.0, 5.0 })
	{
		const Instant at = whole + DurationFromSeconds(ToSeconds(whole - vector.Origin) * multiple);
		const SpringState<Triple> state = vector.Evaluate(at);

		GYRO_CHECK(Magnitude(state.Position - vector.Target) <= Epsilon);
		GYRO_CHECK(Magnitude(state.Velocity) <= Epsilon);
	}
}
