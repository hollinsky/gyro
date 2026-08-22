#include "Geometry/NodeTransform.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <source_location>
#include <string>
#include <string_view>

#include "Testing/Test.h"

// The runtime half of NodeTransform.h's contract. The compile-time half is the static_assert block
// at the foot of that header — layout and offsets, the identity default, the anchor held fixed under
// a quarter and a half turn, back faces, and the whole of the perspective clamp — and is not
// repeated here.
//
// The balance falls further this way than it does in Geometry/Space.h, because trigonometry is not a
// constant expression. Everything that needs a rotation which is not a multiple of a right angle
// lands here: the log map's round trips, the shortest-path guarantee, and the projection of a card
// turned partway.
//
// These are the failure modes the documents name, not arithmetic identities. A rotation that takes
// the long way round is Docs/Decisions.md decision 55's "window travelling a visibly strange route";
// an anchor that drifts is the transition reading as a thing getting bigger rather than as a window
// growing out of the corner it was summoned from; a near plane crossing the quad is the frame whose
// damage bound becomes a NaN.

// Internal linkage rather than an unnamed namespace, which is what Testing/Test.h means by there
// being no namespaces here to keep a helper out of the global name pool.
constexpr float Pi = std::numbers::pi_v<float>;

// Positions here are ~100 local units, where single precision resolves about 1e-5. A tolerance of
// 1/4096 of a logical pixel is sixteen times finer than the finest offset wl_fixed can express, so
// anything these tests admit is invisible by construction — and it is still two orders of magnitude
// looser than the round-off actually available, so a failure means a wrong answer rather than a
// tight bound.
constexpr float PositionTolerance = 1.0F / 4096.0F;

// Angles convert to pixels through the node's bounding radius: a residual of e displaces a corner by
// roughly e*r (Docs/Animation.md#implementation-notes). At the 1000-unit radius of a large window,
// this displaces a corner by a hundredth of a pixel.
constexpr float AngleTolerance = 1.0e-5F;

[[nodiscard]] static float Radians(float degrees)
{
	return degrees * (Pi / 180.0F);
}

// The harness prints the expression for a GYRO_CHECK and nothing else, which for an approximate
// comparison leaves out the two things worth seeing: the values, and the tolerance they missed by.
// These report through the harness directly so a failure names all three, and take the call site as
// a defaulted argument so it points at the assertion rather than at this file.
static bool CheckNear(
	float actual,
	float expected,
	float tolerance,
	std::string_view what,
	const std::source_location& where = std::source_location::current()
)
{
	if (std::abs(actual - expected) <= tolerance)
	{
		return true;
	}

	ReportFailure(what, std::format("{} vs {} (tolerance {})", actual, expected, tolerance), where);
	return false;
}

static bool CheckNear(
	Vector3<double> actual,
	Vector3<double> expected,
	float tolerance,
	std::string_view what,
	const std::source_location& where = std::source_location::current()
)
{
	if (Length(actual - expected) <= static_cast<double>(tolerance))
	{
		return true;
	}

	ReportFailure(what, std::format("{} vs {} (tolerance {})", actual, expected, tolerance), where);
	return false;
}

// Rotations compare by the angle between them, which is the only comparison that means anything:
// it is blind to the double cover by construction, and its units are the ones the settling threshold
// is stated in.
static bool CheckSameRotation(
	Quaternion actual,
	Quaternion expected,
	float tolerance,
	std::string_view what,
	const std::source_location& where = std::source_location::current()
)
{
	const float between = Length(Quaternion::Deviation(actual, expected));

	if (between <= tolerance)
	{
		return true;
	}

	ReportFailure(what, std::format("{} vs {} (off by {} rad)", actual, expected, between), where);
	return false;
}

// The norm Animation/Solve/Spring.h's SpringValue reaches for, and the property
// Docs/Decisions.md decision 17 rests on: a channel settles on the magnitude of the whole vector,
// never per component. Three turns through the same angle about three different axes are therefore
// the same distance from settled, which is what makes a rotation finish at one moment rather than at
// a moment that depends on where its axis happened to point.
//
// Magnitude and Length are the same number under two names, and that is asserted here rather than
// assumed: the first is the solver's concept spelling reached by argument-dependent lookup, the
// second is geometry's own, and a divergence between them would be a settle instant computed from a
// quantity nothing else in the system uses.
GYRO_TEST(Vector3, MagnitudeIsTheWholeVectorNorm)
{
	CheckNear(Magnitude(Vector3<float>{ 3.0F, 4.0F, 12.0F }), 13.0F, PositionTolerance, "the Euclidean norm");
	CheckNear(Magnitude(Vector3<float>{}), 0.0F, PositionTolerance, "and zero at the origin");

	const Vector3<float> logarithms[]{ Quaternion::FromAxisAngle({ 1.0F, 0.0F, 0.0F }, Radians(30.0F)).Log(),
		                               Quaternion::FromAxisAngle({ 0.0F, 1.0F, 0.0F }, Radians(30.0F)).Log(),
		                               Quaternion::FromAxisAngle({ 1.0F, 1.0F, 1.0F }, Radians(30.0F)).Log() };

	for (const Vector3<float>& logarithm : logarithms)
	{
		CheckNear(Magnitude(logarithm), Radians(30.0F), AngleTolerance, "the same angle is the same magnitude");
		CheckNear(Magnitude(logarithm), Length(logarithm), AngleTolerance, "Magnitude is Length");
	}
}

