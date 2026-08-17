#include "Core/Wake.h"

#include <array>
#include <string>

#include "Testing/Test.h"

// The compile-time half of Wake.h's contract is the static_assert block at the foot of that header
// and is not repeated here. What is left is the part that wants a sweep rather than an example: the
// monoid laws, which are the whole reason the fold may be partitioned per output and cached per
// subtree, and which no single pair of operands can establish.

using namespace std::chrono_literals;

namespace
{
[[nodiscard]] Instant At(std::int64_t nanoseconds)
{
	return Monotonic::FromNanoseconds(nanoseconds);
}

// One of every kind, with instants and rates chosen so that no two of them agree on a minimum by
// coincidence — an associativity failure that only shows up when the three operands disagree is the
// one an example-based test misses.
const std::array Wakes = {
	Wake::Never(),
	Wake::At(At(100)),
	Wake::At(At(700)),
	Wake::At(At(-500)),
	Wake::EveryFrame(),
	Wake::EveryFrame(At(300)),
	Wake::AtRate(At(250), 33'333'333ns),
	Wake::AtRate(At(950), 8'333'333ns),
	Wake::AtRate(At(-50), Duration::max()),
};
} // namespace

GYRO_TEST(Wake, SettledIsTheIdentity)
{
	// The idle invariant restated as an algebraic law. A scene in which nothing contributes folds to
	// a value that arms no timer, and it does so however many nothings there are.
	for (const Wake wake : Wakes)
	{
		GYRO_CHECK_EQ(Sooner(Wake::Never(), wake), wake);
		GYRO_CHECK_EQ(Sooner(wake, Wake::Never()), wake);
	}
}

GYRO_TEST(Wake, TheFoldDoesNotDependOnOrder)
{
	// Commutativity and associativity together are what let the scheduler reduce per output, and
	// later per subtree along the dirty path, rather than sweeping every node in tree order.
	for (const Wake left : Wakes)
	{
		for (const Wake right : Wakes)
		{
			GYRO_CHECK_EQ(Sooner(left, right), Sooner(right, left));

			for (const Wake third : Wakes)
			{
				GYRO_CHECK_EQ(Sooner(Sooner(left, right), third), Sooner(left, Sooner(right, third)));
			}
		}
	}
}

GYRO_TEST(Wake, TheFoldIsNeverLessDemandingThanAMember)
{
	// The direction the whole type errs in. A reduction that came out settled while a member was
	// still asking for frames is the compositor dropping to idle with something moving, which is the
	// failure Animation/Solve/Spring.h's envelopes exist to prevent arriving from the scheduler side.
	for (const Wake left : Wakes)
	{
		for (const Wake right : Wakes)
		{
			const Wake folded = Sooner(left, right);

			GYRO_CHECK(folded.Which >= left.Which);
			GYRO_CHECK(folded.Which >= right.Which);

			if (left.Which != Wake::Kind::Settled)
			{
				GYRO_CHECK(folded.When <= left.When);
			}

			if (right.Which != Wake::Kind::Settled)
			{
				GYRO_CHECK(folded.When <= right.When);
			}
		}
	}
}

GYRO_TEST(Wake, AStandingCommitmentAbsorbsAOneShotWithoutSwallowingIt)
{
	// The case an optional<Instant> cannot hold. A marquee wants every frame and a cursor blink wants
	// one at its next edge; the pair wants both, and the answer has to still be standing so that
	// admission control and the VRR servo see a commitment rather than a timer.
	const Wake marquee = Wake::EveryFrame(At(600));
	const Wake blink = Wake::At(At(200));
	const Wake folded = Sooner(marquee, blink);

	GYRO_CHECK_EQ(folded.Which, Wake::Kind::Continuous);
	GYRO_CHECK_EQ(folded.When, At(200));
	GYRO_CHECK_EQ(folded.Interval, Duration::zero());
}

