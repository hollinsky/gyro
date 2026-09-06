#pragma once

#include <cstdint>
#include <vector>

#include "Core/Time.h"
#include "Protocol/Keymap.h"
#include "Wayland/Server/GyroBindingsV1.h"

class Binding;
class BindingsGlobal;
class BindingsManager;
struct KeyEvent;

// `gyro_bindings_v1`: the keys a shell claims, and the seam that lets one exist at all.
//
// **This is what a shell needs before it needs anything else.** A launcher, an overview and a switcher
// are three different windows and one shared prerequisite: a key that opens them, taken before the
// application in front of a person sees it. Everything else a shell does — mapping a surface, listing
// the windows — has a protocol already; this had none, and without it a shell is a program with no way
// to be summoned.
//
// **It is gyro's own protocol and the first one in `Protocols/`.** Nothing upstream describes this:
// the wayland-protocols suite has no shortcut protocol at all, and the compositor-specific ones that
// exist are built for *applications* asking a desktop to route a key at them — a registry of named
// actions, a portal, a permission prompt — which is a different problem with a different party in the
// middle. Here the client is a shell the compositor has already granted the run of a session to, so
// the trust question is answered before the first request and what is left is the mechanism.
//
// **A keysym and a modifier mask, rather than a keycode.** [Input/Chord.h](../Input/Chord.h) matches
// gyro's own escape hatch on keycodes precisely so that the way out of a compositor holding the display
// cannot be taken away by a keymap that failed to compile — and that argument does not transfer, which
// is the point worth being explicit about. A shell binding is not a way out of anything. It is a
// preference a person wrote down as *super and space*, and matching it on a keycode would give the
// position `space` occupies on a US keyboard — which is fine for `space` and wrong for every letter the
// moment somebody selects AZERTY or Dvorak, where a launcher bound to `super and r` opens on the key
// labelled `k`.
//
// **Gyro's own keys are matched first and cannot be claimed.** The composition root feeds every key to
// `Input::Chord` before the clients see it, so `Ctrl+Alt+Esc` and the `Alt+Tab` walk are outside what
// this protocol can reach. That ordering is the escape hatch's whole premise and it is not negotiable:
// a shell that could claim the leader is a shell whose crash takes the machine with it.
//
// **A matched key does not reach the focused window, and neither does its release.** A chord that let
// the key through would be a launcher that opens *and* types a space into whatever was in front of it.
// The release is swallowed for the press's sake, which is `Input::Chord`'s rule reused: a client that
// received neither is consistent and one that received only the release is not.
//
// **Every binding that matches fires, and there is no conflict to report.** One session may hold a
// shell and a screen recorder at once, and refusing the second party's claim is a refusal it can do
// nothing about — while a protocol error would end a client over something a person configured. Two
// claims on one chord is a shell's own bug and a shell's own to notice.
//
// **What is not here is a held chord.** A switcher walked with `Alt` down needs a release event and
// the modifier releases that go with it, and until something wants one the stand-in in
// `Input::Chord` — decision 177's `Alt+Tab` — stays where it is. Adding an event to an interface is
// a version bump and nothing else, which is why it is safe to leave out rather than guess at.
inline constexpr std::uint32_t BindingsVersion = 1;

// Whether a key with these modifiers held is this chord.
//
// **Exact rather than a subset**, which is what makes `Super+Space` not fire under `Ctrl+Super+Space`.
// A subset match would mean every chord a person adds silently shadows the ones it contains, and the
// window it steals the key from is the one they were typing in.
[[nodiscard]] constexpr bool
ChordMatches(KeyModifier claimed, std::uint32_t keysym, KeyModifier held, std::uint32_t pressed) noexcept
{
	return claimed == held && keysym == pressed && keysym != 0;
}

// One claimed chord, and the object its presses go out on.
//
// **It belongs to the global rather than to the manager that made it**, which is the protocol's rule
// held structurally rather than remembered: dropping `gyro_bindings_v1` leaves the chords claimed, so
// a binding reached through the manager would stop being matched the moment a shell tidied away the
// factory it no longer needs. That is a shortcut that works until a shell is written the way the
// interface reads, and then silently does not.
class Binding final : public Wayland::Server::GyroBindingV1Handler
{
public:
	Binding(BindingsGlobal& global, KeyModifier modifiers, std::uint32_t keysym) noexcept
		: m_Global{ &global }, m_Modifiers{ modifiers }, m_Keysym{ keysym }
	{}

	void OnGone() override;

	void OnDestroy() override {}

	// Whether this is the chord, and if so, tell the client. Separate from `Matches` so that the walk
	// over the world's bindings reads as one question rather than two.
	[[nodiscard]] bool Matches(KeyModifier held, std::uint32_t keysym) const noexcept
	{
		return ChordMatches(m_Modifiers, m_Keysym, held, keysym);
	}

	// Send the press, carrying the instant the *device* reported the key.
	void Press(Instant when) const;

	// The global is going away with the host. Nulled rather than followed, because the objects are
	// destroyed in whatever order libwayland tears a client down in.
	void Forget() noexcept { m_Global = nullptr; }

private:
	BindingsGlobal* m_Global = nullptr;

	KeyModifier m_Modifiers = KeyModifier::None;
	std::uint32_t m_Keysym = 0;
};

// One client's `gyro_bindings_v1`, and the bindings it made through it.
class BindingsManager final : public Wayland::Server::GyroBindingsV1Handler
{
public:
	explicit BindingsManager(BindingsGlobal& global) noexcept : m_Global{ &global } {}

	void OnGone() override;

	void OnDestroy() override {}

	// **The chord goes to the global rather than into this object**, per `Binding` above: the manager is
	// a factory and holds nothing that outlives a request.
	[[nodiscard]] Wayland::Server::GyroBindingV1Handler*
	OnClaim(Wayland::Server::GyroBindingsV1Modifier modifiers, std::uint32_t keysym) override;

	void Forget() noexcept { m_Global = nullptr; }

private:
	BindingsGlobal* m_Global = nullptr;
};

// The global, and the one thing the key path asks.
class BindingsGlobal final : public Wayland::Server::GyroBindingsV1Binding
{
public:
	~BindingsGlobal() override;

	BindingsGlobal() = default;
	BindingsGlobal(const BindingsGlobal&) = delete;
	BindingsGlobal& operator=(const BindingsGlobal&) = delete;

	[[nodiscard]] Wayland::Server::GyroBindingsV1Handler* OnBind(wl_client& client, std::uint32_t version) override;

	// **Whether the compositor's clients keep this key, asked once per transition.** A press is matched
	// against the chords; a release is answered from what the press did, which is why this is stateful
	// rather than a pure function of the event.
	//
	// The keymap is the seat's own, and it is read *before* the seat folds this key into it — so the
	// modifiers are the ones that were already held when the key went down, which is what a chord means.
	[[nodiscard]] bool Takes(const KeyEvent& event, const Keymap& keymap);

	void Add(BindingsManager& manager);
	void Remove(BindingsManager& manager) noexcept;

	void Add(Binding& binding);
	void Remove(Binding& binding) noexcept;

private:
	std::vector<BindingsManager*> m_Managers;

	// Every chord claimed on this host, flat rather than per manager. One session holds a shell and
	// perhaps a recorder, each with a handful of chords, so the walk is over single figures on a press.
	std::vector<Binding*> m_Bindings;

	// The keycodes whose press a binding took, held until their release. A vector because a person
	// holding two chords down at once is two entries and there is never a third.
	std::vector<std::uint32_t> m_Taken;
};
