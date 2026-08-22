#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <format>
#include <limits>
#include <numbers>
#include <type_traits>

#include "Geometry/AxisTransform.h"
#include "Geometry/Space.h"

// The node transform: three dimensions, decomposed, about an explicit anchor.
//
// A scene node stores translation, rotation, and scale about an anchor point, and a matrix is a
// *product composed at render time* rather than a member. That is not a storage preference.
// Matrices are never interpolated — lerping two of them shears the result and collapses it through
// configurations that are not rotations at all — so the decomposition is the only form in which a
// transform can be animated, and it is what lets position be springier than scale. See
// Docs/Decisions.md decisions 17 and 55, and Docs/Animation.md#transforms.
//
// **The anchor is load-bearing.** Scale and rotation leave it exactly fixed, so it is what makes a
// window grow out of the corner it was summoned from rather than out of its own middle — which is
// most of what makes a transition read as intentional rather than as a thing getting bigger.
// Decision 17 left it under-specified even in two dimensions; decision 55 makes it explicit.
//
// **Three dimensions from the first node**, per decision 55, and not because anything shipping today
// needs a third axis. Widening a two-dimensional transform later means revisiting every spring,
// every hit test, and every damage bound at once, whereas a 4x4 through a pipeline that is already a
// 3D pipeline costs nothing. Perspective is node-local and **there is no camera**: a per-output
// frustum would project a window straddling a seam differently on each output and change its shape
// across the seam, on precisely the configuration per-output evaluation exists to serve.
//
// **What is deliberately excluded is intersection.** Composition is strict tree order with no depth
// buffer, so two nodes whose quads pass through one another do not render correctly. That is a
// stated limit rather than an oversight: the failure a depth buffer would leave behind is
// order-dependent transparency, every surface here has alpha with a backdrop reading through it, and
// excluding intersection is what makes everything else here affordable. Nothing in this header
// should be read as working toward one, and a reader who assumes the omission is a gap will build
// the wrong thing on top of it.
//
// This is the *general* transform, the one a node carries while something is in flight. The
// restricted axis-aligned form — ninety-degree rotations, flip, positive scale, translation — is a
// separate type belonging to the output and panel adapters, and the classification that decides
// which rung a composed transform sits on lives with that type rather than here. See
// Docs/Architecture.md#resample-once-and-know-when-it-is-zero.
//
// **The edge to that header runs one way and only at the bottom of this one.** `ComposedTransform`
// below is seeded with the output adapter, because that is where an output's origin folds against a
// global-space translation while both are still double — so this file names `AxisTransform` to build
// a matrix out of one, and never to classify anything as one.
//
// Precision follows Geometry/Space.h: positions cross the publication boundary at double and
// everything else at single, so translation is a double triple and every other channel is float.
//
// The axes, stated once because every sign below depends on them. Global space is Y-down, which is
// Wayland's convention and Vulkan's clip space alike, and the basis is right-handed — so **+Z points
// into the screen**, away from the viewer, and the eye sits on the -Z side. A positive rotation
// turns counter-clockwise seen from the positive end of its axis looking back at the origin, which
// about +Z reads as clockwise on screen. Getting this wrong is invisible until something rotates the
// wrong way, so it is written down rather than inferred from a test.

template<std::floating_point T>
struct Vector3
{
	using Scalar = T;

	// An aggregate, for Geometry/Space.h's reason: this is the shape that crosses the publication
	// boundary, and the frame side reconstitutes it from bytes at an offset rather than through a
	// constructor.
	T X{};
	T Y{};
	T Z{};

	friend constexpr bool operator==(Vector3, Vector3) noexcept = default;

	friend constexpr Vector3 operator+(Vector3 left, Vector3 right) noexcept
	{
		return { left.X + right.X, left.Y + right.Y, left.Z + right.Z };
	}

	friend constexpr Vector3 operator-(Vector3 left, Vector3 right) noexcept
	{
		return { left.X - right.X, left.Y - right.Y, left.Z - right.Z };
	}

	friend constexpr Vector3 operator-(Vector3 value) noexcept { return { -value.X, -value.Y, -value.Z }; }

	friend constexpr Vector3 operator*(Vector3 value, T factor) noexcept
	{
		return { value.X * factor, value.Y * factor, value.Z * factor };
	}

	// Division rather than multiplication by the reciprocal, and it is here because a spring needs
	// it. Animation/Solve/Spring.h's SpringValue asks for both, since the closed form divides by the
	// natural frequency and forming 1/omega first loses a bit at every scale where it matters least
	// — a near-settled channel, whose remaining offset is what decides the settle instant.
	friend constexpr Vector3 operator/(Vector3 value, T divisor) noexcept
	{
		return { value.X / divisor, value.Y / divisor, value.Z / divisor };
	}
};

template<std::floating_point T>
[[nodiscard]] constexpr T Dot(Vector3<T> left, Vector3<T> right) noexcept
{
	return left.X * right.X + left.Y * right.Y + left.Z * right.Z;
}

template<std::floating_point T>
[[nodiscard]] constexpr Vector3<T> Cross(Vector3<T> left, Vector3<T> right) noexcept
{
	return { left.Y * right.Z - left.Z * right.Y,
		     left.Z * right.X - left.X * right.Z,
		     left.X * right.Y - left.Y * right.X };
}

template<std::floating_point T>
[[nodiscard]] inline T Length(Vector3<T> value) noexcept
{
	return std::sqrt(Dot(value, value));
}

// The same number under the name Animation/Solve/Spring.h's SpringValue asks for, which is what makes
// a three-component channel solvable at all.
//
// **Two names for one quantity, deliberately, and this is the cheaper direction.** `Length` is the
// geometric name and stays the one geometry uses. `Magnitude` is the solver's concept spelling, and
// the concept reaches it by argument-dependent lookup — so the adapter has to sit beside the type
// rather than beside the solver, and Solve keeps depending on nothing but Core. The alternative is a
// trait specialisation inside Animation naming Vector3, which is the edge Docs/Structure.md refuses:
// the frame half of Animation may not say Geometry.
//
// It is a whole-vector norm rather than a per-component one because settling is per channel and not
// per component (Docs/Decisions.md decision 17). A per-component criterion is axis-dependent, and its
// visible form is the same rotation finishing at different moments depending on where its axis
// happened to point.
template<std::floating_point T>
[[nodiscard]] inline T Magnitude(Vector3<T> value) noexcept
{
	return Length(value);
}

// Named conversions rather than an implicit narrowing, for the reason Geometry/Space.h gives about
// the real and grid forms of one space: the width of a coordinate is a decision somebody made, and
// -Wdouble-promotion is only able to catch the accidental half of it if the deliberate half is
// spelled.
[[nodiscard]] constexpr Vector3<double> Widen(Vector3<float> value) noexcept
{
	return { static_cast<double>(value.X), static_cast<double>(value.Y), static_cast<double>(value.Z) };
}

[[nodiscard]] constexpr Vector3<float> Narrow(Vector3<double> value) noexcept
{
	return { static_cast<float>(value.X), static_cast<float>(value.Y), static_cast<float>(value.Z) };
}