GYRO_TEST(Wake, AOneShotDoesNotDriveTheRateToEveryFrame)
{
	// A timed wake carries no interval, so folding its zero in would price a cursor blink beside a
	// 30 Hz throb at the panel's full rate — the pulse becoming unpriced by arithmetic rather than by
	// omission, which is the failure this whole type exists to make visible.
	const Wake throb = Wake::AtRate(At(1'000), 33'333'333ns);
	const Wake blink = Wake::At(At(500));

	GYRO_CHECK_EQ(Sooner(throb, blink).Interval, Duration{ 33'333'333 });
	GYRO_CHECK_EQ(Sooner(blink, throb).Interval, Duration{ 33'333'333 });
}

GYRO_TEST(Wake, TheFoldNormalizesWhatTheKindDoesNotRead)
{
	// Equality already ignores an interval on a kind that has none, so this is not about comparison.
	// It is about a caller that reaches for .Interval without checking .Which first getting a value
	// with an obvious reading rather than one of its operands' leftovers.
	const Wake folded = Sooner(Wake{ Wake::Kind::Timed, At(300), 5ms }, Wake{ Wake::Kind::Timed, At(700), 9ms });

	GYRO_CHECK_EQ(folded.Which, Wake::Kind::Timed);
	GYRO_CHECK_EQ(folded.When, At(300));
	GYRO_CHECK_EQ(folded.Interval, Duration::zero());
}

GYRO_TEST(Wake, TwoRatesReduceToTheFaster)
{
	const Wake slow = Wake::AtRate(At(400), 33'333'333ns);
	const Wake fast = Wake::AtRate(At(900), 8'333'333ns);
	const Wake folded = Sooner(slow, fast);

	GYRO_CHECK_EQ(folded.Which, Wake::Kind::Continuous);
	GYRO_CHECK_EQ(folded.When, At(400));
	GYRO_CHECK_EQ(folded.Interval, Duration{ 8'333'333 });
}

GYRO_TEST(Wake, ANonsenseRateIsEveryFrame)
{
	// The conservative reading, and the same shape as PeriodFromHertz refusing to turn a bad mode
	// line into an output that owes infinitely many frames — except that here the safe direction is
	// the other one, because a rate is a permission to draw less rather than an obligation to draw.
	GYRO_CHECK_EQ(Wake::AtRate(At(100), -5ms).Interval, Duration::zero());
	GYRO_CHECK_EQ(Wake::AtRate(At(100), Duration::zero()), Wake::EveryFrame(At(100)));
}

GYRO_TEST(Wake, DueIsAtOrBefore)
{
	// The frame loop's question. At-or-before rather than strictly-before, because a wake computed
	// for a predicted presentation time is served by the frame presenting at exactly that instant.
	GYRO_CHECK(Wake::At(At(500)).IsDue(At(500)));
	GYRO_CHECK(Wake::At(At(500)).IsDue(At(501)));
	GYRO_CHECK(!Wake::At(At(500)).IsDue(At(499)));

	GYRO_CHECK(Wake::EveryFrame().IsDue(At(0)));
	GYRO_CHECK(!Wake::EveryFrame(At(500)).IsDue(At(499)));

	// Whatever the leftovers say. A settled wake is what the frame thread blocks indefinitely on.
	GYRO_CHECK(!Wake::Never().IsDue(At(0)));
	GYRO_CHECK(!(Wake{ Wake::Kind::Settled, At(-1'000), 1ms }.IsDue(At(0))));
}

GYRO_TEST(Wake, EqualityReadsTheKindRatherThanTheLeftovers)
{
	// Sooner normalizes what it produces, but nothing normalizes an aggregate somebody wrote by hand,
	// and a stale instant in a field its kind does not read is not a difference.
	GYRO_CHECK_EQ(Wake{ Wake::Kind::Settled, At(7), 3ms }, Wake::Never());
	GYRO_CHECK_EQ(Wake{ Wake::Kind::Timed, At(7), 3ms }, Wake::At(At(7)));

	GYRO_CHECK(Wake::At(At(7)) != Wake::At(At(8)));
	GYRO_CHECK(Wake::At(At(7)) != Wake::EveryFrame(At(7)));
	GYRO_CHECK(Wake::EveryFrame(At(7)) != Wake::AtRate(At(7), 3ms));
}

GYRO_TEST(Wake, FormatsAsSomethingAReportCanBeReadFrom)
{
	// A failing schedulability or idle assertion prints one of these, and "2" would say nothing about
	// which of the three the fold arrived at.
	GYRO_CHECK_EQ(std::format("{}", Wake::Never()), std::string{ "settled" });
	GYRO_CHECK_EQ(std::format("{}", Wake::At(At(500))), std::string{ "at 500ns" });
	GYRO_CHECK_EQ(std::format("{}", Wake::EveryFrame(At(500))), std::string{ "every frame from 500ns" });
	GYRO_CHECK_EQ(std::format("{}", Wake::AtRate(At(500), 30ns)), std::string{ "every 30ns from 500ns" });
}
