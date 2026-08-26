#pragma once

#include <linux/input-event-codes.h>

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

			// Never consumed and never disarming. Releasing `Ctrl+Alt` between the leader and its verb is
			// what a person's hand does on the way to pressing a letter, and a chord that cancelled there
			// would be one nobody could complete.
			return {};
		}

		if (!event.Pressed)
		{
			// A release while armed is swallowed for its press's sake: the press was, so a client that
			// received neither is consistent and one that received only the release is not.
			return { .Action = ChordAction::None, .Consumed = m_Armed };
		}

		if (m_Armed && Elapsed(m_ArmedAt, event.When) > ChordTimeout)
		{
			m_Armed = false;
		}

		if (m_Armed)
		{
			m_Armed = false;

			return { .Action = Verb(event.Code), .Consumed = true };
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
		return code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL || code == KEY_LEFTALT || code == KEY_RIGHTALT;
	}

	// A bitmask per modifier rather than a count, so that a lost release — a key held across whatever
	// takes a device away — cannot leave a modifier stuck down at a depth nothing brings back to zero.
	void Hold(const KeyEvent& event) noexcept
	{
		const bool left = event.Code == KEY_LEFTCTRL || event.Code == KEY_LEFTALT;
		const std::uint8_t bit = left ? 0x1 : 0x2;

		std::uint8_t& held = (event.Code == KEY_LEFTCTRL || event.Code == KEY_RIGHTCTRL) ? m_Control : m_Alt;

		held = event.Pressed ? static_cast<std::uint8_t>(held | bit) : static_cast<std::uint8_t>(held & ~bit);
	}

	// The verbs, and there are two of them on purpose: a recovery console and a gym cycle would both
	// name things that do not exist, and a key bound to a stub is worse than one that says nothing.
	// `Esc` is spelled out as *back out* rather than left to the `None` below it, because leaving a
	// mode is the one thing a person tries first and it should not depend on a fallthrough.
	[[nodiscard]] static constexpr ChordAction Verb(std::uint32_t code) noexcept
	{
		switch (code)
		{
			case KEY_Q:
				return ChordAction::Quit;
			case KEY_T:
				return ChordAction::Trace;
			default:
				return ChordAction::None;
		}
	}

	std::uint8_t m_Control = 0;
	std::uint8_t m_Alt = 0;

	bool m_Armed = false;
	Instant m_ArmedAt{};
};
} // namespace Input
