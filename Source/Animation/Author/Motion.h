#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "Animation/Author/Retarget.h"
#include "Animation/Solve/Spring.h"

// The closed motion vocabulary, and the one table configuration is allowed to touch.
//
// Cohesion is won or lost here rather than in the solver. It fails when a window-open uses
// (0.42, 0.83) and a menu-appear uses (0.45, 0.80) — both defensible in isolation, and the system
// feels subtly incoherent forever after because nobody catches it in review, since both diffs look
// reasonable. The answer is that call sites cannot name numbers at all: they name a motion from the
// list below, and the numbers live here and nowhere else.
//
// **Bundles reference a motion by name, which is what makes the configuration rule structural.**
// Docs/Decisions.md decision 13 requires that configuration expose the vocabulary and never the
// individual transitions, because retuning Standard has to move everything built on Standard
// together. That is usually written as a rule about what a parser may read. It is stronger than that
// here: an Animation/Author/Bundle.h entry holds a Motion, and a Motion has nowhere to put a
// response or a damping ratio, so a configuration file has nowhere to reach even if its author
// wanted to. The granularity is a property of the types rather than of the parser's discipline.
//
// **There is no Motion::Custom enumerator, and Docs/Animation.md#the-motion-catalog names one.** The
// escape hatch survives; its spelling does not. Under a per-call-site model, Custom is a motion a
// call site names instead of Standard, and an enumerator is the natural shape. Under the bundle
// model it cannot be: an enumerator is resolved by looking it up in the table below, and the whole
// content of Custom is that it is *not* in the table. Giving it a row would make it a sixth motion
// that configuration retunes, which is the opposite of an escape hatch. So the hatch is CustomMotion
// below, which is greppable in one command, obvious in review, and — unlike an enumerator — cannot be
// reached from a bundle at all. See Docs/Decisions.md decision 12 for why the authoring units are
// (response, dampingRatio).

enum class Motion : std::uint8_t
{
	Standard,    // the workhorse — most state transitions
	Snappy,      // direct-manipulation feedback, minimal overshoot
	Gentle,      // ambient and background changes, no bounce
	Expressive,  // large or attention-drawing changes
	Interactive, // the residual under a finger: pushback at a constraint, the pull into a snap
};

inline constexpr std::size_t MotionCount = 5;

// Five to seven, and growth past that is cohesion leaking rather than expressiveness arriving.
// Asserted rather than described, so that adding a sixth is a deliberate act with a diff that says
// so.
static_assert(static_cast<std::size_t>(Motion::Interactive) + 1 == MotionCount);

// The vocabulary, in one place that a pass over it can name. Written out rather than generated from
// the count, because a cast from an integer to an enumerator is how a list like this silently starts
// including values nobody declared — and named here rather than spelled at each of its uses, because
// a sixth motion added to the enum and forgotten in a loop is a motion nothing checks.
inline constexpr std::array<Motion, MotionCount> AllMotions{
	Motion::Standard, Motion::Snappy, Motion::Gentle, Motion::Expressive, Motion::Interactive,
};

// The list is the enumeration in order, which is what makes a pass over it a pass over the enum. The
// size is already fixed by MotionCount, so what is left to go wrong is a duplicate standing in for
// the entry somebody meant to add — which reads correctly and silently drops a motion from every
// sweep that uses this.
static_assert([] {
	for (std::size_t index = 0; index < AllMotions.size(); ++index)
	{
		if (static_cast<std::size_t>(AllMotions[index]) != index)
		{
			return false;
		}
	}

	return true;
}());

// One motion, in the units it is authored in. Stored rather than converted so that the table reads
// as what a person tuned: a natural period in seconds, and how much it bounces.
//
// Double whatever the channel is. A channel's precision belongs to the channel — Docs/Architecture.md
// puts translation across the boundary at double and everything else at single — and narrowing here
// would hand a float channel a value that had already been rounded once. Resolve below narrows once,
// at the point the channel type is known.
struct MotionParameters
{
	double Response = 0.4; // the natural period, seconds
	double Damping = 1.0;  // the damping ratio, dimensionless

	friend constexpr bool operator==(MotionParameters, MotionParameters) noexcept = default;
};

