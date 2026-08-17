#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <format>
#include <limits>
#include <numbers>
#include <type_traits>

#include "Core/Time.h"
#include "Core/Wake.h"

// The damped harmonic oscillator, in closed form.
//
// This half of Animation *consumes* coefficients and never produces them, which is why it is the
// half the frame thread may call. Everything that produces a spring — the catalog, retargeting,
// hard-settling — is Animation/Author, on the dispatch side of the publication boundary. The rule
// is the module split in Docs/Structure.md#threads-are-a-second-partition given a form a check can
// eventually key on: a frame-side translation unit including Animation/Author is a violation, and
// nothing here needs to know what is in those headers to say so.
//
// The usual spring integrates numerically, stepping state forward by a delta each frame. That is
// incompatible with three things this codebase is built on, and the third is the one that is easy
// to miss:
//
//   A missed frame yields accumulated error rather than the correct value, so lateness becomes
//   judder instead of latency.
//
//   Evaluation at an arbitrary predicted presentation time is inexpressible, so the same scene
//   cannot be evaluated for a 144 Hz panel and a 60 Hz projector in one iteration — which is the
//   whole of why every compositor with a mutable timeline drops the fast panel to the slow one.
//
//   Settling time cannot be answered without advancing the spring, so the compositor has to be
//   woken to discover it has nothing to do. Docs/Architecture.md#doing-nothing-must-cost-nothing
//   states the opposite as a hard invariant: no timer armed, the frame thread blocked indefinitely.
//
// See Docs/Decisions.md decision 11 and Docs/Animation.md#springs.

enum class SpringRegime : std::uint8_t
{
	Underdamped,
	Critical,
	Overdamped,
};

// The magnitude of a scalar channel, so that the concept below has one spelling for both cases.
// constexpr and free of <cmath>, which std::abs is not dependably either.
template<std::floating_point T>
[[nodiscard]] constexpr T Magnitude(T value) noexcept
{
	return value < T(0) ? -value : value;
}

namespace Detail
{
template<typename V>
struct ScalarType
{};

template<std::floating_point V>
struct ScalarType<V>
{
	using Type = V;
};

// Geometry spells this on every one of its types already, so a value type coming from there
// satisfies this half without being asked to add anything.
template<typename V>
	requires requires { typename V::Scalar; }
struct ScalarType<V>
{
	using Type = typename V::Scalar;
};
} // namespace Detail

template<typename V>
using SpringScalar = typename Detail::ScalarType<V>::Type;

namespace Detail
{
template<typename V>
concept HasScalar = requires { typename ScalarType<V>::Type; };
} // namespace Detail

// What a spring can be solved over.
//
// A vector space with a norm, and nothing more: the closed form is linear in the initial conditions,
// so every transcendental below is a function of scalar time and the value type only ever has to be
// added, subtracted, and scaled. That is what lets one solver serve a scalar opacity, a
// three-component translation, and a rotation in the log map without any of them being a special
// case — and it is why this header still depends on nothing but Core.
//
// **Why not three scalar springs per vector channel.** The trajectories would be identical, because
// the equation is decoupled and shared (omega, zeta, t0) make the components share a solution. What
// would not be identical is settling. Three per-component thresholds is an axis-dependent criterion,
// so the same rotation would settle at different moments depending on which way its axis pointed,
// and a channel could be two-thirds settled — which means nothing, and which the device-grid snap
// happens at. Docs/Animation.md#implementation-notes converts an angular threshold through the
// node's bounding radius, and that is a criterion on the magnitude. It can only be evaluated here.
template<typename V>
concept SpringValue =
	Detail::HasScalar<V> && std::floating_point<SpringScalar<V>> && requires(V value, SpringScalar<V> scalar) {
		{ value + value } -> std::same_as<V>;
		{ value - value } -> std::same_as<V>;
		{ value * scalar } -> std::same_as<V>;
		{ value / scalar } -> std::same_as<V>;
		{ Magnitude(value) } -> std::same_as<SpringScalar<V>>;
	};

