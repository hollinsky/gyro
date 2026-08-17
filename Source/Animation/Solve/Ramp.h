#pragma once

#include <algorithm>
#include <format>
#include <type_traits>

#include "Animation/Solve/Spring.h"
#include "Core/Time.h"
#include "Core/Wake.h"

// The driven regime's closed form: a ramp, clamped to a horizon.
//
// Docs/Decisions.md decision 72 fixes the record — (p0, v0, t0, horizon), read as
// p0 + v0*clamp(T - t0, 0, horizon) — and fixes where it travels: its own homogeneous run beside the
// spring runs, so the frame thread walks two fixed-stride arrays each with one evaluation kind and no
// per-record discriminant. This is the evaluation half of that, and it sits beside
// Animation/Solve/Spring.h for the same reason Spring does: it *consumes* coefficients, which is what
// makes it the half the frame thread may call.
//
// **It is not a corner of Spring, and that is decision 65's finding rather than a preference.** A
// driven progress is exact — equal displacements produce equal changes in progress wherever in the
// range they happen — and that exactness comes from being a direct map, not from any (omega, zeta).
// No spring scrubs. So there is no folding this into the oscillator, and the only question decision
// 72 had to answer was where the discriminant that must exist somewhere should live. It lives in the
// type, which is why there are two files here and one array each rather than one tagged array.
//
// **Author/Drive.h is the other half and must not be confused with this one.** That file maps a
// gesture's displacement to a progress value and runs on the dispatch thread, where the finger is; it
// produces p. This file carries p across the boundary with the rate it was moving at and evaluates it
// per output at that output's own predicted presentation time — decision 50's promise, applied to the
// one channel that is driven rather than sprung.
//
// **The horizon is what keeps a lead from becoming a guess.** Decision 65 carries progress toward
// predicted presentation time to undo the input-to-photon gap, and the bound is what stops the
// correction extrapolating past the evidence: an abrupt stop runs on by v0*horizon until dispatch
// republishes at zero rate, and one output period is the obvious first answer. The *value* is a
// scheduling parameter still open in Docs/Open.md; the mechanism is here, and it is a clamp.
//
// Past the horizon the value holds, which is the property the idle fold rests on. A ramp that kept
// extrapolating would never settle, so a finger lifted while the compositor was busy would leave a
// standing wake commitment behind it — the machine drawing forever because an input event went
// unanswered. Holding makes the wake settle at an instant that is *authored* rather than derived,
// which is the second thing Spring cannot express: Spring::WakeAt asks when a decaying envelope falls
// inside a threshold, and there is no envelope here.
//
// **Single precision, per Docs/Architecture.md#the-spaces**: positions cross at double and everything
// else at single, and a progress is not a position. Author/Drive.h computes in double because that is
// where a device delta is projected onto a travel and the projection wants the headroom; the
// narrowing happens once, at publication, on a value already bounded to [-band, 1 + band].

// What evaluating a ramp yields. Deliberately the same shape and the same field names as
// SpringState<float>, because the one channel that has two regimes is read through both: a gesture
// taken over in flight reads (x, v) from whichever regime it is leaving to seed the one it is
// entering, and a handoff that had to transpose two vocabularies is a handoff somebody eventually
// transposes. Not the same *type*, because two distinct closed forms answering with one record is how
// a discriminant creeps back in.
struct RampState
{
	float Position{};
	float Velocity{};
};

// A driven progress, as it crosses the publication boundary.
//
// An aggregate deliberately, for the reason Core/Handle.h and Animation/Solve/Spring.h both give:
// this is a shape the frame side reconstitutes from bytes at an offset, not through a constructor.
//
// Totality belongs at the ingest, exactly as it does for Spring: Author/Drive.h is where a device
// delta is folded to something finite, and its own words apply unchanged — a progress that is not a
// number reaches a channel that never comes to rest, so the compositor never idles. Solve is a pure
// function of what it is handed.
struct Ramp
{
	Instant Origin{};   // t0, the instant the gesture was sampled at
	Duration Horizon{}; // how far past t0 the lead is carried before the value holds
	float Progress{};   // p0
	float Rate{};       // v0, in progress per second

	// The instant the ramp stops moving, which for this form is exact rather than conservative — the
	// horizon is authored, so there is nothing to bound. Saturating for the same reason
	// Spring::SettlesAt saturates: an instant in the deep past reads as settled, and the arithmetic
	// that would produce one is an addition nobody checked.
	[[nodiscard]] Instant SettlesAt() const noexcept { return Detail::Saturated(Origin, Horizon); }

	// O(1), exact, and stateless, which is the property the whole boundary is built on: evaluating at
	// t1 and then at t2 gives the same answer as evaluating at t2 alone, so a missed frame costs
	// latency rather than accumulating error.
	[[nodiscard]] RampState Evaluate(Instant at) const noexcept
	{
		const Instant holds = SettlesAt();
		const float seconds = Detail::SecondsSince<float>(Origin, std::min(at, holds));

		// The velocity is the rate inside the horizon and nothing outside it, and the outside case is
		// load-bearing rather than tidy. A retarget reads this pair to spring the progress home on
		// release, and a held value that still reported its rate would hand the spring a velocity the
		// content visibly no longer has — the transition would leave the finger's last position moving.
		return { Progress + Rate * seconds, at < holds ? Rate : 0.0f };
	}

	[[nodiscard]] bool IsSettled(Instant at) const noexcept { return at >= SettlesAt(); }

	// What this ramp contributes to the schedule, which is what Core/Wake.h reduces over. Continuous
	// until the horizon and settled past it, per decision 72 — a ramp in flight has no next
	// interesting instant to name, because every instant before it holds is one.
	[[nodiscard]] Wake WakeAt(Instant at) const noexcept
	{
		return IsSettled(at) ? Wake::Never() : Wake::EveryFrame(at);
	}
};

// Prints as 0.25@1.5/s, as SpringState does and for the same reason: a progress that has reached an
// end with rate left is the ordinary reading of a flick, and is indistinguishable from a held one
// otherwise. No format spec is accepted.
//
// The context is a template parameter for the reason recorded at length in Core/Handle.h: naming
// std::format_context leaves std::format working and std::formattable false, so the value goes missing
// from a test failure at exactly the moment it was wanted.
template<>
struct std::formatter<RampState>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(RampState state, Context& context) const
	{
		return std::format_to(context.out(), "{}@{}/s", state.Position, state.Velocity);
	}
};

// The contract everything downstream assumes. The behavioural half waits on the test harness for the
// reason Spring records: the seconds conversion is shared with the oscillator so that Solve has one
// arithmetic ingress from the timebase rather than two, and it is not a constant expression. What is
// left here is the layout, which is what the publication boundary cares about.
static_assert(
	std::is_trivially_copyable_v<Ramp> && std::is_standard_layout_v<Ramp>,
	"A ramp crosses the publication boundary as bytes at an offset"
);
static_assert(sizeof(Ramp) == 24, "One int64 origin, one int64 horizon, and two floats, exactly");
static_assert(alignof(Ramp) == 8, "so a run of them needs no more of the base than a spring does");
static_assert(std::is_trivially_copyable_v<RampState> && std::is_standard_layout_v<RampState>);
static_assert(std::formattable<RampState, char>, "A report prints the state rather than <unprintable>");