// The vocabulary's values, and the fallback a configuration file overlays.
//
// A member per motion rather than an array indexed by the enum, because this is the one structure in
// the catalog a human tunes by hand and a designated initializer is what makes a partial override
// readable. The index operator below is for the passes that need to be generic over the vocabulary,
// which is the overlay and the cohesion test.
//
// **A damping ratio of exactly one is deliberate and is not a placeholder.** "No bounce" is a ratio
// at or above unity, which puts four of these five on or beside Animation/Solve/Spring.h's critical
// band — that header says as much, and says the band is sized so that landing on it is the well
// conditioned case rather than the hostile one.
struct MotionTable
{
	MotionParameters Standard{ .Response = 0.40, .Damping = 1.00 };
	MotionParameters Snappy{ .Response = 0.22, .Damping = 1.00 };
	MotionParameters Gentle{ .Response = 0.60, .Damping = 1.00 };
	MotionParameters Expressive{ .Response = 0.55, .Damping = 0.68 };
	MotionParameters Interactive{ .Response = 0.30, .Damping = 1.00 };

	friend constexpr bool operator==(MotionTable, MotionTable) noexcept = default;

	[[nodiscard]] constexpr const MotionParameters& operator[](Motion motion) const noexcept
	{
		switch (motion)
		{
			case Motion::Snappy:
				return Snappy;
			case Motion::Gentle:
				return Gentle;
			case Motion::Expressive:
				return Expressive;
			case Motion::Interactive:
				return Interactive;
			case Motion::Standard:
				break;
		}

		// Standard is the fallthrough as well as a case, so a value cast in from outside the
		// enumeration resolves to the workhorse rather than to whatever lay at that offset. Nothing
		// in gyro produces one; a configuration file's integer key could.
		return Standard;
	}
};

// The global modifiers, which are the rest of what configuration may express.
//
// Deliberately a short list. Every modifier here moves the whole system at once, which is what makes
// it safe to expose: there is no setting in this struct that can make two transitions disagree with
// each other. A modifier that could would be a transition-level control wearing a global's name.
//
// Speed is a preference rather than a debugging tool. Subtree slow-motion for watching an animation
// is Docs/Animation.md#hierarchical-time's TimeScale, which is a different mechanism at a different
// granularity, and conflating the two would put a debugging affordance in a user's configuration.
struct MotionModifiers
{
	// Larger is faster, so it divides the response. One is the authored table unchanged.
	double Speed = 1.0;

	friend constexpr bool operator==(MotionModifiers, MotionModifiers) noexcept = default;
};

// The range the speed modifier is clamped into. Powers of two, so the clamp rounds nothing, and
// generous in both directions — a sixteenth is a system that looks stopped and sixteen is one whose
// animations are over before they are seen, and neither is a value anyone should reach by accident.
//
// A zero or negative speed is the one that must not survive: it would divide the response to
// infinity or to a negative number, and Retarget.h's clamp would then quietly turn every motion in
// the system into the slowest one it allows. Refusing it here keeps that from being reachable from a
// text file.
inline constexpr double MinimumSpeed = 1.0 / 16.0;
inline constexpr double MaximumSpeed = 16.0;

// One motion's worth of overlay, as a configuration file supplies it: each field present only if the
// file named it.
//
// **A distinct type from MotionParameters, and the distinction is the whole of what keeps a partial
// configuration from resetting what it did not mention.** MotionParameters is an authored value and
// every field of it is populated — it is a point in the space, and a default-constructed one is the
// *taste* values 0.4 and 1.0. An overlay is not a point: a file that sets a motion's damping and says
// nothing about its response has to leave the response authored, so "said nothing" is a state the
// type has to be able to hold. Reused for the overlay, MotionParameters cannot — a parser that filled
// only the field it was given would leave the other reading as an explicit 0.4, and Overlay would lay
// that taste value over the catalog and silently move a motion nobody configured. std::optional is
// what makes absence representable, so the empty overlay overrides nothing and omission cannot clobber.
struct ConfiguredMotion
{
	std::optional<double> Response;
	std::optional<double> Damping;

	friend constexpr bool operator==(ConfiguredMotion, ConfiguredMotion) noexcept = default;
};

