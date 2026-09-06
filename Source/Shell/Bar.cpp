#include "Shell/Bar.h"

#include <spdlog/spdlog.h>
#include <sys/mman.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "Text/Font.h"
#include "Text/Label.h"

namespace
{
// What a keycode is on the wire and what xkbcommon calls the same key. The offset is the X11
// convention libxkbcommon kept, and it is the one place a shell has to know it.
constexpr std::uint32_t XkbOffset = 8;

// The text, and the rule under it. Opaque, because these are the pixels that are *not* glass — the
// material is seen exactly where this shell leaves the buffer alone.
constexpr std::uint32_t Ink = 0xFF'F2'F2'F5;
constexpr std::uint32_t Rule = 0x60'FF'FF'FF;

// The face is a fraction of the panel rather than a fixed cell, because a bitmap glyph does not scale
// and Text/Font.h's ladder is what makes a line legible on a 4K panel and on a 1366x768 one (38). A
// twenty-fourth of the height puts the query at roughly the size a heading is set at.
[[nodiscard]] std::int32_t CellFor(std::int32_t height) noexcept
{
	return std::max(height / 24, 1);
}
} // namespace

void Bar::ChordEvents::OnPressed(std::uint32_t, std::uint32_t, std::uint32_t)
{
	// **The timestamp is dropped, and that is a gap rather than a judgement.** It is the origin
	// gyro stamps an animation with so that two shell processes reacting to one press compose into
	// one gesture (186), and nothing here animates yet: the bar has no entrance of its own to start
	// from t₀. The first thing that does will want this argument.
	m_Bar.Show();
}

void Bar::SurfaceEvents::OnConfigure(std::uint32_t serial)
{
	m_Bar.m_XdgSurface.AckConfigure(serial);

	// A configure is answered with a buffer only where one is owed. gyro configures a chrome surface
	// the way it configures a window, so this arrives again whenever the world changes under a bar
	// that is already up, and attaching there would be a redraw nobody asked for.
	if (m_Bar.m_Mapping)
	{
		m_Bar.m_Mapping = false;
		m_Bar.m_Shown = true;

		m_Bar.Draw();
	}
}

void Bar::WindowEvents::OnClose()
{
	m_Bar.Hide();
}

void Bar::KeyEvents::OnKeymap(Wayland::WlKeyboardKeymapFormat format, Fd descriptor, std::uint32_t size)
{
	if (format != Wayland::WlKeyboardKeymapFormat::XkbV1)
	{
		return;
	}

	// **`MAP_PRIVATE`, which the protocol requires and which is not a detail.** The descriptor is
	// shared with every other client the seat has a keyboard on, and a mapping anybody could write
	// into would be one client editing another's layout.
	void* const mapped = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, descriptor.Borrow().Value, 0);

	if (mapped == MAP_FAILED)
	{
		return;
	}

	xkb_keymap* const keymap = ::xkb_keymap_new_from_string(
		m_Bar.m_Xkb, static_cast<const char*>(mapped), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS
	);

	::munmap(mapped, size);

	if (keymap == nullptr)
	{
		return;
	}

	xkb_state* const state = ::xkb_state_new(keymap);

	if (state == nullptr)
	{
		::xkb_keymap_unref(keymap);

		return;
	}

	// The old pair goes after the new one is built, so a keymap that fails to compile leaves the shell
	// typing the layout it already had rather than typing nothing.
	::xkb_state_unref(m_Bar.m_State);
	::xkb_keymap_unref(m_Bar.m_Keymap);

	m_Bar.m_Keymap = keymap;
	m_Bar.m_State = state;
}

void Bar::KeyEvents::OnEnter(std::uint32_t, Wayland::WlSurface, std::span<const std::byte>)
{
	// **The keys held on entry are not replayed**, which is what `wl_keyboard` says a client must do
	// and is right here for a reason of its own: the chord that summoned this bar is still down half
	// the time, and replaying it would put a `space` in the query the moment it opened.
	m_Bar.m_Query.clear();
}

void Bar::KeyEvents::OnLeave(std::uint32_t, Wayland::WlSurface)
{}