namespace Detail
{
// Exact: halving a power of two is exact until it reaches subnormal, and the counts below are
// nowhere near. Written as a loop rather than reached for from <cmath> because ldexp is not
// constexpr in C++23 and this value has to be usable in a constant expression.
template<std::floating_point T>
[[nodiscard]] constexpr T Halved(int times) noexcept
{
	T value = T(1);

	for (int i = 0; i < times; ++i)
	{
		value /= T(2);
	}

	return value;
}

// Seconds between two instants, for the one computation in gyro that is obliged to leave integer
// nanoseconds: the oscillator is continuous and its solution is transcendental.
//
// Deliberately not Core/Time.h's ToSeconds, which is documented as egress for logs and human-facing
// values. This is arithmetic ingress, it clamps rather than extrapolating backwards, and it narrows
// through double so that the float instantiation gets a correctly rounded result instead of a cast
// of an already-rounded one.
//
// Evaluating before the origin is a caller error rather than a state to represent — t0 is an input
// event's timestamp and evaluation happens at predicted presentation time, so the ordering holds by
// construction — and a negative elapsed would run the exponential backwards into an overflow. It
// yields the initial state instead.
template<std::floating_point T>
[[nodiscard]] inline T SecondsSince(Instant origin, Instant now) noexcept
{
	const Duration elapsed = now - origin;

	if (elapsed <= Duration::zero())
	{
		return T(0);
	}

	return static_cast<T>(static_cast<double>(elapsed.count()) / 1'000'000'000.0);
}

// Instants saturate rather than wrapping. A spring that never settles reports an instant no frame
// will reach, and the alternative is a settle time in the deep past — the same failure the frame
// clock's Invalidate() exists to prevent, arriving from the animation side instead.
[[nodiscard]] inline Instant Saturated(Instant origin, Duration after) noexcept
{
	const std::int64_t base = origin.time_since_epoch().count();
	const std::int64_t offset = after.count();

	if (offset > 0 && base > std::numeric_limits<std::int64_t>::max() - offset)
	{
		return Instant{ Duration::max() };
	}

	return origin + after;
}

// How long an envelope of the given amplitude takes to decay below the threshold. The single shape
// all three regimes reduce to, once each has supplied its own amplitude and rate.
//
// Every guard here returns the conservative answer rather than the convenient one. A non-positive
// rate never decays; a non-positive threshold is never crossed; an amplitude already inside the
// threshold is settled now.
template<std::floating_point T>
[[nodiscard]] inline T DecayTime(T amplitude, T threshold, T rate) noexcept
{
	if (!(amplitude > threshold))
	{
		return T(0);
	}

	if (!(rate > T(0)) || !(threshold > T(0)))
	{
		return std::numeric_limits<T>::infinity();
	}

	return std::log(amplitude / threshold) / rate;
}
} // namespace Detail

// The band around unity damping inside which the critical form is used.
//
// It is a property of the scalar rather than a constant, and the derivation is what makes it one.
// Near unity the underdamped form's damped frequency vanishes, so B = (v0 + zwu0)/wd grows without
// bound while sin(wd t) shrinks to meet it. The limit is well defined and the arithmetic is not:
// at |1 - z| = d the relative error grows like eps/sqrt(d). Meanwhile the critical form is an
// approximation whose own error grows like d. Balancing the two gives d = eps^(2/3), and rounding
// the exponent down yields a band that is exact in binary and errs wide — toward the form that is
// well conditioned here rather than the one that is not.
//
// This is not a degenerate corner. "No bounce" is a damping ratio of one or above, so the catalog
// is expected to sit on or beside this band, and configuration clamps into it rather than being
// refused. The band's value is pinned by a trajectory-continuity test rather than by this comment.
template<std::floating_point T>
inline constexpr T CriticalBand = Detail::Halved<T>((std::numeric_limits<T>::digits - 1) * 2 / 3);

// Authored as (response, dampingRatio) and stored as (omega, zeta) — see Animation/Author, which is
// the only thing entitled to construct one. Response is the natural period and damping is how much
// it bounces; the two are legible and independent, which physical (stiffness, damping, mass) is not.
//
// Scalar whatever the channel's value type is. A vector channel is one spring with one motion, not
// one per component, which is what the concept above is about.
template<std::floating_point T>
struct SpringParameters
{
	T Frequency = T(1); // omega, radians per second
	T Damping = T(1);   // zeta, dimensionless

	friend constexpr bool operator==(SpringParameters, SpringParameters) noexcept = default;
};

// What evaluation yields. Both halves are wanted at once: retargeting needs the velocity to hand
// off, and computing it separately would evaluate the same transcendentals twice.
template<SpringValue V>
struct SpringState
{
	V Position{};
	V Velocity{};
};

// A spring, as it crosses the publication boundary.
//
// An aggregate deliberately, for the reason Core/Handle.h gives: this is a shape the frame side
// reconstitutes from bytes at an offset, not through a constructor. Docs/Decisions.md decision 50
// requires that what crosses is coefficients rather than evaluated values — publishing values would
// be cheaper and would collapse the scene back to a single timeline, which is invisible until a
// second monitor is plugged in.
//
// The invariants are Frequency > 0 and Damping > 0, established by Animation/Author and assumed
// here. Solve is a pure function of what it is handed; totality belongs at the ingest, which is
// ParametersFromResponse, exactly as Core/Time.h puts it at DurationFromSeconds.
template<SpringValue V>
struct Spring
{
	using Scalar = SpringScalar<V>;

