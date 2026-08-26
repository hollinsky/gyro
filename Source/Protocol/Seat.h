#pragma once

#include <cstdint>
#include <vector>

#include "Core/Handle.h"
#include "Core/Input.h"
#include "Core/Result.h"
#include "Protocol/Context.h"
#include "Protocol/Keymap.h"
#include "Wayland/Server/Wayland.h"

struct wl_display;

// `wl_seat`: the keyboard, and who it is talking to.
//
// **Focus is read from the world rather than kept here.** [Scene/Focus.h](../Scene/Focus.h) holds who
// the keyboard is on, because a focused window is a fact about the world — the focus ring and the
// motion that goes with it are authored there, and a shell will declare the model that writes it. What
// this object owns is the *protocol* consequence of that fact: which client has been told it has focus,
// and therefore which one is owed a `leave` when the fact changes.
//
// **The change is noticed by comparing rather than by being told**, once per `Advance`, which is the
// call that already hands this module the world. A signal out of `Scene` would buy nothing: the
// keystroke that moved focus and the step that notices are the same wakeup of the same thread, so the
// latency is identical — and it would have to fire from inside the store's own subtree walk, where an
// observer that authored anything would be re-entering a transaction that is still open.
//
// **Advertised at version 4**, which is `wl_seat.name` and `wl_keyboard.repeat_info`. Every event above
// it belongs to a capability gyro does not have: version 5 is `wl_pointer.axis_discrete`, 6 and 7 are
// its successors and `wl_pointer.axis_relative_direction`. `wl_compositor`'s rule again — the number
// promises the events gyro sends, so it goes up in the commit that builds the thing it names, which
// here is the pointer.
//
// **Key repeat is the client's, and `repeat_info` is how gyro says so.** A compositor that repeated
// keys itself would hold a timer per held key and wake the dispatch thread on it, to produce events a
// toolkit already produces from the two numbers in that event. The numbers are the ones every desktop
// ships and are not configurable yet, for the same reason the layout is not: there is nowhere to keep
// a preference until there is a session.
inline constexpr std::uint32_t SeatVersion = 4;

class SeatGlobal;

// One client's `wl_keyboard`.
//
// **A client may hold several**, which is not a pathology: a toolkit binds `wl_seat` once per display
// connection, and a process with two connections — a GTK application and its own portal helper live in
// the same binary often enough — has two. So the seat keeps a list rather than one per client, and an
// event goes to every keyboard whose client is the focused one.
class ClientKeyboard final : public Wayland::Server::WlKeyboardIgnoring
{
public:
	explicit ClientKeyboard(SeatGlobal& seat) noexcept : m_Seat{ &seat } {}

	// Deregisters before it dies, because the seat holds a borrowed pointer and the next key would find
	// it. `OnGone` runs when the client releases the keyboard and when the client goes away.
	void OnGone() override;

	// **The keymap is owed the moment the object exists**, which is what `OnBound` is for: a client
	// binds a keyboard and expects the layout to be on its way without asking. `repeat_info` goes with
	// it for the same reason.
	void OnBound() override;

	// Whether this keyboard belongs to the client behind `surface`, which is how focus selects the
	// keyboards an event goes to. Two clients never see each other's keystrokes, and libwayland refuses
	// to marshal an event naming another client's surface in any case.
	[[nodiscard]] bool BelongsTo(wl_client* client) const noexcept;

private:
	SeatGlobal* m_Seat = nullptr;
};

// One client's `wl_seat`.
class ClientSeat final : public Wayland::Server::WlSeatIgnoring
{
public:
	explicit ClientSeat(SeatGlobal& seat) noexcept : m_Seat{ &seat } {}

	void OnGone() override { delete this; }

	// `capabilities` and `name`, in that order and both at bind: a client that has not been told there
	// is a keyboard may not ask for one, and the protocol says the events come without being requested.
	void OnBound() override;

	Wayland::Server::WlKeyboardHandler* OnGetKeyboard() override;

	// **Refused rather than answered, because gyro has never advertised either capability.** The
	// protocol names this exactly: asking for a capability the seat has never had is
	// `missing_capability`, and it is the client's error rather than gyro's. Answering with a working
	// object instead would be a pointer a toolkit waits on forever for an `enter` that cannot come.
	//
	// **The error goes out and an inert object comes back**, which reads like a contradiction and is
	// not: the client is being ended and never reaches a request on this object, and returning null
	// instead would have the bindings end it a second time with `no_memory` — so the last thing in the
	// client's log would name an allocation failure that did not happen.
	Wayland::Server::WlPointerHandler* OnGetPointer() override;

	Wayland::Server::WlTouchHandler* OnGetTouch() override;

private:
	SeatGlobal* m_Seat = nullptr;
};

// The global, owned by the host and outliving every client that binds it.
class SeatGlobal final : public Wayland::Server::WlSeatBinding
{
public:
	explicit SeatGlobal(HostContext& context) noexcept : m_Context{ &context } {}

	// Compile the layout. Called before the global goes up, so that a keyboard cannot be bound before
	// there is a keymap to hand it — the alternative is a first client that gets an invalid descriptor
	// and every later one that does not.
	[[nodiscard]] Result<void> Open(wl_display& display);

	[[nodiscard]] const Keymap& Layout() const noexcept { return m_Keymap; }

	Wayland::Server::WlSeatHandler* OnBind(wl_client& client, std::uint32_t version) override;

	// A keyboard came or went. Borrowed pointers, and the keyboard deregisters itself from `OnGone`.
	void Add(ClientKeyboard& keyboard);
	void Remove(ClientKeyboard& keyboard) noexcept;

	// One key, already past the compositor's own chord.
	//
	// **`consumed` says gyro took the key, and the modifier state is folded either way.** What is held
	// down is a property of a person's hands rather than of who is listening, so a `Ctrl` that a chord
	// swallowed still has to reach the state machine — otherwise the next client to take focus is told
	// a modifier is up while a finger is on it.
	void Key(const KeyEvent& event, bool consumed);

	// Bring the clients' idea of focus up to date with the world's, and send nothing where it has not
	// changed. Called from `Advance`, where the store is in hand.
	void SyncFocus(EntityId focused);

private:
	// Everything a keyboard needs to know to address the focused client, or nothing where focus is on a
	// window no client is behind — which is every window gyro authors for itself.
	[[nodiscard]] Wayland::Server::WlSurface FocusedSurface() const noexcept;

	void SendEnter();
	void SendLeave();

	// The next serial, from the display: one counter for the whole connection set, which is what a
	// client compares against when it validates a request of its own against an event of gyro's.
	[[nodiscard]] std::uint32_t NextSerial() const noexcept;

	HostContext* m_Context = nullptr;
	wl_display* m_Display = nullptr;

	Keymap m_Keymap;

	// Borrowed, and each one deregisters itself. A vector because the length is the connections on the
	// machine and it is walked once per keystroke.
	std::vector<ClientKeyboard*> m_Keyboards;

	// **What the clients have been told, which is not what the world says.** The world's focus can move
	// to a window with no client behind it, or to one whose surface has already gone; this is the entity
	// a `leave` is still owed to, and it is cleared when that event goes out.
	EntityId m_Focused{};

	// The keys held down, in the kernel's numbering, for the array a `wl_keyboard.enter` carries. A
	// client taking focus with a key already down has to be told, or it will never see the release.
	std::vector<std::uint32_t> m_Held;

	// The last modifiers sent, so that a keystroke that changes nothing about them sends nothing.
	Keymap::Modifiers m_Modifiers;
};