GYRO_TEST(Quaternion, TheLogMapRoundTrips)
{
	// The awkward cases, and they are awkward for different reasons. The identity divides by zero in
	// the general form; a hair off it is where a settling spring spends its last frames; a hair under
	// half a turn is where the scalar part changes sign; exactly half a turn has no scalar part at
	// all.
	const Quaternion identity{};
	const Quaternion barelyTurned = Quaternion::FromAxisAngle({ 0.0F, 1.0F, 0.0F }, 1.0e-7F);
	const Quaternion small = Quaternion::FromAxisAngle({ 1.0F, 2.0F, 3.0F }, Radians(0.01F));
	const Quaternion ordinary = Quaternion::FromAxisAngle({ 1.0F, 2.0F, 3.0F }, Radians(30.0F));
	const Quaternion nearlyHalf = Quaternion::FromAxisAngle({ 0.0F, 0.0F, 1.0F }, Radians(179.99F));
	const Quaternion half = Quaternion::FromAxisAngle({ 0.0F, 0.0F, 1.0F }, Pi);

	for (const Quaternion rotation : { identity, barelyTurned, small, ordinary, nearlyHalf, half })
	{
		CheckSameRotation(Quaternion::Exp(rotation.Log()), rotation, AngleTolerance, "Exp(Log(q)) == q");
	}
}

GYRO_TEST(Quaternion, TheDoubleCoverIsOneRotation)
{
	const Quaternion rotation = Quaternion::FromAxisAngle({ 1.0F, 0.0F, 1.0F }, Radians(120.0F));
	const Quaternion negated{ -rotation.W, -rotation.X, -rotation.Y, -rotation.Z };

	// Different bytes, and equality here is representational — that is what makes the next two
	// assertions worth making rather than tautologies.
	GYRO_CHECK(!(rotation == negated));

	// The log map canonicalizes, so both representatives produce the same rotation vector and the
	// same reconstruction. Without this, half the retargets in the system would spring the long way
	// round for no reason a reader could see in the code.
	CheckNear(Widen(rotation.Log()), Widen(negated.Log()), AngleTolerance, "Log(q) == Log(-q)");
	CheckSameRotation(Quaternion::Exp(negated.Log()), rotation, AngleTolerance, "Exp(Log(-q)) == q");

	GYRO_CHECK(rotation.Canonical() == negated.Canonical());
	GYRO_CHECK(Length(rotation.Log()) <= Pi + AngleTolerance);
	GYRO_CHECK(Length(negated.Log()) <= Pi + AngleTolerance);
}

GYRO_TEST(Quaternion, TheLogMapIsAxisTimesAngle)
{
	// The convention the settling threshold depends on: the channel's magnitude is the rotation
	// angle in radians, not the half-angle of the textbook logarithm. A factor of two here would
	// leave every angular threshold in the system quietly twice as loose as it reads.
	const Quaternion rotation = Quaternion::FromAxisAngle({ 0.0F, 3.0F, 0.0F }, Radians(72.0F));
	const RotationVector logarithm = rotation.Log();

	CheckNear(Length(logarithm), Radians(72.0F), AngleTolerance, "|Log(q)| is the angle in radians");
	CheckNear(rotation.Angle(), Radians(72.0F), AngleTolerance, "Angle() agrees with the log map");
	CheckNear(Widen(logarithm * (1.0F / Length(logarithm))), { 0.0, 1.0, 0.0 }, AngleTolerance, "and the axis");
}

GYRO_TEST(Quaternion, TheShortestPathIsTaken)
{
	// A turn of 350 degrees about Z is a turn of 10 degrees the other way, and the difference between
	// those two readings is a window that swings the whole way round the screen. This is decision
	// 55's rejection of Euler angles turned into an assertion.
	const Quaternion target = Quaternion::FromAxisAngle({ 0.0F, 0.0F, 1.0F }, Radians(350.0F));
	const Quaternion current{};

	const RotationVector displacement = Quaternion::Deviation(current, target);

	if (!CheckNear(Length(displacement), Radians(10.0F), AngleTolerance, "the short way, not the long way"))
	{
		return;
	}

	// The whole path, not just its endpoints: a spring drives the displacement from u0 to zero, and
	// every rotation it passes through has to be on the short side. The long way round would put an
	// intermediate value at 175 degrees from the start, and would take the test vector's Y component
	// positive on the way.
	float previous = 0.0F;

	for (int step = 0; step <= 32; ++step)
	{
		const float remaining = 1.0F - static_cast<float>(step) / 32.0F;
		const Quaternion sampled = Quaternion::FromDeviation(displacement * remaining, target);
		const float travelled = sampled.Angle();

		GYRO_CHECK(travelled <= Radians(10.0F) + AngleTolerance);
		GYRO_CHECK(travelled >= previous - AngleTolerance);
		GYRO_CHECK(sampled.Rotate({ 1.0F, 0.0F, 0.0F }).Y <= 0.0F);

		previous = travelled;
	}

	// And it arrives where it was sent.
	CheckSameRotation(Quaternion::FromDeviation({}, target), target, AngleTolerance, "u = 0 is the target");
	CheckSameRotation(
		Quaternion::FromDeviation(displacement, target), current, AngleTolerance, "u = u0 is where it started"
	);
}