void Bar::KeyEvents::OnKey(std::uint32_t, std::uint32_t, std::uint32_t key, Wayland::WlKeyboardKeyState state)
{
	if (state != Wayland::WlKeyboardKeyState::Pressed || !m_Bar.m_Shown)
	{
		return;
	}

	if (m_Bar.Edit(key))
	{
		m_Bar.Draw();
	}
}

void Bar::KeyEvents::OnModifiers(
	std::uint32_t,
	std::uint32_t depressed,
	std::uint32_t latched,
	std::uint32_t locked,
	std::uint32_t group
)
{
	if (m_Bar.m_State != nullptr)
	{
		::xkb_state_update_mask(m_Bar.m_State, depressed, latched, locked, 0, 0, group);
	}
}

void Bar::KeyEvents::OnRepeatInfo(std::int32_t, std::int32_t)
{}

Bar::~Bar()
{
	::xkb_state_unref(m_State);
	::xkb_keymap_unref(m_Keymap);
	::xkb_context_unref(m_Xkb);
}

Result<void> Bar::Open(Session& session, Launcher& launcher, std::uint32_t modifiers, std::uint32_t keysym)
{
	m_Session = &session;
	m_Launcher = &launcher;

	m_Xkb = ::xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	if (m_Xkb == nullptr)
	{
		return Failure(ENOMEM, "creating the shell's keyboard context");
	}

	const std::int32_t width = session.Width();
	const std::int32_t height = session.Height();

	if (width <= 0 || height <= 0)
	{
		// A shell with no output has nothing to size a full-screen surface against, and guessing would
		// put a bar of the wrong size in front of the first panel that lights up.
		return Failure(ENODEV, "sizing the run bar against an output");
	}

	if (Result<void> opened = m_Canvas.Open(session.Shm(), width, height); !opened)
	{
		return opened;
	}

	m_Surface = session.Compositor().CreateSurface(m_PixelEvents);
	m_XdgSurface = session.Shell().GetXdgSurface(m_Surface, m_SurfaceEvents);

	if (!m_Surface.IsValid() || !m_XdgSurface.IsValid())
	{
		return Failure(EPROTO, "asking the compositor for a surface");
	}

	m_Window = m_XdgSurface.GetToplevel(m_WindowEvents);

	if (!m_Window.IsValid())
	{
		return Failure(EPROTO, "asking the compositor for a toplevel");
	}

	// **Before anything is ever committed on this surface.** Chrome is declared before the first map
	// and never taken back, so this and the material below are the two requests that must not be late:
	// a toplevel that mapped first would be a window, in the alt-tab and behind whatever is in front
	// of it, for the rest of its life.
	m_Chrome = session.Chrome().GetChrome(m_Window);

	if (!m_Chrome.IsValid())
	{
		return Failure(EPROTO, "declaring the run bar to be chrome");
	}

	m_Chrome.SetMaterial(Wayland::GyroChromeV1Material::Glass);
	m_Window.SetTitle("gyro run bar");
	m_Window.SetAppId("dev.gyro.shell");

	// The buffer is drawn at device pixels, so the surface has to say so or gyro would take the buffer
	// for a surface twice the size on a HiDPI panel.
	m_Surface.SetBufferScale(session.Scale());

	m_Chord = session.Bindings().Claim(static_cast<Wayland::GyroBindingsV1Modifier>(modifiers), keysym, m_ChordEvents);

	if (!m_Chord.IsValid())
	{
		return Failure(EPROTO, "claiming the run bar's chord");
	}

	m_Keyboard = session.Seat().GetKeyboard(m_KeyEvents);

	if (!m_Keyboard.IsValid())
	{
		return Failure(EPROTO, "asking the seat for a keyboard");
	}

	return session.Flush();
}

void Bar::Show()
{
	if (m_Shown || m_Mapping)
	{
		return;
	}

	// An empty commit is what starts the negotiation: gyro answers it with a configure, and the buffer
	// goes on the commit that acknowledges it. This is the whole handshake rather than a second attach
	// because an unmap took the last configure with it.
	m_Mapping = true;

	m_Surface.Commit();
}

void Bar::Hide()
{
	if (!m_Shown)
	{
		return;
	}

	m_Shown = false;
	m_Query.clear();

	// Attaching nothing unmaps the surface. The toplevel, the chrome declaration and the material all
	// live on and are what the next summon negotiates from.
	m_Surface.Attach({}, 0, 0);
	m_Surface.Commit();
}

