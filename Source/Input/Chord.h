#pragma once

#include <linux/input-event-codes.h>

#include <array>
#include <cstdint>

#include "Core/Time.h"
#include "Seam/Input.h"

// The compositor's own keys: a leader chord, and the handful of verbs behind it.
//
// **gyro has no other way out.** It is a boot service holding DRM master from first-open with no VT
// to switch to, so a run that reaches the panel owns the screen until the process ends — and on the
// hardware this is developed on there is no sysrq key at all: the kernel's handler is bound to the
// keyboard, `kernel.sysrq` is Fedora's default of 16, and no key on the board produces `KEY_SYSRQ`.
// Somebody sitting in front of a working gyro with a full-screen client on it must be able to stop it
// without a second machine.
//
// **A leader rather than one chord per verb**, because every additional chord is another argument
// about which physical keys exist on which keyboard. One reserved combination is spent once, and the
// verbs after it are letters. `Ctrl+Alt+Esc` is the combination: `Ctrl`, `Alt` and `Esc` are present
// and in the same places on every keyboard gyro is expected to meet, including Chromebooks with no
// F-row and no `Delete`, and it avoids the two ranges the kernel's own console handler claims —
// `Ctrl+Alt+F1`…`F12` and `Ctrl+Alt+Del`. The one binding it collides with anywhere is KDE's force-kill
// cursor, which gyro can only meet as a nested client, where the host swallows it first.
//
// **Rejected: `Ctrl+Alt+Backspace` straight to quit.** The X11 zap is one keystroke from ending a
// session, and it spends the only reserved combination on the least interesting verb. **Rejected:
// `Super`.** That is the shell's namespace for window management and should not be spent on debugging.
//
// **What this is not is a rescue.** These keys are read on the dispatch thread, so a dispatch thread
// that is stuck never sees them; a frame thread that spins is `RLIMIT_RTTIME`'s to kill, and a machine
// wedged below both is still the network's problem. What the chord buys is leaving a *working*
// compositor, which today has no exit at all, and a place to hang debug verbs that is not a signal.
//
// **Matched on keycodes rather than on keysyms**, which is the same argument one layer down: the
// escape hatch must not depend on the layer above it working. A keymap that failed to compile, a
// layout somebody selected, a seat that has not been created yet — none of them may take the way out
// away, and a physical key is the only thing none of them can move.
//
// **Alt+Tab is the one binding here that is not behind the leader**, and it is not debugging: it is
// the third stand-in for the shell that does not exist yet, beside the two in
// [Protocol/Floor.h](../Protocol/Floor.h) — where a window opens and where a click puts focus is
// already gyro's to decide, and *which window a person meant next* is the same question with a
// keyboard in front of it. It cannot be a verb behind the leader for a reason about the gesture
// rather than about the key: cycling needs somewhere to stop, and a held modifier is the only thing
// on a keyboard that says *I am still choosing* and then says *this one*. The leader disarms after
// one verb, so it can only ever step once — and one step against a most-recently-used stack is a swap
// between the last two windows, which never reaches a third.
//
// **`Alt` held and `Tab` pressed steps, `Shift` reverses it, and letting `Alt` go lands.** Every
// Wayland compositor takes this combination, so no client is losing a key it could have expected to
// keep, and it is where a person's hand already goes. The caveat is the leader's own, one level worse:
// a nested gyro is behind another compositor's bindings and `Alt+Tab` is the *first* thing a host
// claims, so this works on a panel and not inside a window.

namespace Input
{
// How long an armed chord waits for its verb before giving up.
//
// **Measured against the *event's* timestamp rather than against a clock**, because that is the only
// instant this class has and the only one that means what it should: two seconds of somebody thinking
// about it, not two seconds of a compositor being busy. The cost is that an arming with no key after
// it stays armed until the next key arrives, which nothing can currently perceive — when the armed
// state grows the badge it needs, the badge's own deadline is what will retire it.
inline constexpr Duration ChordTimeout = std::chrono::seconds{ 2 };

// What a chord asked for. `None` is every key that is not one, which is almost all of them.
enum class ChordAction : std::uint8_t
{
	None,

	// Stop the compositor.
	Quit,

	// Write what the trace ring is holding, which is the same request `SIGUSR1` makes.
	Trace,