GYRO_TEST(Quaternion, ASpringMayOvershootPastTheAntipode)
{
	// Nothing clamps the channel, so an underdamped spring on a large residual walks the rotation
	// vector past pi. Exp has to wrap rather than saturate, or the overshoot of a nearly-half turn
	// would present as the node sticking at the antipode for a frame.
	const Quaternion target{};
	const Quaternion current = Quaternion::FromAxisAngle({ 0.0F, 0.0F, 1.0F }, Radians(170.0F));
	const RotationVector displacement = Quaternion::Deviation(current, target);

	const Quaternion overshot = Quaternion::FromDeviation(displacement * 1.2F, target);

	// 204 degrees about the axis is 156 degrees about its negation, and that is the rotation the
	// reconstruction has to name.
	CheckNear(Length(Quaternion::Deviation(overshot, target)), Radians(156.0F), AngleTolerance, "204 degrees wraps");
}

// The body-frame angular velocity of a path, estimated from the path itself rather than from any
// Jacobian — which is what makes the transport test a statement about the physics rather than a
// re-derivation of the matrix product it is checking. Centred, so the truncation is O(h^2).
[[nodiscard]] static RotationVector
BodyAngularVelocity(Quaternion target, RotationVector deviation, RotationVector velocity, float step)
{
	const Quaternion before = Quaternion::FromDeviation(deviation - velocity * step, target);
	const Quaternion after = Quaternion::FromDeviation(deviation + velocity * step, target);

	return Quaternion::Deviation(after, before) * (1.0F / (2.0F * step));
}

GYRO_TEST(Quaternion, TheBodyAngularVelocitySurvivesReAnchoring)
{
	// The invariant a retarget has to preserve. A rotation in flight has one angular velocity, and
	// which target its deviation happens to be measured from is bookkeeping — so the same path
	// described from two charts has to report the same velocity, and that is what says the transport
	// is right rather than merely plausible.
	// A node a quarter turn into a flip, retargeted to a chart a quarter turn away about a
	// perpendicular axis, with the velocity perpendicular to both. Not a corner case: an interruption
	// mid-flight is what the whole retargeting path exists for, and the two deviations being large and
	// unrelated is the ordinary shape of one.
	const Quaternion oldTarget{};
	const RotationVector deviation{ Pi * 0.5F, 0.0F, 0.0F };
	const RotationVector velocity{ 0.0F, 1.0F, 0.0F };

	const Quaternion current = Quaternion::FromDeviation(deviation, oldTarget);
	const Quaternion newTarget = current * Quaternion::Exp({ 0.0F, 0.0F, -Pi * 0.5F });

	const RotationVector reAnchored = Quaternion::Deviation(current, newTarget);
	const RotationVector transported = Quaternion::TransportVelocity(deviation, reAnchored, velocity);

	constexpr float Coarse = 0.04F;
	constexpr float Fine = 0.02F;

	const RotationVector before = BodyAngularVelocity(oldTarget, deviation, velocity, Fine);
	const RotationVector after = BodyAngularVelocity(newTarget, reAnchored, transported, Fine);

	// The tolerance is the centred difference's own truncation with well over an order of magnitude
	// spare, not a fudge: at h = 0.02 the estimate itself is only good to a few times 1e-5 rad/s, and
	// nothing here can be measured more finely than the ruler measuring it.
	CheckNear(Widen(after - before), { 0.0, 0.0, 0.0 }, 1.0e-3F, "the same path has the same velocity");

	// What copying the velocity across instead would have cost, which is why this function exists:
	// nine tenths of a radian per second against a velocity of one. Not a refinement — the node
	// leaves the interruption turning about a visibly different axis.
	const RotationVector copied = BodyAngularVelocity(newTarget, reAnchored, velocity, Fine);

	GYRO_CHECK(Length(copied - before) > 0.5F);

	// And the two discrepancies are different in kind, which is the part a single tolerance cannot
	// say. The transport's is the ruler's, so halving the step quarters it; the copy's is real, so
	// halving the step does not touch it. An error that vanishes in the limit and one that does not.
	const RotationVector beforeCoarse = BodyAngularVelocity(oldTarget, deviation, velocity, Coarse);
	const RotationVector afterCoarse = BodyAngularVelocity(newTarget, reAnchored, transported, Coarse);
	const RotationVector copiedCoarse = BodyAngularVelocity(newTarget, reAnchored, velocity, Coarse);

	GYRO_CHECK(Length(after - before) < 0.4F * Length(afterCoarse - beforeCoarse));
	GYRO_CHECK(Length(copied - before) > 0.9F * Length(copiedCoarse - beforeCoarse));
}

GYRO_TEST(Quaternion, TransportIsExactAtAKnownConfiguration)
{
	// One configuration worked out by hand, because the property test above is blind to a transposed
	// Jacobian's sign in a way this is not. From the identity chart to one a quarter turn about Z
	// away, carrying a unit velocity along X:
	//
	//     J(0) = I, so omega = (1, 0, 0)
	//     J^-1(u) v = v + (1/2)[u]x v + c [u]x^2 v      with |u| = pi/2
	//     [u]x v = (pi/2) Y,  [u]x^2 v = -(pi/2)^2 X,  c (pi/2)^2 = 1 - pi/4
	//     v' = (1 - (1 - pi/4)) X + (pi/4) Y = (pi/4)(X + Y)
	//
	// The left Jacobian, or a transposed one, gives (pi/4)(X - Y) — same magnitude, mirrored, and
	// indistinguishable from the right answer by any test that only checks how fast the node turns.
	const RotationVector from{};
	const RotationVector to{ 0.0F, 0.0F, Pi * 0.5F };
	const RotationVector velocity{ 1.0F, 0.0F, 0.0F };

	const RotationVector transported = Quaternion::TransportVelocity(from, to, velocity);

	constexpr double Quarter = std::numbers::pi_v<double> / 4.0;

	CheckNear(Widen(transported), { Quarter, Quarter, 0.0 }, AngleTolerance, "the exact worked case");
}

