#pragma once

#include <cstdint>

#include "Core/Fd.h"
#include "Core/Result.h"

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

// The layout gyro hands its clients, and the state machine that tells them which modifiers are down.
//
// **A keymap is unavoidable, and that is why xkbcommon is linked at all.** `wl_keyboard.key` carries
// the kernel's keycode and nothing else, so a client cannot know that a key is `q` — the whole of the
// answer is `wl_keyboard.keymap`, which is a descriptor holding an XKB text keymap. There is no
// version of a seat that skips it: a client that never receives one has a keyboard whose every key is
// a number.
//
// **It sits beside the seat rather than beside the devices, which is what
// [Core/Input.h](../Core/Input.h) and [Seam/Input.h](../Seam/Input.h) already say.** A keycode is what a device produces, and gyro's own
// escape chord is matched on keycodes precisely so that the way out of a compositor holding DRM master
// does not depend on a layout having compiled (148). Translation is for the party that has somebody to
// translate *for*, and that party is here.
//
// **The layout is the one the environment names**, which is `xkb_keymap_new_from_names` with no names
// supplied: xkbcommon reads `XKB_DEFAULT_LAYOUT` and its four siblings and falls back to `us`. That is
// a stand-in for a preference gyro has nowhere to keep yet — there is no session agent and no
// configuration — and it is the same set every other compositor honours, so a person who has already
// set one gets it. What replaces it is a per-session layout arriving with the session model, at which
// point this object becomes one per seat rather than one per compositor.
//
// **The descriptor a client receives is read-only and sealed.** It is gyro's own `memfd`, reopened
// without write access and sealed against growing, shrinking and writing before it goes out, because
// the alternative is one client editing the layout every other client on the machine mapped. The same
// descriptor goes to every keyboard: it is immutable, so there is nothing to keep apart.
class Keymap
{
public:
	Keymap() = default;

	~Keymap();

	Keymap(const Keymap&) = delete;
	Keymap& operator=(const Keymap&) = delete;
	Keymap(Keymap&&) = delete;
	Keymap& operator=(Keymap&&) = delete;

	// Compile the layout and lay it out in a descriptor clients can map. Fails where xkbcommon cannot
	// be brought up or the keymap does not compile, which is a machine whose XKB data is missing or an
	// environment naming a layout that does not exist.
	[[nodiscard]] Result<void> Open();

	[[nodiscard]] bool IsOpen() const noexcept { return m_Keymap != nullptr; }

	// The descriptor and its length, as `wl_keyboard.keymap`'s two arguments. Borrowed: every keyboard
	// sends the same one and libwayland duplicates it into the message, so this object goes on owning it.
	[[nodiscard]] RawFd Descriptor() const noexcept { return m_Fd.Borrow(); }

	// The size the client is told to map, which includes the terminating byte the format requires.
	[[nodiscard]] std::uint32_t Size() const noexcept { return m_Size; }

	// The modifier state as the four numbers `wl_keyboard.modifiers` carries.
	struct Modifiers
	{
		std::uint32_t Depressed = 0;
		std::uint32_t Latched = 0;
		std::uint32_t Locked = 0;
		std::uint32_t Group = 0;

		friend constexpr bool operator==(Modifiers, Modifiers) noexcept = default;
	};

	// Fold one key transition into the state, and answer what the modifiers are afterwards.
	//
	// **Every key is fed here, including the ones the compositor's own chord swallowed.** The state is
	// what is physically held down, and a `Ctrl` that gyro consumed is still a `Ctrl` a person's finger
	// is on — dropping it would leave the state claiming a modifier is up while it is down, and the
	// client that next takes focus would be told so.
	//
	// The keycode is the kernel's, as `Core/Input.h` reports it; the offset XKB numbers keys by is
	// applied here, which is the one place in gyro that knows about it.
	Modifiers Update(std::uint32_t code, bool pressed) noexcept;

	// The modifiers as they stand, for the event that goes out with a `wl_keyboard.enter`.
	[[nodiscard]] Modifiers Current() const noexcept;

private:
	xkb_context* m_Context = nullptr;
	xkb_keymap* m_Keymap = nullptr;
	xkb_state* m_State = nullptr;

	Fd m_Fd;
	std::uint32_t m_Size = 0;
};