	Instant Origin{};                      // t0
	SpringParameters<Scalar> Parameters{}; // omega, zeta
	V Target{};
	V Offset{};   // u0 = position(t0) - target
	V Velocity{}; // v0

	[[nodiscard]] constexpr SpringRegime Regime() const noexcept
	{
		// A damping ratio that is not a number lands here rather than in either transcendental
		// branch, and the critical form contains no reference to it — so the one input that could
		// poison a trajectory cannot.
		if (Parameters.Damping > Scalar(1) + CriticalBand<Scalar>)
		{
			return SpringRegime::Overdamped;
		}

		if (Parameters.Damping < Scalar(1) - CriticalBand<Scalar>)
		{
			return SpringRegime::Underdamped;
		}

		return SpringRegime::Critical;
	}

	// O(1), exact, and stateless. Evaluating at t1 and then at t2 gives the same answer as
	// evaluating at t2 alone, which is the property a missed frame is paid for with.
	[[nodiscard]] SpringState<V> Evaluate(Instant at) const noexcept
	{
		const Scalar t = Detail::SecondsSince<Scalar>(Origin, at);
		const Scalar w = Parameters.Frequency;
		const Scalar z = Parameters.Damping;
		const V u0 = Offset;
		const V v0 = Velocity;

		switch (Regime())
		{
			case SpringRegime::Underdamped:
			{
				const Scalar wd = w * std::sqrt(Scalar(1) - z * z);
				const Scalar decay = std::exp(-z * w * t);
				const Scalar cosine = std::cos(wd * t);
				const Scalar sine = std::sin(wd * t);
				const V b = (v0 + u0 * (z * w)) / wd;

				return {
					Target + (u0 * cosine + b * sine) * decay,
					(v0 * cosine - (u0 * (w * w) + v0 * (z * w)) * (sine / wd)) * decay,
				};
			}

			case SpringRegime::Critical:
			{
				const Scalar decay = std::exp(-w * t);
				const V c = v0 + u0 * w;

				return { Target + (u0 + c * t) * decay, (v0 - c * (w * t)) * decay };
			}

			case SpringRegime::Overdamped:
			{
				const Roots roots = SolveRoots();
				const V slowTerm = roots.C1 * std::exp(roots.Slow * t);
				const V fastTerm = roots.C2 * std::exp(roots.Fast * t);

				return { Target + slowTerm + fastTerm, slowTerm * roots.Slow + fastTerm * roots.Fast };
			}
		}

		return { Target, V{} };
	}

