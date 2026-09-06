// `setenv` is POSIX rather than ISO C and glibc gates it behind a feature-test macro. Named here for
// the reason the other tests in this directory name their own: what is wanted from the platform is
// stated rather than inherited.
#define _POSIX_C_SOURCE 200809L

#include "Protocol/Bindings.h"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <cstdlib>

#include "Protocol/Keymap.h"
#include "Testing/Test.h"
#include "Wayland/Server/GyroBindingsV1.h"

// The two halves of a chord that are arithmetic rather than a conversation: what matches what, and
// what a keycode means under a layout.
//
// Everything else here — a chord claimed over a socket, a press that reaches the shell and not the
// window, a release swallowed after its press — is a sequence of events between two processes, and
// Integration/ProtocolRoundTrip.Test.cpp is where gyro's server is put in front of a demarshaller that
// is not its own.

GYRO_TEST(Bindings, AChordIsTheModifiersItNamedAndNoOthers)
{
	// **The failure this is here to prevent is a subset match**, which is the natural way to write the
	// comparison and is wrong in a way nobody reports as a bug in the compositor: with it, a shell that
	// claims `Super+Space` also swallows `Ctrl+Super+Space`, and the application that wanted the second
	// one simply never receives a keystroke its author is certain it sent.
	GYRO_CHECK(ChordMatches(KeyModifier::Super, XKB_KEY_space, KeyModifier::Super, XKB_KEY_space));

	GYRO_CHECK(
		!ChordMatches(KeyModifier::Super, XKB_KEY_space, KeyModifier::Super | KeyModifier::Control, XKB_KEY_space)
	);

	GYRO_CHECK(
		!ChordMatches(KeyModifier::Super | KeyModifier::Control, XKB_KEY_space, KeyModifier::Super, XKB_KEY_space)
	);
	GYRO_CHECK(!ChordMatches(KeyModifier::Super, XKB_KEY_space, KeyModifier::None, XKB_KEY_space));
	GYRO_CHECK(!ChordMatches(KeyModifier::Super, XKB_KEY_space, KeyModifier::Super, XKB_KEY_Tab));
}

GYRO_TEST(Bindings, AChordWithNoModifiersIsAChord)
{
	// A bare key is a legitimate claim — a shell that wants `F1` to be a help overlay, or the media keys
	// on a laptop, names no modifier at all — so `None` has to match `None` rather than reading as
	// *unset* and matching nothing.
	GYRO_CHECK(ChordMatches(KeyModifier::None, XKB_KEY_F1, KeyModifier::None, XKB_KEY_F1));
	GYRO_CHECK(!ChordMatches(KeyModifier::None, XKB_KEY_F1, KeyModifier::Shift, XKB_KEY_F1));
}

GYRO_TEST(Bindings, AKeyThatProducesNothingIsNeverAChord)
{
	// `Keysym` answers `XKB_KEY_NoSymbol` for a key the layout binds nothing to and for a keymap that
	// never compiled, and both of those would otherwise match a binding a client claimed for keysym zero
	// — which is every dead key on the board firing a launcher.
	GYRO_CHECK(!ChordMatches(KeyModifier::None, XKB_KEY_NoSymbol, KeyModifier::None, XKB_KEY_NoSymbol));
}

GYRO_TEST(Bindings, TheWireModifiersAreTheOnesTheKeymapAnswersWith)
{
	// **Two enumerations, one set of values, and this is what holds them together.** `KeyModifier` is
	// Protocol/Keymap.h's and `GyroBindingsV1Modifier` is generated from the XML, and a chord is matched
	// by casting one to the other — so a value that moved on either side would be a shell claiming
	// `Super` and gyro matching `Alt`, silently and only for the modifier that moved.
	using Wire = Wayland::Server::GyroBindingsV1Modifier;

	GYRO_CHECK(static_cast<std::uint32_t>(Wire::Shift) == static_cast<std::uint32_t>(KeyModifier::Shift));
	GYRO_CHECK(static_cast<std::uint32_t>(Wire::Control) == static_cast<std::uint32_t>(KeyModifier::Control));
	GYRO_CHECK(static_cast<std::uint32_t>(Wire::Alt) == static_cast<std::uint32_t>(KeyModifier::Alt));
	GYRO_CHECK(static_cast<std::uint32_t>(Wire::Super) == static_cast<std::uint32_t>(KeyModifier::Super));
}