// **The animatable rotation channel.** Axis times angle, in radians, so its magnitude *is* the
// rotation angle — which is what Docs/Animation.md#implementation-notes assumes when it converts an
// angular residual to pixels through the node's bounding radius: a residual of e displaces a corner
// by roughly e*r. That costs a factor of two against the textbook quaternion logarithm, which is
// half-angle; the settling threshold is worth more than the convention.
using RotationVector = Vector3<float>;

namespace Detail
{
// Below this, a quaternion's vector part and a rotation vector's length are indistinguishable from
// their own series expansions in single precision. The relative error of the truncated series at the
// seam is about t^2/3 ~ 3e-9 for the log and t^2/24 for the exponential, both under float's 6e-8
// epsilon, so the two branches agree to the last bit where they meet rather than stepping.
inline constexpr float SmallVector = 1.0e-4F;
inline constexpr float SmallAngle = 1.0e-4F;

// The seam for the two Jacobians below, and it sits two orders of magnitude higher than the ones
// above because the cancellation it avoids is worse: t - sin(t) and 1 - (t/2)cot(t/2) are both
// differences of quantities that agree to their first several digits, and by t = 0.01 they have lost
// enough of them to matter. Both coefficients multiply [u]x^2, which is itself O(t^2), so the series
// and the closed form land within an ulp of the identity term where they meet — the seam is invisible
// in the answer rather than merely small.
inline constexpr float SmallJacobianAngle = 1.0e-2F;

// The right Jacobian of SO(3) and its inverse, applied to a vector rather than assembled as a
// matrix. A matrix type here would be a matrix somebody eventually stores, and three cross products
// is the cheaper half of the trade anyway.
//
//     J(u)      = I - a(t)[u]x + b(t)[u]x^2         t = |u|
//     J^-1(u)   = I + (1/2)[u]x + c(t)[u]x^2
//
//     a(t) = (1 - cos t) / t^2  =  (1/2)(sin(t/2) / (t/2))^2
//     b(t) = (t - sin t) / t^3
//     c(t) = (1 - (t/2)cot(t/2)) / t^2
//
// a and c are spelled by half angles rather than in the form the literature prints — (1 - cos t) and
// (1 + cos t) over 2t sin t. Both of those lose every significant digit somewhere inside this
// function's domain rather than outside it: the first near the identity, which is where a settling
// spring lives, and the second near half a turn, which is an exactly opposite target and an ordinary
// thing for a shell to ask for.
//
// **Well conditioned across the whole domain.** J^-1 goes singular only at |u| = 2*pi, and Log
// canonicalizes to |u| <= pi, so the worst conditioning reachable here is the factor of pi/2 at
// exactly half a turn. A reader who knows that the exponential map has a singularity will look for a
// guard; there is nothing to guard.
[[nodiscard]] inline Vector3<float> ApplyRightJacobian(RotationVector deviation, Vector3<float> rate) noexcept
{
	const float angle = Length(deviation);
	const Vector3<float> once = Cross(deviation, rate);
	const Vector3<float> twice = Cross(deviation, once);

	if (angle < SmallJacobianAngle)
	{
		const float squared = angle * angle;

		return rate - once * (0.5F - squared / 24.0F) + twice * (1.0F / 6.0F - squared / 120.0F);
	}

	const float half = angle * 0.5F;
	const float sinc = std::sin(half) / half;

	return rate - once * (0.5F * sinc * sinc) + twice * ((angle - std::sin(angle)) / (angle * angle * angle));
}

[[nodiscard]] inline Vector3<float> ApplyInverseRightJacobian(RotationVector deviation, Vector3<float> rate) noexcept
{
	const float angle = Length(deviation);
	const Vector3<float> once = Cross(deviation, rate);
	const Vector3<float> twice = Cross(deviation, once);

	if (angle < SmallJacobianAngle)
	{
		const float squared = angle * angle;

		return rate + once * 0.5F + twice * (1.0F / 12.0F + squared / 720.0F);
	}

	const float half = angle * 0.5F;

	return rate + once * 0.5F + twice * ((1.0F - half * std::cos(half) / std::sin(half)) / (angle * angle));
}
} // namespace Detail

// Rotation, as a unit quaternion. Euler triples gimbal and interpolate along non-shortest paths, and
// both present as a window travelling a visibly strange route — the failure mode hardest to
// attribute to its cause, because nothing about the code that produced it looks wrong. Decision 55
// rejects them for that.
//
// Scalar part first, which is the mathematical convention and makes the identity read as
// { 1, 0, 0, 0 }. The order is pinned by a static_assert at the foot of this header rather than left
// to whoever writes the shader's uniform upload.
struct Quaternion
{
	float W = 1.0F;
	float X = 0.0F;
	float Y = 0.0F;
	float Z = 0.0F;

	// **Representational, not rotational.** q and -q are the same rotation and compare unequal here,
	// deliberately: this is the comparison that answers "are these the same bytes", which is what
	// dirty tracking wants once it compares Canonical() forms. "Are these the same rotation" is
	// Deviation's magnitude, and conflating the two would make a node dirty every frame it drifted
	// across the double cover.
	friend constexpr bool operator==(Quaternion, Quaternion) noexcept = default;

	[[nodiscard]] constexpr Vector3<float> Vector() const noexcept { return { X, Y, Z }; }

	[[nodiscard]] constexpr Quaternion Conjugate() const noexcept { return { W, -X, -Y, -Z }; }

	// The representative with a non-negative scalar part, which is the one whose log map is the
	// shortest turn. Publishing this form is what keeps the double cover out of every downstream
	// comparison.
	[[nodiscard]] constexpr Quaternion Canonical() const noexcept
	{
		return W < 0.0F ? Quaternion{ -W, -X, -Y, -Z } : *this;
	}

	[[nodiscard]] inline float Length() const noexcept { return std::sqrt(W * W + X * X + Y * Y + Z * Z); }

	// A zero quaternion is not a rotation, and reaching here with one means somebody published
	// uninitialized bytes. Identity rather than a NaN, because a NaN entering the geometry poisons
	// the damage bound and therefore the whole output, which is a far harder failure to trace back
	// than a node that did not turn.
	[[nodiscard]] inline Quaternion Normalized() const noexcept
	{
		const float length = Length();

		if (!(length > 0.0F))
		{
			return {};
		}

		const float inverse = 1.0F / length;

		return { W * inverse, X * inverse, Y * inverse, Z * inverse };
	}

	// The rotation angle in radians, in [0, pi]. Taken about the canonical representative, so a
	// rotation of 350 degrees reports 10.
	[[nodiscard]] inline float Angle() const noexcept
	{
		return 2.0F * std::atan2(std::sqrt(X * X + Y * Y + Z * Z), std::abs(W));
	}

	// Hamilton product. Composition is right-to-left as usual: (a * b) applies b first, in a's frame.
	friend constexpr Quaternion operator*(Quaternion left, Quaternion right) noexcept
	{
		return { left.W * right.W - left.X * right.X - left.Y * right.Y - left.Z * right.Z,
			     left.W * right.X + left.X * right.W + left.Y * right.Z - left.Z * right.Y,
			     left.W * right.Y - left.X * right.Z + left.Y * right.W + left.Z * right.X,
			     left.W * right.Z + left.X * right.Y - left.Y * right.X + left.Z * right.W };
	}

