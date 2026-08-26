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
