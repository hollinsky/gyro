#pragma once

#include <cstdint>
#include <format>
#include <type_traits>

#include "Core/Time.h"
#include "Core/Wake.h"

// Whether a disruptive operation may run now, should wait for an instant already known, or will
// interrupt something whatever it does.
//
// Docs/Decisions.md decision 73 makes reconfiguration — a mode set, and the class of long
// non-composite work behind it — something the frame thread initiates rather than performs. It is
// imperceptible when nothing is moving and a held frame when something is, so the policy is to run
// it when nothing is. This is that predicate. It reads Wake.h's fold rather than a clock, and the
// distinction is the whole value of it.
//
// **Nothing here waits speculatively.** A wall-clock timeout spends its entire budget for nothing in
// precisely the case that most needs the deferral — continuous motion, where quiet never arrives at
// all — and it cannot tell that case from motion about to end, because a boolean *is something
// moving* has no term for *later*. Decision 69 gave the fold a third case for that reason, and this
// is the second thing to read it: the answer is either an instant the fold already knows or a
// decision to proceed, and never a hope that waiting will help.
//
// **One horizon does two jobs, and they are one quantity seen from two sides.** It is how far ahead
// motion is looked for, and it is how long the caller will wait. A willingness shorter than the
// lookahead would refuse quiet it had already found; a lookahead shorter than the willingness would
// wait for quiet it never checked for. Two numbers here would be a way to spell an inconsistency.
//
// **Which callers may consult this at all is not its business.** Decision 73 has resume and device
// migration unable to wait for anything and hotplug unwilling to, since a connector probe alone
// outlasts any horizon worth setting. Those callers do not ask; the predicate does not know them.

struct Quiet
{
	enum class Kind : std::uint8_t
	{
		Now = 0,    // nothing falls due inside the horizon — run it
		At = 1,     // motion ends at When, inside the horizon — wait for it
		Beyond = 2, // quiet lies further off than the horizon, or never — run it and interrupt
	};

	Kind Which = Kind::Now;
	Instant When{}; // At only; meaningless otherwise

	[[nodiscard]] static constexpr Quiet Immediately() noexcept { return { Kind::Now, Instant{} }; }

	[[nodiscard]] static constexpr Quiet Awaiting(Instant when) noexcept { return { Kind::At, when }; }

	[[nodiscard]] static constexpr Quiet OutOfReach() noexcept { return { Kind::Beyond, Instant{} }; }

	// Whether the caller proceeds without waiting. True for both answers that are not a wait, which
	// is the distinction a caller acts on; Which is what it reports. Keeping them separate is the
	// point of three cases rather than two — Beyond is a frame somebody loses and Now is not, and a
	// count of them is the measurement decision 73 says is the only one a person can perceive.
	[[nodiscard]] constexpr bool ProceedsNow() const noexcept { return Which != Kind::At; }

	// Compares what the kind makes meaningful and nothing else, for Wake's reason: two answers that
	// carry different leftovers in a field neither reads are the same answer.
	[[nodiscard]] friend constexpr bool operator==(Quiet left, Quiet right) noexcept
	{
		if (left.Which != right.Which)
		{
			return false;
		}

		return left.Which != Kind::At || left.When == right.When;
	}
};

// The predicate. Pure, and total over every fold Wake can produce.
//
// A non-positive horizon is a caller unwilling to wait, and falls out rather than being special
// cased: nothing is ever due before an instant that has already passed, so every fold but an overdue
// one answers Now.
[[nodiscard]] constexpr Quiet QuietWithin(Wake fold, Instant now, Duration horizon) noexcept
{
	// Nothing owed before the horizon runs out. This is the whole of what Now claims — quiet for at
	// least the horizon, never quiet for good — which is why a standing commitment whose first edge
	// is far enough away answers exactly as a settled fold does. Nothing here bounds how long the
	// operation itself takes, so a stronger claim would be one this cannot keep.
	if (!fold.IsDue(now + horizon))
	{
		return Quiet::Immediately();
	}

	// Due inside the horizon, and settled once that one frame is served: waiting reaches quiet, at an
	// instant the fold already names. Wake.h puts the obligation that a timed instant is strictly
	// after the instant it was computed for on contributors, so this does not answer with the past —
	// and a contributor that breaks it produces the spin that header already names, absorbed here by
	// the caller re-testing against a fold recomputed at When rather than acting on this one twice.
	if (fold.Which == Wake::Kind::Timed)
	{
		return Quiet::Awaiting(fold.When);
	}

	// A standing commitment falling due inside the horizon: a video, a marquee, a spinner, a throb.
	// Waiting cannot reach quiet, so waiting is pure cost, and the honest answer is to proceed and
	// hold the frame. This is the case a timeout gets wrong by burning the whole budget first.
	return Quiet::OutOfReach();
}

// Prints as now, at 500ms, or beyond. No format spec is accepted.
//
// The context is a template parameter for the reason recorded at length in Core/Time.h: naming
// std::format_context leaves std::format working and std::formattable false, so the value goes
// missing from a test failure at exactly the moment it was wanted.
template<>
struct std::formatter<Quiet>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(Quiet quiet, Context& context) const
	{
		switch (quiet.Which)
		{
			case Quiet::Kind::Now:
				return std::format_to(context.out(), "now");

			case Quiet::Kind::At:
				return std::format_to(context.out(), "at {}", quiet.When);

			case Quiet::Kind::Beyond:
				return std::format_to(context.out(), "beyond");
		}

		return std::format_to(context.out(), "<invalid>");
	}
};

// The contract the frame loop will assume. What is swept in Quiet.Test.cpp is the mapping from every
// fold to every answer; what is here is the part a wrong answer changes silently.
static_assert(
	std::is_trivially_copyable_v<Quiet> && std::is_standard_layout_v<Quiet>,
	"An answer is copied into a decision, never allocated behind one"
);
static_assert(std::formattable<Quiet, char>, "A report prints the answer rather than <unprintable>");

static_assert(Quiet{}.Which == Quiet::Kind::Now, "A default-constructed answer blocks nothing");

// One of each answer, at its boundary, because each is a single comparison away from becoming
// another and none of the three would fail loudly. The pair straddling the horizon is the one worth
// having here rather than in the sweep: it fixes which side of `<=` the horizon lands on, and that
// is the comparison a reader is most likely to "tidy".
static_assert(
	QuietWithin(Wake::Never(), Instant{}, Duration::max()) == Quiet::Immediately(),
	"Nothing moving is quiet however long the caller would have waited"
);
static_assert(
	QuietWithin(Wake::EveryFrame(), Instant{}, Duration::zero()) == Quiet::OutOfReach(),
	"A standing commitment already due is not reached by waiting, however briefly"
);
static_assert(
	QuietWithin(Wake::At(Monotonic::FromNanoseconds(100)), Instant{}, Duration{ 100 }) ==
		Quiet::Awaiting(Monotonic::FromNanoseconds(100)),
	"Motion ending exactly on the horizon is waited for, not abandoned"
);
static_assert(
	QuietWithin(Wake::At(Monotonic::FromNanoseconds(101)), Instant{}, Duration{ 100 }) == Quiet::Immediately(),
	"Motion beginning one tick past the horizon is not motion this caller can see"
);
