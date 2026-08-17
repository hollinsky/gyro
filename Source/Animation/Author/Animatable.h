#pragma once

#include <type_traits>

#include "Animation/Author/Retarget.h"
#include "Animation/Solve/Spring.h"
#include "Core/Time.h"
#include "Core/Wake.h"

// One animatable property, as the dispatch thread holds it.
//
// A typed wrapper over the model value plus inline spring state: no keypaths, no boxing, no string
// lookup, no allocation. Every one of those exists in other systems to serve a runtime-named
// property, and nothing here names a property at runtime — the shell prescribes state and never
// motion (Docs/Decisions.md decision 51), so the differ knows statically which member it is looking
// at. What that buys is not only speed. A keypath makes "opacity" a string that can be misspelled,
// a boxed value makes the type a runtime question, and both put an allocation on the path a commit
// takes. See Docs/Animation.md#animatable.
//
// **Dispatch-side, and this file is in Author for that reason rather than by filing.** Decision 50
// has the snapshot carry coefficients, so the frame thread evaluates a flat array through
// Animation/Solve/Spring.h and never sees this type. CheckLayering.cmake enforces the direction:
// Solve may not include Author, and Author including Solve — as this file does — is the expected
// shape.
//
// **Two values, three readers.** Docs/Animation.md#model-versus-presentation: layout and
// window-management policy read the model, rendering reads the presentation, and hit-testing reads
// the model by default so that clicking a window in flight hits it where it logically is rather than
// where it happens to be drawn. Neither result is ever written back, because one model value yields
// a different presentation value on each output — evaluated at that output's predicted presentation
// time, and snapped to that output's device grid once it settles.
//
// **There is no stored "is animating" flag, and its absence is the design.** A spring whose offset
// and velocity are both zero has the identically-zero solution: it is at its target, at rest, for
// every instant, in every regime, whatever its coefficients are. So activity is a property of the
// coefficients rather than a fact to be maintained beside them, and a bool would be a second
// representation of what the first already carries — exactly what Docs/Animation.md#storage rules
// out for the flat array, *derived, never maintained*, and the argument does not stop being true one
// level down. It also makes three statements free that would otherwise need care: an at-rest
// property is settled without consulting a threshold, a transition to where the property already is
// is not a transition, and SetImmediate and "never animated" are the same state rather than two that
// behave alike.
//
// **The settling thresholds are arguments because they are not this type's to know.** Geometric ones
// are in device pixels of the finest grid the node intersects; the policy for opacity, blur radius,
// and corner radius is an open question in Docs/Open.md. Spring::SettlesAt takes them as arguments
// for the same reason, and passing them through rather than storing a default is what keeps the
// unresolved half unresolved instead of answered by whoever wrote the default first.
//
// **A chart-valued channel does not come through AnimateTo.** Rotation is the case:
// Docs/Animation.md#transforms springs it in the log map, the log map is anchored at the target, and
// a new target is a new chart — so the subtraction below relates two vectors that live in different
// ones and the stored velocity is a tangent vector at the wrong base point. Animation/Author's
// Retarget records the whole of it, including why there is no cheap test for when the transport can
// be skipped. Such a channel is an Animatable over the deviation against a target of zero, driven
// through Geometry's transport, and that is a construction rather than a call into this file.
//
// **The driven regime of an interactive transition is not expressible here, and saying so is the
// point of this paragraph.** Docs/Animation.md#progress-is-an-ordinary-animatable and decision 65
// make progress an `Animatable<float>` *like any other*, with two regimes: sprung, and driven by a
// finger. The driven one publishes coefficients too — (p0, v0, t0, horizon), read as
//
//     p(T) = p0 + v0 * clamp(T - t0, 0, horizon)
//
// — and that is not reachable from any (omega, zeta). A spring converges on a target; this is a ramp
// that stops at an authored instant. Three members would each grow a second case, and the third is
// the one with teeth. Presentation and PresentationState would switch on the kind. Coefficients
// returns a Spring<V> by reference, which is the shape the publisher serializes, so the snapshot's
// per-node record would become tagged rather than uniform. And NextWake decides the behaviour that
// decision 65 is *for*: **held is settled** wants every frame until t0 + horizon and nothing after,
// and a spring's settle instant is computed from its coefficients rather than authored, so
// Spring::WakeAt cannot answer it however the wake is spelled.
//
// There were two ways out and they are not equivalent, and Docs/Decisions.md decision 72 takes the
// first: the driven regime lives outside this type. It is its own coefficient record with its own
// homogeneous array in the snapshot, so this type stays spring-only and the flat array of active
// springs stays one shape rather than a tagged one. What gives is Animation.md's *like any other*,
// which decision 72 reads as a claim about the *free* regime — the sprung one, which is an ordinary
// Animatable and composes with the idle fold and interruption as such — rather than about the driven
// one, whose exactness comes from being a direct map and not from any (omega, zeta). The rejected
// shape carried a discriminated pair here, made the size assertion below a statement about the larger
// arm, and put a branch on every float channel — opacity, blur, corner radius — to serve a case only
// progress ever reaches. That it was the snapshot's layout as much as this file's is why it was a
// decision and not a default.
//
// **What is deferred for a smaller reason.** Docs/Animation.md#storage has registration into the
// flat array of active springs happen inside this type's own methods, when a spring becomes active
// or settles. Becoming active is AnimateTo. Settling is not an event any method here observes — the
// settle instant is analytic, so it is the wake fold that discovers it, dispatch-side and with the
// thresholds in hand — and the method that retires a settled spring belongs with the publisher that
// owns the array. Writing the hook before the array exists would fix that interface from the wrong
// end.