	// Move focus to the next window, and to the previous one. Both raise what they land on, because
	// with no shell drawing a switcher the raise *is* the switcher — the Floorplanner centres every
	// window on the same point, so a step that only moved focus would be a gesture a person cannot see.
	CycleFocus,
	CycleFocusBack,

	// The hand came off `Alt`: whatever the cycle is on is where a person meant to be. It is a separate
	// action rather than something the steps do themselves because that is the whole reason the binding
	// is a held modifier — until this arrives the walk can still go further, and the order it is walking
	// must not be rewritten underneath it.
	CycleFocusEnd,

	// Lock every screen showing a session, and unlock every screen this locked.
	//
	// **One verb for both directions, and the second half of it is the development one.** Decision 43
	// makes locking an output reassignment rather than a mode, so the press that locks and the press
	// that unlocks are the same kind of thing to everything downstream — but a lock a keystroke undoes
	// is not a lock, and what is entitled to unlock a screen is the login agent authenticating a
	// person. This toggles until there is one, so that both halves of the transition can be looked at
	// on a machine with no greeter on it.
	Lock,

	// Write what the panel is showing, as a PAM per output.
	//
	// **The debug capture rather than a screenshot feature.** It forces the frame it captures to
	// composite everything — see Frame/Loop.h's `m_Capture` — so that the file is the whole screen
	// rather than whatever the GPU happened to be responsible for once the display engine took its
	// share. That costs a frame drawn differently from its neighbours, which is why this is a verb
	// behind the leader and not a thing a person does every day: if compositing and promotion ever
	// disagree, pressing this key is *visible* as a flash. That flash is the diagnostic.
	Screenshot,
};

// What the caller does with a key after the chord has seen it.
struct ChordVerdict
{
	ChordAction Action = ChordAction::None;

	// **Whether the key belongs to the compositor rather than to whatever has focus.** Everything
	// typed while the chord is armed is swallowed, including the key that fails to name a verb, so a
	// mistyped `Ctrl+Alt+Esc W` cannot reach a client as a stray `w`. The modifiers themselves are
	// never swallowed: they are held before anything knows a chord is coming, and a seat that saw the
	// `Esc` disappear but not the `Ctrl` is the state a client can already reconcile.
	bool Consumed = false;
};

// One verb, in the two forms it is asked for.
//
// **A table rather than a switch, because there are two askers and one vocabulary.** A verb arrives
// as a keycode from a keyboard and as a letter from [Trigger.h](Trigger.h)'s development pipe, and
// two switches keyed on the same three verbs is how `s` comes to mean a screenshot in one and
// nothing in the other. Each row names something that exists: a recovery console and a gym cycle
// would both name things that do not, and a key bound to a stub is worse than one that says nothing.
struct Verb
{
	// What is printed on the key, which is also what a person writes into the pipe.
	char Letter;

	// What the kernel calls that key. Matched on rather than on a keysym for the same reason the
	// leader is: the way out may not depend on a keymap having compiled or on a layout somebody chose.
	std::uint32_t Code;

