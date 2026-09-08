#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "Core/Result.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Shell/Canvas.h"
#include "Shell/Launch.h"
#include "Shell/Metrics.h"
#include "Shell/Session.h"
#include "Wayland/FractionalScaleV1.h"
#include "Wayland/GyroBindingsV1.h"
#include "Wayland/GyroChromeV1.h"
#include "Wayland/Viewporter.h"
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
// **Everything it draws is sized in logical pixels**, which is Shell/Metrics.h's subject: the card,
// its corner, its hairline and the text in it are numbers the shell chooses at the size a person
// perceives, multiplied by the scale gyro says this surface is drawn at. The two facts that takes come
// from the compositor rather than from a panel — `xdg_toplevel.configure_bounds` for how much room
// there is, `wp_fractional_scale_v1` for the scale — and both are about *this surface*, which is the
// whole reason the bar no longer reads `wl_output`.
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
	[[nodiscard]] Result<void>
	Open(Session& session, Launcher& launcher, std::uint32_t modifiers, std::uint32_t keysym);

	// Brings the bar up as though the chord had been pressed.
	//
	// **For the machine where the chord cannot be pressed**, which is every headless and dumping run:
	// those backends have no keyboard, so without this there is no way to put the bar in front of a
	// renderer at all and the only instrument for the surface is a panel and a hand.
	void Summon() { Show(); }

	// Whether the bar is on screen, which is what the loop reports and what a test would assert.
	[[nodiscard]] bool Shown() const noexcept { return m_Shown; }

	// What has been typed, for a test that wants to see the edit without a panel in front of it.
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

		// How much room the bar has, in logical pixels, and the number that retired reading a mode off
		// whichever output was announced first.
		//
		// **The compositor's answer for this surface rather than the shell's guess from a panel.** gyro
		// puts chrome on the output holding the pointer (141) and resolves these bounds against that
		// output, so a bar summoned on the monitor beside a laptop is sized for the monitor. A shell
		// reading `wl_output.mode` had no way to know which screen it was about to appear on, and got the
		// laptop's grid whenever the two disagreed.
		//
		// **Logical rather than device pixels**, which is the other half of it: this is the size the card
		// is laid out in, and the scale below is what turns it into a buffer.
		void OnConfigureBounds(std::int32_t width, std::int32_t height) override;

	private:
		Bar& m_Bar;
	};

	// What gyro asks the bar's own surface to be drawn at, as the exact rational.
	//
	// **The fractional scale rather than `wl_surface.preferred_buffer_scale`, and they are not two
	// opinions to reconcile.** The integer event carries the ceiling of this same number, so a shell
	// reading both would have the coarser answer to a question it already has the exact one to. What
	// the difference buys is visible: on a panel deriving 1.25, the integer path draws the card at 2x
	// and gyro shrinks the result by 0.625, which turns a one-pixel hairline and an antialiased corner
	// into a smear. Drawing at 1.25 exactly leaves both as they were authored.
	//
	// The bar states what that buffer means with `wp_viewport.set_destination`, because
	// `wl_surface.set_buffer_scale` is an integer and always was — which is why Viewporter had to land
	// before this could (171).
	class ScaleEvents final : public Wayland::WpFractionalScaleV1Listener
	{
	public:
		explicit ScaleEvents(Bar& bar) noexcept : m_Bar{ bar } {}

		void OnPreferredScale(std::uint32_t scale) override;

	private:
		Bar& m_Bar;
	};

	// The surface's own events, every one of which the bar has a better source for. `enter` and `leave`
	// are which outputs it is on, which it does not lay out against; `preferred_buffer_scale` is the
	// ceiling of what `ScaleEvents` carries exactly; `preferred_buffer_transform` gyro never sends.
	class SurfacePixels final : public Wayland::WlSurfaceIgnoring
	{};

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

	// Brings the canvas and the viewport into line with the bounds and the scale currently known, and
	// answers whether anything about the geometry moved. Reallocates only on a real change: gyro
	// re-configures a bar that is already up whenever the world moves under it, and throwing the pool
	// away on each of those would be a launcher that stutters while somebody else's window opens.
	[[nodiscard]] bool Resize();

	// Paints the scrim, the card and the query into the next free buffer and commits it. Silent where
	// both buffers are still held — see `Canvas::Next`.
	void Draw();

	// The card, its corner and its edge, drawn into `words` with coverage taken from a signed distance
	// so the curve is not a staircase. Split out because it is the only arithmetic here that is about
	// shape rather than about protocol.
	void DrawCard(std::span<std::uint32_t> words) const;

	// The prompt, what has been typed, and the caret after it. Clipped to the card, and scrolled to
	// keep the caret in view once a command outgrows it — a launcher that hid the end of what a person
	// was typing would be one they could not tell they had mistyped.
	void DrawQuery(std::span<std::uint32_t> words) const;

	// One key press turned into an edit. Returns whether anything changed, so a modifier that reaches
	// this does not cost a redraw.
	[[nodiscard]] bool Edit(std::uint32_t keycode);

	// The bar comes down and what was typed is started, in that order — see the definition.
	void Launch();

	Session* m_Session = nullptr;
	Launcher* m_Launcher = nullptr;
	ChordEvents m_ChordEvents{ *this };
	SurfaceEvents m_SurfaceEvents{ *this };
	WindowEvents m_WindowEvents{ *this };
	KeyEvents m_KeyEvents{ *this };
	ScaleEvents m_ScaleEvents{ *this };
	SurfacePixels m_SurfacePixels;
	Wayland::WlSurface m_Surface;
	Wayland::XdgSurface m_XdgSurface;
	Wayland::XdgToplevel m_Window;
	Wayland::GyroChromeV1 m_Chrome;
	Wayland::GyroBindingV1 m_Chord;
	Wayland::WlKeyboard m_Keyboard;
	Wayland::WpViewport m_Viewport;
	Wayland::WpFractionalScaleV1 m_Fractional;
	Canvas m_Canvas;
	std::string m_Query;

	// The room the bar has and the scale it is drawn at — the two the compositor supplies and
	// everything in Shell/Metrics.h is resolved from. Zero bounds mean no configure has arrived yet,
	// which is every moment before the first summon.
	PixelSize<BufferSpace> m_Bounds{};
	Scale m_Scale;

	// What those two resolved to, held rather than recomputed because `Draw` reads it several times per
	// keystroke and `Resize` is what decides it changed.
	BarMetrics m_Metrics{};

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
