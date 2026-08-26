#include "Protocol/Seat.h"

#include <wayland-server-core.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>

#include "Protocol/Surface.h"

namespace
{
// The repeat every desktop ships: twenty-five keys a second after four hundred milliseconds. Not
// gyro's own numbers and not configurable — Seat.h says why, and a client that disagrees with them
// repeats on its own schedule anyway, because the repeating is its.
constexpr std::int32_t RepeatRate = 25;
constexpr std::int32_t RepeatDelay = 400;

// The seat's name. One seat, and the same string libinput was pointed at.
constexpr const char* SeatName = "seat0";

// What comes back with the error above: an object the client is holding an id for and will never send
// a request on, because the connection is over. It deletes itself the way every object here does.
class RefusedPointer final : public Wayland::Server::WlPointerIgnoring
{
public:
	void OnGone() override { delete this; }
};

class RefusedTouch final : public Wayland::Server::WlTouchIgnoring
{
public:
	void OnGone() override { delete this; }
};

// Milliseconds of the compositor's clock, truncated and wrapping at thirty-two bits. Decision 57's
// conversion at the edge, and the same one `ClientSurface::Present` performs for a frame callback: the
// domain is an `Instant` everywhere inside gyro and becomes the unit the protocol demands exactly
// here. A client differences two of these to time a repeat, and a difference across the wrap is
// correct in unsigned arithmetic.
[[nodiscard]] std::uint32_t Milliseconds(Instant at) noexcept
{
	return static_cast<std::uint32_t>(static_cast<std::uint64_t>(Monotonic::ToNanoseconds(at) / 1'000'000));
}
} // namespace

void ClientKeyboard::OnGone()
{
	m_Seat->Remove(*this);

	delete this;
}

void ClientKeyboard::OnBound()
{
	const Keymap& layout = m_Seat->Layout();

	Object().Keymap(Wayland::Server::WlKeyboardKeymapFormat::XkbV1, layout.Descriptor(), layout.Size());
	Object().RepeatInfo(RepeatRate, RepeatDelay);
}

bool ClientKeyboard::BelongsTo(wl_client* client) const noexcept
{
	return client != nullptr && Object().WireClient() == client;
}

void ClientSeat::OnBound()
{
	Object().Capabilities(Wayland::Server::WlSeatCapability::Keyboard);
	Object().Name(SeatName);
}

Wayland::Server::WlKeyboardHandler* ClientSeat::OnGetKeyboard()
{
	ClientKeyboard* const keyboard = new ClientKeyboard{ *m_Seat };

	m_Seat->Add(*keyboard);

	return keyboard;
}

Wayland::Server::WlPointerHandler* ClientSeat::OnGetPointer()
{
	Object().PostError(Wayland::Server::WlSeatError::MissingCapability, "this seat has no pointer");

	return new RefusedPointer{};
}

Wayland::Server::WlTouchHandler* ClientSeat::OnGetTouch()
{
	Object().PostError(Wayland::Server::WlSeatError::MissingCapability, "this seat has no touch");

	return new RefusedTouch{};
}

Result<void> SeatGlobal::Open(wl_display& display)
{
	m_Display = &display;

	return m_Keymap.Open();
}

Wayland::Server::WlSeatHandler* SeatGlobal::OnBind(wl_client& client, std::uint32_t version)
{
	(void)client;
	(void)version;

	return new ClientSeat{ *this };
}

void SeatGlobal::Add(ClientKeyboard& keyboard)
{
	m_Keyboards.push_back(&keyboard);

	// **A client that binds a keyboard while it already has focus is told so at once.** A toolkit
	// creates its window and its keyboard in whichever order it likes, and the one that maps first would
	// otherwise sit focused and deaf until something else moved focus away and back.
	if (!m_Focused.IsNull())
	{
		const Wayland::Server::WlSurface surface = FocusedSurface();

		if (surface.IsValid() && keyboard.BelongsTo(surface.WireClient()))
		{
			const std::uint32_t serial = NextSerial();
			const auto keys = std::as_bytes(std::span{ m_Held });

			keyboard.Object().Enter(serial, surface, keys);
			keyboard.Object().Modifiers(
				serial, m_Modifiers.Depressed, m_Modifiers.Latched, m_Modifiers.Locked, m_Modifiers.Group
			);
		}
	}
}

void SeatGlobal::Remove(ClientKeyboard& keyboard) noexcept
{
	std::erase(m_Keyboards, &keyboard);
}

