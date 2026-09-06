#include "Scene/Focus.h"

#include <array>

#include "Core/Clock.h"
#include "Core/Handle.h"
#include "Core/Session.h"
#include "Geometry/Scale.h"
#include "Scene/Commit.h"
#include "Scene/Output.h"
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

GYRO_TEST(SceneFocus, ALauncherTakesTheKeyboardAndTheWalkStepsOverIt)
{
	SceneFocus focus;
	const EntityId first{ 1, 1 };
	const EntityId second{ 2, 1 };
	const EntityId launcher{ 3, 1 };

	focus.Offer(first);
	focus.Offer(second);

	// **The half chrome keeps.** A launcher a person cannot type into is not a launcher, so it takes the
	// keyboard the moment it maps exactly as a window does.
	focus.Offer(launcher, FocusKind::Chrome);
	GYRO_CHECK(focus.Focused() == launcher);

	// **The half it does not.** The walk goes *down* the stack, so the first step off the launcher is the
	// window it is covering — and every step after it is the walk a person would have got with no
	// launcher on screen at all. What the launcher never is, is somewhere the walk stops.
	GYRO_CHECK(focus.CycleNext() == second);
	GYRO_CHECK(focus.CycleNext() == first);

	// The lap back over the top: the launcher sits between `first` and `second` in the order and the
	// walk passes it without stopping, which is the step a single modular hop would have landed on.
	GYRO_CHECK(focus.CycleNext() == second);
}

GYRO_TEST(SceneFocus, DismissingTheLauncherPutsAPersonBackOnTheWindowTheyWereUsing)
{
	SceneFocus focus;
	const EntityId window{ 1, 1 };
	const EntityId launcher{ 2, 1 };

	focus.Offer(window);
	focus.Offer(launcher, FocusKind::Chrome);

	GYRO_CHECK(focus.Focused() == launcher);

	// The shell unmaps its launcher, which retires the entity and withdraws it here. Focus falls to the
	// entity beneath, which is the stack policy doing exactly what it does for a window closing — and
	// what it has to do, because typing into nothing after dismissing a launcher is the failure a person
	// would report as *the keyboard stopped working*.
	focus.Withdraw(launcher);

	GYRO_CHECK(focus.Focused() == window);
}

GYRO_TEST(SceneFocus, AScreenWithNothingButChromeOnItHasNowhereToCycleTo)
{
	SceneFocus focus;
	const EntityId panel{ 1, 1 };
	const EntityId launcher{ 2, 1 };

	focus.Offer(panel, FocusKind::Chrome);
	focus.Offer(launcher, FocusKind::Chrome);

	// **Two chrome entries in a row is what makes the walk a loop rather than one modular step**, and a
	// stack that is *all* chrome is where that loop has to terminate: a person pressing `Alt+Tab` with
	// only a panel and a launcher on screen gets nothing, rather than the compositor spinning looking
	// for a window that is not there.
	GYRO_CHECK(focus.CycleNext().IsNull());
	GYRO_CHECK(focus.CyclePrevious().IsNull());

	// And nothing was disturbed by the asking: the launcher still has the keyboard.
	GYRO_CHECK(focus.Focused() == launcher);
	GYRO_CHECK(focus.EndCycle().IsNull());
	GYRO_CHECK(focus.Focused() == launcher);
}

GYRO_TEST(SceneFocus, ChromeIsStillSomethingAClickCanFocus)
{
	SceneFocus focus;
	const EntityId window{ 1, 1 };
	const EntityId launcher{ 2, 1 };

	focus.Offer(launcher, FocusKind::Chrome);
	focus.Offer(window);

	// Click-to-focus (162) asks `Contains` on the way up from what the pointer hit and then names the
	// entity outright. Chrome is in the stack, so a person clicking back onto the launcher after
	// clicking a window gets the keyboard back — the walk is the only reader that treats it differently.
	GYRO_CHECK(focus.Contains(launcher));
	GYRO_CHECK(focus.Focus(launcher));
	GYRO_CHECK(focus.Focused() == launcher);
}