	// Constant-expression friendly, because it is nothing but multiplies and adds — which is what
	// lets the axis-aligned cases of the contract be static_asserts rather than tests.
	[[nodiscard]] constexpr Vector3<float> Rotate(Vector3<float> value) const noexcept
	{
		const Vector3<float> axis{ X, Y, Z };
		const Vector3<float> doubled = Cross(axis, value) * 2.0F;

		return value + doubled * W + Cross(axis, doubled);
	}

	// **The log map.** Returns the rotation vector: the axis scaled by the angle, canonicalized so
	// its length never exceeds pi. That bound *is* the shortest-path guarantee — there is no branch
	// to choose later and no place for a 350-degree turn to survive as anything but a 10-degree one.
	[[nodiscard]] inline RotationVector Log() const noexcept;

	// The inverse. Total: a vector longer than pi is a spring that overshot past the antipode, and it
	// reconstructs to the correct rotation rather than to a wrapped one, so nothing downstream has to
	// clamp the channel to keep it meaningful.
	[[nodiscard]] inline static Quaternion Exp(RotationVector rotation) noexcept;

	// Ingest. The axis is normalized here so a caller may hand over any non-zero direction; a zero
	// axis is not a rotation and yields the identity.
	[[nodiscard]] inline static Quaternion FromAxisAngle(Vector3<float> axis, float radians) noexcept;

	// ----- The spring solver's entire contract with this header -----
	//
	// The animatable channel is a RotationVector, not a Quaternion. The solver springs the 3-vector
	// with the same closed form as every other channel (decision 11), and the quaternion is
	// reconstructed on the way out. Evaluation, on the frame thread:
	//
	//     u0 = Quaternion::Deviation(current, target);   // the spring's initial displacement
	//     ... solve u(t) toward zero ...
	//     q  = Quaternion::FromDeviation(u(t), target);  // the value the render path reads
	//
	// Deviation is the geodesic displacement of `current` from `target` — the axis and angle of the
	// single turn that carries one to the other — so |u0| <= pi always, and the reconstructed path is
	// the shortest one by construction rather than by a check. u = 0 is exactly the target, which is
	// what makes the settling threshold on this channel the same kind of quantity as on every other:
	// radians of residual, converted to pixels through the node's bounding radius.
	//
	// Reconstruction is from the stored target every frame rather than from the previous frame's
	// quaternion, so nothing accumulates and nothing needs renormalizing — the same property closed
	// form buys everywhere else in the system.
	//
	// **Retargeting has a third call, and leaving it out is a real defect rather than a rounding.**
	// Interruption re-anchors the chart, and the velocity is stated in the old one. Retargeting on
	// the dispatch thread (Docs/Structure.md puts Animation's Author half there) reads:
	//
	//     u = Quaternion::Deviation(current, newTarget);
	//     v = Quaternion::TransportVelocity(previousDeviation, u, previousVelocity);
	//
	// The physical invariant underneath it is that the body-frame angular velocity does not care
	// which target the deviation is measured from: omega = J(u) * u', so a chart change costs
	// J^-1(u_new) * J(u_old). Expanding the two Jacobians leaves I + (1/2)[u_new - u_old]x, which is
	// **first order in the change of deviation** and not second — a retarget that moves the deviation
	// by a right angle sends the angular velocity about forty degrees off course, which reads as a
	// window that changes direction when it is interrupted. Copying the velocity across is wrong at
	// exactly the moment interruption is the whole point.
	[[nodiscard]] inline static RotationVector Deviation(Quaternion current, Quaternion target) noexcept
	{
		return (target.Conjugate() * current).Log();
	}

	[[nodiscard]] inline static Quaternion FromDeviation(RotationVector deviation, Quaternion target) noexcept
	{
		return target * Exp(deviation);
	}

	// Re-expresses a spring velocity from the chart anchored at `from` into the one anchored at `to`,
	// holding the body-frame angular velocity fixed. All three arguments and the result are rotation
	// vectors: the two deviations in radians, the velocity in radians per second.
	//
	// Exact rather than expanded, because it costs two Jacobians and two cross products *once per
	// retarget on the dispatch thread*, which is event rate on the side of the publication boundary
	// that is allowed to allocate and block. The frame thread never calls this — it evaluates u(t)
	// from published coefficients and calls FromDeviation, and has no use for an angular velocity at
	// all. Pricing this against the frame budget is pricing it against the wrong budget.
	//
	// Geometry owns the chart, so Geometry owns all three of its conversions. That is what lets
	// Animation stay flat and scalar with no dependency on this module's understanding of rotation.
	[[nodiscard]] inline static RotationVector
	TransportVelocity(RotationVector from, RotationVector to, RotationVector velocity) noexcept
	{
		return Detail::ApplyInverseRightJacobian(to, Detail::ApplyRightJacobian(from, velocity));
	}
};

[[nodiscard]] constexpr float Dot(Quaternion left, Quaternion right) noexcept
{
	return left.W * right.W + left.X * right.X + left.Y * right.Y + left.Z * right.Z;
}

inline RotationVector Quaternion::Log() const noexcept
{
	const Quaternion canonical = Canonical();
	const Vector3<float> vector = canonical.Vector();

	// Qualified, because the body of a member definition is in class scope and Length is also the
	// name of a member here — unqualified lookup would find that one and stop, without ever reaching
	// the free function or trying argument-dependent lookup.
	const float vectorLength = ::Length(vector);

	if (vectorLength < Detail::SmallVector)
	{
		// angle / |v| tends to 2 / w as the turn goes to nothing. Series rather than the general form
		// because the general form is 0/0 there, and the seam is chosen above so the two agree.
		return canonical.W > 0.0F ? vector * (2.0F / canonical.W) : RotationVector{};
	}

	// atan2 rather than acos of the scalar part: acos loses half its digits near the identity, which
	// is precisely where a settling spring spends its last frames.
	const float angle = 2.0F * std::atan2(vectorLength, canonical.W);

	return vector * (angle / vectorLength);
}

inline Quaternion Quaternion::Exp(RotationVector rotation) noexcept
{
	const float angle = ::Length(rotation);

	if (angle < Detail::SmallAngle)
	{
		const float scale = 0.5F - angle * angle / 48.0F;

		return { std::cos(angle * 0.5F), rotation.X * scale, rotation.Y * scale, rotation.Z * scale };
	}

	const float half = angle * 0.5F;
	const float scale = std::sin(half) / angle;

	return { std::cos(half), rotation.X * scale, rotation.Y * scale, rotation.Z * scale };
}

inline Quaternion Quaternion::FromAxisAngle(Vector3<float> axis, float radians) noexcept
{
	const float length = ::Length(axis);

	if (!(length > 0.0F))
	{
		return {};
	}

	return Exp(axis * (radians / length));
}