// Constrained on Animation/Solve's own concept rather than on one of its own, because SpringValue
// already asks for exactly the right thing — a vector space with a norm — and a second name for one
// set of requirements is a second thing to keep true.
template<SpringValue V>
class Animatable
{
public:
	using Value = V;
	using Scalar = SpringScalar<V>;

	constexpr Animatable() noexcept = default;

	// The value the property starts at, at rest. A node is constructed carrying the state the shell
	// would have set had it been asked, which is what makes the first commit against it an ordinary
	// transition rather than a case of its own.
	constexpr explicit Animatable(V value) noexcept : m_Spring{ .Target = value } {}

	// What the shell set. Read by layout, by window-management policy, and by hit-testing.
	[[nodiscard]] constexpr const V& Model() const noexcept { return m_Spring.Target; }

	// Where the property has got to, and how fast, at an output's predicted presentation time.
	//
	// Both halves at once because both are wanted at once and computing them separately evaluates
	// the same transcendentals twice. Velocity is not a rendering input — it is what a gesture
	// taking over a transition in flight reads, which Docs/Animation.md#progress-is-an-ordinary-animatable
	// calls *one scalar* and which would otherwise have to be reached through Coefficients(), an
	// accessor that exists for the publisher rather than for a dispatch-side read.
	//
	// Pure and closed form, so evaluating at two instants in either order gives the same answers —
	// which is what a missed frame is paid for with, and what lets one property serve a 144 Hz panel
	// and a 60 Hz projector in the same iteration.
	[[nodiscard]] constexpr SpringState<V> PresentationState(Instant at) const noexcept
	{
		return IsAtRest() ? SpringState<V>{ m_Spring.Target, V{} } : m_Spring.Evaluate(at);
	}

	// What is drawn. Defined through the state above rather than beside it, so the two cannot
	// disagree about where the property is.
	[[nodiscard]] constexpr V Presentation(Instant at) const noexcept { return PresentationState(at).Position; }

	// What this property contributes to the idle fold, which Core/Wake.h reduces with Sooner. A
	// property in flight owes every frame the output offers until it settles, and nothing after;
	// there is no next interesting instant it could name instead, because every instant until then
	// is one. Docs/Animation.md#settling-answers-with-a-wake-not-a-boolean carries the argument for
	// the type against the boolean this method used to return.
	//
	// At rest the answer is Settled whatever the thresholds are, which is the only part of settling
	// that can be stated before the non-geometric policy exists.
	[[nodiscard]] constexpr Wake NextWake(Instant at, SettleThresholds<Scalar> thresholds) const noexcept
	{
		return IsAtRest() ? Wake::Never() : m_Spring.WakeAt(at, thresholds);
	}

