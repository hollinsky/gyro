#include "Protocol/Seat.h"

#include <wayland-server-core.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>

#include "Protocol/Floor.h"
#include "Protocol/Surface.h"
#include "Scene/Hit.h"

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

// A surface-local coordinate as the wire spells one: 24.8 fixed point, which is the *only* place a
// pointer position is ever quantized. Decision 52's rule arriving at the far edge — everything inside
// gyro carries the subpixel position, and a client asked for it in 256ths.
//
// Truncating rather than rounding, and toward negative infinity rather than toward zero: the grid a
// client reads this on is the same half-open grid its input region is stated on, so a position of
// 10.996 belongs to the column at 10 and rounding it to 11 would put the pointer one column past the
// edge it is actually inside. `std::floor` rather than a cast for the negative half, which a grab
// reaches every time somebody drags off the left of a window.
[[nodiscard]] wl_fixed_t Fixed(double value) noexcept
{
	return static_cast<wl_fixed_t>(std::floor(value * 256.0));
}

// The same, for the surface-local coordinates, which are single precision because a surface is bounded
// by its own size (`Geometry/Space.h`). A separate overload rather than a promotion at the call site,
// because the build treats an implicit widening as an error and this is where it would happen.
[[nodiscard]] wl_fixed_t Fixed(float value) noexcept
{
	return Fixed(static_cast<double>(value));
}

[[nodiscard]] Wayland::Server::WlPointerAxis WireAxis(ScrollAxis axis) noexcept
{
	return axis == ScrollAxis::Horizontal ? Wayland::Server::WlPointerAxis::HorizontalScroll :
	                                        Wayland::Server::WlPointerAxis::VerticalScroll;
}