// Node-local perspective, held as a strength rather than as a distance, so that the guard decision 55
// asks for is an invariant of the type and not a check somewhere downstream.
//
// The eye distance is measured **in the node's own bounding radii** rather than in local units. That
// is the whole trick. Every point of the node's quad is within one radius of the anchor, so an eye at
// k radii gives a homogeneous weight of 1 + z/(k*r), bounded into [1 - 1/k, 1 + 1/k] — positive for
// every point, at every scale, on every frame, with nothing to verify at the call site. A stored
// absolute distance cannot promise that: the scale channel is animatable, so a node that grows while
// a perspective distance stays put walks its own far corner through the near plane, which is exactly
// the frame where the projection divides by zero and the damage bound becomes a NaN.
//
// A dimensionless strength is also the only form the motion catalog can author. Decision 13 keeps
// numeric construction inside the catalog, and a catalog entry naming a distance in pixels would
// read as a different amount of perspective on a 200-pixel thumbnail and a 2000-pixel window; naming
// it in radii reads the same on both.
class Perspective
{
public:
	// Two radii is the closest the eye may come, and what it buys is that **a node's own quad can
	// never reach its own eye**. Every point of the quad is within one radius of the anchor and the
	// eye is at least two away, so the weight over the node's own extent stays in [0.5, 1.5] with
	// nothing to check. At exactly one radius the near corner sits in the eye and its projection is
	// unbounded.
	//
	// It previously also claimed to bound the projected area of any node at four times its
	// unprojected area, "a number the effect-cost admission test of decision 29 can key on".
	// *(Corrected 2026-08-22.)* That cited decision 29 in the form decision 29 rejects: `C` is a
	// budget gyro enforces rather than a cost it predicts, so nothing consumes a per-node area bound
	// and nothing ever did. The clamp that claimed to provide it is gone with the claim; see
	// ComposedTransform below for what replaced it and why the trade was free.
	static constexpr float MinimumRadii = 2.0F;

	// Orthographic, because a transform that was never given a perspective has none. There is no
	// null state to check for: every node has a real projection and most of them are this one.
	constexpr Perspective() = default;

	[[nodiscard]] static constexpr Perspective None() noexcept { return Perspective{}; }

	// Total and clamping rather than fallible, the standing Geometry/Scale.h gives configuration: a
	// system-layer compositor does not fail to start because a motion was authored with the eye
	// inside the window. A distance nearer than the clamp is honoured as far as it can be; one that
	// is not a distance at all — zero, negative, NaN — means nothing, and orthographic is the reading
	// that cannot produce a NaN on the frame path.
	[[nodiscard]] static constexpr Perspective FromRadii(float distanceInRadii) noexcept
	{
		if (!(distanceInRadii >= MinimumRadii))
		{
			return distanceInRadii > 0.0F ? Perspective{ 1.0F / MinimumRadii } : Perspective{};
		}

		return Perspective{ 1.0F / distanceInRadii };
	}

	// The reciprocal of the eye distance in radii: 0 is orthographic and 0.5 is as strong as the
	// clamp permits.
	[[nodiscard]] constexpr float Strength() const noexcept { return m_Strength; }

	[[nodiscard]] constexpr bool IsNone() const noexcept { return m_Strength == 0.0F; }

	// The eye, resolved against the node's own radius: the reciprocal of its distance, so a weight is
	// `1 + depth * this` and orthographic is exactly zero. **This is the form the projection has to
	// take to compose**, and the reason is the whole of ComposedTransform below — a weight that is
	// affine in the depth is the ordinary homogeneous projection and folds into a matrix, and one
	// that is not is not a matrix at any depth.
	//
	// Resolved once per node rather than once per point, which is also the cheaper direction: the
	// divide happens where the radius is known and every point above it costs a multiply-add.
	//
	// A radius of zero, or none at all, reads as orthographic. A node with no extent has no depth to
	// project, and this is the one place the answer could otherwise be a division by zero.
	[[nodiscard]] constexpr float InverseEyeDistance(float boundingRadius) const noexcept
	{
		return boundingRadius > 0.0F ? m_Strength / boundingRadius : 0.0F;
	}

	// The homogeneous weight a point at local depth `depth` is divided by. Exactly 1 when there is no
	// perspective, so the orthographic path stays bit-exact.
	//
	// **The depth is not clamped**, and the history is worth keeping because the clamp read as a
	// safety guard and was the opposite. *(Revised 2026-08-22.)* It saturated `depth` against the
	// radius, on the stated contract that |depth| <= boundingRadius and that a caller breaking it had
	// handed over a radius belonging to some other node. The frame side's walk breaks it as ordinary
	// business — an ancestor's radius is its own, and a descendant's corner lands where it lands — and
	// two things came of it on screen. A window pulled out of a workspace stopped growing partway
	// through while still visibly moving, because its weight had frozen; and a quad with some corners
	// saturated and some not stopped being a projective image of a rectangle, so its texture swam
	// across it, which is the exact artefact Seam/Renderer.h's weights exist to remove. A guard that
	// fires on content anyone would author is not a guard.
	//
	// What the clamp was really protecting — no division by zero, nothing infinite reaching a damage
	// bound — is protected at the composed weight instead, where it belongs: see `Projected` below.
	// Over the node's own quad the two forms are identical, because MinimumRadii is what makes the
	// saturation unreachable there.
	[[nodiscard]] constexpr float Weight(float depth, float boundingRadius) const noexcept
	{
		return 1.0F + depth * InverseEyeDistance(boundingRadius);
	}

private:
	constexpr explicit Perspective(float strength) noexcept : m_Strength{ strength } {}

	float m_Strength = 0.0F;
};

// The stored form. An aggregate, and the default is the identity transform rather than a null state.
//
// The channels are ordered widest first so the structure has no interior holes; the four bytes after
// the projection are alignment and are the only padding in it. The offsets are asserted at the foot
// of this header, because the frame side resolves a published transform by position from a base
// address that differs from the one it was written at.
struct NodeTransform
{
	// In the parent's space, and double for Geometry/Space.h's reason — the root of a chain is global
	// space, where single precision runs out at exactly wl_fixed's resolution.
	Vector3<double> Translation{};

	Quaternion Rotation{};

	// Not Geometry/Scale.h's Scale, which is the output's exact rational and does integer size
	// arithmetic. This one is the node's own factor per axis, it is allowed to be negative, and a
	// negative one mirrors — see FacesViewer.
	Vector3<float> Scale{ 1.0F, 1.0F, 1.0F };

	// In the node's own space, and single precision because it is bounded by the node: a local
	// coordinate is a few thousand units at the outside, where float still resolves a thousandth of a
	// pixel. It may lie outside the quad — an anchor at the top-left corner of the screen is how a
	// window is summoned from the corner it was summoned from.
	Vector3<float> Anchor{};

	Perspective Projection{};

