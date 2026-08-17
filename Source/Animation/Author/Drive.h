#pragma once

#include <limits>

#include "Geometry/Space.h"

// The map from a gesture's displacement to progress.
//
// This is the one thing in the catalog that is not a spring, and Docs/Decisions.md decision 13 puts
// it here rather than in the shell for the reason everything else in the catalog is here: how far a
// gesture must travel to mean *all the way*, and how it behaves past the ends, is motion design by
// every test that decision applies. Left to the shell it would be per-shell, which is the same
// incohesion the springs are centralized to prevent, arriving through the one channel the shell
// legitimately touches.
//
// An interactive transition is a bundle whose channels are functions of one scalar p, and p is an
// ordinary Animatable<float> with two regimes: driven while a finger is down, and sprung to an end on
// release. Only the first needs this file. See Docs/Animation.md#interactive-transitions and decision
// 65, which is also where the rejected alternative lives — letting the gesture move the springs'
// targets, which lags the finger by 2v/omega and inverts through zero on every reversal.
//
// **Travel is a displacement in global space, and the type is doing real work.** It has to be
// output-independent: a travel expressed in device pixels would make the same swipe mean a different
// fraction of the transition on a 1x and a 1.5x monitor, which is invisible on one display and wrong
// the moment a second is attached. Global space is the only space that is continuous,
// output-independent, and ungridded, and its basis unit — one logical pixel at scale 1 — is exactly
// what libinput's accelerated deltas are denominated in, so a gesture delta *is* a value of this type
// with no adapter between them.
//
// That last claim is worth the sentence it takes, because it is the one that could rot. libinput
// normalizes accelerated relative motion to a 1000dpi mouse and documents the result as pixels on a
// low-dpi screen, with scaling left to the caller; swipe deltas come out of the same filter as
// pointer deltas on a touchpad, so the two are interchangeable. What is *not* interchangeable is the
// unaccelerated pair — pointer-unaccelerated is raw device units while gesture-unaccelerated is
// dpi-normalized and additionally scaled — and touch is millimetres from the device's own corner. So
// there are two real conversions in front of this type, and its space tag is what obliges them to be
// written rather than assumed.
//
// **A vector rather than a distance, and that is not decoration.** The mapping needs the gesture's
// direction anyway — a workspace swipe is horizontal and an overview swipe is vertical — and folding
// it into the same value means one expression covers both. A diagonal gesture projects correctly
// instead of needing a case, and a reversal comes out negative from the arithmetic rather than from a
// comparison somebody has to remember to write.
//
// **Total, for the reason Animation/Author/Retarget.h's clamps are total.** This is the ingest
// boundary for a number that arrives from a device, and the value it produces reaches a spring: a
// progress that is not finite gives an Animatable an offset and a velocity that are never zero, so it
// never comes to rest, so the compositor never idles. That is the invariant Retarget.h's damping floor
// exists to keep out of a configuration file, and a gesture delta is a weaker source than a
// configuration file rather than a stronger one. Retarget.h's own words apply unchanged: the floor is
// what makes settling a property of the type rather than of the input.
//
// So the two guards below answer with the conservative motion rather than propagating. That is a
// reversal of what this file said first, which returned a not-a-number unchanged on the argument that
// folding it to zero destroys the evidence of an upstream defect. The argument is right and loses
// anyway: the evidence is worth one gesture and the idle invariant is worth the machine. **The input
// side owes the rejection**, at the point the delta is read from libinput, where the device that
// produced it is still in hand — the same shape as the velocity estimator it already owes, and the
// place a diagnostic can name something. What the guards here buy is that a missed rejection costs a
// gesture rather than the power budget.
//
// **What is deliberately not here.** Detents — the interior progress values a release can land on
// besides the two ends — are named by decision 13 and are absent, by the same test that keeps
// staggering out of Animation/Author/Bundle.h: a transition authored today has no detents, that is
// correct for all of them, and a field defaulting to none would change nothing about any. They also
// cannot be inert data, because choosing between a detent and an end at release needs a flick
// threshold in progress per second, and that number is input-side tuning nobody has measured. The
// lead horizon that carries p toward predicted presentation time is likewise absent and is not this
// file's in any case — it is a scheduling parameter, open in Docs/Open.md, and one output period is
// the obvious first answer.