	// The instant after which the spring is inside both thresholds and stays there.
	//
	// Conservative in one direction on purpose, and the asymmetry is what sets the whole design of
	// this function. An answer later than the truth costs a few redundant composites at the tail of
	// an animation. An answer earlier than the truth drops the compositor to idle with something
	// still moving, and the user watches an animation freeze mid-flight. So every bound below is an
	// envelope the trajectory provably sits inside, never an estimate of where it actually is.
	//
	// Both thresholds are arguments rather than constants, because neither is this module's to
	// know: the geometric ones are in device pixels of the finest grid a node intersects, and the
	// policy for opacity, blur radius, and corner radius is an open question in Docs/Decisions.md.
	// Springs never arrive, so a position threshold alone leaves the system damaging forever at
	// sub-pixel amplitude.
	//
	// On a vector channel both thresholds are on the magnitude, which is the whole reason the solver
	// is generic over the value type rather than instantiated once per component.
	[[nodiscard]] Instant SettlesAt(Scalar positionEpsilon, Scalar velocityEpsilon) const noexcept
	{
		const Scalar w = Parameters.Frequency;
		const Scalar z = Parameters.Damping;
		const V u0 = Offset;
		const V v0 = Velocity;

		Scalar seconds = std::numeric_limits<Scalar>::infinity();

		switch (Regime())
		{
			case SpringRegime::Underdamped:
			{
				// The envelope is exact here rather than merely conservative. Writing the solution
				// as a single phasor, position is bounded by A*exp(-zwt) and velocity by
				// A*w*exp(-zwt) — the velocity amplitude being A*sqrt(z^2w^2 + wd^2), which
				// collapses to A*w. The only slack left is the phase within the final cycle.
				//
				// On a vector channel the same bound holds with A over the magnitudes, by
				// Cauchy-Schwarz on the (cos, sin) pair: ||u0*cos + b*sin|| <= sqrt(||u0||^2 + ||b||^2).
				const Scalar wd = w * std::sqrt(Scalar(1) - z * z);
				const V b = (v0 + u0 * (z * w)) / wd;
				const Scalar amplitude = std::hypot(Magnitude(u0), Magnitude(b));
				const Scalar rate = z * w;

				seconds = std::max(
					Detail::DecayTime(amplitude, positionEpsilon, rate),
					Detail::DecayTime(amplitude * w, velocityEpsilon, rate)
				);
				break;
			}

			case SpringRegime::Critical:
			{
				// The only regime that pays for the closed form. (u0 + Ct)*exp(-wt) < eps is
				// transcendental and has no closed solution, so the polynomial factor is absorbed
				// into a slower exponential: t*exp(-awt) <= 1/(awe) for any a in (0, 1), which
				// bounds the whole expression by a pure exponential decaying at (1-a)w.
				//
				// a = 1/2 keeps the arithmetic to one logarithm and costs up to twice the true
				// settling time in the worst case — around six tenths of a second of extra armed
				// timers on a half-second response, at the tail of an animation rather than in
				// steady-state idle. Tightening it means optimizing a or taking a Newton step from
				// here, and neither changes the property the tests assert.
				constexpr Scalar Alpha = Scalar(1) / Scalar(2);
				const V c = v0 + u0 * w;
				const Scalar reach = Magnitude(c) / (Alpha * w * std::numbers::e_v<Scalar>);
				const Scalar rate = (Scalar(1) - Alpha) * w;

				seconds = std::max(
					Detail::DecayTime(Magnitude(u0) + reach, positionEpsilon, rate),
					Detail::DecayTime(Magnitude(v0) + w * reach, velocityEpsilon, rate)
				);
				break;
			}

			case SpringRegime::Overdamped:
			{
				// Both terms decay, so the slower root bounds the sum. The only slack is the
				// triangle inequality, which is tight whenever the two coefficients share a sign.
				const Roots roots = SolveRoots();
				const Scalar rate = -roots.Slow;

				seconds = std::max(
					Detail::DecayTime(Magnitude(roots.C1) + Magnitude(roots.C2), positionEpsilon, rate),
					Detail::DecayTime(
						Magnitude(roots.C1) * -roots.Slow + Magnitude(roots.C2) * -roots.Fast, velocityEpsilon, rate
					)
				);
				break;
			}
		}

		return Detail::Saturated(Origin, DurationFromSeconds(static_cast<double>(seconds)));
	}

	// Defined in terms of SettlesAt rather than by evaluating and comparing, so the two cannot
	// disagree. An exact test would sometimes report settled before the conservative instant, which
	// is two answers to one question and the way the scheduler and the differ drift apart.
	[[nodiscard]] bool IsSettled(Instant at, Scalar positionEpsilon, Scalar velocityEpsilon) const noexcept
	{
		return at >= SettlesAt(positionEpsilon, velocityEpsilon);
	}

	// What this spring contributes to the schedule, which is what Core/Wake.h reduces over.
	//
	// A spring in flight cannot name a next interesting instant, because every instant between here
	// and settling is one — so the answer is a standing commitment to every frame, and then nothing.
	// This is the shape a boolean at this seam is adequate for and a next-instant is not, and it is
	// why the type has a third case rather than being an optional.
	//
	// The saturation in SettlesAt composes with this in the safe direction rather than by
	// arrangement. An undamped oscillator settles at an instant no frame reaches, so the comparison
	// is false forever and the answer stays continuous — correct, and visible to whatever prices it.
	// A representation whose "never again" could be spelled from a "never settles" would instead have
	// reported the one motion in the design that genuinely never stops as the one thing idle is
	// allowed to fold away.
	[[nodiscard]] Wake WakeAt(Instant at, Scalar positionEpsilon, Scalar velocityEpsilon) const noexcept
	{
		return IsSettled(at, positionEpsilon, velocityEpsilon) ? Wake::Never() : Wake::EveryFrame(at);
	}

private:
	struct Roots
	{
		Scalar Slow{};
		Scalar Fast{};
		V C1{};
		V C2{};
	};