	// Maps a point of the node's own space into its parent's. The order is the decomposition read
	// outwards: displace from the anchor, scale, rotate, project, restore the anchor, translate.
	//
	// `boundingRadius` is the node's own, as BoundingRadius below computes it, and it is what makes
	// Perspective's guard hold at whatever the scale channel currently is. It is unused when the
	// projection is orthographic.
	//
	// Deliberately not a matrix. A matrix is a product composed on the render path, where the layout
	// and the precision at which translation folds against the output origin are that path's to
	// choose; storing one here is how a decomposition rots into something somebody lerps.
	[[nodiscard]] constexpr Vector3<double> Apply(Vector3<float> local, float boundingRadius) const noexcept
	{
		const Vector3<float> displaced{ (local.X - Anchor.X) * Scale.X,
			                            (local.Y - Anchor.Y) * Scale.Y,
			                            (local.Z - Anchor.Z) * Scale.Z };
		const Vector3<float> rotated = Rotation.Rotate(displaced);
		const float weight = Projection.Weight(rotated.Z, boundingRadius);
		const Vector3<float> projected = rotated * (1.0F / weight);

		// The anchor is restored in the node's own space, before the translation crosses into the
		// parent's. At the anchor itself the displacement is identically zero, so this is exact
		// rather than nearly so: no rounding is reachable on that path at all.
		return { Translation.X + static_cast<double>(Anchor.X + projected.X),
			     Translation.Y + static_cast<double>(Anchor.Y + projected.Y),
			     Translation.Z + static_cast<double>(Anchor.Z + projected.Z) };
	}

	// Whether the side of the quad the node's content is on still faces the viewer. Back faces cull,
	// per decision 55 — a window rotated past ninety degrees showing mirrored text reads as a bug,
	// and a card flip is two nodes and a catalog transition rather than a double-sided quad. There is
	// no per-node override, because the override *is* the second node.
	//
	// A negative scale on exactly one of X and Y mirrors the winding and counts as showing the back,
	// which is what the rasterizer will do with it in any case and is the same artefact by a
	// different route. The projection cannot change the answer: Perspective's weight is positive
	// everywhere, so it scales the quad about the anchor without ever turning it inside out.
	//
	// **This is the one-node statement of the rule and the walk does not use it.** *(Noted
	// 2026-08-22.)* Decision 93 culls on the signed area of the projected quad instead, because these
	// answers do not multiply: two nodes each turned eighty degrees about X both face front and their
	// composition, at a hundred and sixty, does not. A chain has one orientation and it is the
	// composed one, so asking each level separately is not a cheaper form of the same question — it
	// is a different and wrong one.
	[[nodiscard]] constexpr bool FacesViewer() const noexcept
	{
		// (R * Z).z for a unit quaternion, and the mirroring factor of the two in-plane axes.
		const float towardScreen = 1.0F - 2.0F * (Rotation.X * Rotation.X + Rotation.Y * Rotation.Y);

		return Scale.X * Scale.Y * towardScreen > 0.0F;
	}

	// The radius Perspective is guaranteed against: the longest scaled displacement from the anchor
	// to a corner of the node's quad, which spans [0, width] x [0, height] at z = 0 in the node's own
	// space — the convention surface-local space already uses.
	//
	// Rotation preserves length, so this is a function of the scale channel and the node's extent
	// alone. It does not move while a rotation animates, which is what keeps a card flip from having
	// to recompute it per frame.
	[[nodiscard]] inline float BoundingRadius(float width, float height) const noexcept
	{
		const float left = std::abs(Anchor.X);
		const float right = std::abs(width - Anchor.X);
		const float top = std::abs(Anchor.Y);
		const float bottom = std::abs(height - Anchor.Y);

		const float x = (left > right ? left : right) * std::abs(Scale.X);
		const float y = (top > bottom ? top : bottom) * std::abs(Scale.Y);
		const float z = std::abs(Anchor.Z) * std::abs(Scale.Z);

		return std::sqrt(x * x + y * y + z * z);
	}
};

// One corner, projected, with the divisor that produced it.
//
// The weight is what Seam/Renderer.h calls the accumulated divisor for that corner, and a renderer
// divides by it to interpolate anything across the quad. Without it the interpolation is affine per
// triangle, which is the warp every early 3D console is remembered for.
struct Projected
{
	Vector3<double> Position{};

	float Weight = 1.0F;

	// **The one real singularity, and it is a visibility question rather than an arithmetic one.** A
	// weight at or below zero means the point has passed through the eye of some node above it and is
	// behind the viewer, which cannot be drawn at all — the same category as a back face, which
	// decision 55 already culls unconditionally with no per-node override. The caller culls the node.
	//
	// The floor sits at a magnification of 1024 rather than at zero, and it is a degeneracy guard
	// rather than a policy. A node's own quad cannot reach its own eye at all (see
	// Perspective::MinimumRadii), and a chain would have to stack the strongest perspective the type
	// can express through ten levels to come near this, so it is four orders of magnitude clear of
	// anything the motion catalog could author. It exists so that the *shape* of the failure is a
	// disappearance rather than a NaN.
	//
	// Deliberately **not** a magnification budget. A cull is a window vanishing, and decision 29 ends
	// "`Admit()` returns a degraded plan, never a refusal" while decision 30 answers a shortfall by
	// spending less; a geometric limit standing in for a cost limit would be neither, and would fire
	// at around four times magnification, which a tilted deck holding a flipping card reaches
	// legitimately.
	static constexpr float MinimumWeight = 1.0F / 1024.0F;

	[[nodiscard]] constexpr bool IsVisible() const noexcept { return Weight >= MinimumWeight; }
};

// The transform chain, composed. This is what decision 86's walk carries: the scan over the preorder
// run pushes one of these per node and pops it on the way out, so a node's four corners cost four
// multiplies rather than four walks of the ancestor chain — constant per node rather than growing
// with nesting depth.
//
// **It is a matrix, and that does not contradict NodeTransform above.** That type refuses to *store*
// one, because a stored decomposition is one somebody eventually lerps, and lerping two matrices
// shears the result through configurations that are not rotations. This one is composed on the render
// path and holds nothing between frames, which is exactly what `Apply` asks for: "a product composed
// on the render path, where the layout and the precision at which translation folds against the
// output origin are that path's to choose".
//
// **Why the chain folds at all**, which is not obvious and cost an afternoon to establish. `Apply`
// divides before handing the point to its parent, so the parent rotates an already-divided point and
// none of this is matrix composition on its face. It works because each node's weight is affine in
// the point that node is handed — the projection is the ordinary homogeneous one, 1/d in the bottom
// row — so the divides telescope, and the accumulated divisor is then literally the W component.
// There is nothing to accumulate separately and no product to form.
//
// **The only thing that could break it is a nonlinear weight**, which is what this header carried
// until 2026-08-22: saturating the depth against a radius made the weight piecewise, and a piecewise
// weight is not a matrix at any depth. Removing the clamp is what makes the walk O(1) per node, so
// the correctness fix and the cost fix are the same edit.
//
// Row-major, acting on column vectors, and double throughout — the translation column reaches global
// space, where single precision runs out at exactly wl_fixed's resolution. The default is the
// identity, for the reason the rest of this header defaults to identities: the root of a walk needs
// no initialization pass.
struct ComposedTransform
{
	double M[4][4]{ { 1.0, 0.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0, 0.0 }, { 0.0, 0.0, 0.0, 1.0 } };

