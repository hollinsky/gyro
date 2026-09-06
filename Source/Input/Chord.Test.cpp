#include "Input/Chord.h"

#include "Testing/Test.h"

// The way out of a compositor that owns the screen, and the four ways it must not fire: not from a
// letter, not from a stale arming, not from the modifiers alone, and not into a client.

namespace
{
using namespace Input;

// A keystroke a microsecond after the last one, which is what makes a sequence read as one gesture
// without every test spelling out a timeline it does not care about.
class Keyboard
{
public:
	[[nodiscard]] ChordVerdict Press(std::uint32_t code) { return Send(code, true); }

	[[nodiscard]] ChordVerdict Release(std::uint32_t code) { return Send(code, false); }

	[[nodiscard]] ChordVerdict PressAfter(std::uint32_t code, Duration gap)
	{
		m_When = Advanced(m_When, gap);

		return Send(code, true);
	}

	[[nodiscard]] bool IsArmed() const noexcept { return m_Chord.IsArmed(); }

private:
	[[nodiscard]] ChordVerdict Send(std::uint32_t code, bool pressed)
	{
		m_When = Advanced(m_When, std::chrono::microseconds{ 1 });

		return m_Chord.Feed(KeyEvent{ .Code = code, .Pressed = pressed, .When = m_When });
	}

	Chord m_Chord;
	Instant m_When = Monotonic::FromMicroseconds(1'000'000);
};

// The leader itself, which every test below starts with and none of them is about.
[[nodiscard]] ChordVerdict Arm(Keyboard& keyboard)
{
	GYRO_CHECK(!keyboard.Press(KEY_LEFTCTRL).Consumed);
	GYRO_CHECK(!keyboard.Press(KEY_LEFTALT).Consumed);

	return keyboard.Press(KEY_ESC);
}
} // namespace

GYRO_TEST(Chord, QuitsOnTheLeaderAndAVerb)
{
	Keyboard keyboard;

	const ChordVerdict armed = Arm(keyboard);

	GYRO_CHECK(armed.Action == ChordAction::None);
	GYRO_CHECK(armed.Consumed);
	GYRO_CHECK(keyboard.IsArmed());

	const ChordVerdict quit = keyboard.Press(KEY_Q);

	GYRO_CHECK(quit.Action == ChordAction::Quit);
	GYRO_CHECK(quit.Consumed);
	GYRO_CHECK(!keyboard.IsArmed());
}

GYRO_TEST(Chord, TracesOnT)
{
	Keyboard keyboard;

	GYRO_CHECK(Arm(keyboard).Consumed);
	GYRO_CHECK(keyboard.Press(KEY_T).Action == ChordAction::Trace);
}

GYRO_TEST(Chord, ScreenshotsOnS)
{
	Keyboard keyboard;

	GYRO_CHECK(Arm(keyboard).Consumed);

	const ChordVerdict shot = keyboard.Press(KEY_S);

	GYRO_CHECK(shot.Action == ChordAction::Screenshot);
	GYRO_CHECK(shot.Consumed);
	GYRO_CHECK(!keyboard.IsArmed());
}

GYRO_TEST(Chord, LocksOnL)
{
	Keyboard keyboard;

	GYRO_CHECK(Arm(keyboard).Consumed);

	const ChordVerdict lock = keyboard.Press(KEY_L);

	GYRO_CHECK(lock.Action == ChordAction::Lock);
	GYRO_CHECK(lock.Consumed);
	GYRO_CHECK(!keyboard.IsArmed());
}

// `s` unarmed is a letter and nothing else, which is the property every verb has to have: the leader
// is what makes a key gyro's, and a key that screenshotted whenever it was pressed would make the
// compositor unusable for typing.
GYRO_TEST(Chord, LeavesAnUnarmedSAlone)
{
	Keyboard keyboard;

	const ChordVerdict typed = keyboard.Press(KEY_S);

	GYRO_CHECK(typed.Action == ChordAction::None);
	GYRO_CHECK(!typed.Consumed);
}

GYRO_TEST(Chord, SurvivesTheModifiersBeingReleasedFirst)
{
	// What a hand actually does on the way to the letter. A chord that disarmed here is one nobody
	// could complete without holding three keys at once.
	Keyboard keyboard;

	GYRO_CHECK(Arm(keyboard).Consumed);
	GYRO_CHECK(!keyboard.Release(KEY_LEFTALT).Consumed);
	GYRO_CHECK(!keyboard.Release(KEY_LEFTCTRL).Consumed);
	GYRO_CHECK(keyboard.IsArmed());
	GYRO_CHECK(keyboard.Press(KEY_Q).Action == ChordAction::Quit);
}

