#include "Shell/Bar.h"

#include <spdlog/spdlog.h>
#include <sys/mman.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <string_view>

#include "Text/Font.h"
#include "Text/Label.h"

namespace
{
// What a keycode is on the wire and what xkbcommon calls the same key. The offset is the X11
// convention libxkbcommon kept, and it is the one place a shell has to know it.
constexpr std::uint32_t XkbOffset = 8;

// **Every word here is premultiplied, which `wl_shm`'s `ARGB8888` requires and is easy to get wrong.**
// A client computes `colour x alpha` and stores that, so white at a tenth is `0x1A1A1A1A` rather than
// `0x1AFFFFFF` — and the second is not merely a shade off, it is a colour brighter than opaque white
// that gyro linearises and composites as one (Docs/Architecture.md#premultiplied-alpha-is-the-sharp-edge).
//
// The card is a lighter pane of the material behind it rather than a slab on top of it: the fill is
// weak enough that the smoke still moves under it, and the edge is what gives it a boundary. An opaque
// card would be a rectangle sitting on a blur, which is the look of a dialog rather than of a surface
// the compositor dressed.
constexpr std::uint32_t CardFill = 0x1F'1F'1F'1F;
constexpr std::uint32_t CardEdge = 0x47'47'47'47;

// The query, the mark in front of it, and the caret after it. The prompt and the placeholder are
// dimmed so that what a person typed is the brightest thing on the card.
constexpr std::uint32_t Ink = 0xFF'F2'F2'F5;
constexpr std::uint32_t Dim = 0x73'73'73'73;
constexpr std::uint32_t CaretInk = 0xD9'D9'D9'D9;

// `>` rather than a chevron a designer would reach for. Spleen is a fixed set of glyphs and the
// prettier marks — U+203A, U+276F — are not in it, so asking for one draws the notdef box: a hollow
// rectangle in front of the query, which looks like a bug rather than like a prompt.
constexpr std::string_view Prompt = ">";

// What the card says when nothing has been typed. It is what makes an empty bar read as a place to
// type rather than as a screen that has gone blurry, which is the job the old underline had.
constexpr std::string_view Placeholder = "Type a command";

// A premultiplied word scaled by coverage in [0, 1]. Every channel including alpha, which is what
// premultiplied means: the colour and its weight move together, so a partly covered texel is simply a
// smaller version of the same word and never a different hue.
[[nodiscard]] std::uint32_t Weigh(std::uint32_t word, float coverage) noexcept
{
	if (coverage <= 0.0F)
	{
		return 0;
	}

	if (coverage >= 1.0F)
	{
		return word;
	}

	std::uint32_t out = 0;

	for (int shift = 0; shift < 32; shift += 8)
	{
		const float channel = static_cast<float>((word >> shift) & 0xFFU) * coverage;

		out |= static_cast<std::uint32_t>(channel + 0.5F) << shift;
	}

	return out;
}

// `src` over `dst`, both premultiplied: `src + dst x (1 - src.a)`. The ordinary Porter-Duff over, and
// it is one line because premultiplication is what removes the division from it.
[[nodiscard]] std::uint32_t Over(std::uint32_t dst, std::uint32_t src) noexcept
{
	const std::uint32_t alpha = (src >> 24) & 0xFFU;

	if (alpha == 0xFFU || dst == 0)
	{
		return src;
	}

	if (alpha == 0)
	{
		return dst;
	}

	const std::uint32_t inverse = 255U - alpha;
	std::uint32_t out = 0;

	for (int shift = 0; shift < 32; shift += 8)
	{
		const std::uint32_t under = ((dst >> shift) & 0xFFU) * inverse;

		// Rounded division by 255 rather than a shift by 8. The shift is darker by up to a full level per
		// channel, which on the card's own edge — a hairline over a fill over the smoke — is three
		// roundings in a row all going the same way.
		const std::uint32_t channel = ((src >> shift) & 0xFFU) + (under + 127U) / 255U;

		out |= (channel > 0xFFU ? 0xFFU : channel) << shift;
	}

	return out;
}

// Signed distance from a point to a rounded rectangle, negative inside. `half` is the box's half
// extent measured from its centre and `radius` the corner.
//
// **Coverage from a distance rather than a per-corner special case**, because the distance is exact
// everywhere: the straight edges, the curves and the join between them all fall out of the same
// expression, so there is no seam where a hand-written corner would meet a hand-written side. It is
// also the same formulation the Vulkan renderer's corner mask uses, which is worth keeping identical
// on the day a chrome surface can ask gyro for a radius instead of drawing its own.
[[nodiscard]] float RoundedDistance(float x, float y, float halfWidth, float halfHeight, float radius) noexcept
{
	const float insetX = std::abs(x) - (halfWidth - radius);
	const float insetY = std::abs(y) - (halfHeight - radius);

	const float outsideX = std::max(insetX, 0.0F);
	const float outsideY = std::max(insetY, 0.0F);

	return std::min(std::max(insetX, insetY), 0.0F) + std::hypot(outsideX, outsideY) - radius;
}

// A distance turned into coverage, which is the whole of the antialiasing: a texel whose centre is
// half a pixel inside the boundary is fully covered, half a pixel outside it is empty, and the ramp
// between is linear. Exact enough at any scale the bar is drawn at, and it costs no sampling.
[[nodiscard]] float Coverage(float distance) noexcept
{
	return std::clamp(0.5F - distance, 0.0F, 1.0F);
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

	// **The bounds this configure carried are resolved before anything is drawn from them.** They
	// arrived on `xdg_toplevel.configure_bounds`, which the protocol puts immediately before the
	// configure a client answers, so by here they are as current as they will get.
	const bool moved = m_Bar.Resize();

	// A configure is answered with a buffer only where one is owed. gyro configures a chrome surface
	// the way it configures a window, so this arrives again whenever the world changes under a bar
	// that is already up, and attaching there would be a redraw nobody asked for.
	if (m_Bar.m_Mapping)
	{
		m_Bar.m_Mapping = false;
		m_Bar.m_Shown = true;

		m_Bar.Draw();

		return;
	}

	// **Unless the geometry actually moved**, which is a person dragging the bar's screen out from
	// under it — unplugging a monitor, or a session being handed to a different panel. The card is laid
	// out against bounds that have just changed, so what is on screen is a launcher sized for a screen
	// that is no longer there.
	if (m_Bar.m_Shown && moved)
	{
		m_Bar.Draw();
	}
}

void Bar::WindowEvents::OnClose()
{
	m_Bar.Hide();
}

void Bar::WindowEvents::OnConfigureBounds(std::int32_t width, std::int32_t height)
{
	// **Zero is *no screen* rather than a small one, and it is left alone.** gyro sends it for a session
	// that is on no output at all, and taking it would throw away the last real size in exchange for a
	// number nothing can be laid out against. `Resize` refuses to draw at zero either way; keeping the
	// old bounds means a bar summoned after the screen comes back is right on the first frame.
	if (width <= 0 || height <= 0)
	{
		return;
	}

	m_Bar.m_Bounds = { width, height };
}

void Bar::ScaleEvents::OnPreferredScale(std::uint32_t scale)
{
	m_Bar.m_Scale = Scale::FromNumerator(static_cast<std::int32_t>(scale));

	// **Acted on immediately when the bar is up, rather than waiting for a configure.** A scale change
	// with no size change behind it is a person dragging the bar onto a differently dense screen, and
	// gyro has no reason to re-configure a surface whose logical size did not move — so a bar that
	// waited would sit there at the old density until something else happened to it.
	if (m_Bar.m_Shown && m_Bar.Resize())
	{
		m_Bar.Draw();
	}
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

	m_Surface = session.Compositor().CreateSurface(m_SurfacePixels);

	if (!m_Surface.IsValid())
	{
		return Failure(EPROTO, "asking the compositor for a surface");
	}

	// **Before the first commit, because the density has to be in hand before the first buffer is
	// sized.** Drawing the opening frame at a guessed scale and correcting it is a launcher that
	// flickers once on the way up, every time. The round trip below is what turns *asked for* into
	// *known*.
	m_Fractional = session.FractionalScale().GetFractionalScale(m_Surface, m_ScaleEvents);

	if (!m_Fractional.IsValid())
	{
		return Failure(EPROTO, "asking the compositor what scale the run bar is drawn at");
	}

	// **How the buffer's size stops meaning its own pixel count.** The bar draws at the exact rational
	// and then says what that buffer is worth in logical pixels, which `wl_surface.set_buffer_scale`
	// cannot express because it is an integer. Without this the card would land at its device pixel
	// count in surface coordinates and cover the wrong fraction of the screen — the same failure
	// Protocol/Viewporter.h describes Firefox hitting from the other side.
	m_Viewport = session.Viewporter().GetViewport(m_Surface);

	if (!m_Viewport.IsValid())
	{
		return Failure(EPROTO, "asking the compositor for the run bar's viewport");
	}

	m_XdgSurface = session.Shell().GetXdgSurface(m_Surface, m_SurfaceEvents);

	if (!m_XdgSurface.IsValid())
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

	m_Chrome.SetMaterial(Wayland::GyroChromeV1Material::Smoke);
	m_Window.SetTitle("gyro run bar");
	m_Window.SetAppId("dev.gyro.shell");

	// **`set_buffer_scale` is never sent, and its absence is the point.** The surface stays at buffer
	// scale 1 and the viewport carries the size instead, so the one place the bar's density is written
	// down is the destination `Resize` sets. Sending both would be two answers to how big this buffer
	// is, and the integer one is only ever a rounding of the other.

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

	// **One round trip, and it is placed here rather than beside the request it answers.** gyro sends
	// the first `preferred_scale` from the sweep that walks the windows, so it reaches a surface only
	// once that surface *is* one — asking for it immediately after `get_fractional_scale` returns
	// nothing, because the toplevel above did not exist yet. Waiting until the whole bar is declared is
	// what makes the answer arrive, and it costs one sleep at startup on a process that then sleeps
	// until somebody presses a key.
	//
	// Losing it is not subtle: the bar would size its buffer at the logical extent, gyro would take
	// those pixels for a surface 1.2x larger than it is, and the launcher would be soft and oversized
	// on the first screen anybody saw it on.
	if (Result<void> settled = session.Roundtrip(); !settled)
	{
		return settled;
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

bool Bar::Resize()
{
	if (m_Session == nullptr || m_Bounds.IsEmpty())
	{
		return false;
	}

	const BarMetrics metrics = BarMetrics::For(m_Bounds, m_Scale);

	if (m_Canvas.Is(metrics.Surface.Width, metrics.Surface.Height) && metrics.Fits == m_Metrics.Fits)
	{
		// The same screen at the same density, which is what almost every configure is: gyro re-sends one
		// whenever the world moves under a bar that is up. Reallocating here would throw a pool away and
		// make another one identical to it, several times while somebody else's window opens.
		m_Metrics = metrics;

		return false;
	}

	m_Metrics = metrics;

	if (Result<void> opened = m_Canvas.Open(m_Session->Shm(), metrics.Surface.Width, metrics.Surface.Height); !opened)
	{
		spdlog::error("gyro-shell: {}", opened.error());

		return false;
	}

	// **The logical size, which is what makes the buffer above mean anything.** The surface is
	// `m_Bounds` across however many device pixels the scale asked for, and this is the request that
	// says so — see `Open`.
	m_Viewport.SetDestination(m_Bounds.Width, m_Bounds.Height);

	return true;
}

void Bar::Draw()
{
	if (!m_Metrics.Fits)
	{
		// Nothing is drawn rather than something cropped. A screen too narrow for the smallest card is a
		// launcher whose text would run off both ends, and the scrim alone would be a person's desktop
		// going dark with no way to tell why.
		return;
	}

	const std::span<std::uint32_t> words = m_Canvas.Next();

	if (words.empty())
	{
		return;
	}

	// **Transparent, and that is the drawing rather than the absence of one.** Every texel left at zero
	// is a texel gyro fills with smoke, so clearing the buffer is what puts the whole screen under the
	// material — the scrim a launcher wants, and simultaneously the reason a click anywhere dismisses
	// this rather than landing on the window underneath. What follows are the only pixels this shell
	// contributes.
	std::memset(words.data(), 0, words.size() * sizeof(std::uint32_t));

	DrawCard(words);
	DrawQuery(words);

	m_Surface.Attach(m_Canvas.Take(), 0, 0);
	m_Surface.DamageBuffer(0, 0, m_Metrics.Surface.Width, m_Metrics.Surface.Height);
	m_Surface.Commit();
}

void Bar::DrawCard(std::span<std::uint32_t> words) const
{
	const std::int32_t stride = m_Metrics.Surface.Width;

	// The card's centre, in the surface's own pixels and offset by half a texel: a distance is measured
	// to a texel's centre rather than to its corner, and forgetting that is a shape half a pixel out in
	// both axes — which on a hairline is the difference between one crisp line and two grey ones.
	const float centreX = static_cast<float>(m_Metrics.Origin.X) + static_cast<float>(m_Metrics.Card.Width) / 2.0F;
	const float centreY = static_cast<float>(m_Metrics.Origin.Y) + static_cast<float>(m_Metrics.Card.Height) / 2.0F;

	const float halfWidth = static_cast<float>(m_Metrics.Card.Width) / 2.0F;
	const float halfHeight = static_cast<float>(m_Metrics.Card.Height) / 2.0F;

	// Clamped below half the shorter side, which is what a radius means: past that the two corners on an
	// edge meet and the shape stops being a rounded rectangle. The metrics pick a radius well inside
	// that, so this only bites on a card squeezed by a very small screen.
	const float radius = std::min(static_cast<float>(m_Metrics.Radius), std::min(halfWidth, halfHeight));
	const float edge = static_cast<float>(m_Metrics.Edge);

	// One texel of bleed, because a texel whose centre is just outside the shape still takes partial
	// coverage from it — the antialiased fringe lives here and cropping to the exact box would clip it.
	const std::int32_t left = std::max(m_Metrics.Origin.X - 1, 0);
	const std::int32_t top = std::max(m_Metrics.Origin.Y - 1, 0);
	const std::int32_t right = std::min(m_Metrics.Origin.X + m_Metrics.Card.Width + 1, m_Metrics.Surface.Width);
	const std::int32_t bottom = std::min(m_Metrics.Origin.Y + m_Metrics.Card.Height + 1, m_Metrics.Surface.Height);

	for (std::int32_t row = top; row < bottom; ++row)
	{
		const float y = static_cast<float>(row) + 0.5F - centreY;

		for (std::int32_t column = left; column < right; ++column)
		{
			const float x = static_cast<float>(column) + 0.5F - centreX;
			const float distance = RoundedDistance(x, y, halfWidth, halfHeight, radius);

			// Two coverages from the one distance: the card's own outline, and the same outline pushed
			// inward by the hairline's width. The fill is the inner one and the edge is what lies between
			// them, so the two meet exactly and no seam is possible between the border and what it borders.
			const float outer = Coverage(distance);

			if (outer <= 0.0F)
			{
				continue;
			}

			const float inner = Coverage(distance + edge);

			std::uint32_t word = Weigh(CardFill, inner);

			word = Over(word, Weigh(CardEdge, outer - inner));

			words[static_cast<std::size_t>(row) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(column)] =
				word;
		}
	}
}

void Bar::DrawQuery(std::span<std::uint32_t> words) const
{
	const Face& face = *m_Metrics.Text;
	const PixelSize<BufferSpace> cell = face.CellSize();
	const std::int32_t stride = m_Metrics.Surface.Width;

	// The text's box inside the card: the padding on both sides, and the top edge the glyphs hang from.
	const std::int32_t textTop = m_Metrics.Origin.Y + m_Metrics.PadY;
	const std::int32_t textLeft = m_Metrics.Origin.X + m_Metrics.PadX;
	const std::int32_t textRight = m_Metrics.Origin.X + m_Metrics.Card.Width - m_Metrics.PadX;

	// Anything this draws is confined to the card, so a query longer than the room cannot spill onto the
	// scrim. Taken as a closure rather than repeated three times: the prompt, the query and the caret
	// are all clipped the same way and getting one of them wrong is a mark left outside the card.
	const auto plot = [&](std::int32_t x, std::int32_t y, std::uint32_t word) noexcept {
		if (word == 0 || x < textLeft || x >= textRight || y < m_Metrics.Origin.Y ||
		    y >= m_Metrics.Origin.Y + m_Metrics.Card.Height)
		{
			return;
		}

		std::uint32_t& target =
			words[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x)];

		target = Over(target, word);
	};

	// The prompt, and one cell of air after it. A mark rather than an icon, for the reason `Prompt`
	// carries: the face has what it has.
	if (Result<Label> mark = Label::Draw(face, Prompt, Dim))
	{
		const PixelSize<BufferSpace> size = mark->Size();

		for (std::int32_t row = 0; row < size.Height; ++row)
		{
			for (std::int32_t column = 0; column < size.Width; ++column)
			{
				plot(textLeft + column, textTop + row, mark->At(column, row));
			}
		}
	}

	const std::int32_t queryLeft = textLeft + 2 * cell.Width;

	if (queryLeft >= textRight)
	{
		return;
	}

	// **The placeholder is drawn where the query would be and the caret still sits in front of it**, so
	// an empty bar reads as a prompt waiting rather than as a label. It is dimmed for the same reason
	// the mark is: nothing on the card should compete with what a person typed.
	const bool empty = m_Query.empty();
	const std::string_view text = empty ? Placeholder : std::string_view{ m_Query };

	// The caret always sits where the next character will land, so with nothing typed it is in front of
	// the placeholder rather than on top of its first letter. That gap is the placeholder's whole
	// indent; typed text starts at the left edge and the caret follows it instead.
	const std::int32_t lead = empty ? m_Metrics.Caret + cell.Width / 2 : 0;
	const std::int32_t room = textRight - queryLeft - lead - m_Metrics.Caret;
	std::int32_t skip = 0;
	std::int32_t width = 0;

	if (Result<Label> label = Label::Draw(face, text, empty ? Dim : Ink))
	{
		const PixelSize<BufferSpace> size = label->Size();

		width = std::min(size.Width, room);

		// **The tail rather than the head, once a command outgrows the card.** A launcher showing the
		// first forty characters of what somebody is typing is one they cannot proofread: the caret and
		// the letter just pressed are the two things that have to stay on screen, and both are at the end.
		// A placeholder is never scrolled — it is not being typed into.
		skip = empty ? 0 : std::max(size.Width - room, 0);

		for (std::int32_t row = 0; row < size.Height; ++row)
		{
			for (std::int32_t column = 0; column < width; ++column)
			{
				plot(queryLeft + lead + column, textTop + row, label->At(skip + column, row));
			}
		}
	}

	// The caret, hard against what has been typed. A block rather than a line under the text, because
	// the card no longer has a rule to be part of — it is the one mark that says the keyboard is here.
	const std::int32_t caretLeft = queryLeft + (empty ? 0 : width);

	for (std::int32_t row = 0; row < cell.Height; ++row)
	{
		for (std::int32_t column = 0; column < m_Metrics.Caret; ++column)
		{
			plot(caretLeft + column, textTop + row, CaretInk);
		}
	}
}