	// Set the model value and move towards it under the given motion, from wherever the property had
	// got to and at whatever speed it was going.
	//
	// `t0` is the originating input event's timestamp rather than the moment of handling, so a late
	// commit renders already in progress by exactly the elapsed amount instead of starting from
	// zero: lateness costs the first frame or two of an animation, never its shape.
	//
	// The motion arrives resolved. Numeric spring construction lives in the catalog and nowhere else
	// (Docs/Animation.md#the-motion-catalog), and this type is machinery on the path between the two
	// rather than a call site — the differ hands over what a named transition resolved to. When the
	// catalog exists, resolution happens in front of this call and not inside it.
	//
	// One path rather than a rest case and a flight case, because PresentationState already answers
	// both: at rest it is the target at zero velocity, which is exactly what a fresh spring wants to
	// be handed. That the result is identical to Animation/Author's Retarget is pinned by a test —
	// a divergence would show up as a first frame starting from slightly the wrong place and
	// nowhere else.
	constexpr void AnimateTo(const V& target, SpringParameters<Scalar> motion, Instant t0) noexcept
	{
		const SpringState<V> from = PresentationState(t0);

		m_Spring = Begin(motion, from.Position, from.Velocity, target, t0);
	}

	// The same transition, entered at a velocity that came from somewhere other than this property.
	//
	// This is the release of a gesture. Decision 65's free regime springs progress toward an end
	// "with `v0` the gesture's velocity", and that velocity is a fit over a short window on the
	// input side rather than anything this property was doing — a finger decelerates as it lifts, so
	// the property's own velocity at release is near zero and handing that off is precisely the
	// flick that dies on release. The overload above preserves; this one imposes.
	//
	// Also the way a gesture takes over a transition already in flight: read PresentationState, map
	// the velocity into the new channel's units, and re-enter here.
	constexpr void AnimateTo(const V& target, SpringParameters<Scalar> motion, Instant t0, const V& velocity) noexcept
	{
		m_Spring = Begin(motion, Presentation(t0), velocity, target, t0);
	}

	// This value, now, with no motion. What a property that is not being animated is set through.
	//
	// A whole new spring rather than a hard settle of the old one, so that nothing survives that the
	// next AnimateTo would have to be careful about: the origin and the coefficients of a motion that
	// has been cancelled are leftovers, and at rest nothing reads them.
	constexpr void SetImmediate(const V& value) noexcept { m_Spring = Spring<V>{ .Target = value }; }

	// Put the property on the model value it is already heading for, at rest, now.
	//
	// Not a convenience and not an optimization, which is why it has a name rather than being spelled
	// SetImmediate(Model()) at each of its two callers. Resume runs it over everything so that nobody
	// wakes into the middle of yesterday's transition, and the snapshot atlas runs it to finish exits
	// early when it is out of room — a path that has to work with no GPU at all, which is why it is
	// arithmetic on coefficients rather than anything that touches a device. Animation/Author's
	// HardSettle is the same operation on a bare spring.
	constexpr void Settle() noexcept { SetImmediate(m_Spring.Target); }

	// Whether this property owes anything. Derived from the coefficients rather than tracked beside
	// them — see the header comment, where that is the point rather than an implementation note.
	//
	// Cheap rather than free, and the difference is worth stating because the cost is what the
	// derivation is traded against: on a scalar channel this is two compares, and on a vector one it
	// is two evaluations of the channel's norm, which for a translation or a log-map rotation is a
	// square root. Still an order off the transcendentals it guards, and paid once per access rather
	// than per component. It assumes what any norm gives — that the norm of the zero vector is zero —
	// which SpringValue asks for by requiring a norm at all and cannot state as an axiom.
	//
	// Not the same question as *settled*: a spring inside its thresholds has finished as far as the
	// schedule is concerned but still carries a residual offset, and it stays not-at-rest until
	// something retires it. Both answers are correct for their own question, and NextWake is the one
	// that decides whether a frame is owed.
	[[nodiscard]] constexpr bool IsAtRest() const noexcept
	{
		return Magnitude(m_Spring.Offset) == Scalar(0) && Magnitude(m_Spring.Velocity) == Scalar(0);
	}

	// What the publisher emits into the snapshot. Decision 50 has coefficients cross the boundary
	// rather than evaluated values, and this is the read that serializes them; the frame side
	// reconstitutes a Spring<V> from bytes at an offset and evaluates it there.
	//
	// Const, so the rule that only this type's own methods can change activity state survives having
	// an accessor at all. Not the way to read a value or a velocity on the dispatch side — that is
	// PresentationState, which short-circuits at rest and cannot be mistaken for the publication
	// path.
	[[nodiscard]] constexpr const Spring<V>& Coefficients() const noexcept { return m_Spring; }

private:
	Spring<V> m_Spring{};
};