void SeatGlobal::SyncFocus(EntityId focused)
{
	if (focused == m_Focused)
	{
		return;
	}

	// The leave first and against the *old* focus, which is why it runs before the member moves: the
	// event names the surface being left, and a client told it has focus twice with no leave between
	// has a key it believes is still down.
	SendLeave();

	m_Focused = focused;

	SendEnter();
}

void SeatGlobal::Key(const KeyEvent& event, bool consumed)
{
	// The state before the routing, and unconditionally. Seat.h carries why a consumed key still counts:
	// what is held down is a fact about a person's hands.
	const Keymap::Modifiers modifiers = m_Keymap.Update(event.Code, event.Pressed);

	if (event.Pressed)
	{
		if (std::find(m_Held.begin(), m_Held.end(), event.Code) == m_Held.end())
		{
			m_Held.push_back(event.Code);
		}
	}
	else
	{
		std::erase(m_Held, event.Code);
	}

	const Wayland::Server::WlSurface surface = FocusedSurface();

	if (!surface.IsValid())
	{
		// Nothing has focus, or what has it is a window gyro authored for itself. The state above is
		// still current, so whoever takes focus next is told the truth.
		m_Modifiers = modifiers;

		return;
	}

	wl_client* const client = surface.WireClient();

	for (ClientKeyboard* const keyboard : m_Keyboards)
	{
		if (!keyboard->BelongsTo(client))
		{
			continue;
		}

		// **The modifiers go out before the key and only when they changed.** Before, because a client
		// reads the key against the state it was pressed in — a `Ctrl` arriving after the `c` it modified
		// is a copy the application performs as a character. Only when they changed, because a `modifiers`
		// per keystroke is three quarters of the traffic on this interface and says nothing new.
		if (modifiers != m_Modifiers)
		{
			keyboard->Object().Modifiers(
				NextSerial(), modifiers.Depressed, modifiers.Latched, modifiers.Locked, modifiers.Group
			);
		}

		// A key gyro took is one the client never hears about, which is the escape chord's whole point
		// (148) — a mistyped verb must not arrive as a stray letter in whatever has focus.
		if (!consumed)
		{
			keyboard->Object().Key(
				NextSerial(),
				Milliseconds(event.When),
				event.Code,
				event.Pressed ? Wayland::Server::WlKeyboardKeyState::Pressed :
								Wayland::Server::WlKeyboardKeyState::Released
			);
		}
	}

	m_Modifiers = modifiers;
}

Wayland::Server::WlSurface SeatGlobal::FocusedSurface() const noexcept
{
	if (m_Focused.IsNull())
	{
		return {};
	}

	const ClientSurface* const surface = m_Context->SurfaceOf(m_Focused);

	return surface != nullptr ? surface->Object() : Wayland::Server::WlSurface{};
}

void SeatGlobal::SendEnter()
{
	const Wayland::Server::WlSurface surface = FocusedSurface();

	if (!surface.IsValid())
	{
		return;
	}

	wl_client* const client = surface.WireClient();
	const std::uint32_t serial = NextSerial();

	// The keys already down travel with the event, as the protocol requires: a person who holds a key
	// while a window opens under it must not have the compositor swallow the release.
	const auto keys = std::as_bytes(std::span{ m_Held });

	for (ClientKeyboard* const keyboard : m_Keyboards)
	{
		if (!keyboard->BelongsTo(client))
		{
			continue;
		}

		keyboard->Object().Enter(serial, surface, keys);
		keyboard->Object().Modifiers(
			serial, m_Modifiers.Depressed, m_Modifiers.Latched, m_Modifiers.Locked, m_Modifiers.Group
		);
	}
}

void SeatGlobal::SendLeave()
{
	const Wayland::Server::WlSurface surface = FocusedSurface();

	if (!surface.IsValid())
	{
		// The window went away underneath the focus — unmapped, or the client disconnected — so there is
		// nobody to tell. The protocol says as much: a destroyed surface takes its focus with it and no
		// `leave` is owed for one.
		return;
	}

	wl_client* const client = surface.WireClient();
	const std::uint32_t serial = NextSerial();

	for (ClientKeyboard* const keyboard : m_Keyboards)
	{
		if (keyboard->BelongsTo(client))
		{
			keyboard->Object().Leave(serial, surface);
		}
	}
}

std::uint32_t SeatGlobal::NextSerial() const noexcept
{
	return m_Display != nullptr ? ::wl_display_next_serial(m_Display) : 0;
}