// One configured value laid over one authored value.
//
// **Per key, and that is the requirement rather than an implementation detail.** A system-layer
// compositor cannot fail to start because someone fat-fingered a damping ratio, so a bad value takes
// the authored value beside it rather than discarding the table, the motion, or even the other half
// of the same motion. Docs/Animation.md#runtime-configuration states it; this is where it is true.
//
// Three ways a field defers to the authored value, and they are not the same thing. An *absent*
// optional is the configuration declining to speak — the ordinary case, and the one the distinct
// overlay type exists to make representable. A *present but not-a-number* value is the configuration
// speaking nonsense — a fat-fingered key — and it falls back too, rather than reaching a spring. An
// *out-of-range* value is neither: it is a real intent past the arithmetic headroom, so it clamps to
// the edge instead of falling back, since a big response means slow and not unchanged.
//
// The fallback is the *catalog's* value, which is what makes it different from the fallback inside
// ParametersFromResponse. That one falls back to the type's own default, because it is ingest and has
// nothing better to reach for. This one has something better: the constexpr entry the configuration
// file was overlaying in the first place.
[[nodiscard]] constexpr double
OverlayValue(double authored, std::optional<double> configured, double lowest, double highest) noexcept
{
	// Absent, or present and not-a-number, both take the authored value beside them. Self-comparison
	// rather than std::isnan, which is not dependably constexpr across libraries; infinities need no
	// case of their own, since they clamp.
	if (!configured || *configured != *configured)
	{
		return authored;
	}

	return std::clamp(*configured, lowest, highest);
}

[[nodiscard]] constexpr MotionParameters Overlay(MotionParameters authored, ConfiguredMotion configured) noexcept
{
	return {
		OverlayValue(
			authored.Response,
			configured.Response,
			SpringLimits::MinimumResponse<double>,
			SpringLimits::MaximumResponse<double>
		),
		OverlayValue(
			authored.Damping,
			configured.Damping,
			SpringLimits::MinimumDamping<double>,
			SpringLimits::MaximumDamping<double>
		),
	};
}

// A motion, resolved into the coefficients a spring is built from.
//
// The narrowing to the channel's precision happens here and once, so a float channel gets a
// correctly rounded conversion of the authored value rather than a cast of a value that had already
// been rounded to double along the way.
//
// Totality is Retarget.h's ParametersFromResponse, which clamps rather than failing, so the two
// guards below are belt and braces against a modifier that arrived from a file rather than from the
// table — and the speed clamp specifically stops a zero from turning a division into an infinity
// before the clamp downstream ever sees it.
template<std::floating_point T>
[[nodiscard]] constexpr SpringParameters<T>
Resolve(Motion motion, const MotionTable& table, MotionModifiers modifiers = {}) noexcept
{
	const MotionParameters authored = table[motion];
	const double speed =
		modifiers.Speed == modifiers.Speed ? std::clamp(modifiers.Speed, MinimumSpeed, MaximumSpeed) : 1.0;

	return ParametersFromResponse(static_cast<T>(authored.Response / speed), static_cast<T>(authored.Damping));
}

// The escape hatch, and the name is the whole of what it adds.
//
// It forwards to Animation/Author/Retarget.h's ParametersFromResponse and is deliberately not a
// synonym for it. That function is the ingest conversion the entire authoring path runs through —
// Resolve above calls it for every motion in the table, and every test that builds a spring calls it
// too — so grepping for it finds the machinery rather than the call sites that stepped outside the
// vocabulary. Docs/Decisions.md decision 13 asks for a hatch that is greppable in a single command,
// and a name of its own is what makes that true rather than nearly true.
//
// Every use is a claim that no motion in the table is right, which is a claim about the vocabulary
// and belongs in review as one. Nothing in gyro uses it yet, which is the expected state: a hatch
// with call sites accumulating quietly is the vocabulary failing.
template<std::floating_point T>
[[nodiscard]] constexpr SpringParameters<T> CustomMotion(T response, T damping) noexcept
{
	return ParametersFromResponse(response, damping);
}

// The contract everything downstream assumes. Authoring is arithmetic, so nearly all of it is
// available here — nothing below reaches exp, sin, or sqrt.