GYRO_TEST(Chord, TakesEitherSideOfTheModifiers)
{
	Keyboard keyboard;

	GYRO_CHECK(!keyboard.Press(KEY_RIGHTCTRL).Consumed);
	GYRO_CHECK(!keyboard.Press(KEY_RIGHTALT).Consumed);
	GYRO_CHECK(keyboard.Press(KEY_ESC).Consumed);
	GYRO_CHECK(keyboard.Press(KEY_Q).Action == ChordAction::Quit);
}

GYRO_TEST(Chord, SwallowsAKeyThatNamesNoVerb)
{
	// The failure this prevents is a mistyped chord arriving at whatever has focus as a stray letter,
	// which for a text field is a character somebody did not type.
	Keyboard keyboard;

	GYRO_CHECK(Arm(keyboard).Consumed);

	const ChordVerdict stray = keyboard.Press(KEY_W);

	GYRO_CHECK(stray.Action == ChordAction::None);
	GYRO_CHECK(stray.Consumed);
	GYRO_CHECK(!keyboard.IsArmed());

	// And the next key is the client's again.
	const ChordVerdict after = keyboard.Press(KEY_Q);

	GYRO_CHECK(after.Action == ChordAction::None);
	GYRO_CHECK(!after.Consumed);
}

GYRO_TEST(Chord, BacksOutOnEscape)
{
	Keyboard keyboard;

	GYRO_CHECK(Arm(keyboard).Consumed);

	const ChordVerdict out = keyboard.Press(KEY_ESC);

	GYRO_CHECK(out.Action == ChordAction::None);
	GYRO_CHECK(out.Consumed);
	GYRO_CHECK(!keyboard.IsArmed());
}

GYRO_TEST(Chord, Expires)
{
	// A leader pressed and forgotten must not turn the next `q` — typed into an editor an hour later —
	// into the end of the session.
	Keyboard keyboard;

	GYRO_CHECK(Arm(keyboard).Consumed);

	const ChordVerdict late = keyboard.PressAfter(KEY_Q, ChordTimeout + std::chrono::milliseconds{ 1 });

	GYRO_CHECK(late.Action == ChordAction::None);
	GYRO_CHECK(!late.Consumed);
}

GYRO_TEST(Chord, NeedsBothModifiers)
{
	Keyboard keyboard;

	GYRO_CHECK(!keyboard.Press(KEY_LEFTCTRL).Consumed);

	const ChordVerdict escape = keyboard.Press(KEY_ESC);

	GYRO_CHECK(!escape.Consumed);
	GYRO_CHECK(!keyboard.IsArmed());

	// And `Esc` on its own is the client's, which is the key this whole mechanism most has to leave
	// alone: it is how a dialog is dismissed.
	Keyboard bare;

	GYRO_CHECK(!bare.Press(KEY_ESC).Consumed);
}

GYRO_TEST(Chord, DoesNotStickWhenAModifierIsHeldTwice)
{
	// Both `Ctrl` keys down and one released is still `Ctrl` held, and releasing the second is not.
	Keyboard keyboard;

	GYRO_CHECK(!keyboard.Press(KEY_LEFTCTRL).Consumed);
	GYRO_CHECK(!keyboard.Press(KEY_RIGHTCTRL).Consumed);
	GYRO_CHECK(!keyboard.Release(KEY_LEFTCTRL).Consumed);
	GYRO_CHECK(!keyboard.Press(KEY_LEFTALT).Consumed);
	GYRO_CHECK(keyboard.Press(KEY_ESC).Consumed);

	Keyboard released;

	GYRO_CHECK(!released.Press(KEY_LEFTCTRL).Consumed);
	GYRO_CHECK(!released.Press(KEY_RIGHTCTRL).Consumed);
	GYRO_CHECK(!released.Release(KEY_LEFTCTRL).Consumed);
	GYRO_CHECK(!released.Release(KEY_RIGHTCTRL).Consumed);
	GYRO_CHECK(!released.Press(KEY_LEFTALT).Consumed);
	GYRO_CHECK(!released.Press(KEY_ESC).Consumed);
}

// `Alt+Tab`, which is the one binding here that is not behind the leader: the walk, the direction, the
// keys it must not take, and the release that lands it.

