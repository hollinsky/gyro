#include "Scene/Focus.h"

#include "Core/Clock.h"
#include "Core/Handle.h"
#include "Scene/Commit.h"
#include "Scene/Store.h"
#include "Testing/Test.h"

// The stack policy Focus.h describes, and the one thing about it that is not policy: focus leaves a
// window when its author does, on the retirement rather than on the free.

namespace
{
ManualClock Clock;
} // namespace

GYRO_TEST(SceneFocus, NothingIsFocusedBeforeAnythingIsOffered)
{
	const SceneFocus focus;

	GYRO_CHECK(focus.Focused().IsNull());
	GYRO_CHECK_EQ(focus.Count(), std::size_t{ 0 });
}

GYRO_TEST(SceneFocus, TheNewestWindowTakesFocusAndTheOlderOneWaitsBehindIt)
{
	SceneFocus focus;
	const EntityId first{ 1, 1 };
	const EntityId second{ 2, 1 };

	focus.Offer(first);
	GYRO_CHECK(focus.Focused() == first);

	focus.Offer(second);
	GYRO_CHECK(focus.Focused() == second);

	// The one behind it is still focusable, which is what makes the fall-back below a person's previous
	// window rather than nothing.
	GYRO_CHECK_EQ(focus.Count(), std::size_t{ 2 });
}

GYRO_TEST(SceneFocus, ClosingTheFocusedWindowHandsFocusBackAndNotToNothing)
{
	SceneFocus focus;
	const EntityId first{ 1, 1 };
	const EntityId second{ 2, 1 };

	focus.Offer(first);
	focus.Offer(second);
	focus.Withdraw(second);

	GYRO_CHECK(focus.Focused() == first);
}

GYRO_TEST(SceneFocus, ClosingAWindowNobodyIsTypingIntoMovesNothing)
{
	SceneFocus focus;
	const EntityId first{ 1, 1 };
	const EntityId second{ 2, 1 };

	focus.Offer(first);
	focus.Offer(second);
	focus.Withdraw(first);

	GYRO_CHECK(focus.Focused() == second);
	GYRO_CHECK_EQ(focus.Count(), std::size_t{ 1 });
}

GYRO_TEST(SceneFocus, OfferingAWindowTwiceRaisesItRatherThanAddingIt)
{
	SceneFocus focus;
	const EntityId first{ 1, 1 };
	const EntityId second{ 2, 1 };

	focus.Offer(first);
	focus.Offer(second);
	focus.Offer(first);

	GYRO_CHECK(focus.Focused() == first);
	GYRO_CHECK_EQ(focus.Count(), std::size_t{ 2 });

	// And the stack under it is intact: the window it displaced is still what focus falls to.
	focus.Withdraw(first);
	GYRO_CHECK(focus.Focused() == second);
}

GYRO_TEST(SceneFocus, FocusingAWindowThatWasNeverMappedIsRefused)
{
	SceneFocus focus;
	const EntityId mapped{ 1, 1 };

	focus.Offer(mapped);

	GYRO_CHECK(!focus.Focus(EntityId{ 9, 1 }));
	GYRO_CHECK(focus.Focused() == mapped);
	GYRO_CHECK_EQ(focus.Count(), std::size_t{ 1 });
}

GYRO_TEST(SceneFocus, FocusingReordersTheStackRatherThanSwappingTwoEntries)
{
	SceneFocus focus;
	const EntityId first{ 1, 1 };
	const EntityId second{ 2, 1 };
	const EntityId third{ 3, 1 };

	focus.Offer(first);
	focus.Offer(second);
	focus.Offer(third);

	GYRO_CHECK(focus.Focus(first));
	GYRO_CHECK(focus.Focused() == first);

	// `second` and `third` keep their order behind it. A swap would put `first` where `third` was and
	// make the next fall-back the window a person used least recently.
	focus.Withdraw(first);
	GYRO_CHECK(focus.Focused() == third);
}

GYRO_TEST(SceneFocus, RetiringAWindowTakesFocusOffItWhileItIsStillOnScreen)
{
	SceneStore store{ Clock };
	const EntityId window = *store.CreateContainer({}, {});

	store.Focus().Offer(window);
	GYRO_CHECK(store.Focus().Focused() == window);

	{
		SceneCommit commit{ store, CommitAuthor::Client };
		GYRO_CHECK(commit.Retire(window));
	}

	// Decision 114 leaves the entity in the tree, being drawn, for as long as its exit runs — and focus
	// is gone the moment the client is, so the keystrokes stop before the pixels do.
	GYRO_CHECK(store.IsLive(window));
	GYRO_CHECK(store.Focus().Focused().IsNull());
}

GYRO_TEST(SceneFocus, RetiringAWindowTakesFocusOffEverythingUnderIt)
{
	SceneStore store{ Clock };
	const EntityId window = *store.CreateContainer({}, {});
	const EntityId child = *store.CreateContainer(window, {});
	const EntityId other = *store.CreateContainer({}, {});

	store.Focus().Offer(other);
	store.Focus().Offer(child);

	{
		SceneCommit commit{ store, CommitAuthor::Client };
		GYRO_CHECK(commit.Retire(window));
	}

	GYRO_CHECK(store.Focus().Focused() == other);
}