	// One node as a homogeneous matrix, which is `Apply`'s comment read literally: displace from the
	// anchor, scale, rotate, project, restore the anchor, translate.
	//
	// Written out rather than multiplied up from six factors, because this runs once per node per
	// frame. The derivation, so the next reader checks it rather than trusts it: with `basis` the
	// columns below, a local point `u` goes to `basis * (u - Anchor)`, the weight is
	// `1 + q * (basis * (u - Anchor)).z`, and the result is `Translation + Anchor + that / weight`.
	// Clearing the divide gives the numerator and denominator rows directly. NodeTransform.Test.cpp
	// checks it against `Apply` over a sweep rather than at a point.
	[[nodiscard]] static constexpr ComposedTransform ForNode(const NodeTransform& node, float boundingRadius) noexcept
	{
		// The rotation and the scale together, as the images of the three axes. Rotate is linear, so
		// its columns are what it does to each axis, and folding the scale in here is what keeps a
		// negative scale mirroring rather than needing a case.
		const Vector3<float> basisX = node.Rotation.Rotate({ node.Scale.X, 0.0F, 0.0F });
		const Vector3<float> basisY = node.Rotation.Rotate({ 0.0F, node.Scale.Y, 0.0F });
		const Vector3<float> basisZ = node.Rotation.Rotate({ 0.0F, 0.0F, node.Scale.Z });

		const double basis[3][3]{ { basisX.X, basisY.X, basisZ.X },
			                      { basisX.Y, basisY.Y, basisZ.Y },
			                      { basisX.Z, basisY.Z, basisZ.Z } };

		const double eye = node.Projection.InverseEyeDistance(boundingRadius);

		// Where the anchor itself lands under the basis, and the constant term of the weight.
		const double anchorX = node.Anchor.X;
		const double anchorY = node.Anchor.Y;
		const double anchorZ = node.Anchor.Z;

		const double anchored[3]{ basis[0][0] * anchorX + basis[0][1] * anchorY + basis[0][2] * anchorZ,
			                      basis[1][0] * anchorX + basis[1][1] * anchorY + basis[1][2] * anchorZ,
			                      basis[2][0] * anchorX + basis[2][1] * anchorY + basis[2][2] * anchorZ };
		const double constant = 1.0 - eye * anchored[2];

		// The anchor is restored in the node's own space, before the translation crosses into the
		// parent's — so the two add here rather than composing, and at the anchor itself the
		// displacement is identically zero and this is exact rather than nearly so.
		const double origin[3]{ node.Translation.X + anchorX,
			                    node.Translation.Y + anchorY,
			                    node.Translation.Z + anchorZ };

		ComposedTransform composed{};

		for (int row = 0; row < 3; ++row)
		{
			for (int column = 0; column < 3; ++column)
			{
				composed.M[row][column] = basis[row][column] + eye * origin[row] * basis[2][column];
			}

			composed.M[row][3] = constant * origin[row] - anchored[row];
		}

		composed.M[3][0] = eye * basis[2][0];
		composed.M[3][1] = eye * basis[2][1];
		composed.M[3][2] = eye * basis[2][2];
		composed.M[3][3] = constant;

		return composed;
	}

	// One output's view of the world, which is what a walk over that output's scene starts from:
	// global space onto its device grid, from Geometry/AxisTransform.h's output adapter.
	//
	// **The whole of why this is a matrix is where the origin gets subtracted.** A node's translation
	// column reaches global space, where a desk of monitors puts a window's corner some hundreds of
	// thousands of units from the origin and single precision has 1/256 of a pixel left underneath it
	// (Geometry/Space.h). Composing the view in *front* of the chain folds the output's origin into
	// that column once, in double, at the push — so the four corners are already small numbers by the
	// time anything narrows, and the difference is exact where projecting to global and converting
	// four corners loses a sixteenth of a pixel per corner. It is what Seam/Renderer.h means by "the
	// precision at which translation folds against the output origin belongs to the render path", and
	// the fold happens here rather than being a subtraction somebody remembers to write.
	//
	// **Depth ends here, and the row is zeroed rather than passed through.** Decision 55 composites
	// in strict tree order with no depth buffer, so an output's grid is two-dimensional and there is
	// no such thing as a device-space Z. Zeroing loses nothing downstream — this is the outermost
	// factor, so its third row reaches only the third row of any product and never a corner's X, Y,
	// or weight — and it makes the value a reader takes out obviously not a depth, rather than a
	// plausible one in the wrong units.
	[[nodiscard]] static constexpr ComposedTransform ForView(AxisTransform<GlobalSpace, DeviceSpace> output) noexcept
	{
		// The eight orientations, read off the images of the two axes rather than written out again.
		// Geometry/AxisTransform.h holds the one table of them and states the Y-down convention it is
		// written under; a second copy here is a second chance to get a quarter turn's signs wrong,
		// on the rotated output nobody has to hand.
		const Detail::Axes<double> alongX = Detail::Orient(output.Orientation, 1.0, 0.0);
		const Detail::Axes<double> alongY = Detail::Orient(output.Orientation, 0.0, 1.0);

		ComposedTransform composed{};

		composed.M[0][0] = output.ScaleX * alongX.X;
		composed.M[0][1] = output.ScaleX * alongY.X;
		composed.M[0][3] = output.Translation.X;

		composed.M[1][0] = output.ScaleY * alongX.Y;
		composed.M[1][1] = output.ScaleY * alongY.Y;
		composed.M[1][3] = output.Translation.Y;

		composed.M[2][2] = 0.0;

		return composed;
	}

	// Descend into a child: the chain this transform stands for, followed by the child's own. The
	// walk pushes the result and restores this one on the way back out, so the stack is one of these
	// per level and is bounded by decision 90's depth cap.
	[[nodiscard]] constexpr ComposedTransform Push(const NodeTransform& child, float childRadius) const noexcept
	{
		const ComposedTransform own = ForNode(child, childRadius);

		ComposedTransform composed{};

		for (int row = 0; row < 4; ++row)
		{
			for (int column = 0; column < 4; ++column)
			{
				composed.M[row][column] = M[row][0] * own.M[0][column] + M[row][1] * own.M[1][column] +
				                          M[row][2] * own.M[2][column] + M[row][3] * own.M[3][column];
			}
		}

		return composed;
	}

	// A point of the deepest node's own space, taken all the way out through every ancestor.
	//
	// The divisor is floored at `Projected::MinimumWeight`, and the floor is reachable only on points
	// the caller is about to cull. That is the difference between this and the clamp it replaced:
	// this one cannot touch geometry that gets drawn, so it bounds the wrongness of a mistake instead
	// of introducing one. A caller that ignores `IsVisible()` draws a wrong quad; one that met an
	// infinity would put a NaN in a damage bound and take every window on the output with it.
	[[nodiscard]] constexpr Projected Project(Vector3<float> local) const noexcept
	{
		const double x = local.X;
		const double y = local.Y;
		const double z = local.Z;

		const double weight = M[3][0] * x + M[3][1] * y + M[3][2] * z + M[3][3];
		const double divisor = weight >= static_cast<double>(Projected::MinimumWeight) ?
		                           weight :
		                           static_cast<double>(Projected::MinimumWeight);

		return { { (M[0][0] * x + M[0][1] * y + M[0][2] * z + M[0][3]) / divisor,
			       (M[1][0] * x + M[1][1] * y + M[1][2] * z + M[1][3]) / divisor,
			       (M[2][0] * x + M[2][1] * y + M[2][2] * z + M[2][3]) / divisor },
			     static_cast<float>(weight) };
	}
};

