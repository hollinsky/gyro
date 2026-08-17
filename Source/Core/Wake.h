#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <type_traits>

#include "Core/Time.h"

// When the next frame is owed, and whether another is owed after it.
//
// Docs/Architecture.md#doing-nothing-must-cost-nothing states the invariant: when nothing is
// animating and nothing has committed, no timer is armed and the frame thread blocks indefinitely.
// That is a *fold*. Every animating channel, every pending timeout, every retiring entity
// contributes an answer, and the schedule is what they reduce to. This is the type of the
// contribution; Sooner is the reduction.
//
// **A boolean cannot be the contribution.** An IsSettled(t) per channel makes the fold an OR, and an
// OR can express only two of the three things a channel has to be able to say. It forecloses every
// motion that is neither settled nor settling — the blinking cursor the recovery console owes
// (Docs/Architecture.md#the-pre-vulkan-console), an indeterminate spinner, a marquee, a breathing
// focus ring. Adding the first of them later means touching the fold, admission control, and the
// wake path at once. This type costs the same today and does not.
//
// **An instant cannot be the contribution either**, which is the half that is easy to miss. A spring
// in flight has no next interesting instant to name, because every instant between now and settling
// is one; it needs a frame each time the output offers one. So the two kinds of periodic motion the
// boolean forecloses are not one kind: a blink is discrete and wants a wake per edge with nothing
// drawn between them, while a marquee is continuous and wants every frame for as long as it lives.
// An optional<Instant> fits the first and has to spell the second as "wake now", which is
// indistinguishable from a one-shot that is merely overdue — and that distinction is where a
// standing commitment gets priced by the VRR servo and by admission control.
//
// **The third case is also what keeps a never-settling spring from reading as idle.** SettlesAt
// saturates for an undamped oscillator, so any representation whose "never again" could be spelled
// from a "never settles" is one keystroke away from dropping the compositor to idle with something
// still moving. Kind::Continuous cannot be reached from that value by accident; see
// Spring<V>::WakeAt, where the comparison against a saturated instant is false forever and the
// answer is therefore a standing commitment rather than an absence of one.
//
// Contributors owe two things the fold cannot check for them. A Timed instant must be strictly after
// the instant it was computed for, or the scheduler spins at full rate on a wake it has already
// served. And a periodic contributor computes its next edge from its own origin — t0 + n*period —
// never from its last wake, since a wake is served at the following vblank and adding to a rounded
// value accumulates the drift closed form exists to avoid.

struct Wake
{
	// Ordered by demand, and Sooner depends on the order: the larger enumerator wins.
	enum class Kind : std::uint8_t
	{
		Settled = 0,    // nothing further, ever
		Timed = 1,      // one frame, at When
		Continuous = 2, // a frame at When and another every Interval after it, without end
	};

	Kind Which = Kind::Settled;
	Instant When{};      // Timed and Continuous; meaningless when Settled
	Duration Interval{}; // Continuous only; zero is every frame the output offers

	// This crosses the publication boundary in the snapshot header, one per output, so the seven
	// bytes of padding after Which are the publisher's to value-initialize rather than to leave as
	// whatever the arena held — the same obligation Animation/Solve/Spring.h records for its own.

	[[nodiscard]] static constexpr Wake Never() noexcept { return {}; }

	[[nodiscard]] static constexpr Wake At(Instant when) noexcept { return { Kind::Timed, when, Duration::zero() }; }

	// The default origin is the epoch rather than a caller's "now", so that a contributor with no
	// instant to name errs toward being owed a frame already. Waking early costs a composite; waking
	// late is the animation freezing.
	[[nodiscard]] static constexpr Wake EveryFrame(Instant from = Instant{}) noexcept
	{
		return { Kind::Continuous, from, Duration::zero() };
	}

	// A standing commitment that does not want every frame. A throb authored at 30 Hz is a quarter of
	// the composites on a 144 Hz panel and is a rate the VRR servo can hold, which is the whole
	// difference between a periodic motion being priced and being absorbed. A non-positive interval
	// is every frame, which is the conservative reading of a nonsense rate.
	[[nodiscard]] static constexpr Wake AtRate(Instant next, Duration interval) noexcept
	{
		return { Kind::Continuous, next, std::max(interval, Duration::zero()) };
	}

	// Whether a frame is owed at or before the given instant. Settled is never due, whatever else is
	// in the record.
	[[nodiscard]] constexpr bool IsDue(Instant at) const noexcept { return Which != Kind::Settled && When <= at; }

	// Compares what the kind makes meaningful and nothing else. The alternative is a defaulted
	// comparison plus a rule that every field a kind does not read must be zeroed, which is a rule
	// held by whoever writes the next aggregate initializer — and two settled wakes carrying
	// different leftovers are the same wake by any reading that matters.
	[[nodiscard]] friend constexpr bool operator==(Wake left, Wake right) noexcept
	{
		if (left.Which != right.Which)
		{
			return false;
		}

		if (left.Which == Kind::Settled)
		{
			return true;
		}

		return left.When == right.When && (left.Which != Kind::Continuous || left.Interval == right.Interval);
	}
};