GYRO_TEST(Quaternion, TransportIsTheIdentityWhereItShouldBe)
{
	const RotationVector velocity{ 0.7F, -1.3F, 0.2F };

	// A retarget that did not move the deviation cannot move the velocity, and the two Jacobians have
	// to cancel to the last few bits rather than approximately — including at half a turn, which is
	// the worst conditioning the canonicalized domain can reach.
	for (const RotationVector deviation : { RotationVector{},
	                                        RotationVector{ 0.3F, -0.2F, 0.5F },
	                                        RotationVector{ 0.0F, 0.0F, Pi },
	                                        RotationVector{ Pi * 0.577F, Pi * 0.577F, Pi * 0.577F } })
	{
		CheckNear(
			Widen(Quaternion::TransportVelocity(deviation, deviation, velocity) - velocity),
			{ 0.0, 0.0, 0.0 },
			1.0e-5F,
			"an unmoved chart transports nothing"
		);
	}

	// And it degrades to the copy that the naive implementation would have made, as both deviations
	// go to nothing — which is why the omission survived review: it is correct in exactly the case
	// somebody would have tried by hand.
	CheckNear(
		Widen(Quaternion::TransportVelocity({ 1.0e-6F, 0.0F, 0.0F }, { 0.0F, 2.0e-6F, 0.0F }, velocity) - velocity),
		{ 0.0, 0.0, 0.0 },
		1.0e-5F,
		"near the identity the correction vanishes"
	);
}

GYRO_TEST(Quaternion, TheJacobianSeamIsInvisible)
{
	// Both Jacobians fall into a series below Detail::SmallJacobianAngle. Straddling the seam by a
	// millionth of it makes any disagreement between the two branches the whole of the difference,
	// since the inputs themselves differ by far less than either branch's error. A series coefficient
	// off by a factor — 1/6 where 1/12 belongs — steps by about 1e-5 here and would otherwise never
	// show up anywhere, because below the seam these terms are too small to see and above it they are
	// not used.
	constexpr float Seam = Detail::SmallJacobianAngle;
	constexpr float Nudge = Seam * 1.0e-6F;

	const RotationVector velocity{ 1.0F, 0.5F, -0.3F };
	const RotationVector far{ 0.4F, -0.9F, 0.2F };

	const RotationVector belowFrom{ 0.0F, Seam - Nudge, 0.0F };
	const RotationVector aboveFrom{ 0.0F, Seam + Nudge, 0.0F };

	CheckNear(
		Widen(
			Quaternion::TransportVelocity(belowFrom, far, velocity) -
			Quaternion::TransportVelocity(aboveFrom, far, velocity)
		),
		{ 0.0, 0.0, 0.0 },
		1.0e-6F,
		"the right Jacobian's seam"
	);

	CheckNear(
		Widen(
			Quaternion::TransportVelocity(far, belowFrom, velocity) -
			Quaternion::TransportVelocity(far, aboveFrom, velocity)
		),
		{ 0.0, 0.0, 0.0 },
		1.0e-6F,
		"and its inverse's"
	);
}

GYRO_TEST(NodeTransform, TheAnchorStaysFixed)
{
	// The reason the anchor exists, in its general form — the header's static_assert block covers the
	// right-angle cases that a constant expression can reach.
	const NodeTransform transform{ .Translation = { 1920.0, 1080.0, 0.0 },
		                           .Rotation = Quaternion::FromAxisAngle({ 1.0F, 2.0F, 3.0F }, Radians(37.0F)),
		                           .Scale = { 2.5F, 0.4F, 1.0F },
		                           .Anchor = { 30.0F, 70.0F, 0.0F },
		                           .Projection = Perspective::FromRadii(3.0F) };

	const float radius = transform.BoundingRadius(100.0F, 100.0F);

	// Exact, not near. The displacement from the anchor to itself is identically zero, so nothing on
	// the path from it to the answer has anything to round — which is worth asserting as exactly as
	// it is true, since an anchor that drifts by a hundredth of a pixel per frame is a window that
	// crawls during a long transition.
	GYRO_CHECK_EQ(
		transform.Apply(transform.Anchor, radius),
		Vector3<double>{ transform.Translation.X + static_cast<double>(transform.Anchor.X),
	                     transform.Translation.Y + static_cast<double>(transform.Anchor.Y),
	                     transform.Translation.Z + static_cast<double>(transform.Anchor.Z) }
	);

	// And it is the scale and the rotation that are held off it, not the translation: moving the node
	// moves the anchor with it.
	const NodeTransform unscaled{ .Translation = transform.Translation, .Anchor = transform.Anchor };

	GYRO_CHECK_EQ(transform.Apply(transform.Anchor, radius), unscaled.Apply(unscaled.Anchor, radius));
}

