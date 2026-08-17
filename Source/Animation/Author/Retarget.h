#pragma once

#include <algorithm>
#include <concepts>
#include <numbers>

#include "Animation/Solve/Spring.h"
#include "Core/Time.h"

// Everything that produces spring coefficients.
//
// The split against Animation/Solve is by direction rather than by purity: Solve consumes
// coefficients, this produces them. That is worth more than it looks, because producing coefficients
// is definitionally dispatch-side work — Docs/Decisions.md decision 50 has the snapshot carry
// coefficients one way and the frame thread never write back — so the module boundary and the thread
// boundary are the same line, and the header-granularity check Docs/Structure.md defers can be
// written against the path alone.
//
// Interruption is the whole game, and closed form makes it four floats and no allocation. Read the
// current position and velocity, subtract the new target, stamp the origin. Velocity is preserved
// exactly, which is what lets a dismissed menu reverse out of wherever it had got to instead of
// jumping — and what lets a gesture hand its velocity off on release without a timeline to scrub.
//
// See Docs/Decisions.md decisions 11 and 12, and Docs/Animation.md#retargeting.

// The ranges configuration is clamped into. Arithmetic headroom rather than taste, and generous by
// a wide margin in both directions: the fastest response here is under a millisecond and the slowest
// is a minute, neither of which any catalog entry will be near. Every bound is a power of two, so
// the clamp introduces no rounding of its own.
//
// The damping floor is the load-bearing one. A damping ratio of zero is an undamped oscillator: it
// never settles, so the compositor never drops to idle and Docs/Architecture.md's hardest power
// invariant is lost to a configuration file. The floor is what makes settling a property of the type
// rather than of the input.
struct SpringLimits
{
	template<std::floating_point T>
	static constexpr T MinimumResponse = T(1) / T(1024);

	template<std::floating_point T>
	static constexpr T MaximumResponse = T(64);

	template<std::floating_point T>
	static constexpr T MinimumDamping = T(1) / T(64);

	template<std::floating_point T>
	static constexpr T MaximumDamping = T(64);
};

// Authoring units to stored units. Response is the natural period — how quickly it moves — and
// damping is how much it bounces; omega is 2*pi/response.
//
// Total, and clamping rather than fallible, for the reason Core/Time.h's DurationFromSeconds is:
// a system-layer compositor does not fail to start because someone fat-fingered a damping ratio.
// Values outside the range clamp into it and a value that is not a number falls back to this type's
// own default, per key rather than wholesale — which is what Docs/Animation.md#runtime-configuration
// asks for. The per-key fallback that matters is the constexpr catalog entry a configuration file
// overlays, and that belongs to the catalog rather than here.
template<std::floating_point T>
[[nodiscard]] constexpr SpringParameters<T> ParametersFromResponse(T response, T damping) noexcept
{
	SpringParameters<T> parameters{};

	// Self-comparison rather than std::isnan, which is not dependably constexpr across libraries.
	// Infinities need no case of their own; they clamp.
	if (response == response)
	{
		const T clamped = std::clamp(response, SpringLimits::MinimumResponse<T>, SpringLimits::MaximumResponse<T>);

		parameters.Frequency = T(2) * std::numbers::pi_v<T> / clamped;
	}

	if (damping == damping)
	{
		parameters.Damping = std::clamp(damping, SpringLimits::MinimumDamping<T>, SpringLimits::MaximumDamping<T>);
	}

	return parameters;
}

// A spring starting from a stated position and velocity. The velocity argument is not a convenience:
// a transition that begins under a finger inherits the gesture's velocity, and starting from rest
// when the content was already moving is the discontinuity retargeting exists to avoid.
template<SpringValue V>
[[nodiscard]] constexpr Spring<V>
Begin(SpringParameters<SpringScalar<V>> parameters, V position, V velocity, V target, Instant origin) noexcept
{
	return { origin, parameters, target, position - target, velocity };
}

// Retarget onto a new motion as well as a new target. This is what hot-reloading the catalog is:
// because the spring is closed form, applying new parameters mid-flight is an ordinary retarget from
// the current state, so live retuning produces no visible discontinuity even while things are moving.
//
// **A channel whose coordinates are a chart rather than a vector space does not come through here.**
// Rotation is that case: Docs/Animation.md#transforms springs it in the log map, and the log map is
// anchored at the target — so a new target is a new chart, and `Position - target` subtracts two
// vectors that live in different ones.
//
// Velocity is the half that would go wrong quietly. The stored velocity is a tangent vector at the
// old base point, and re-anchoring relates the two through the right-Jacobian of the exponential
// map: the correct new velocity is J_r^-1(u')*J_r(u)*v, and copying it across unchanged is wrong by
// roughly (u' - u) x v / 2. That is first order in the change of deviation rather than second, so a
// retarget moving the deviation by ninety degrees puts the angular velocity some thirty-eight
// degrees off course. Position stays continuous either way, so it does not read as a jump — it reads
// as the interruption having gone somewhere slightly wrong, which is harder to attribute.
//
// Geometry owns the chart and therefore owns all three conversions, so the rotation path is
//
//     u = Quaternion::Deviation(current, newTarget);
//     v = Quaternion::TransportVelocity(previousDeviation, u, previousVelocity);
//     Begin(parameters, u, v, {}, at);
//
// — the ordinary constructor above, against a target of zero, with nothing here needing to know a
// manifold was involved. The transport is two Jacobians and a matrix-vector product on the dispatch
// thread at event rate, which is nowhere near a budget. Docs/Animation.md#priorities' second
// priority is what makes that the right shape: the correct path is one named call rather than a
// discipline, so it is also the easy one.
template<SpringValue V>
[[nodiscard]] inline Spring<V>
Retarget(const Spring<V>& spring, Instant at, V target, SpringParameters<SpringScalar<V>> parameters) noexcept
{
	const SpringState<V> state = spring.Evaluate(at);

	return { at, parameters, target, state.Position - target, state.Velocity };
}