// What did the scrolling, which decides whether a toolkit runs kinetic scrolling at all: only a finger
// can lift, so only a finger has a flick to decay. `Core/Input.h` carries the argument; this is the
// translation.
[[nodiscard]] Wayland::Server::WlPointerAxisSource WireSource(ScrollSource source) noexcept
{
	switch (source)
	{
		case ScrollSource::Finger:
			return Wayland::Server::WlPointerAxisSource::Finger;

		case ScrollSource::Continuous:
			return Wayland::Server::WlPointerAxisSource::Continuous;

		case ScrollSource::Wheel:
			break;
	}

	return Wayland::Server::WlPointerAxisSource::Wheel;
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

void ClientPointer::OnGone()
{
	m_Seat->Remove(*this);

	delete this;
}

bool ClientPointer::BelongsTo(wl_client* client) const noexcept
{
	return client != nullptr && Object().WireClient() == client;
}

void ClientSeat::OnBound()
{
	Object().Capabilities(Wayland::Server::WlSeatCapability::Keyboard | Wayland::Server::WlSeatCapability::Pointer);
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
	ClientPointer* const pointer = new ClientPointer{ *m_Seat };

	m_Seat->Add(*pointer);

	return pointer;
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

void SeatGlobal::Add(ClientPointer& pointer)
{
	m_Pointers.push_back(&pointer);

	// **No enter for a pointer bound while it is already over the window**, which is the one place this
	// differs from `Add(ClientKeyboard&)` above and the difference is the protocol's. A `wl_keyboard`
	// with focus and no `enter` is deaf forever, because nothing else will move focus; a pointer is
	// resolved from scratch on the next `SyncPointer`, which is the next dispatch iteration — so
	// clearing what the clients have been told is all this owes, and it costs one redundant enter
	// against the surface the pointer was already on.
	m_Pointed = {};
}

void SeatGlobal::Remove(ClientPointer& pointer) noexcept
{
	std::erase(m_Pointers, &pointer);
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
	return SurfaceFor(m_Focused);
}

Wayland::Server::WlSurface SeatGlobal::SurfaceFor(EntityId id) const noexcept
{
	if (id.IsNull())
	{
		return {};
	}

	const ClientSurface* const surface = m_Context->SurfaceOf(id);

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

void SeatGlobal::Button(const PointerButton& event)
{
	m_Queue.push_back(SeatPointerEvent{ .What = SeatPointerEvent::Kind::Button, .Pressed = event });
}

void SeatGlobal::Scroll(const PointerScroll& event)
{
	m_Queue.push_back(SeatPointerEvent{ .What = SeatPointerEvent::Kind::Scroll, .Scrolled = event });
}

void SeatGlobal::SyncPointer(SceneStore& scene, Instant now)
{
	// **A drag is written before anything is resolved against the world**, because the window being
	// moved is part of that world: the hit test below, the menu a press might dismiss and the placement
	// a popup is about to be given all read a scene in which the window has already arrived where the
	// hand is. Doing it after would resolve this iteration's events against last iteration's picture,
	// which is one frame of the window trailing the pointer — the exact artefact decision 51 keeps this
	// gesture inside the compositor to avoid.
	m_Drag.Track(scene, m_MovedAt.value_or(now));

	// Where the pointer is now, before anything the devices said is routed: a press lands on what the
	// motion in front of it moved onto, which is what makes clicking a window a person just slid onto
	// work in the wakeup they slid onto it.
	Refocus(Resolve(scene), now);

	// **The grab opens on the first button and closes on the last**, and it is settled before the events
	// go out rather than after, so that the surface a release is delivered to is the one that took the
	// press even when both are in this queue.
	const bool grabbed = !m_Grab.IsNull();

	for (SeatPointerEvent& event : m_Queue)
	{
		if (event.What != SeatPointerEvent::Kind::Button)
		{
			continue;
		}

		if (event.Pressed.Pressed)
		{
			if (m_ButtonsDown++ == 0)
			{
				m_Grab = m_Pointed;

				// **The serial belongs to the press rather than to the grab, so it is claimed here and
				// filled in at delivery.** Cleared first because a press over gyro's own background sends
				// nothing: leaving the last gesture's number standing would let a client quote a serial
				// from a click that happened inside somebody else's window.
				m_GrabSerial.reset();
				event.OpensGrab = true;

				// **Before the focus, because a press outside an open menu is first of all a press that
				// closes it.** Dismissing retires the popup and `SceneStore::Retire` takes it off the focus
				// stack, so doing this second would focus a menu on its way out and leave a person's next
				// keystroke going to a window that is no longer on screen. [Popup.h](Popup.h) has what a
				// grab does and does not do here.
				m_Context->Popups().DismissOutside(scene, m_Grab);

				// **The press that opens the grab is the press that focuses**, and no other: a second
				// button going down inside a gesture, or a release ending one, must not move focus, and
				// under the grab they are not even asking about what is under the pointer any more.
				// Decision 162, and the policy is `Protocol/Floor.h`'s beside the placement rather than
				// here, so that a shell takes both away in one commit.
				FocusByClick(scene, m_Grab);
			}
		}
		else if (m_ButtonsDown > 0 && --m_ButtonsDown == 0)
		{
			m_Grab = {};
			m_GrabSerial.reset();

			// **The button coming up ends the drag, and nothing else does.** A person lets go of a window
			// where they let go of it: there is no snap back, no commit to wait for and nothing to
			// confirm, because the window has been at that position on every frame of the gesture already.
			m_Drag.End();
		}
	}

	Deliver();

	m_Queue.clear();
	m_MovedAt.reset();

	// **The grab ended, so where the pointer is is a question again.** A person who presses on a window,
	// drags off it and lets go is over something else by then, and the enter for it has to go out now
	// rather than on whatever wakes the thread next — which, on a world that has settled, may be
	// nothing at all until they move again.
	if (grabbed && m_Grab.IsNull())
	{
		Refocus(Resolve(scene), now);
	}
}

SeatGlobal::PointerTarget SeatGlobal::Resolve(const SceneStore& scene) const
{
	// **A pointer nobody can see is on nothing.** A machine driven by a touchscreen has a position and
	// no cursor (`Scene/Pointer.h`), and this is also what keeps a compositor that has never seen a mouse
	// from sending an `enter` for whatever happens to be at the origin.
	if (!scene.Pointer().IsVisible())
	{
		return {};
	}

	// **A window being dragged takes the pointer away from its own client**, which is the compositor
	// grab superseding the protocol's implicit one. The client is told with a `leave` — that is what
	// `Refocus` does with this answer — and it is the truthful thing to say: the pointer is driving the
	// window rather than pointing into it, and a toolkit that kept receiving motion would run its own
	// hover and drag logic underneath a gesture it is not in. The `enter` goes back out when the button
	// comes up, from the same call at the end of `SyncPointer` an ordinary grab ends through.
	//
	// **So the client never sees the release that ends the drag**, having been given the press that
	// started it. That is the protocol's own answer for a grab taken away — a `leave` is what cancels a
	// toolkit's own tracking, and it is why it goes out rather than being suppressed — and it is the
	// same shape every compositor's interactive move has.
	if (m_Drag.IsActive())
	{
		return {};
	}

	const Point<GlobalSpace> position = scene.Pointer().Position();

	if (!m_Grab.IsNull())
	{
		const std::optional<Point<SurfaceSpace>> local = LocalOn(scene, m_Grab, position);

		// Nothing where the grabbed window went away or was hidden underneath the gesture. The client is
		// told it lost the pointer, which is the truth: what it was tracking is not on screen.
		return local ? PointerTarget{ .Node = m_Grab, .Local = *local } : PointerTarget{};
	}

	const SceneHit hit = HitTest(scene, position);

	return { .Node = hit.Node, .Local = hit.Local };
}

void SeatGlobal::Refocus(const PointerTarget& target, Instant now)
{
	if (target.Node != m_Pointed)
	{
		// The leave first and against the *old* surface, for `SyncFocus`'s reason: the event names what
		// is being left, so it goes out before the member moves.
		SendPointerLeave();

		m_Pointed = target.Node;
		m_Local = target.Local;

		SendPointerEnter(target.Local);

		return;
	}

	// **A motion that moved nothing sends nothing**, which is not a micro-optimization: a window
	// animating under a still pointer is republished every frame, and a `motion` per publication would
	// tell a toolkit the hand moved sixty times a second while it was resting on the desk. Toolkits
	// start hover timers and cancel tooltips off exactly that event.
	if (m_Pointed.IsNull() || target.Local == m_Local)
	{
		return;
	}

	m_Local = target.Local;

	const Wayland::Server::WlSurface surface = SurfaceFor(m_Pointed);

	if (!surface.IsValid())
	{
		return;
	}

	wl_client* const client = surface.WireClient();

	// The device's own instant where a device moved this iteration, and dispatch's now where the *window*
	// moved under a still hand — there is no hand movement to timestamp in the second case, and carrying
	// the last one would date a motion to a wakeup it did not happen in.
	const std::uint32_t time = Milliseconds(m_MovedAt.value_or(now));

	for (ClientPointer* const pointer : m_Pointers)
	{
		if (!pointer->BelongsTo(client))
		{
			continue;
		}

		pointer->Object().Motion(time, Fixed(target.Local.X), Fixed(target.Local.Y));

		if (pointer->Grouped())
		{
			pointer->Object().Frame();
		}
	}
}

void SeatGlobal::SendPointerEnter(Point<SurfaceSpace> local)
{
	const Wayland::Server::WlSurface surface = SurfaceFor(m_Pointed);

	if (!surface.IsValid())
	{
		// The pointer is over a window gyro authored for itself — the splash, the recovery console, the
		// floor between two windows — or over nothing at all. There is no client on the far side of it,
		// which is a case rather than an error.
		return;
	}

	wl_client* const client = surface.WireClient();
	const std::uint32_t serial = NextSerial();

	for (ClientPointer* const pointer : m_Pointers)
	{
		if (!pointer->BelongsTo(client))
		{
			continue;
		}

		pointer->Object().Enter(serial, surface, Fixed(local.X), Fixed(local.Y));

		if (pointer->Grouped())
		{
			pointer->Object().Frame();
		}
	}
}

void SeatGlobal::SendPointerLeave()
{
	const Wayland::Server::WlSurface surface = SurfaceFor(m_Pointed);

	if (!surface.IsValid())
	{
		// The window went away underneath the pointer, so there is nobody to tell — the same case
		// `SendLeave` names for the keyboard, and the protocol's own rule that a destroyed surface takes
		// its focus with it.
		return;
	}

	wl_client* const client = surface.WireClient();
	const std::uint32_t serial = NextSerial();

	for (ClientPointer* const pointer : m_Pointers)
	{
		if (!pointer->BelongsTo(client))
		{
			continue;
		}

		pointer->Object().Leave(serial, surface);

		if (pointer->Grouped())
		{
			pointer->Object().Frame();
		}
	}
}

void SeatGlobal::Deliver()
{
	if (m_Queue.empty())
	{
		return;
	}

	const Wayland::Server::WlSurface surface = SurfaceFor(m_Pointed);

	if (!surface.IsValid())
	{
		// Clicked on gyro's own background, or on nothing. The queue is still cleared by the caller: an
		// event nobody was under is spent rather than owed to whoever appears next.
		return;
	}

	wl_client* const client = surface.WireClient();

	// **The events are the outer loop and the client's pointer objects the inner one**, which is the
	// order that makes a serial mean something: one physical press is one number, and a client holding
	// two `wl_pointer`s on one connection — a toolkit and its portal helper in the same binary — was
	// otherwise told a different serial on each. Every request in the protocol that quotes a serial back
	// is asking *which event was this*, and an answer that depended on which object carried it cannot be
	// checked.
	bool sent = false;

	for (const SeatPointerEvent& event : m_Queue)
	{
		if (event.What == SeatPointerEvent::Kind::Button)
		{
			const std::uint32_t serial = NextSerial();

			// The press that opened the grab, now that it has a number and has actually gone out to
			// somebody. This one entry is the whole of the ledger `BeginMove` checks a request against.
			if (event.OpensGrab)
			{
				m_GrabSerial = serial;
			}

			for (ClientPointer* const pointer : m_Pointers)
			{
				if (!pointer->BelongsTo(client))
				{
					continue;
				}

				pointer->Object().Button(
					serial,
					Milliseconds(event.Pressed.When),
					event.Pressed.Code,
					event.Pressed.Pressed ? Wayland::Server::WlPointerButtonState::Pressed :
											Wayland::Server::WlPointerButtonState::Released
				);
			}

			sent = true;

			continue;
		}

		const PointerScroll& scroll = event.Scrolled;
		const Wayland::Server::WlPointerAxis axis = WireAxis(scroll.Axis);
		const std::uint32_t time = Milliseconds(scroll.When);

		// Whole detents only. `axis_discrete` cannot say *a third of a notch*, and a high-resolution
		// wheel reports exactly that — so the fine motion travels in the distance below, where it is
		// expressible, and this carries the clicks a person actually felt.
		const std::int32_t detents = static_cast<std::int32_t>(scroll.Clicks120 / 120.0);

		for (ClientPointer* const pointer : m_Pointers)
		{
			if (!pointer->BelongsTo(client))
			{
				continue;
			}

			// **The source first, then the detents, then the distance**, which is the order the protocol
			// fixes and the order a toolkit reads them in: it decides whether to run kinetic scrolling
			// from the source, and it cannot decide that after it has already consumed the distance.
			if (pointer->Grouped())
			{
				pointer->Object().AxisSource(WireSource(scroll.Source));
			}

			if (scroll.Stop)
			{
				// The fingers left the pad, which carries no distance and is the whole of what a flick
				// decays from. Only a `Finger` produces one, so a client below version 5 simply never
				// hears that a gesture ended — which is what it would have had from any compositor of
				// that era.
				if (pointer->Grouped())
				{
					pointer->Object().AxisStop(time, axis);
				}

				continue;
			}

			if (pointer->Grouped() && detents != 0)
			{
				pointer->Object().AxisDiscrete(axis, detents);
			}

			pointer->Object().Axis(time, axis, Fixed(scroll.Distance));
		}

		sent = true;
	}

	if (!sent)
	{
		return;
	}

	// The group closing everything above it: a press and the scroll that came with it are one physical
	// event and a toolkit is entitled to see them that way. Every pointer of this client was handed the
	// same queue, so there is one answer to whether a frame is owed rather than one per object.
	for (ClientPointer* const pointer : m_Pointers)
	{
		if (pointer->BelongsTo(client) && pointer->Grouped())
		{
			pointer->Object().Frame();
		}
	}
}

bool SeatGlobal::BeginMove(SceneStore& scene, EntityId window, std::uint32_t serial)
{
	// One drag at a time, because there is one pointer. A second request inside the same gesture is a
	// client asking for a grab it already holds, or asking to take one off another window.
	if (m_Drag.IsActive())
	{
		return false;
	}

	// No button down, or a number that is not the one gyro sent with the press that put it down. Both
	// are a client asking to be handed the pointer without a person having pressed anything.
	if (m_Grab.IsNull() || !m_GrabSerial || serial != *m_GrabSerial)
	{
		return false;
	}

	// And the press has to have landed in the window being asked for. `FocusTargetFor` resolves the node
	// under the pointer to the window around it, which is the walk click-to-focus already does — so a
	// press on a client's own surface names its own window, and a press inside an open menu names the
	// menu rather than the window behind it.
	if (window.IsNull() || FocusTargetFor(scene, m_Grab) != window)
	{
		return false;
	}

	return m_Drag.Begin(scene, window);
}

std::uint32_t SeatGlobal::NextSerial() const noexcept
{
	return m_Display != nullptr ? ::wl_display_next_serial(m_Display) : 0;
}