GYRO_TEST(NodeTransform, AWindowGrowsOutOfTheCornerItWasSummonedFrom)
{
	// The whole of what the anchor buys, stated the way the experience document states it. A 100x100
	// node anchored at its top-left corner and scaled by two keeps that corner and puts the opposite
	// one twice as far away; the same node anchored at its middle grows in every direction at once,
	// which is the version that reads as a thing getting bigger rather than as an intent.
	const NodeTransform fromCorner{ .Scale = { 2.0F, 2.0F, 1.0F }, .Anchor = { 0.0F, 0.0F, 0.0F } };
	const float cornerRadius = fromCorner.BoundingRadius(100.0F, 100.0F);

	CheckNear(
		fromCorner.Apply({ 0.0F, 0.0F, 0.0F }, cornerRadius), { 0.0, 0.0, 0.0 }, PositionTolerance, "corner held"
	);
	CheckNear(
		fromCorner.Apply({ 100.0F, 100.0F, 0.0F }, cornerRadius), { 200.0, 200.0, 0.0 }, PositionTolerance, "far corner"
	);

	const NodeTransform fromMiddle{ .Scale = { 2.0F, 2.0F, 1.0F }, .Anchor = { 50.0F, 50.0F, 0.0F } };
	const float middleRadius = fromMiddle.BoundingRadius(100.0F, 100.0F);

	CheckNear(
		fromMiddle.Apply({ 0.0F, 0.0F, 0.0F }, middleRadius), { -50.0, -50.0, 0.0 }, PositionTolerance, "corner moves"
	);
	CheckNear(
		fromMiddle.Apply({ 100.0F, 100.0F, 0.0F }, middleRadius),
		{ 150.0, 150.0, 0.0 },
		PositionTolerance,
		"and so does"
	);
}

GYRO_TEST(NodeTransform, RotationTurnsAboutTheAnchorToo)
{
	// A quarter turn about Z, about the middle of the node. Y is down, so a positive turn about the
	// axis pointing into the screen carries the top-left corner to the top-right — which is the sign
	// convention the header states, checked rather than assumed.
	const NodeTransform transform{ .Rotation = Quaternion::FromAxisAngle({ 0.0F, 0.0F, 1.0F }, Radians(90.0F)),
		                           .Anchor = { 50.0F, 50.0F, 0.0F } };
	const float radius = transform.BoundingRadius(100.0F, 100.0F);

	CheckNear(transform.Apply({ 50.0F, 50.0F, 0.0F }, radius), { 50.0, 50.0, 0.0 }, PositionTolerance, "anchor held");
	CheckNear(
		transform.Apply({ 0.0F, 0.0F, 0.0F }, radius), { 100.0, 0.0, 0.0 }, PositionTolerance, "top-left goes up"
	);
	CheckNear(transform.Apply({ 100.0F, 0.0F, 0.0F }, radius), { 100.0, 100.0, 0.0 }, PositionTolerance, "and round");
}

GYRO_TEST(NodeTransform, PerspectiveForeshortens)
{
	// What perspective is actually for: the near edge of a turned card is longer than the far one.
	// Without the divide both edges project to the same length and the card reads as a sheared
	// rectangle rather than as a card.
	const NodeTransform transform{ .Rotation = Quaternion::FromAxisAngle({ 0.0F, 1.0F, 0.0F }, Radians(45.0F)),
		                           .Anchor = { 50.0F, 50.0F, 0.0F },
		                           .Projection = Perspective::FromRadii(3.0F) };
	const float radius = transform.BoundingRadius(100.0F, 100.0F);

	const Vector3<double> left = transform.Apply({ 0.0F, 50.0F, 0.0F }, radius);
	const Vector3<double> right = transform.Apply({ 100.0F, 50.0F, 0.0F }, radius);

	// A positive turn about +Y carries +X toward the viewer, so the right edge is the near one and it
	// is the one that ends up further from the anchor.
	GYRO_CHECK(left.Z > 0.0);
	GYRO_CHECK(right.Z < 0.0);
	GYRO_CHECK(std::abs(right.X - 50.0) > std::abs(left.X - 50.0));

	// Orthographically the two are the same distance from the anchor, which is the comparison that
	// says the asymmetry above came from the projection rather than from the rotation.
	const NodeTransform flat{ .Rotation = transform.Rotation, .Anchor = transform.Anchor };
	const Vector3<double> middle{ 50.0, 50.0, 0.0 };

	const float flatLeft = static_cast<float>(Length(flat.Apply({ 0.0F, 50.0F, 0.0F }, radius) - middle));
	const float flatRight = static_cast<float>(Length(flat.Apply({ 100.0F, 50.0F, 0.0F }, radius) - middle));

	CheckNear(flatLeft, flatRight, PositionTolerance, "orthographic is symmetric");
}