// Membership without the order, which is what the pointer asks on the way up from what it hit.
GYRO_TEST(SceneFocus, TheStackAnswersWhetherItHoldsAWindowWithoutMovingIt)
{
	SceneStore store{ Clock };
	const EntityId first = *store.CreateContainer({}, {});
	const EntityId second = *store.CreateContainer({}, {});
	const EntityId never = *store.CreateContainer({}, {});

	store.Focus().Offer(first);
	store.Focus().Offer(second);

	GYRO_CHECK(store.Focus().Contains(first));
	GYRO_CHECK(store.Focus().Contains(second));
	GYRO_CHECK(!store.Focus().Contains(never));
	GYRO_CHECK(!store.Focus().Contains({}));

	// Asking is not focusing, which is the whole reason this exists rather than the caller trying
	// `Focus` and reading the answer.
	GYRO_CHECK(store.Focus().Focused() == second);
}

// The walk `Alt+Tab` lands on: that it reaches every window rather than swapping the last two, that
// nothing under it moves until the hand comes off, and the three ways a gesture ends other than that.

GYRO_TEST(SceneFocus, CyclingReachesEveryWindowAndWrapsBackToTheTop)
{
	SceneFocus focus;
	const EntityId first{ 1, 1 };
	const EntityId second{ 2, 1 };
	const EntityId third{ 3, 1 };

	focus.Offer(first);
	focus.Offer(second);
	focus.Offer(third);

	// One step is the window a person was using before this one, and the keyboard goes with it — which
	// is what makes what they are looking at what they would type into.
	GYRO_CHECK(focus.CycleNext() == second);
	GYRO_CHECK(focus.Focused() == second);

	// Two steps reaches the third window. This is the case a stack rotated at every step cannot reach:
	// it would have put `second` back on top and sent the next press to `third` and back forever.
	GYRO_CHECK(focus.CycleNext() == first);
	GYRO_CHECK(focus.Focused() == first);

	GYRO_CHECK(focus.CycleNext() == third);
	GYRO_CHECK(focus.CyclePrevious() == first);
}

GYRO_TEST(SceneFocus, WhereACycleLandsBecomesTheMostRecentWindow)
{
	SceneFocus focus;
	const EntityId first{ 1, 1 };
	const EntityId second{ 2, 1 };
	const EntityId third{ 3, 1 };

	focus.Offer(first);
	focus.Offer(second);
	focus.Offer(third);

	GYRO_CHECK(focus.CycleNext() == second);
	GYRO_CHECK(focus.EndCycle() == second);
	GYRO_CHECK(focus.Focused() == second);

	// And the next gesture starts from there, which is the whole point of writing the landing into the
	// order: one press of `Alt+Tab` goes back to the window a person just came from.
	GYRO_CHECK(focus.CycleNext() == third);
	GYRO_CHECK(focus.EndCycle() == third);
	GYRO_CHECK(focus.CycleNext() == second);
}

GYRO_TEST(SceneFocus, AWindowClosingUnderACycleLeavesTheWalkOnSomethingThatExists)
{
	SceneFocus focus;
	const EntityId first{ 1, 1 };
	const EntityId second{ 2, 1 };
	const EntityId third{ 3, 1 };

	focus.Offer(first);
	focus.Offer(second);
	focus.Offer(third);

	GYRO_CHECK(focus.CycleNext() == second);

	// The application a person had stepped onto exits while they are still holding `Alt`.
	focus.Withdraw(second);

	// Focus falls back to the top of the stack rather than staying on a name nothing can draw, and the
	// walk resumes from there rather than from wherever an index would have been left pointing.
	GYRO_CHECK(focus.Focused() == third);
	GYRO_CHECK(focus.CycleNext() == first);

	// Landing on a window that has gone changes nothing at all.
	focus.Withdraw(first);
	GYRO_CHECK(focus.EndCycle().IsNull());
	GYRO_CHECK(focus.Focused() == third);
}

GYRO_TEST(SceneFocus, ReachingForTheMouseOrOpeningAWindowEndsAHalfFinishedCycle)
{
	SceneFocus focus;
	const EntityId first{ 1, 1 };
	const EntityId second{ 2, 1 };
	const EntityId third{ 3, 1 };

	focus.Offer(first);
	focus.Offer(second);

	GYRO_CHECK(focus.CycleNext() == first);

	// A click names a window outright, and it wins: a person who abandons an `Alt+Tab` by clicking
	// somewhere must not keep typing into the window the walk was left on.
	GYRO_CHECK(focus.Focus(second));
	GYRO_CHECK(focus.Focused() == second);
	GYRO_CHECK(focus.EndCycle().IsNull());
	GYRO_CHECK(focus.Focused() == second);

	// So does a window opening, which is a new answer to the question the walk was asking.
	GYRO_CHECK(focus.CycleNext() == first);
	focus.Offer(third);
	GYRO_CHECK(focus.Focused() == third);
}

GYRO_TEST(SceneFocus, CyclingWithOneWindowOrNoneIsANoOp)
{
	SceneFocus focus;

	GYRO_CHECK(focus.CycleNext().IsNull());
	GYRO_CHECK(focus.EndCycle().IsNull());

	const EntityId only{ 1, 1 };

	focus.Offer(only);

	GYRO_CHECK(focus.CycleNext() == only);
	GYRO_CHECK(focus.Focused() == only);
	GYRO_CHECK(focus.EndCycle() == only);
}
