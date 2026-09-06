#pragma once

#include <cstdint>
#include <string>

#include "Core/Result.h"
#include "Shell/Canvas.h"
#include "Shell/Session.h"
#include "Wayland/GyroBindingsV1.h"
#include "Wayland/GyroChromeV1.h"
#include "Wayland/Wayland.h"
#include "Wayland/XdgShell.h"

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

// The run bar: a surface a person summons with a key, types into, and dismisses.
//
// **It covers the whole output**, which is a decision about the material rather than about taste. A
// chrome surface is dressed in glass and gyro draws that behind whatever the surface leaves
// transparent, so a full-output surface is the screen blurring behind a line of text — and it is
// simultaneously the only way this shell can dismiss on a click, because a chrome surface has no grab
// and a click beside a small bar would land on the window underneath it. The alternative was a strip
// in the middle of the screen, which would need a grab protocol that does not exist to be dismissible
// by anything but the keyboard.
//
// **Declared chrome once, mapped and unmapped forever after.** `gyro_chrome_v1` is fixed for the life
// of a toplevel and must be asked for before the first map, so the toplevel outlives every
// appearance: summoning the bar is an xdg-shell negotiation from the start — a commit with no buffer,
// a configure, an acknowledgement, a buffer — and dismissing it is an attach of nothing.
//
// **What it does not do is grab the keyboard for itself.** Chrome takes the keyboard when it maps
// because gyro gives it (187), and the walk through the windows steps over it, so dismissing the bar
// puts a person back in what they were typing into. That is the compositor's behaviour and this file
// relies on it rather than reimplementing it.
class Bar
{
public:
	Bar() = default;
	~Bar();

	Bar(const Bar&) = delete;
	Bar& operator=(const Bar&) = delete;

	// Builds the surface, declares it chrome, dresses it in glass, claims the chord and asks for a
	// keyboard. Nothing is on screen when this returns: the bar is summoned rather than started.
	[[nodiscard]] Result<void> Open(Session& session, std::uint32_t modifiers, std::uint32_t keysym);

	// Whether the bar is on screen, which is what the loop reports and what a test would assert.
	[[nodiscard]] bool Shown() const noexcept { return m_Shown; }

	// What has been typed, for the caller that will turn it into a program to run.
	[[nodiscard]] const std::string& Query() const noexcept { return m_Query; }

private:
	class ChordEvents final : public Wayland::GyroBindingV1Listener
	{
	public:
		explicit ChordEvents(Bar& bar) noexcept : m_Bar{ bar } {}

		void OnPressed(std::uint32_t secondsHigh, std::uint32_t secondsLow, std::uint32_t nanoseconds) override;

	private:
		Bar& m_Bar;
	};

	class SurfaceEvents final : public Wayland::XdgSurfaceListener
	{
	public:
		explicit SurfaceEvents(Bar& bar) noexcept : m_Bar{ bar } {}

		void OnConfigure(std::uint32_t serial) override;

	private:
		Bar& m_Bar;
	};

	class WindowEvents final : public Wayland::XdgToplevelIgnoring
	{
	public:
		explicit WindowEvents(Bar& bar) noexcept : m_Bar{ bar } {}

		void OnClose() override;

	private:
		Bar& m_Bar;
	};

	class KeyEvents final : public Wayland::WlKeyboardListener
	{
	public:
		explicit KeyEvents(Bar& bar) noexcept : m_Bar{ bar } {}

		void OnKeymap(Wayland::WlKeyboardKeymapFormat format, Fd descriptor, std::uint32_t size) override;

		void OnEnter(std::uint32_t serial, Wayland::WlSurface surface, std::span<const std::byte> keys) override;

		void OnLeave(std::uint32_t serial, Wayland::WlSurface surface) override;

		void
		OnKey(std::uint32_t serial, std::uint32_t time, std::uint32_t key, Wayland::WlKeyboardKeyState state) override;

		void OnModifiers(
			std::uint32_t serial,
			std::uint32_t depressed,
			std::uint32_t latched,
			std::uint32_t locked,
			std::uint32_t group
		) override;

		void OnRepeatInfo(std::int32_t rate, std::int32_t delay) override;

	private:
		Bar& m_Bar;
	};

	// The bar goes up. A no-op while it is already up, because a chord claimed by this client fires on
	// every press and a person leaning on the key is not asking for two launchers.
	void Show();

	// The bar comes down: an attach of nothing, which is how a Wayland surface stops existing in the
	// world without the client giving up its id.
	void Hide();

	// Paints the query into the next free buffer and commits it. Silent where both buffers are still
	// held — see `Canvas::Next`.
	void Draw();

	// One key press turned into an edit. Returns whether anything changed, so a modifier that reaches
	// this does not cost a redraw.
	[[nodiscard]] bool Edit(std::uint32_t keycode);

	Session* m_Session = nullptr;
	ChordEvents m_ChordEvents{ *this };
	SurfaceEvents m_SurfaceEvents{ *this };
	WindowEvents m_WindowEvents{ *this };
	KeyEvents m_KeyEvents{ *this };
	Wayland::WlSurfaceIgnoring m_PixelEvents;
	Wayland::WlSurface m_Surface;
	Wayland::XdgSurface m_XdgSurface;
	Wayland::XdgToplevel m_Window;
	Wayland::GyroChromeV1 m_Chrome;
	Wayland::GyroBindingV1 m_Chord;
	Wayland::WlKeyboard m_Keyboard;
	Canvas m_Canvas;
	std::string m_Query;

	// The layout the compositor sent, which is the only way a keycode becomes a character. A shell
	// that mapped keycodes itself would type QWERTY at a person using Dvorak.
	xkb_context* m_Xkb = nullptr;
	xkb_keymap* m_Keymap = nullptr;
	xkb_state* m_State = nullptr;

	bool m_Shown = false;

	// Set by the chord and cleared by the configure that answers it: the acknowledgement has to be
	// followed by a buffer, and the same event arrives for a resize the bar did not ask for.
	bool m_Mapping = false;
};