	ChordAction Action;
};

inline constexpr std::array<Verb, 4> Verbs{ {
	{ 'q', KEY_Q, ChordAction::Quit },
	{ 't', KEY_T, ChordAction::Trace },
	{ 's', KEY_S, ChordAction::Screenshot },
	{ 'l', KEY_L, ChordAction::Lock },
} };

// What a keycode asks for, and `None` for every key that asks for nothing. `Esc` is among those
// deliberately — leaving a mode is the one thing a person tries first, and it backing out is what
// `None` already does at the call site rather than a fallthrough somebody has to find.
[[nodiscard]] constexpr ChordAction VerbFor(std::uint32_t code) noexcept
{
	for (const Verb& verb : Verbs)
	{
		if (verb.Code == code)
		{
			return verb.Action;
		}
	}

	return ChordAction::None;
}

// The same question asked with the letter instead, which is the only form a pipe can carry. A
// separate name rather than an overload, because `KEY_Q` is an `int` and both conversions from one
// are the same rank — an overload set here is a call that does not compile at the site that wants it
// most.
[[nodiscard]] constexpr ChordAction VerbForLetter(char letter) noexcept
{
	for (const Verb& verb : Verbs)
	{
		if (verb.Letter == letter)
		{
			return verb.Action;
		}
	}

	return ChordAction::None;
}

// The leader state machine. One per input path; it holds only what is being held down.
class Chord
{
public:
	// Show it one key and be told whose it is.
	[[nodiscard]] ChordVerdict Feed(const KeyEvent& event) noexcept
	{
		if (IsModifier(event.Code))
		{
			Hold(event);

			// **The last `Alt` coming up is what ends a cycle**, and it is the one thing a modifier here
			// produces an action for. Both sides are watched together, so a person holding both and
			// releasing one goes on choosing.
			if (m_Cycling && m_Alt == 0)
			{
				m_Cycling = false;

				return { .Action = ChordAction::CycleFocusEnd };
			}

			// Never consumed and never disarming. Releasing `Ctrl+Alt` between the leader and its verb is
			// what a person's hand does on the way to pressing a letter, and a chord that cancelled there
			// would be one nobody could complete.
			return {};
		}

		if (!event.Pressed)
		{
			// A release while armed is swallowed for its press's sake: the press was, so a client that
			// received neither is consistent and one that received only the release is not. A `Tab` coming
			// up mid-cycle is the same debt.
			return { .Action = ChordAction::None, .Consumed = m_Armed || (m_Cycling && event.Code == KEY_TAB) };
		}

		if (m_Armed && Elapsed(m_ArmedAt, event.When) > ChordTimeout)
		{
			m_Armed = false;
		}

		if (m_Armed)
		{
			m_Armed = false;

			return { .Action = VerbFor(event.Code), .Consumed = true };
		}

		// **Before the leader and after it**, which is to say it is neither: the leader is a mode and this
		// is a modifier a person is holding, so a `Tab` that arrives while a verb is being waited for was
		// already swallowed above as a key that named none.
		if (event.Code == KEY_TAB && m_Alt != 0)
		{
			m_Cycling = true;

			return { .Action = m_Shift != 0 ? ChordAction::CycleFocusBack : ChordAction::CycleFocus, .Consumed = true };
		}

		if (event.Code == KEY_ESC && m_Control != 0 && m_Alt != 0)
		{
			m_Armed = true;
			m_ArmedAt = event.When;

			return { .Action = ChordAction::None, .Consumed = true };
		}

		return {};
	}

	// Whether a verb is currently being waited for. Nothing reads it yet; it is what the badge will.
	[[nodiscard]] bool IsArmed() const noexcept { return m_Armed; }

private:
	// Both sides of each modifier, because a chord that only works with the left `Ctrl` is a chord
	// somebody reports as intermittent.
	[[nodiscard]] static constexpr bool IsModifier(std::uint32_t code) noexcept
	{
		return code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL || code == KEY_LEFTALT || code == KEY_RIGHTALT ||
		       code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT;
	}

	// A bitmask per modifier rather than a count, so that a lost release — a key held across whatever
	// takes a device away — cannot leave a modifier stuck down at a depth nothing brings back to zero.
	void Hold(const KeyEvent& event) noexcept
	{
		const bool left = event.Code == KEY_LEFTCTRL || event.Code == KEY_LEFTALT || event.Code == KEY_LEFTSHIFT;
		const std::uint8_t bit = left ? 0x1 : 0x2;

		std::uint8_t& held = Held(event.Code);

		held = event.Pressed ? static_cast<std::uint8_t>(held | bit) : static_cast<std::uint8_t>(held & ~bit);
	}

	// Which of the three a code belongs to. `IsModifier` has already said it is one of them.
	[[nodiscard]] std::uint8_t& Held(std::uint32_t code) noexcept
	{
		if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL)
		{
			return m_Control;
		}

		return (code == KEY_LEFTALT || code == KEY_RIGHTALT) ? m_Alt : m_Shift;
	}

	std::uint8_t m_Control = 0;
	std::uint8_t m_Alt = 0;
	std::uint8_t m_Shift = 0;

	// Whether a walk through the windows is in flight, which is only ever true with `Alt` down. It says
	// nothing about *where* the walk is: that is the world's, and this class has never seen one.
	bool m_Cycling = false;

	bool m_Armed = false;
	Instant m_ArmedAt{};
};
} // namespace Input