GYRO_TEST(NodeTransform, TheNearPlaneNeverCrossesTheQuad)
{
	// The guarantee, swept rather than sampled at the one value it was designed for. Every authored
	// perspective — including the ones nobody should write — has to leave the weight positive at
	// every depth *the node's own quad* can reach, because a single non-positive weight is a division
	// that makes the node's damage bound infinite and takes the whole output's frame with it.
	//
	// This holds by construction rather than by clamping: MinimumRadii puts the eye at least two
	// radii out and every point of the quad is within one, so the bound below is arithmetic. A
	// descendant is a different question and is ComposedTransform's — see the tests further down.
	for (const float authored : { 0.001F, 0.5F, 1.0F, 1.999F, 2.0F, 8.0F, 1.0e9F, -4.0F, 0.0F })
	{
		const Perspective projection = Perspective::FromRadii(authored);

		GYRO_CHECK(projection.Strength() <= 1.0F / Perspective::MinimumRadii);

		for (int step = -64; step <= 64; ++step)
		{
			const float depth = 100.0F * static_cast<float>(step) / 64.0F;
			const float weight = projection.Weight(depth, 100.0F);

			GYRO_CHECK(weight >= 0.5F);
			GYRO_CHECK(weight <= 1.5F);
		}
	}

	// And at the boundary itself, where the eye is as close as the clamp allows and the point is the
	// nearest one the quad has.
	const Perspective closest = Perspective::FromRadii(Perspective::MinimumRadii);

	GYRO_CHECK_EQ(closest.Weight(-100.0F, 100.0F), 0.5F);
	GYRO_CHECK_EQ(closest.Weight(100.0F, 100.0F), 1.5F);

	// The same claim through the transform, which is where it has to hold: a node turned edge-on
	// under the strongest perspective the type permits still lands somewhere finite.
	const NodeTransform transform{ .Rotation = Quaternion::FromAxisAngle({ 0.0F, 1.0F, 0.0F }, Radians(90.0F)),
		                           .Anchor = { 50.0F, 50.0F, 0.0F },
		                           .Projection = Perspective::FromRadii(1.0F) };
	const float radius = transform.BoundingRadius(100.0F, 100.0F);

	for (const Vector3<float> corner : { Vector3<float>{ 0.0F, 0.0F, 0.0F },
	                                     Vector3<float>{ 100.0F, 0.0F, 0.0F },
	                                     Vector3<float>{ 0.0F, 100.0F, 0.0F },
	                                     Vector3<float>{ 100.0F, 100.0F, 0.0F } })
	{
		const Vector3<double> projected = transform.Apply(corner, radius);

		GYRO_CHECK(std::isfinite(projected.X) && std::isfinite(projected.Y) && std::isfinite(projected.Z));
	}
}

// The overview, in the shape the frame thread walks it: a tilted deck holding a turned card holding a
// window. Perspective at more than one level is the case that matters and the case that was broken —
// with no camera, a container is the only thing that can give its children a shared vanishing point,
// so a deck of workspaces that reads as a space rather than as a collage puts the projection on an
// ancestor and every window in it is a descendant.
struct Chain
{
	NodeTransform Nodes[3];
	float Radii[3];

	[[nodiscard]] ComposedTransform Composed() const
	{
		ComposedTransform composed{};

		for (int level = 0; level < 3; ++level)
		{
			composed = composed.Push(Nodes[level], Radii[level]);
		}

		return composed;
	}

	// The same chain walked a point at a time, which is what the composition has to agree with.
	[[nodiscard]] Vector3<double> Walk(Vector3<float> local) const
	{
		Vector3<double> point{ local.X, local.Y, local.Z };

		for (int level = 2; level >= 0; --level)
		{
			point = Nodes[level].Apply(
				{ static_cast<float>(point.X), static_cast<float>(point.Y), static_cast<float>(point.Z) }, Radii[level]
			);
		}

		return point;
	}
};

// `lift` is how far the window has been pulled toward the viewer, out of the plane of the workspace
// holding it — the gesture that broke, and the one worth parameterising because the failure is a
// function of exactly this number. Past the *workspace's* bounding radius, which is a quantity with
// nothing to do with the window, the clamp this header used to carry stopped answering.
[[nodiscard]] static Chain Overview(double lift = 0.0)
{
	Chain chain{ .Nodes = { { .Translation = { 500.0, 300.0, 0.0 },
		                      .Rotation = Quaternion::FromAxisAngle({ 1.0F, 0.3F, 0.0F }, Radians(35.0F)),
		                      .Scale = { 1.1F, 0.9F, 1.0F },
		                      .Anchor = { 400.0F, 300.0F, 0.0F },
		                      .Projection = Perspective::FromRadii(3.0F) },
		                    { .Translation = { 40.0, 30.0, -25.0 },
		                      .Rotation = Quaternion::FromAxisAngle({ 0.0F, 1.0F, 0.0F }, Radians(55.0F)),
		                      .Scale = { 0.8F, 0.8F, 1.0F },
		                      .Anchor = { 100.0F, 60.0F, 0.0F },
		                      .Projection = Perspective::FromRadii(2.5F) },
		                    { .Translation = { 12.0, 8.0, -10.0 - lift },
		                      .Rotation = Quaternion::FromAxisAngle({ 0.2F, 1.0F, 0.1F }, Radians(20.0F)),
		                      .Anchor = { 50.0F, 40.0F, 0.0F },
		                      .Projection = Perspective::FromRadii(4.0F) } },
		         .Radii = {} };

	chain.Radii[0] = chain.Nodes[0].BoundingRadius(800.0F, 600.0F);
	chain.Radii[1] = chain.Nodes[1].BoundingRadius(200.0F, 120.0F);
	chain.Radii[2] = chain.Nodes[2].BoundingRadius(100.0F, 80.0F);

	return chain;
}