void Bar::Launch()
{
	// **Taken before the bar comes down, because coming down is what clears it.** Hiding first is also
	// the order a person perceives: the screen is theirs again on the keystroke rather than after a
	// fork, and a browser takes long enough to put a window up that a launcher still on screen while it
	// starts would read as a press that did not register.
	const std::string query = m_Query;

	Hide();

	if (m_Launcher == nullptr || query.empty())
	{
		return;
	}

	if (Result<void> started = m_Launcher->Run(query); !started)
	{
		spdlog::error("gyro-shell: running {}: {}", query, started.error());
	}
}

bool Bar::Edit(std::uint32_t keycode)
{
	if (m_State == nullptr)
	{
		return false;
	}

	const xkb_keysym_t keysym = ::xkb_state_key_get_one_sym(m_State, keycode + XkbOffset);

	if (keysym == XKB_KEY_Escape)
	{
		Hide();

		return false;
	}

	if (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter)
	{
		Launch();

		return false;
	}

	if (keysym == XKB_KEY_BackSpace)
	{
		if (m_Query.empty())
		{
			return false;
		}

		// Back over the whole code point rather than the byte, so deleting a character a person typed
		// once does not take three presses and leave a broken sequence in between.
		std::size_t taken = m_Query.size();

		while (taken > 0 && (static_cast<unsigned char>(m_Query[taken - 1]) & 0xC0U) == 0x80U)
		{
			--taken;
		}

		m_Query.resize(taken > 0 ? taken - 1 : 0);

		return true;
	}

	char text[8] = {};
	const int written = ::xkb_state_key_get_utf8(m_State, keycode + XkbOffset, text, sizeof(text));

	if (written <= 0 || text[0] < ' ')
	{
		// A control character is not an edit. Enter is answered above, before this, because it arrives
		// here as an ordinary carriage return and would otherwise be indistinguishable from a key that
		// does nothing.
		return false;
	}

	m_Query.append(text, static_cast<std::size_t>(written));

	return true;
}

void Bar::Draw()
{
	const std::span<std::uint32_t> words = m_Canvas.Next();

	if (words.empty())
	{
		return;
	}

	const std::int32_t width = m_Canvas.Width();
	const std::int32_t height = m_Canvas.Height();

	// **Transparent, and that is the drawing rather than the absence of one.** Every texel left at zero
	// is a texel gyro fills with glass, so clearing the buffer is what puts the whole screen under the
	// material. What follows are the only pixels this shell contributes.
	std::memset(words.data(), 0, words.size() * sizeof(std::uint32_t));

	const Face& face = Nearest(CellFor(height));
	const std::int32_t left = width / 6;
	const std::int32_t baseline = height / 3;

	if (Result<Label> label = Label::Draw(face, m_Query.empty() ? " " : m_Query, Ink))
	{
		const PixelSize<BufferSpace> size = label->Size();

		for (std::int32_t row = 0; row < size.Height && baseline + row < height; ++row)
		{
			for (std::int32_t column = 0; column < size.Width && left + column < width; ++column)
			{
				const std::uint32_t texel = label->At(column, row);

				if (texel != 0)
				{
					words
						[static_cast<std::size_t>(baseline + row) * static_cast<std::size_t>(width) +
					     static_cast<std::size_t>(left + column)] = texel;
				}
			}
		}
	}

	// The rule the query sits on. It is what makes a bar with nothing typed into it look like a place
	// to type rather than a screen that has gone blurry.
	const std::int32_t underline = baseline + face.CellSize().Height + face.CellSize().Height / 3;
	const std::int32_t right = width - left;

	if (underline < height)
	{
		for (std::int32_t column = left; column < right && column < width; ++column)
		{
			words
				[static_cast<std::size_t>(underline) * static_cast<std::size_t>(width) +
			     static_cast<std::size_t>(column)] = Rule;
		}
	}

	m_Surface.Attach(m_Canvas.Take(), 0, 0);
	m_Surface.DamageBuffer(0, 0, width, height);
	m_Surface.Commit();
}