GYRO_TEST(Chord, AltTabStepsForEveryPressAndLandsWhenAltComesUp)
{
	Keyboard keyboard;

	GYRO_CHECK(!keyboard.Press(KEY_LEFTALT).Consumed);

	// Every press is a step, because a person walking past three windows presses three times.
	const ChordVerdict first = keyboard.Press(KEY_TAB);
	const ChordVerdict second = keyboard.Press(KEY_TAB);

	GYRO_CHECK(first.Action == ChordAction::CycleFocus);
	GYRO_CHECK(first.Consumed);
	GYRO_CHECK(second.Action == ChordAction::CycleFocus);
	GYRO_CHECK(second.Consumed);

	// The release is swallowed for its press's sake, exactly as an armed chord's is.
	GYRO_CHECK(keyboard.Release(KEY_TAB).Consumed);

	const ChordVerdict landed = keyboard.Release(KEY_LEFTALT);

	GYRO_CHECK(landed.Action == ChordAction::CycleFocusEnd);

	// The modifier itself is never taken from the client: what a person is holding is a fact about their
	// hands rather than about who the compositor decided was listening.
	GYRO_CHECK(!landed.Consumed);
}

GYRO_TEST(Chord, ShiftWalksTheOtherWay)
{
	Keyboard keyboard;

	GYRO_CHECK(!keyboard.Press(KEY_LEFTALT).Consumed);
	GYRO_CHECK(keyboard.Press(KEY_TAB).Action == ChordAction::CycleFocus);

	GYRO_CHECK(!keyboard.Press(KEY_RIGHTSHIFT).Consumed);
	GYRO_CHECK(keyboard.Press(KEY_TAB).Action == ChordAction::CycleFocusBack);

	// And letting go of `Shift` in the middle of a gesture goes on walking forwards, because the hand
	// that reached for it is still on `Alt`.
	GYRO_CHECK(keyboard.Release(KEY_RIGHTSHIFT).Action == ChordAction::None);
	GYRO_CHECK(keyboard.Press(KEY_TAB).Action == ChordAction::CycleFocus);
}

GYRO_TEST(Chord, TabIsTheClientsWhenNobodyIsHoldingAlt)
{
	Keyboard keyboard;

	const ChordVerdict bare = keyboard.Press(KEY_TAB);

	GYRO_CHECK(bare.Action == ChordAction::None);
	GYRO_CHECK(!bare.Consumed);

	// Which is the key this mechanism most has to leave alone: it is how a person moves between the
	// fields of a form.
	GYRO_CHECK(!keyboard.Release(KEY_TAB).Consumed);

	// And a stray `Alt` release with nothing walking is not a landing.
	GYRO_CHECK(!keyboard.Press(KEY_LEFTALT).Consumed);
	GYRO_CHECK(keyboard.Release(KEY_LEFTALT).Action == ChordAction::None);
}

GYRO_TEST(Chord, TheWalkOutlivesOneOfTwoAltKeys)
{
	// Both `Alt` keys down and one released is still `Alt` held, so the walk is still in flight — the
	// same argument the leader's own modifiers rest on.
	Keyboard keyboard;

	GYRO_CHECK(!keyboard.Press(KEY_LEFTALT).Consumed);
	GYRO_CHECK(!keyboard.Press(KEY_RIGHTALT).Consumed);
	GYRO_CHECK(keyboard.Press(KEY_TAB).Action == ChordAction::CycleFocus);

	GYRO_CHECK(keyboard.Release(KEY_LEFTALT).Action == ChordAction::None);
	GYRO_CHECK(keyboard.Release(KEY_RIGHTALT).Action == ChordAction::CycleFocusEnd);
}

GYRO_TEST(Chord, TheLeaderSwallowsTabRatherThanWalking)
{
	// `Ctrl+Alt+Esc` and then `Tab` is a verb that does not exist, and the key is taken for the reason
	// every mistyped verb is: a stray `Tab` must not reach a client out of a chord it did not complete.
	Keyboard keyboard;

	GYRO_CHECK(Arm(keyboard).Consumed);

	const ChordVerdict verb = keyboard.Press(KEY_TAB);

	GYRO_CHECK(verb.Action == ChordAction::None);
	GYRO_CHECK(verb.Consumed);

	// And nothing is walking, so the hand coming off `Alt` lands nothing.
	GYRO_CHECK(keyboard.Release(KEY_LEFTALT).Action == ChordAction::None);
}