// The ordinary case: same motion, new destination.
//
// `at` is the originating input event's timestamp rather than the moment of handling, which is what
// makes a late commit render already in progress by exactly the elapsed amount instead of starting
// from zero. Lateness costs the first frame or two of an animation, never its shape.
template<SpringValue V>
[[nodiscard]] inline Spring<V> Retarget(const Spring<V>& spring, Instant at, V target) noexcept
{
	return Retarget(spring, at, target, spring.Parameters);
}

// Put the spring on its target, at rest, now.
//
// Not an optimization and not a convenience. Resume runs it over every spring in the system so that
// nobody wakes into the middle of yesterday's transition, and the snapshot atlas runs it to finish
// exits early when it is out of room — a path that has to work with no GPU at all, which is why it
// is arithmetic on coefficients rather than anything that touches a device.
//
// This one *is* safe on a chart, because the target is unchanged: zeroing the deviation lands on
// whatever the target already was, in the chart it was already anchored in.
template<SpringValue V>
[[nodiscard]] constexpr Spring<V> HardSettle(const Spring<V>& spring) noexcept
{
	return { spring.Origin, spring.Parameters, spring.Target, V{}, V{} };
}

// The contract everything downstream assumes. The runtime half waits on the test harness — but more
// of it is available here than in Animation/Solve, because authoring is arithmetic and evaluation is
// transcendental. Nothing below calls exp, sin, or sqrt.
static_assert(ParametersFromResponse(1.0, 0.5).Damping == 0.5);
static_assert(ParametersFromResponse(1.0, 0.5).Frequency == 2.0 * std::numbers::pi);
static_assert(
	ParametersFromResponse(2.0, 1.0).Frequency == std::numbers::pi,
	"Twice the response is half the frequency"
);

// Clamped rather than refused, at both ends and in both channels.
static_assert(ParametersFromResponse(0.0, 1.0).Frequency == ParametersFromResponse(1.0 / 1024.0, 1.0).Frequency);
static_assert(ParametersFromResponse(-3.0, 1.0).Frequency == ParametersFromResponse(1.0 / 1024.0, 1.0).Frequency);
static_assert(ParametersFromResponse(1e9, 1.0).Frequency == ParametersFromResponse(64.0, 1.0).Frequency);
static_assert(ParametersFromResponse(1.0, 0.0).Damping == SpringLimits::MinimumDamping<double>);
static_assert(ParametersFromResponse(1.0, -1.0).Damping == SpringLimits::MinimumDamping<double>);
static_assert(ParametersFromResponse(1.0, 1e9).Damping == SpringLimits::MaximumDamping<double>);
static_assert(ParametersFromResponse(1.0, 0.0).Damping > 0.0, "A spring that never settles is never idle");

// Infinities clamp to the same edges rather than needing a case of their own.
static_assert(
	ParametersFromResponse(std::numeric_limits<double>::infinity(), 1.0).Frequency ==
	ParametersFromResponse(64.0, 1.0).Frequency
);
static_assert(
	ParametersFromResponse(-std::numeric_limits<double>::infinity(), 1.0).Frequency ==
	ParametersFromResponse(1.0 / 1024.0, 1.0).Frequency
);

// Not a number falls back per key: the bad channel takes the default and the good one is kept.
static_assert(
	ParametersFromResponse(std::numeric_limits<double>::quiet_NaN(), 0.25) ==
	SpringParameters<double>{ SpringParameters<double>{}.Frequency, 0.25 }
);
static_assert(
	ParametersFromResponse(1.0, std::numeric_limits<double>::quiet_NaN()) ==
	SpringParameters<double>{ 2.0 * std::numbers::pi, SpringParameters<double>{}.Damping }
);

static_assert(Begin(SpringParameters<double>{}, 10.0, 2.0, 4.0, Instant{}).Offset == 6.0);
static_assert(Begin(SpringParameters<double>{}, 10.0, 2.0, 4.0, Instant{}).Velocity == 2.0);
static_assert(HardSettle(Begin(SpringParameters<double>{}, 10.0, 2.0, 4.0, Instant{})).Offset == 0.0);
static_assert(HardSettle(Begin(SpringParameters<double>{}, 10.0, 2.0, 4.0, Instant{})).Velocity == 0.0);
static_assert(
	HardSettle(Begin(SpringParameters<double>{}, 10.0, 2.0, 4.0, Instant{})).Target == 4.0,
	"Hard settling moves the spring to its target, not the target to the spring"
);