// How a gesture's displacement becomes progress.
//
// A default-constructed mapping has zero travel, which means *not drivable*, so a bundle that is not
// interactive needs no optional and says nothing. That is why the degenerate case below returns zero
// rather than being a precondition: it is a state entries are expected to be in, not a mistake.
struct DriveMapping
{
	// The displacement that means all the way. Its magnitude is how far, its direction is which way.
	Offset<GlobalSpace> Travel{};

	// How far past either end the gesture can ever reach, in progress. The pull past the end is
	// compressed onto this asymptotically, so the content follows the finger with visibly increasing
	// resistance and never separates from it — which is what tells a person they are at the end
	// without a hard stop that reads as the gesture having been dropped.
	//
	// Zero is a hard clamp, and is what a transition with nothing past its ends asks for.
	//
	// **The default is a value rather than nothing, and it is the value that is right when an entry
	// says nothing.** A band changes how a gesture feels, so it is the kind of default this file is
	// otherwise suspicious of — staggering is absent precisely because a default would be inert and
	// the anchor is mandatory in spirit precisely because a default would not be harmless. This one
	// passes the anchor's test rather than staggering's: content that stops dead under a finger still
	// on the glass is wrong for every gesture, not merely unlike the one the author had in mind, so
	// the harmful default here is zero and an entry that wants a hard stop is the one making a claim.
	double RubberBand = 0.1;

	friend constexpr bool operator==(DriveMapping, DriveMapping) noexcept = default;

	[[nodiscard]] constexpr bool IsDrivable() const noexcept { return Dot(Travel, Travel) > 0.0; }
};

namespace Detail
{
// The excess past an end, compressed onto the band.
//
// e*b/(e+b) rather than any of the other curves that asymptote, because of what it does at zero: its
// value is zero and its first derivative is one, so it joins the linear region with no visible kink
// at the moment the finger crosses the end. A curve that met the boundary at any other slope would
// read as the content snagging exactly where it is supposed to feel continuous.
[[nodiscard]] constexpr double Compressed(double excess, double band) noexcept
{
	// Not-a-number and non-positive bands both land here, and a hard clamp is the conservative
	// answer: it refuses to move past the end rather than moving an unbounded distance past it.
	if (!(band > 0.0) || !(excess > 0.0))
	{
		return 0.0;
	}

	// An infinite excess is the projection having overflowed rather than a finger having gone that
	// far, and the band is the limit this curve approaches, so it is both the mathematically right
	// answer and the one that keeps the bound below true. Written as a guard rather than left to the
	// arithmetic because the arithmetic does not do it: inf*b/(inf+b) is inf/inf, which is a
	// not-a-number, and it would reach a spring as one.
	if (!(excess < std::numeric_limits<double>::infinity()))
	{
		return band;
	}

	return excess * band / (excess + band);
}
} // namespace Detail

// Where a gesture has got to, before either end is applied. Linear, exact, and signed.
//
// The projection of the displacement onto the travel, normalized by the travel's own square — so a
// displacement equal to the travel is one, one across it is zero, and one against it is negative.
[[nodiscard]] constexpr double RawProgress(const DriveMapping& mapping, Offset<GlobalSpace> displacement) noexcept
{
	const double extent = Dot(mapping.Travel, mapping.Travel);

	// A mapping with no travel is one that was never meant to be driven. Answering zero rather than
	// dividing keeps a bundle that is not interactive from being a special case at every call site,
	// and the negated comparison takes a travel that is not a number with it.
	if (!(extent > 0.0))
	{
		return 0.0;
	}

	const double raw = Dot(displacement, mapping.Travel) / extent;

	// Self-comparison rather than std::isnan, which is not dependably constexpr across libraries, and
	// as Retarget.h and Motion.h both spell it. A displacement that is not a number is a gesture the
	// transition cannot be anywhere in, so it is the gesture's origin — the one answer that is both
	// bounded and stationary, which is what keeps it out of the spring. An infinite one is not folded
	// here: it is a real direction with an unusable magnitude, and the band below is what it is for.
	return raw == raw ? raw : 0.0;
}

