#include "Core/Quiet.h"

#include <array>

#include "Testing/Test.h"

// The boundary cases are the static_assert block at the foot of Quiet.h and are not repeated here.
// What is left is the part that wants a sweep: the predicate is a total function from a fold, an
// instant, and a horizon to one of three answers, and what matters is not any individual answer but
// the laws that hold across all of them. Each law below is one a plausible edit breaks silently.

using namespace std::chrono_literals;

namespace
{
[[nodiscard]] Instant At(std::int64_t nanoseconds)
{
	return Monotonic::FromNanoseconds(nanoseconds);
}

// One of every kind, placed on both sides of the instants the sweep uses as `now`, so that due,
// not-yet-due, and long-overdue are all reached for every kind rather than only for the convenient
// one. The rates differ from each other for the same reason Wake.Test.cpp's do: a coincidence
// between two of them would hide a term being read that should not be.
const std::array Folds = {
	Wake::Never(),
	Wake::At(At(-1'000)),
	Wake::At(At(0)),
	Wake::At(At(50)),
	Wake::At(At(100)),
	Wake::At(At(101)),
	Wake::At(At(5'000)),
	Wake::EveryFrame(),
	Wake::EveryFrame(At(-1'000)),
	Wake::EveryFrame(At(100)),
	Wake::EveryFrame(At(5'000)),
	Wake::AtRate(At(50), 33'333'333ns),
	Wake::AtRate(At(5'000), 8'333'333ns),
};

const std::array Nows = { At(-500), At(0), At(100), At(1'000) };

// Zero and a negative are in here because a caller unwilling to wait is a real caller — decision 73
// has resume and migration unable to defer at all — and the predicate is meant to answer that by
// falling out rather than by a branch somebody has to remember to write.
const std::array Horizons = { Duration{ -250 }, Duration::zero(), Duration{ 1 }, Duration{ 100 }, Duration{ 10'000 } };
} // namespace

GYRO_TEST(Quiet, EveryAnswerAgreesWithTheRuleItStandsFor)
{
	// The three answers restated independently of the implementation, so that the test fails if the
	// classification drifts rather than only if it crashes. Now claims nothing falls due inside the
	// horizon; At claims the fold settles once one more frame is served; Beyond claims neither.
	std::array<int, 3> reached{};

	for (const Wake fold : Folds)
	{
		for (const Instant now : Nows)
		{
			for (const Duration horizon : Horizons)
			{
				const Quiet answer = QuietWithin(fold, now, horizon);
				const bool dueInside = fold.IsDue(now + horizon);

				++reached.at(static_cast<std::size_t>(answer.Which));

				switch (answer.Which)
				{
					case Quiet::Kind::Now:
						GYRO_CHECK(!dueInside);
						break;

					case Quiet::Kind::At:
						GYRO_CHECK(dueInside);
						GYRO_CHECK_EQ(fold.Which, Wake::Kind::Timed);
						GYRO_CHECK_EQ(answer.When, fold.When);
						break;

					case Quiet::Kind::Beyond:
						GYRO_CHECK(dueInside);
						GYRO_CHECK_EQ(fold.Which, Wake::Kind::Continuous);
						break;
				}
			}
		}
	}

	// Every law in this file is of the form *if the answer is X then Y*, and every one of them holds
	// vacuously over a sweep that never produces X. The operand tables above are the sort of thing a
	// later edit trims, so what keeps these tests from quietly becoming assertions about nothing is
	// checking that the sweep still reaches all three.
	GYRO_CHECK(reached.at(static_cast<std::size_t>(Quiet::Kind::Now)) > 0);
	GYRO_CHECK(reached.at(static_cast<std::size_t>(Quiet::Kind::At)) > 0);
	GYRO_CHECK(reached.at(static_cast<std::size_t>(Quiet::Kind::Beyond)) > 0);
}

GYRO_TEST(Quiet, ItNeverWaitsLongerThanItWasGiven)
{
	// The promise the horizon *is*. A caller hands over how long it will wait and gets back either a
	// decision to proceed or an instant inside that window — never an instant outside it, which is
	// the failure that would turn a bounded deferral into the indefinite delay Experience.md says a
	// deliberate action must never become.
	for (const Wake fold : Folds)
	{
		for (const Instant now : Nows)
		{
			for (const Duration horizon : Horizons)
			{
				const Quiet answer = QuietWithin(fold, now, horizon);

				if (answer.Which == Quiet::Kind::At)
				{
					GYRO_CHECK(answer.When <= now + horizon);
				}
			}
		}
	}
}

GYRO_TEST(Quiet, WaitingIsOnlyEverOfferedWhereItReachesQuiet)
{
	// The property the whole design turns on, and the one a wall-clock timeout cannot have. A wait is
	// offered only against a fold that settles, so the caller that waits is never one that would have
	// waited out its whole budget and interrupted the motion anyway. Continuous motion — a video, a
	// marquee, a spinner — is answered immediately, at every horizon, from every instant.
	for (const Wake fold : Folds)
	{
		for (const Instant now : Nows)
		{
			for (const Duration horizon : Horizons)
			{
				if (fold.Which == Wake::Kind::Continuous)
				{
					GYRO_CHECK(QuietWithin(fold, now, horizon).ProceedsNow());
				}
			}
		}
	}
}

GYRO_TEST(Quiet, LookingFurtherAheadNeverMakesItKeener)
{
	// Monotone in the horizon: a caller willing to wait longer is never *more* willing to proceed
	// immediately. Stated because the inverted comparison passes every single-case test — each
	// individual answer still looks reasonable — and produces a predicate that grows more eager to
	// interrupt the longer it is told it may wait.
	for (const Wake fold : Folds)
	{
		for (const Instant now : Nows)
		{
			for (const Duration shorter : Horizons)
			{
				for (const Duration longer : Horizons)
				{
					if (longer < shorter)
					{
						continue;
					}

					if (QuietWithin(fold, now, longer).Which == Quiet::Kind::Now)
					{
						GYRO_CHECK_EQ(QuietWithin(fold, now, shorter).Which, Quiet::Kind::Now);
					}
				}
			}
		}
	}
}

GYRO_TEST(Quiet, TheThreeCasesDecision73Names)
{
	// The worked examples, in the words the decision uses, so that the intent survives a refactor
	// that keeps every law above true. A 50 ms horizon is the figure decision 73 starts from.
	const Instant now = At(1'000'000);
	const Duration horizon = 50ms;

	// Nothing is moving: the reconfiguration is free and there is nothing to hold.
	GYRO_CHECK_EQ(QuietWithin(Wake::Never(), now, horizon), Quiet::Immediately());

	// A transition settling in 20 ms: quiet is coming, it is known when, and it is worth the wait.
	const Instant settling = now + 20ms;
	GYRO_CHECK_EQ(QuietWithin(Wake::At(settling), now, horizon), Quiet::Awaiting(settling));

	// A transition settling in 300 ms: further off than this caller will wait, so waiting is not
	// offered — but neither is the motion visible to it, so it proceeds rather than reporting a loss.
	GYRO_CHECK_EQ(QuietWithin(Wake::At(now + 300ms), now, horizon), Quiet::Immediately());

	// A client committing steadily: quiet never arrives, so waiting is pure cost. This is the answer
	// a timeout gets wrong by spending its whole budget before reaching the same place.
	GYRO_CHECK_EQ(QuietWithin(Wake::EveryFrame(now), now, horizon), Quiet::OutOfReach());

	// A 30 Hz throb: a standing commitment like any other, however little of each frame it wants.
	GYRO_CHECK_EQ(QuietWithin(Wake::AtRate(now, 33'333'333ns), now, horizon), Quiet::OutOfReach());
}

GYRO_TEST(Quiet, AnUnwillingCallerIsNeverMadeToWait)
{
	// Decision 73 has callers that cannot defer at all — resume, device migration — and they are
	// served by a horizon of zero rather than by a second entry point. What they must never get back
	// is an instant in the future, and the degenerate answer they can get, a wait until an instant
	// already past, is one their next iteration serves without delay.
	for (const Wake fold : Folds)
	{
		for (const Instant now : Nows)
		{
			const Quiet answer = QuietWithin(fold, now, Duration::zero());

			if (answer.Which == Quiet::Kind::At)
			{
				GYRO_CHECK(answer.When <= now);
			}
		}
	}
}