GYRO_TEST(NodeTransform, TheComposedChainIsTheChainWalkedAPointAtATime)
{
	// The composition is not obvious and is worth pinning rather than trusting. Apply divides before
	// handing the point up, so the parent rotates an already-divided point — this is not matrix
	// composition on its face, and it works only because each node's weight is affine in the point it
	// is handed. If that ever stops being true this is the test that says so.
	// At rest and mid-gesture both. The lifted chain is the one that matters: a window inside its
	// workspace stays within that workspace's own extent, so a chain at rest agrees under almost any
	// projection, honest or not.
	for (const double lift : { 0.0, 2.0 * static_cast<double>(Overview().Radii[1]) })
	{
		const Chain chain = Overview(lift);
		const ComposedTransform composed = chain.Composed();

		for (const Vector3<float> corner : { Vector3<float>{ 0.0F, 0.0F, 0.0F },
		                                     Vector3<float>{ 100.0F, 0.0F, 0.0F },
		                                     Vector3<float>{ 100.0F, 80.0F, 0.0F },
		                                     Vector3<float>{ 0.0F, 80.0F, 0.0F },
		                                     Vector3<float>{ 37.0F, 19.0F, -12.0F } })
		{
			CheckNear(
				composed.Project(corner).Position, chain.Walk(corner), PositionTolerance, "composed equals walked"
			);
		}
	}
}

GYRO_TEST(NodeTransform, ATextureStaysNailedToARotatedWindow)
{
	// The artefact this whole mechanism exists to prevent, stated as the property a renderer needs.
	// Interpolating anything across a projected quad is correct exactly when the map from the node's
	// own space to the screen is projective — a ratio of two affine functions — and the weight is its
	// denominator. Where it is not, the interpolation is affine per triangle and the content slides
	// across the window as it turns, which is the warp every early 3D console is remembered for.
	//
	// Checked as second differences along an edge: for a projective map both the weight and the
	// position scaled by it are affine in the parameter, so both must vanish. This is the test that
	// fails against the clamp that used to live in Perspective, and it fails by five orders of
	// magnitude rather than marginally.
	//
	// The positions come from the *walk* rather than from the composed matrix, deliberately. The
	// claim is about what the transforms mean, not about whether a matrix is a matrix — the walk is
	// the definition, and a matrix built over a projection that was not projective would satisfy this
	// while the thing on screen did not. The two agree by the test above; that is what makes it
	// legal to take the weight from the fast path and the position from the slow one.
	// Lifted two workspace-radii out, which is where the clamp used to start answering with a
	// constant. An unlifted window sits inside its workspace's own extent and never reaches it, so a
	// chain at rest passes this whether the projection is honest or not — the bug only exists during
	// the gesture, which is exactly when someone is looking at the window closely.
	const Chain chain = Overview(2.0 * static_cast<double>(Overview().Radii[1]));
	const ComposedTransform composed = chain.Composed();

	double worstWeight = 0.0;
	double worstScaled = 0.0;

	for (int step = 1; step < 32; ++step)
	{
		double weights[3]{};
		double scaled[3]{};

		for (int sample = 0; sample < 3; ++sample)
		{
			const float along = 100.0F * static_cast<float>(step + sample - 1) / 32.0F;
			const double weight = composed.Project({ along, 0.0F, 0.0F }).Weight;

			weights[sample] = weight;
			scaled[sample] = chain.Walk({ along, 0.0F, 0.0F }).X * weight;
		}

		worstWeight = std::max(worstWeight, std::abs(weights[2] - 2.0 * weights[1] + weights[0]));
		worstScaled = std::max(worstScaled, std::abs(scaled[2] - 2.0 * scaled[1] + scaled[0]));
	}

	// Loose enough to be about the claim rather than about float rounding: the residual is single
	// precision on a coordinate of a few hundred units, and a map that is not projective misses this
	// by whole pixels.
	GYRO_CHECK(worstWeight < 1.0e-4);
	GYRO_CHECK(worstScaled < 1.0e-1);
}

GYRO_TEST(NodeTransform, AWindowPulledOutOfAWorkspaceKeepsGrowing)
{
	// The regression, named for what it looked like. A window lifted toward the viewer out of a
	// workspace that carries the perspective used to stop growing partway through — its weight froze
	// once its depth passed the *workspace's* bounding radius, which is a number that has nothing to
	// do with the window — while its translation kept running. On screen that reads as the window
	// hitting a pane of glass, with the animation still visibly in flight behind it.
	//
	// So the claim is monotonicity, over a travel well past the ancestor's radius: every step nearer
	// the viewer magnifies strictly more than the last.
	// Measured as the window's diagonal on screen, through the walk rather than through the composed
	// matrix. The claim is about what a person watches — the window is getting bigger — so the
	// quantity has to be a length, and it has to come from the transforms' own meaning rather than
	// from the fast path that was built to agree with them.
	const float workspaceRadius = Overview().Radii[1];

	GYRO_CHECK(workspaceRadius > 0.0F);

	double previous = 0.0;

	// Two and a half workspace-radii, which is past the radius the clamp keyed on by a factor of two
	// and a half and still short of the workspace's eye — the window is being pulled out, not through.
	// Every sample is checked to be in front of it, so a change that moved the eye turns this into a
	// failure here rather than into a monotonicity that quietly stops meaning anything.
	for (int step = 0; step <= 24; ++step)
	{
		const Chain lifted = Overview(2.5 * static_cast<double>(workspaceRadius) * step / 24.0);
		const ComposedTransform composed = lifted.Composed();

		const Vector3<double> nearCorner = lifted.Walk({ 0.0F, 0.0F, 0.0F });
		const Vector3<double> farCorner = lifted.Walk({ 100.0F, 80.0F, 0.0F });
		const double diagonal = Length(farCorner - nearCorner);

		GYRO_CHECK(composed.Project({ 0.0F, 0.0F, 0.0F }).IsVisible());
		GYRO_CHECK(composed.Project({ 100.0F, 80.0F, 0.0F }).IsVisible());
		GYRO_CHECK(diagonal > previous);
		previous = diagonal;
	}
}