// Prints as (1, 2, 0), rot(90deg @ (0, 0, 1)), perspective(3 radii), and the transform as all of
// them. The rotation is rendered as an axis and an angle rather than as four floats because the
// failure this system has is "the window took a strange route", which is an observation about an
// angle and an axis — nobody has ever read a quaternion's components and noticed anything.
//
// The context is a template parameter for the reason recorded at length in Core/Handle.h: naming
// std::format_context leaves std::format working and std::formattable false, so a value goes missing
// from a test failure at exactly the moment it was wanted.
namespace Detail
{
template<typename Value>
struct PlainFormatter
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }
};
} // namespace Detail

template<std::floating_point T>
struct std::formatter<Vector3<T>> : Detail::PlainFormatter<Vector3<T>>
{
	template<typename Context>
	auto format(Vector3<T> value, Context& context) const
	{
		return std::format_to(context.out(), "({}, {}, {})", value.X, value.Y, value.Z);
	}
};

template<>
struct std::formatter<Quaternion> : Detail::PlainFormatter<Quaternion>
{
	template<typename Context>
	auto format(Quaternion rotation, Context& context) const
	{
		const RotationVector logarithm = rotation.Log();
		const float angle = Length(logarithm);

		if (!(angle > 0.0F))
		{
			return std::format_to(context.out(), "rot(none)");
		}

		const float degrees = angle * (180.0F / std::numbers::pi_v<float>);

		// Divided rather than multiplied by a reciprocal, so an axis-aligned turn prints as (0, 0, 1)
		// rather than as (0, 0, 0.99999994) — the rounding is invisible in the value and glaring in
		// the log line.
		return std::format_to(context.out(), "rot({}deg @ {})", degrees, logarithm / angle);
	}
};

template<>
struct std::formatter<Perspective> : Detail::PlainFormatter<Perspective>
{
	template<typename Context>
	auto format(Perspective projection, Context& context) const
	{
		if (projection.IsNone())
		{
			return std::format_to(context.out(), "perspective(none)");
		}

		return std::format_to(context.out(), "perspective({} radii)", 1.0F / projection.Strength());
	}
};

template<>
struct std::formatter<NodeTransform> : Detail::PlainFormatter<NodeTransform>
{
	template<typename Context>
	auto format(const NodeTransform& transform, Context& context) const
	{
		return std::format_to(
			context.out(),
			"transform[translate{} {} scale{} anchor{} {}]",
			transform.Translation,
			transform.Rotation,
			transform.Scale,
			transform.Anchor,
			transform.Projection
		);
	}
};

// The contract everything downstream assumes. The runtime half is NodeTransform.Test.cpp, and it
// holds more than usual here: trigonometry is not a constant expression, so the round trips through
// the log map, the shortest-path guarantee, and everything involving a rotation that is not a
// multiple of a right angle had to go there.

// The published representation. Aggregates, resolvable by offset, and no interior padding.
static_assert(std::is_trivially_copyable_v<NodeTransform> && std::is_standard_layout_v<NodeTransform>);
static_assert(std::is_aggregate_v<NodeTransform> && std::is_aggregate_v<Quaternion>);
static_assert(std::is_trivially_copyable_v<Perspective> && std::is_standard_layout_v<Perspective>);
static_assert(sizeof(Vector3<double>) == 3 * sizeof(double) && sizeof(Vector3<float>) == 3 * sizeof(float));
static_assert(sizeof(Quaternion) == 4 * sizeof(float));
static_assert(sizeof(Perspective) == sizeof(float));
static_assert(offsetof(NodeTransform, Translation) == 0);
static_assert(offsetof(NodeTransform, Rotation) == 24);
static_assert(offsetof(NodeTransform, Scale) == 40);
static_assert(offsetof(NodeTransform, Anchor) == 52);
static_assert(offsetof(NodeTransform, Projection) == 64);
static_assert(sizeof(NodeTransform) == 72, "Widest first, no interior holes, four bytes of alignment at the tail");

// The scalar part is first and the identity is { 1, 0, 0, 0 }. Whoever uploads one to a shader reads
// this rather than guessing, and a change to the order fails here rather than in a rotation that is
// wrong in a way that looks plausible.
static_assert(std::bit_cast<std::array<float, 4>>(Quaternion{}) == std::array<float, 4>{ 1.0F, 0.0F, 0.0F, 0.0F });
static_assert(
	std::bit_cast<Quaternion>(std::array<float, 4>{ 0.0F, 0.0F, 0.0F, 1.0F }) == Quaternion{ 0.0F, 0.0F, 0.0F, 1.0F }
);

// Division is exact where the divisor is a power of two, which is the property the formatter above
// leans on: an axis-aligned turn prints as (0, 0, 1) rather than as (0, 0, 0.99999994), because the
// components are divided by the angle rather than multiplied by its reciprocal. The same choice is
// why Animation/Solve/Spring.h's SpringValue asks for the operator at all.
static_assert(Vector3<float>{ 3.0F, 6.0F, -9.0F } / 3.0F == Vector3<float>{ 1.0F, 2.0F, -3.0F });

// The default is the identity transform, not a null state — Geometry/Scale.h's argument, and the
// reason a node that has never been animated needs no initialization pass.
static_assert(NodeTransform{}.Apply({ 3.0F, 4.0F, 5.0F }, 10.0F) == Vector3<double>{ 3.0, 4.0, 5.0 });
static_assert(NodeTransform{}.FacesViewer());
static_assert(Quaternion{}.Rotate({ 3.0F, 4.0F, 5.0F }) == Vector3<float>{ 3.0F, 4.0F, 5.0F });

// **The anchor is fixed under scale and rotation, exactly.** The half-turn about Z spells as
// { 0, 0, 0, 1 } with no trigonometry, so the whole of this one is a constant expression; the general
// case is in the test file. Exact rather than approximate because the displacement from the anchor to
// itself is identically zero and there is nothing left to round.
static_assert(
	NodeTransform{ .Translation = { 100.0, 200.0, 0.0 },
                   .Rotation = { 0.0F, 0.0F, 0.0F, 1.0F },
                   .Scale = { 3.0F, 7.0F, 1.0F },
                   .Anchor = { 40.0F, 50.0F, 0.0F } }
		.Apply({ 40.0F, 50.0F, 0.0F }, 100.0F) == Vector3<double>{ 140.0, 250.0, 0.0 }
);

// A half turn about Z, about an anchored corner. The corner opposite the anchor lands on the far side
// of it, which is the two-dimensional case of what the anchor is for.
static_assert(
	NodeTransform{ .Rotation = { 0.0F, 0.0F, 0.0F, 1.0F } }.Apply({ 100.0F, 100.0F, 0.0F }, 200.0F) ==
	Vector3<double>{ -100.0, -100.0, 0.0 }
);