// The table's own values land in the regime their description claims. "No bounce" is a damping ratio
// at or above one, and Expressive is the only entry that is allowed to overshoot.
static_assert(MotionTable{}[Motion::Standard].Damping >= 1.0);
static_assert(MotionTable{}[Motion::Snappy].Damping >= 1.0, "Direct-manipulation feedback does not overshoot");
static_assert(MotionTable{}[Motion::Gentle].Damping >= 1.0);
static_assert(MotionTable{}[Motion::Interactive].Damping >= 1.0, "Nothing bounces under a finger");
static_assert(MotionTable{}[Motion::Expressive].Damping < 1.0, "The one entry whose job is to be noticed");

// Every entry resolves inside the arithmetic range Retarget.h clamps to, so no authored value is
// silently moved by its own ingest. A table whose entries needed clamping would be one where the
// number in the source is not the number the system runs.
static_assert(
	[] {
		constexpr MotionTable Table{};

		for (const Motion motion : AllMotions)
		{
			const MotionParameters entry = Table[motion];

			if (entry != Overlay(entry, { entry.Response, entry.Damping }))
			{
				return false;
			}
		}

		return true;
	}(),
	"The authored table is already inside the range configuration is clamped into"
);

// Resolution is the composition of the two conversions and nothing else.
static_assert(Resolve<double>(Motion::Standard, {}) == ParametersFromResponse(0.40, 1.00));
static_assert(
	Resolve<double>(Motion::Standard, {}, { .Speed = 2.0 }) == ParametersFromResponse(0.20, 1.00),
	"Twice the speed is half the response, not twice the frequency by some other route"
);
static_assert(Resolve<float>(Motion::Gentle, {}).Damping == 1.0F, "The channel's precision, chosen once");

// A speed that is not a number, or one outside the range, does not reach the division.
static_assert(
	Resolve<double>(Motion::Standard, {}, { .Speed = 0.0 }) ==
	Resolve<double>(Motion::Standard, {}, { .Speed = MinimumSpeed })
);
static_assert(
	Resolve<double>(Motion::Standard, {}, { .Speed = -1.0 }) ==
	Resolve<double>(Motion::Standard, {}, { .Speed = MinimumSpeed })
);
static_assert(
	Resolve<double>(Motion::Standard, {}, { .Speed = std::numeric_limits<double>::quiet_NaN() }) ==
		Resolve<double>(Motion::Standard, {}),
	"Nonsense takes the authored pacing rather than the slowest the clamp allows"
);

// The overlay falls back per key, which is the half a wholesale fallback would get wrong.
static_assert(
	Overlay(
		{ .Response = 0.40, .Damping = 1.00 },
		{ .Response = std::numeric_limits<double>::quiet_NaN(), .Damping = 0.70 }
	) == MotionParameters{ 0.40, 0.70 },
	"The bad half takes the authored value and the good half is kept"
);
static_assert(
	Overlay({ .Response = 0.40, .Damping = 1.00 }, { .Response = 1e9, .Damping = 1.00 }).Response ==
		SpringLimits::MaximumResponse<double>,
	"Out of range clamps rather than falling back, so a big number means slow rather than unchanged"
);

// A field the configuration does not mention is left authored rather than reset. This is the case a
// shared MotionParameters could not express — a default-constructed one reads as 0.4 and 1.0, so
// setting one field would silently move the other — and the distinct overlay type is what turns
// *said nothing* into a value the fallback can see.
static_assert(
	Overlay({ .Response = 0.60, .Damping = 1.00 }, {}) == MotionParameters{ 0.60, 1.00 },
	"An overlay that says nothing about a motion leaves it entirely authored"
);
static_assert(
	Overlay({ .Response = 0.60, .Damping = 1.00 }, { .Damping = 0.70 }) == MotionParameters{ 0.60, 0.70 },
	"Setting one field leaves the other authored rather than resetting it to a taste default"
);

// The hatch is the ingest conversion under a findable name and nothing else — including its totality,
// so stepping outside the vocabulary does not also step outside the clamps that keep a spring
// settling.
static_assert(CustomMotion(0.31, 0.72) == ParametersFromResponse(0.31, 0.72));
static_assert(CustomMotion(0.0, 0.0).Damping == SpringLimits::MinimumDamping<double>);