// Which sessions are on screen, and the keyboard leaving with the screen. Every claim here fails as a
// person typing into a window they cannot see: the machine locked and the password going to whatever
// was in front of it.
namespace
{
constexpr auto Mine = static_cast<SessionId>(1);
constexpr auto Theirs = static_cast<SessionId>(2);

[[nodiscard]] SceneOutput Panel(SessionId session)
{
	SceneOutput panel{ .Bounds = { { 0.0, 0.0 }, { 1000.0, 1000.0 } },
		               .Density = Scale::FromInteger(1),
		               .Grid = { 1000, 1000 } };
	panel.Session = session;

	return panel;
}
} // namespace

GYRO_TEST(SceneFocus, AWindowOfASessionNoScreenIsShowingDoesNotHoldTheKeyboard)
{
	SceneFocus focus;
	const EntityId mine{ 1, 1 };
	const EntityId theirs{ 2, 1 };

	focus.Offer(mine, FocusKind::Window, Mine);
	focus.Offer(theirs, FocusKind::Window, Theirs);

	// Nothing has been said about screens yet, so nothing of anybody's is reachable — which is the state
	// of a machine before an agent has connected and is the honest answer rather than the newest entry.
	const std::array showingMine{ Panel(Mine) };
	focus.Present(showingMine);

	// The other session's window is the newest and would be the answer on the stack policy alone.
	GYRO_CHECK(focus.Focused() == mine);

	const std::array showingTheirs{ Panel(Theirs) };
	focus.Present(showingTheirs);

	GYRO_CHECK(focus.Focused() == theirs);
}

GYRO_TEST(SceneFocus, GyrosOwnIsReachableWhateverIsOnTheScreens)
{
	SceneFocus focus;
	const EntityId console{ 1, 1 };

	// `SessionId::None`, which is what every author with no session behind it offers: the recovery
	// console, the splash, a gym.
	focus.Offer(console);

	const std::array showingTheirs{ Panel(Theirs) };
	focus.Present(showingTheirs);

	GYRO_CHECK(focus.Focused() == console);
}

GYRO_TEST(SceneFocus, TheWalkStepsOverAWindowNobodyIsLookingAt)
{
	SceneFocus focus;
	const EntityId first{ 1, 1 };
	const EntityId hidden{ 2, 1 };
	const EntityId second{ 3, 1 };

	focus.Offer(first, FocusKind::Window, Mine);
	focus.Offer(hidden, FocusKind::Window, Theirs);
	focus.Offer(second, FocusKind::Window, Mine);

	const std::array showingMine{ Panel(Mine) };
	focus.Present(showingMine);

	// Two windows to pass between, not three: the middle entry belongs to a session on no screen, and a
	// cycle that stopped on it would put the keyboard somewhere with nothing to show for it.
	GYRO_CHECK(focus.CycleNext() == first);
	GYRO_CHECK(focus.CycleNext() == second);
}

GYRO_TEST(SceneFocus, HandingTheOnlyScreenToSomebodyElseTakesTheKeyboardWithIt)
{
	ManualClock clock;
	SceneStore store{ clock };

	const std::array outputs{ Panel(Mine) };
	store.SetOutputs(outputs);

	const EntityId window = store.CreateContainer({}, { .Extent = { 400.0F, 300.0F } }).value();
	GYRO_REQUIRE(store.SetSession(window, Mine));

	store.Focus().Offer(window, FocusKind::Window, Mine);
	GYRO_REQUIRE(store.Focus().Focused() == window);

	// The reassignment decision 43 makes locking out of, asked of the store rather than of the stack:
	// the output goes to another session and the window is still there, still drawn wherever that
	// session is shown, and no longer somewhere a keystroke can reach.
	store.SetOutputSession(outputs[0].Id, Theirs);

	GYRO_CHECK(store.Focus().Focused().IsNull());
	GYRO_CHECK(store.Focus().Contains(window));

	store.SetOutputSession(outputs[0].Id, Mine);

	GYRO_CHECK(store.Focus().Focused() == window);
}