// The contract everything downstream assumes. More of it is available here than in Animation/Solve,
// because every path below stays on the at-rest side and therefore never reaches exp, sin, or sqrt —
// which is itself worth noticing: the cases a constant expression can reach are exactly the ones the
// derived-activity design made free.
//
// Both precisions, because they are two records rather than one instantiated twice:
// Docs/Architecture.md#the-spaces puts positions across the boundary at double and everything else at
// single, and Spring<float> carries four bytes of tail padding that the publisher must
// value-initialize rather than leave as whatever the arena held.
static_assert(std::is_trivially_copyable_v<Animatable<float>>, "A property is held by value, in the node that owns it");
static_assert(std::is_trivially_copyable_v<Animatable<double>>);

// Nothing is stored beside the coefficients: activity is derived from them rather than flagged. This
// is the assertion the driven regime of decision 65 has to break if that regime moves inside this
// type, at which point it becomes a statement about the larger arm rather than about the whole.
static_assert(sizeof(Animatable<float>) == sizeof(Spring<float>));
static_assert(sizeof(Animatable<double>) == sizeof(Spring<double>));

// Deliberately not asserted standard layout. This type does not cross the publication boundary;
// Coefficients() is what crosses, and Animation/Solve asserts the layout of that.

static_assert(Animatable<double>{}.Model() == 0.0, "A property starts at its value type's own zero");
static_assert(Animatable<double>{ 3.0 }.Model() == 3.0);
static_assert(Animatable<double>{ 3.0 }.IsAtRest(), "and starts at rest");
static_assert(Animatable<double>{ 3.0 }.Presentation(Monotonic::FromNanoseconds(1'000'000'000)) == 3.0);
static_assert(Animatable<double>{ 3.0 }.PresentationState(Monotonic::FromNanoseconds(1'000'000'000)).Velocity == 0.0);

// At rest is settled whatever the thresholds are — including thresholds of zero, which no real
// spring would ever reach.
static_assert(Animatable<double>{ 3.0 }.NextWake(Instant{}, {}) == Wake::Never());
static_assert(Animatable<double>{ 3.0 }.NextWake(Monotonic::FromNanoseconds(7), {}) == Wake::Never());

static_assert(
	[] {
		Animatable<double> value{ 3.0 };

		value.AnimateTo(10.0, ParametersFromResponse(0.4, 1.0), Instant{});

		return value.Model() == 10.0 && !value.IsAtRest();
	}(),
	"The model is the target from the moment it is set; the presentation is the half that takes time"
);

static_assert(
	[] {
		Animatable<double> value{ 3.0 };

		value.AnimateTo(3.0, ParametersFromResponse(0.4, 1.0), Instant{});

		return value.IsAtRest();
	}(),
	"A transition to where the property already is is not a transition, and owes no frames"
);

// The velocity overload is what keeps a flick alive across release, so a property that was at rest
// is not at rest afterwards even though its target is where it already was.
static_assert(
	[] {
		Animatable<double> value{ 3.0 };

		value.AnimateTo(3.0, ParametersFromResponse(0.4, 1.0), Instant{}, 250.0);

		return !value.IsAtRest() && value.Coefficients().Velocity == 250.0;
	}(),
	"An imposed velocity is a motion even when the target has not moved"
);

static_assert(
	[] {
		Animatable<double> value{ 3.0 };

		value.AnimateTo(10.0, ParametersFromResponse(0.4, 1.0), Instant{});
		value.SetImmediate(4.0);

		return value.IsAtRest() && value.Model() == 4.0 &&
	           value.Presentation(Monotonic::FromNanoseconds(1'000'000)) == 4.0;
	}(),
	"SetImmediate stops the motion rather than retargeting it"
);

static_assert(
	[] {
		Animatable<double> value{ 3.0 };

		value.AnimateTo(10.0, ParametersFromResponse(0.4, 1.0), Instant{}, 250.0);
		value.Settle();

		return value.IsAtRest() && value.Model() == 10.0;
	}(),
	"Settling moves the property to its target, not the target to the property"
);