// Where a gesture has got to, as the transition's channels read it.
//
// Inside the ends this is exactly the projection: scrubbing is exact because nothing is converging on
// anything, which is the whole reason progress is driven rather than sprung. Outside them the excess
// is compressed onto the band, so the value is bounded by [-RubberBand, 1 + RubberBand] however far
// the finger goes.
[[nodiscard]] constexpr double Progress(const DriveMapping& mapping, Offset<GlobalSpace> displacement) noexcept
{
	const double raw = RawProgress(mapping, displacement);

	if (raw > 1.0)
	{
		return 1.0 + Detail::Compressed(raw - 1.0, mapping.RubberBand);
	}

	if (raw < 0.0)
	{
		return -Detail::Compressed(-raw, mapping.RubberBand);
	}

	// Nothing that is not a number reaches here — the projection folds it — so this is the linear
	// interior and only the linear interior, and the bound the header claims holds for every
	// displacement rather than for every displacement worth having.
	return raw;
}

// The contract everything downstream assumes. All of it, because this file is arithmetic — nothing
// here reaches a transcendental, which is what lets the mapping be checked rather than sampled.

namespace Detail
{
// The mappings the assertions below are written against. In Detail because they are fixtures rather
// than vocabulary: this header is included by the catalog and therefore by everything downstream of
// it, and a name as ordinary as Horizontal sitting in the global namespace is one an unrelated file
// collides with and has no reason to expect.
//
// Exact is the second because scrubbing has to be checked on powers of two throughout, so that the
// assertion is about the mapping rather than about where the rounding of a decimal fraction happened
// to land. Its band is zero, which is also the hard-clamp case.
inline constexpr DriveMapping Horizontal{ .Travel = { 1200.0, 0.0 }, .RubberBand = 0.1 };
inline constexpr DriveMapping Exact{ .Travel = { 1024.0, 0.0 }, .RubberBand = 0.0 };
} // namespace Detail

static_assert(!DriveMapping{}.IsDrivable(), "A bundle that is not interactive says so by having no travel");
static_assert(Detail::Horizontal.IsDrivable());
static_assert(Progress(DriveMapping{}, { 500.0, 0.0 }) == 0.0, "and cannot be driven anywhere");

// The three properties the projection exists for.
static_assert(Progress(Detail::Horizontal, { 0.0, 0.0 }) == 0.0);
static_assert(Progress(Detail::Horizontal, { 1200.0, 0.0 }) == 1.0, "The travel is what means all the way");
static_assert(Progress(Detail::Horizontal, { 600.0, 0.0 }) == 0.5);
static_assert(Progress(Detail::Horizontal, { 0.0, 900.0 }) == 0.0, "Motion across the travel is not motion along it");

// A diagonal projects rather than needing a case of its own, and the cross-axis component is
// discarded exactly rather than approximately.
static_assert(Progress(Detail::Horizontal, { 600.0, 4000.0 }) == 0.5);

// Scrubbing is exact inside the ends: equal displacements produce equal changes in progress wherever
// in the range they happen, which is the property a spring cannot have and the reason the driven
// regime is not one.
static_assert(Progress(Detail::Exact, { 512.0, 0.0 }) - Progress(Detail::Exact, { 256.0, 0.0 }) == 0.25);
static_assert(
	Progress(Detail::Exact, { 896.0, 0.0 }) - Progress(Detail::Exact, { 640.0, 0.0 }) == 0.25,
	"and at the far end too"
);

// A reversal is negative, and falls out of the arithmetic rather than out of a comparison.
static_assert(Progress(Detail::Horizontal, { -600.0, 0.0 }) < 0.0);
static_assert(RawProgress(Detail::Exact, { -512.0, 0.0 }) == -0.5, "Backwards is the forwards case with a sign");