	[[nodiscard]] Roots SolveRoots() const noexcept
	{
		const Scalar w = Parameters.Frequency;
		const Scalar z = Parameters.Damping;
		const Scalar discriminant = std::sqrt(z * z - Scalar(1));

		// The slow root is w(z - sqrt(z^2 - 1)), which is a subtraction of near-equals for large z
		// and loses most of its significant digits there. Multiplying above and below by
		// (z + sqrt(z^2 - 1)) gives an algebraically identical form with no cancellation in it — and
		// this is the value SettlesAt divides by, so the digits it loses are the ones that decide
		// when the compositor is allowed to stop drawing.
		const Scalar slow = -w / (z + discriminant);
		const Scalar fast = -w * (z + discriminant);

		// Spelled as the difference of the two roots rather than as its closed form 2w*sqrt(z^2 - 1),
		// so that C1 + C2 recovers u0 exactly rather than to within a rounding of the denominator.
		const Scalar span = slow - fast;

		return { slow, fast, (Velocity - Offset * fast) / span, (Offset * slow - Velocity) / span };
	}
};

// Prints as 12.5@-3.2/s. Both halves, because a spring that has reached its target with velocity
// left is the ordinary reading of a retarget and is indistinguishable from a settled one otherwise.
// No format spec is accepted.
//
// Constrained on the value type being printable rather than assuming it. The test harness decides
// whether to print a value by asking std::formattable, so an unconstrained formatter whose body
// happened not to compile would turn a helpful report into a hard error at the moment of failure.
//
// The context is a template parameter for the reason recorded at length in Core/Handle.h: naming
// std::format_context leaves std::format working and std::formattable false, so a value goes missing
// from a test failure at exactly the moment it was wanted.
template<SpringValue V>
	requires std::formattable<V, char>
struct std::formatter<SpringState<V>>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const SpringState<V>& state, Context& context) const
	{
		return std::format_to(context.out(), "{}@{}/s", state.Position, state.Velocity);
	}
};

// The contract everything downstream assumes. The runtime half waits on the test harness — the
// behavioural half of this file cannot be asserted here at all, because exp, sin, and sqrt are not
// constexpr until C++26. What is left is the layout, which is what the publication boundary cares
// about, and the regime classification, which is the one branch a wrong answer silently changes.
static_assert(SpringValue<float> && SpringValue<double>, "A scalar channel is a one-dimensional one");
static_assert(!SpringValue<int>, "The value type is a vector space over a floating-point field");
static_assert(!SpringValue<Instant>, "and an instant is not one");

static_assert(
	std::is_trivially_copyable_v<Spring<float>> && std::is_standard_layout_v<Spring<float>>,
	"A spring crosses the publication boundary as bytes at an offset"
);
static_assert(std::is_trivially_copyable_v<Spring<double>> && std::is_standard_layout_v<Spring<double>>);

// Two records rather than one, because Docs/Architecture.md#the-spaces puts positions across the
// boundary at double precision and everything else at single — and a vector channel is a third. The
// float form carries four bytes of tail padding, which the publisher must value-initialize rather
// than leave as whatever the arena held: a shared mapping makes uninitialized padding somebody
// else's business.
static_assert(sizeof(Spring<float>) == 32, "One int64 origin and five floats, padded to alignment");
static_assert(sizeof(Spring<double>) == 48, "One int64 origin and five doubles, exactly");
static_assert(std::formattable<SpringState<double>, char>, "A report prints the state rather than <unprintable>");

// Rounded down from eps^(2/3), so the band is exact in binary and wider rather than narrower.
static_assert(CriticalBand<float> == 0x1p-15f);
static_assert(CriticalBand<double> == 0x1p-34);
static_assert(static_cast<double>(CriticalBand<float>) > CriticalBand<double>, "Less precision wants a wider band");

static_assert(Magnitude(-3.5) == 3.5 && Magnitude(3.5) == 3.5 && Magnitude(0.0) == 0.0);

static_assert(Spring<double>{ .Parameters = { 1.0, 0.5 } }.Regime() == SpringRegime::Underdamped);
static_assert(Spring<double>{ .Parameters = { 1.0, 1.0 } }.Regime() == SpringRegime::Critical);
static_assert(Spring<double>{ .Parameters = { 1.0, 4.0 } }.Regime() == SpringRegime::Overdamped);
static_assert(
	Spring<double>{ .Parameters = { 1.0, 1.0 - CriticalBand<double> / 2 } }.Regime() == SpringRegime::Critical,
	"The band is entered from below"
);
static_assert(
	Spring<double>{ .Parameters = { 1.0, 1.0 + CriticalBand<double> / 2 } }.Regime() == SpringRegime::Critical,
	"and from above"
);
static_assert(
	Spring<double>{ .Parameters = { 1.0, std::numeric_limits<double>::quiet_NaN() } }.Regime() ==
		SpringRegime::Critical,
	"The one form that does not mention the damping ratio is where a NaN lands"
);