// The reduction: the more demanding of two contributions.
//
// A commutative monoid with Never() as its identity, which is the property worth having rather than
// an incidental one. Associativity is what allows the fold to be partitioned per output — a blinking
// cursor on one panel must not wake the other, and a scene-wide reduce would reintroduce exactly the
// coupling per-output damage removed — and what allows it to be cached per subtree and recomputed
// along the dirty path alone, instead of swept over every node each time anything moves.
[[nodiscard]] constexpr Wake Sooner(Wake left, Wake right) noexcept
{
	if (left.Which == Wake::Kind::Settled)
	{
		return right;
	}

	if (right.Which == Wake::Kind::Settled)
	{
		return left;
	}

	const Wake::Kind which = std::max(left.Which, right.Which);
	const Instant when = std::min(left.When, right.When);

	// The result is normalized rather than merely correct: a kind that does not read an interval
	// carries a zero, so that a caller reaching for one without checking the kind first gets a value
	// with an obvious reading instead of a leftover.
	if (which != Wake::Kind::Continuous)
	{
		return { which, when, Duration::zero() };
	}

	// The interval is a minimum over the *continuous* contributors alone. Folding a timed wake's
	// zero into it would read a one-shot as a demand for every frame, so a cursor blink beside a
	// 30 Hz throb would price the pair at the panel's full rate.
	if (left.Which != right.Which)
	{
		return { which, when, left.Which == Wake::Kind::Continuous ? left.Interval : right.Interval };
	}

	return { which, when, std::min(left.Interval, right.Interval) };
}

// Prints as settled, at 500ms, every frame from 500ms, or every 33333333ns from 500ms. No format
// spec is accepted.
//
// The context is a template parameter for the reason recorded at length in Core/Time.h: naming
// std::format_context leaves std::format working and std::formattable false, so the value goes
// missing from a test failure at exactly the moment it was wanted.
template<>
struct std::formatter<Wake>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(Wake wake, Context& context) const
	{
		switch (wake.Which)
		{
			case Wake::Kind::Settled:
				return std::format_to(context.out(), "settled");

			case Wake::Kind::Timed:
				return std::format_to(context.out(), "at {}", wake.When);

			case Wake::Kind::Continuous:
				if (wake.Interval == Duration::zero())
				{
					return std::format_to(context.out(), "every frame from {}", wake.When);
				}

				return std::format_to(context.out(), "every {} from {}", wake.Interval, wake.When);
		}

		return std::format_to(context.out(), "<invalid>");
	}
};

// The contract everything downstream assumes. The monoid laws are swept in Wake.Test.cpp; what is
// here is the part a wrong answer changes silently.
static_assert(
	Wake::Kind::Settled < Wake::Kind::Timed && Wake::Kind::Timed < Wake::Kind::Continuous,
	"Sooner takes the more demanding kind by taking the larger enumerator"
);

static_assert(
	std::is_trivially_copyable_v<Wake> && std::is_standard_layout_v<Wake>,
	"A wake crosses the publication boundary as bytes at an offset"
);
static_assert(sizeof(Wake) == 24, "One byte of kind, padded, and two int64s");
static_assert(std::formattable<Wake, char>, "A report prints the wake rather than <unprintable>");

static_assert(Wake{}.Which == Wake::Kind::Settled, "A default-constructed wake asks for nothing");
static_assert(!Wake::Never().IsDue(Monotonic::FromNanoseconds(1'000'000'000)));
static_assert(Wake::At(Monotonic::FromNanoseconds(500)).IsDue(Monotonic::FromNanoseconds(500)));
static_assert(!Wake::At(Monotonic::FromNanoseconds(500)).IsDue(Monotonic::FromNanoseconds(499)));

// Settled is the identity from both sides, which is what makes an idle scene fold to idle.
static_assert(Sooner(Wake::Never(), Wake::Never()) == Wake::Never());
static_assert(Sooner(Wake::Never(), Wake::At(Instant{})) == Wake::At(Instant{}));
static_assert(Sooner(Wake::At(Instant{}), Wake::Never()) == Wake::At(Instant{}));

// A standing commitment absorbs a one-shot without swallowing its instant: the pair is continuous,
// and the next frame is still owed at the earlier of the two.
static_assert(
	Sooner(Wake::AtRate(Monotonic::FromNanoseconds(900), Duration{ 30 }), Wake::At(Monotonic::FromNanoseconds(400))) ==
		Wake::AtRate(Monotonic::FromNanoseconds(400), Duration{ 30 }),
	"A timed contributor moves the instant and leaves the rate alone"
);

static_assert(
	Sooner(Wake::AtRate(Instant{}, Duration{ 30 }), Wake::AtRate(Instant{}, Duration{ 10 })).Interval == Duration{ 10 },
	"Two rates reduce to the faster"
);

// Equality reads the kind, not the leftovers behind it.
static_assert(Wake{ Wake::Kind::Settled, Monotonic::FromNanoseconds(7), Duration{ 3 } } == Wake::Never());
static_assert(Wake{ Wake::Kind::Timed, Instant{}, Duration{ 3 } } == Wake::At(Instant{}));
static_assert(Wake::EveryFrame(Instant{}) != Wake::AtRate(Instant{}, Duration{ 3 }));