// The band is an asymptote rather than a clamp: always past the end, never past the bound, however
// hard the gesture is pulled.
static_assert(Progress(Detail::Horizontal, { 1201.0, 0.0 }) > 1.0);
static_assert(Progress(Detail::Horizontal, { 12000.0, 0.0 }) < 1.0 + Detail::Horizontal.RubberBand);
static_assert(Progress(Detail::Horizontal, { 1.2e9, 0.0 }) < 1.0 + Detail::Horizontal.RubberBand);
static_assert(
	Progress(Detail::Horizontal, { 12000.0, 0.0 }) < Progress(Detail::Horizontal, { 120000.0, 0.0 }),
	"Pulling harder still moves, or the content reads as having come loose from the finger"
);

// Both ends behave alike, which is not automatic — the low end is the one a reversal reaches and the
// one an off-by-a-sign would leave unbounded.
static_assert(Progress(Detail::Horizontal, { -12000.0, 0.0 }) > -Detail::Horizontal.RubberBand);
static_assert(Progress(Detail::Horizontal, { -1.2e9, 0.0 }) > -Detail::Horizontal.RubberBand);

// A zero band is a hard clamp, which is what a transition with nothing past its ends asks for.
static_assert(Progress({ .Travel = { 1200.0, 0.0 }, .RubberBand = 0.0 }, { 12000.0, 0.0 }) == 1.0);
static_assert(Progress({ .Travel = { 1200.0, 0.0 }, .RubberBand = 0.0 }, { -12000.0, 0.0 }) == 0.0);

// The join at the end is continuous in slope, which is what this curve was chosen for over the
// others that asymptote. The step immediately outside the end is within a few percent of the step
// immediately inside it, so a finger crossing the boundary feels resistance appear rather than the
// content snag at exactly the moment it is supposed to feel continuous.
static_assert(
	Progress(Detail::Horizontal, { 1199.0, 0.0 }) < 1.0 && Progress(Detail::Horizontal, { 1201.0, 0.0 }) > 1.0
);
static_assert(
	Progress(Detail::Horizontal, { 1202.0, 0.0 }) - Progress(Detail::Horizontal, { 1200.0, 0.0 }) >
	0.95 * (Progress(Detail::Horizontal, { 1200.0, 0.0 }) - Progress(Detail::Horizontal, { 1198.0, 0.0 }))
);

// **Totality, which is the half that keeps a gesture out of the power budget.** Every one of these
// reaches a spring if it is wrong, and a spring that is handed a value which is not a number never
// comes to rest — so the bound above has to hold for every displacement rather than for every
// plausible one.
namespace Detail
{
inline constexpr double NotANumber = std::numeric_limits<double>::quiet_NaN();
inline constexpr double Unbounded = std::numeric_limits<double>::infinity();
} // namespace Detail

static_assert(
	Progress(Detail::Horizontal, { Detail::NotANumber, 0.0 }) == 0.0,
	"A gesture the transition cannot be anywhere in is at its origin, which is bounded and still"
);
static_assert(Progress(Detail::Horizontal, { 0.0, Detail::NotANumber }) == 0.0, "including across the travel");
static_assert(
	Progress({ .Travel = { Detail::NotANumber, 0.0 } }, { 600.0, 0.0 }) == 0.0,
	"and a travel that is nonsense"
);

// An infinity is a real direction with an unusable magnitude, so it lands on the bound rather than
// at the origin — which is the answer the curve converges to anyway, reached by a guard because the
// arithmetic that would produce it is inf/inf.
static_assert(Progress(Detail::Horizontal, { Detail::Unbounded, 0.0 }) == 1.0 + Detail::Horizontal.RubberBand);
static_assert(Progress(Detail::Horizontal, { -Detail::Unbounded, 0.0 }) == -Detail::Horizontal.RubberBand);
static_assert(Progress(Detail::Exact, { Detail::Unbounded, 0.0 }) == 1.0, "and a hard clamp stays a hard clamp");

// The case that motivated the guard is the overflow rather than the infinity somebody typed — a
// displacement finite in itself whose projection is not — and it is in Drive.Test.cpp rather than
// here, because producing an infinity from finite operands is not a constant expression.

// A travel that is not a number is not drivable, so the bundle holding it is not interactive and the
// gesture never begins. The guard in the projection is the second line rather than the first.
static_assert(!DriveMapping{ .Travel = { Detail::NotANumber, 0.0 } }.IsDrivable());