GYRO_TEST(Bindings, AChordNamesTheKeyAPersonSeesRatherThanThePositionItSitsAt)
{
	// **The whole argument for matching on a keysym, run twice.** `KEY_T` is the position the letter `t`
	// occupies on a US keyboard and the position `y` occupies on Dvorak — so a shell configured with
	// *control, alt and t* has to reach the key a person can see the letter on, and a compositor
	// matching keycodes would open a terminal from the key marked `y`.
	//
	// This is the case where gyro's own escape chord goes the other way on purpose: Input/Chord.h reads
	// keycodes precisely so that the way out of a compositor holding the display does not move when
	// somebody selects a layout. A shell's shortcut is not a way out of anything.
	::setenv("XKB_DEFAULT_LAYOUT", "us", 1);
	::setenv("XKB_DEFAULT_VARIANT", "", 1);

	Keymap american;

	GYRO_REQUIRE(american.Open().has_value());
	GYRO_CHECK(american.Keysym(KEY_T) == XKB_KEY_t);

	::setenv("XKB_DEFAULT_VARIANT", "dvorak", 1);

	Keymap dvorak;

	GYRO_REQUIRE(dvorak.Open().has_value());
	GYRO_CHECK(dvorak.Keysym(KEY_T) == XKB_KEY_y);

	::setenv("XKB_DEFAULT_VARIANT", "", 1);
}

GYRO_TEST(Bindings, AChordIsTheUnshiftedKeyWhateverIsHeldOverIt)
{
	// **Level zero rather than the level the modifiers select**, which is what makes the two halves of a
	// chord independent. A person writes *shift and 2*, not *at* — and on a German layout the same
	// physical key shifts to `"` instead, so a compositor reading the shifted symbol would need a
	// different configuration file per layout for a shortcut that is described identically in both.
	::setenv("XKB_DEFAULT_LAYOUT", "us", 1);
	::setenv("XKB_DEFAULT_VARIANT", "", 1);

	Keymap keymap;

	GYRO_REQUIRE(keymap.Open().has_value());

	(void)keymap.Update(KEY_LEFTSHIFT, true);

	GYRO_CHECK(keymap.Held() == KeyModifier::Shift);
	GYRO_CHECK(keymap.Keysym(KEY_2) == XKB_KEY_2);

	(void)keymap.Update(KEY_LEFTSHIFT, false);

	GYRO_CHECK(keymap.Held() == KeyModifier::None);
}

GYRO_TEST(Bindings, TheFourModifiersAreReadUnderTheirXkbNames)
{
	// `Alt` is `Mod1` and `Super` is `Logo` in every layout xkeyboard-config ships, and neither is
	// spelled the way it is printed on the key. A name that did not resolve would not fail to compile —
	// `xkb_state_mod_name_is_active` answers *not held* for a modifier it has never heard of — so a
	// shortcut with `Super` in it would simply never fire, which is the silent half of this file.
	::setenv("XKB_DEFAULT_LAYOUT", "us", 1);
	::setenv("XKB_DEFAULT_VARIANT", "", 1);

	Keymap keymap;

	GYRO_REQUIRE(keymap.Open().has_value());

	(void)keymap.Update(KEY_LEFTMETA, true);
	(void)keymap.Update(KEY_LEFTALT, true);

	GYRO_CHECK(keymap.Held() == (KeyModifier::Super | KeyModifier::Alt));

	(void)keymap.Update(KEY_LEFTCTRL, true);

	GYRO_CHECK(keymap.Held() == (KeyModifier::Super | KeyModifier::Alt | KeyModifier::Control));

	(void)keymap.Update(KEY_LEFTMETA, false);
	(void)keymap.Update(KEY_LEFTALT, false);
	(void)keymap.Update(KEY_LEFTCTRL, false);

	GYRO_CHECK(keymap.Held() == KeyModifier::None);
}