// Back faces. A half turn about Y shows the back; a mirror in one axis shows it by the other route;
// two mirrors are a half turn about Z and show the front again.
static_assert(!NodeTransform{ .Rotation = { 0.0F, 0.0F, 1.0F, 0.0F } }.FacesViewer(), "Half a turn shows the back");
static_assert(!NodeTransform{ .Scale = { -1.0F, 1.0F, 1.0F } }.FacesViewer(), "A mirror is the back by another route");
static_assert(NodeTransform{ .Scale = { -1.0F, -1.0F, 1.0F } }.FacesViewer(), "Two mirrors are a half turn");
static_assert(NodeTransform{ .Rotation = { 0.0F, 0.0F, 0.0F, 1.0F } }.FacesViewer(), "A turn in the plane faces front");

// The authored perspective. It clamps below two radii, it reads nothing as orthographic, and over
// the node's own quad — the only extent it is stated against — the weight stays in [0.5, 1.5].
static_assert(Perspective::FromRadii(4.0F).Strength() == 0.25F);
static_assert(Perspective::FromRadii(1.0F).Strength() == Perspective::FromRadii(Perspective::MinimumRadii).Strength());
static_assert(Perspective::FromRadii(0.0F).IsNone() && Perspective::FromRadii(-3.0F).IsNone());
static_assert(Perspective::FromRadii(std::numeric_limits<float>::quiet_NaN()).IsNone());
static_assert(Perspective::FromRadii(std::numeric_limits<float>::infinity()).IsNone(), "Infinitely far is flat");
static_assert(Perspective::None().Weight(37.0F, 100.0F) == 1.0F, "Orthographic stays bit-exact");
static_assert(Perspective::None().InverseEyeDistance(100.0F) == 0.0F);
static_assert(Perspective::FromRadii(1.0F).Weight(-100.0F, 100.0F) == 0.5F, "The near plane never crosses the quad");
static_assert(Perspective::FromRadii(1.0F).Weight(100.0F, 100.0F) == 1.5F);
static_assert(Perspective::FromRadii(2.0F).Weight(5.0F, 0.0F) == 1.0F, "A node with no extent has no depth either");

// The weight is affine in the depth and stays that way past the node's own radius, which is the one
// property the chain folds on. It was previously saturated here, and the depth below is the case that
// exposed it: a descendant an ancestor's radius away in depth used to read as exactly the boundary.
static_assert(Perspective::FromRadii(2.0F).Weight(-200.0F, 100.0F) == 0.0F, "The eye itself, not clamped short of it");
static_assert(Perspective::FromRadii(2.0F).Weight(-400.0F, 100.0F) == -1.0F, "And past it, which is behind the viewer");
static_assert(
	Perspective::FromRadii(4.0F).Weight(50.0F, 100.0F) - 1.0F ==
		2.0F * (Perspective::FromRadii(4.0F).Weight(25.0F, 100.0F) - 1.0F),
	"Affine in the depth, which is what lets a chain of these fold into one matrix"
);

// The composed chain. A default node composes to the identity exactly, so an unanimated subtree costs
// nothing in precision as well as nothing in work.
static_assert(ComposedTransform{}.Project({ 3.0F, 4.0F, 5.0F }).Position == Vector3<double>{ 3.0, 4.0, 5.0 });
static_assert(ComposedTransform{}.Project({ 3.0F, 4.0F, 5.0F }).Weight == 1.0F);
static_assert(
	ComposedTransform::ForNode(NodeTransform{}, 10.0F).Project({ 3.0F, 4.0F, 5.0F }).Position ==
	Vector3<double>{ 3.0, 4.0, 5.0 }
);
static_assert(
	ComposedTransform{}.Push(NodeTransform{}, 10.0F).Project({ 3.0F, 4.0F, 5.0F }).Position ==
	Vector3<double>{ 3.0, 4.0, 5.0 }
);

// The chain agrees with Apply on the single-node case that has no rounding in it at all: the anchor is
// fixed, so the displacement is identically zero on both paths.
static_assert(
	ComposedTransform::ForNode(
		NodeTransform{ .Translation = { 100.0, 200.0, 0.0 },
                       .Rotation = { 0.0F, 0.0F, 0.0F, 1.0F },
                       .Scale = { 3.0F, 7.0F, 1.0F },
                       .Anchor = { 40.0F, 50.0F, 0.0F } },
		100.0F
	)
		.Project({ 40.0F, 50.0F, 0.0F })
		.Position == Vector3<double>{ 140.0, 250.0, 0.0 }
);

// Visibility is the composed weight's question and nothing else's. A NaN is not visible, which the
// clamp this replaced could not say — it turned one into a plausible 1.0 and drew whatever followed.
static_assert(Projected{}.IsVisible(), "The default is an orthographic point, which is always visible");
static_assert(!Projected{ .Weight = 0.0F }.IsVisible(), "At the eye");
static_assert(!Projected{ .Weight = -1.0F }.IsVisible(), "Behind the viewer");
static_assert(!Projected{ .Weight = std::numeric_limits<float>::quiet_NaN() }.IsVisible());
static_assert(Projected{ .Weight = Projected::MinimumWeight }.IsVisible(), "The floor itself is still drawn");
static_assert(Projected::MinimumWeight * 1024.0F == 1.0F, "A magnification of 1024, exactly, so it never rounds");

// The spring solver's contract, in the form the compiler can hold it to. The channel is a
// three-vector and a quaternion is not one, so a solver that tried to spring the rotation directly
// would not compile rather than would interpolate along a shear.
static_assert(std::is_same_v<RotationVector, Vector3<float>>);
static_assert(std::is_same_v<decltype(Quaternion::Deviation(Quaternion{}, Quaternion{})), RotationVector>);
static_assert(std::is_same_v<decltype(Quaternion::FromDeviation(RotationVector{}, Quaternion{})), Quaternion>);
static_assert(
	std::is_same_v<
		decltype(Quaternion::TransportVelocity(RotationVector{}, RotationVector{}, RotationVector{})),
		RotationVector>,
	"Retargeting re-anchors the chart, so the velocity converts with it"
);
static_assert(std::is_same_v<decltype(Quaternion{}.Log()), RotationVector>);
static_assert(std::is_same_v<decltype(Quaternion::Exp(RotationVector{})), Quaternion>);
static_assert(!std::is_convertible_v<Quaternion, RotationVector> && !std::is_convertible_v<RotationVector, Quaternion>);

// Two precisions are two types, as two spaces are in Geometry/Space.h. Widening is a named
// conversion somebody had to write.
static_assert(!std::is_convertible_v<Vector3<float>, Vector3<double>>);
static_assert(!std::is_convertible_v<Vector3<double>, Vector3<float>>);
static_assert(Widen(Vector3<float>{ 1.0F, 2.0F, 3.0F }) == Vector3<double>{ 1.0, 2.0, 3.0 });

static_assert(std::formattable<NodeTransform, char>, "A report prints the transform rather than <unprintable>");
static_assert(std::formattable<Quaternion, char> && std::formattable<Perspective, char>);
static_assert(std::formattable<Vector3<float>, char> && std::formattable<Vector3<double>, char>);