GYRO_TEST(NodeTransform, PassingThroughTheEyeIsCulledRatherThanDrawn)
{
	// The one real singularity, and the only thing the composed weight is guarded for. A node that
	// has travelled through the eye of an ancestor is behind the viewer, and no division rescues
	// that — it is unrenderable in the same way a back face is, and decision 55 already culls those
	// unconditionally.
	//
	// Two claims, and the second is the one that keeps a bug from becoming an outage: it is not
	// visible, and the position it reports is still finite. A caller that ignores the flag draws a
	// wrong quad; an infinity here would put a NaN in a damage bound and take every window on the
	// output with it.
	const Projected projected = Overview(1.0e6).Composed().Project({ 50.0F, 40.0F, 0.0F });

	GYRO_CHECK(!projected.IsVisible());
	GYRO_CHECK(std::isfinite(projected.Position.X) && std::isfinite(projected.Position.Y));
	GYRO_CHECK(std::isfinite(projected.Position.Z));

	// A node in front of its ancestors' eyes is drawn, which is the other half of the claim and the
	// reason the floor sits four orders of magnitude away from anything authorable rather than at a
	// magnification anyone could reach.
	GYRO_CHECK(Overview().Composed().Project({ 50.0F, 40.0F, 0.0F }).IsVisible());
}

GYRO_TEST(NodeTransform, TheBoundingRadiusCoversEveryCorner)
{
	// Perspective's guarantee is stated against this number, so it has to be an upper bound on every
	// depth the quad can reach rather than an estimate of one. Rotation preserves length, so a sweep
	// over rotations is a sweep over the whole claim.
	const NodeTransform transform{ .Scale = { 1.5F, 0.75F, 1.0F }, .Anchor = { 10.0F, 90.0F, 0.0F } };
	const float radius = transform.BoundingRadius(100.0F, 100.0F);

	for (int step = 0; step < 16; ++step)
	{
		const float angle = Radians(360.0F * static_cast<float>(step) / 16.0F);
		const Quaternion rotation = Quaternion::FromAxisAngle({ 1.0F, 1.0F, 1.0F }, angle);

		for (const Vector3<float> corner : { Vector3<float>{ 0.0F, 0.0F, 0.0F },
		                                     Vector3<float>{ 100.0F, 0.0F, 0.0F },
		                                     Vector3<float>{ 0.0F, 100.0F, 0.0F },
		                                     Vector3<float>{ 100.0F, 100.0F, 0.0F } })
		{
			const Vector3<float> displaced{ (corner.X - transform.Anchor.X) * transform.Scale.X,
				                            (corner.Y - transform.Anchor.Y) * transform.Scale.Y,
				                            0.0F };

			GYRO_CHECK(std::abs(rotation.Rotate(displaced).Z) <= radius + PositionTolerance);
		}
	}
}

GYRO_TEST(NodeTransform, BackFacesAreVisibleToTheCuller)
{
	// Past a quarter turn the node is showing its back, and the back of a window is mirrored text.
	// The sweep is what makes this a statement about the whole range rather than about the two
	// angles someone thought to try.
	for (int degrees = 0; degrees <= 180; degrees += 5)
	{
		const NodeTransform transform{
			.Rotation = Quaternion::FromAxisAngle({ 0.0F, 1.0F, 0.0F }, Radians(static_cast<float>(degrees)))
		};

		if (degrees == 90)
		{
			// Edge-on has no area either way, and which side it reports is not a question worth
			// having an answer to.
			continue;
		}

		GYRO_CHECK_EQ(transform.FacesViewer(), degrees < 90);
	}

	// A card flip is two nodes precisely because of this: turning one node past ninety degrees does
	// not show its other side, it shows nothing.
	const NodeTransform flipped{ .Rotation = Quaternion::FromAxisAngle({ 0.0F, 1.0F, 0.0F }, Radians(135.0F)) };

	GYRO_CHECK(!flipped.FacesViewer());
}

GYRO_TEST(NodeTransform, FormatsForALogReader)
{
	GYRO_CHECK_EQ(std::format("{}", Vector3<double>{ 1.0, 2.5, 0.0 }), std::string{ "(1, 2.5, 0)" });
	GYRO_CHECK_EQ(std::format("{}", Quaternion{}), std::string{ "rot(none)" });
	GYRO_CHECK_EQ(std::format("{}", Perspective::None()), std::string{ "perspective(none)" });
	GYRO_CHECK_EQ(std::format("{}", Perspective::FromRadii(4.0F)), std::string{ "perspective(4 radii)" });

	// The rotation prints as an angle about an axis because that is the reading a person can act on.
	// Four floats would be correct and useless — "the window took a strange route" is an observation
	// about a direction and an amount, and this is the line that would have to confirm it.
	const Quaternion quarter = Quaternion::FromAxisAngle({ 0.0F, 0.0F, 2.0F }, Radians(90.0F));
	const std::string turned = std::format("{}", quarter);

	GYRO_CHECK(turned.starts_with("rot("));
	GYRO_CHECK(turned.ends_with("deg @ (0, 0, 1))"));
	CheckNear(quarter.Angle(), Radians(90.0F), AngleTolerance, "and the number in it is the angle");

	GYRO_CHECK_EQ(
		std::format("{}", NodeTransform{}),
		std::string{ "transform[translate(0, 0, 0) rot(none) scale(1, 1, 1) anchor(0, 0, 0) perspective(none)]" }
	);
}
