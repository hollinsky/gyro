#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "Core/Handle.h"
#include "Core/Input.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Geometry/Space.h"
#include "Protocol/Context.h"
#include "Protocol/Keymap.h"
#include "Scene/Store.h"
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
// **Advertised at version 5**, which is `wl_pointer.frame` and the three axis events beside it —
// `axis_source`, `axis_stop`, `axis_discrete`. The number goes up in the commit that builds the thing
// it names, which is `wl_compositor`'s rule, and what it names here is the whole of what libinput
// gives gyro about a scroll: which device did it, when the fingers left, and how many detents. Not 8,
// which is `axis_value120` and the high-resolution wheel — `Core/Input.h` carries the 120ths already
// and the events for them arrive with their own bump rather than with a version claiming three
// intermediate contracts nothing here implements.
//
// **`wl_pointer.set_cursor` is accepted and ignored, which is a stated gap rather than a silence.**
// Decision 152 has gyro drawing its own glyph, and what a client sends here is a *surface* rather than
// a name — so there is nothing in the request to map onto a shape gyro draws, and honouring it means a
// client buffer under the pointer at input rate. What that costs today is a text field with an arrow
// over it instead of an I-beam. The answer is `wp_cursor_shape_v1`, which names shapes instead of
// handing over pixels, and it is its own commit.
//
// **Key repeat is the client's, and `repeat_info` is how gyro says so.** A compositor that repeated
// keys itself would hold a timer per held key and wake the dispatch thread on it, to produce events a
// toolkit already produces from the two numbers in that event. The numbers are the ones every desktop
// ships and are not configurable yet, for the same reason the layout is not: there is nowhere to keep
// a preference until there is a session.
inline constexpr std::uint32_t SeatVersion = 5;

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

// One client's `wl_pointer`.
//
// **A list rather than one per client, for `ClientKeyboard`'s reason**: a toolkit binds `wl_seat` once
// per display connection, and a process with two connections has two of these.
class ClientPointer final : public Wayland::Server::WlPointerIgnoring
{
public:
	explicit ClientPointer(SeatGlobal& seat) noexcept : m_Seat{ &seat } {}

	// Deregisters before it dies, because the seat holds a borrowed pointer and the next motion would
	// find it.
	void OnGone() override;

	[[nodiscard]] bool BelongsTo(wl_client* client) const noexcept;

	// Whether this object can be told about a `frame` and the axis events beside it. A client is
	// entitled to bind `wl_seat` below what gyro advertises, and an event it has no opcode for is a
	// message it cannot parse rather than one it ignores.
	[[nodiscard]] bool Grouped() const noexcept { return Object().IsValid() && Object().Version() >= 5; }

private:
	SeatGlobal* m_Seat = nullptr;
};

// One physical event, held until the world is in hand.
//
// **The devices are drained outside `Advance` and a button has to be routed inside one**, because
// routing means hit-testing and hit-testing means the store — which `ISceneAuthor` hands in as an
// argument precisely so that nobody keeps a copy of it ([Context.h](Context.h)). So what arrives from
// the root is parked here in the order it arrived and delivered when the step runs, which is the same
// wakeup: the descriptor that woke the dispatch thread is the one these came from.
//
// **A tagged pair rather than a variant, and it costs sixteen bytes of nothing.** The queue is the
// events of one wakeup — one, on almost every iteration — and what a discriminated union would buy is
// less than what a reader loses working out which arm is live.
struct SeatPointerEvent
{
	enum class Kind : std::uint8_t
	{
		Button,
		Scroll,
	};

	Kind What = Kind::Button;

	PointerButton Pressed{};
	PointerScroll Scrolled{};
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

	// A pointer came or went. Borrowed pointers, like the keyboards, and each one deregisters itself.
	void Add(ClientPointer& pointer);
	void Remove(ClientPointer& pointer) noexcept;

	// The devices' three verbs, all of them queueing rather than sending. `Moved` takes only the instant
	// because the position itself is `Scene/Pointer.h`'s and there is exactly one copy of it (152) —
	// what the seat cannot get from the store is *when* the hand moved, which is the number a client
	// estimates a velocity from and `Core/Input.h`'s rule about not narrowing at ingest.
	void Moved(Instant when) noexcept { m_MovedAt = when; }
	void Button(const PointerButton& event);
	void Scroll(const PointerScroll& event);

	// Route everything the devices said into the world it happened in. Called from `Advance` beside
	// `SyncFocus`, and after it for the same reason: the commit that mapped a window is what makes it
	// something the pointer can be on.
	//
	// **The pointer is at one place for the whole iteration**, and that is a real limit rather than an
	// implementation detail. A drain carrying a move, a press and another move delivers the press at the
	// position the second move reached — so a press and a release that straddle two different windows
	// inside one wakeup both land on the second. The iteration is one input event long whenever the
	// dispatch thread is keeping up, which is the case this is chosen for; when it is not, everything in
	// that wakeup is late together and the alternative — replaying positions the store no longer holds —
	// would mean a second copy of the pointer, which is the thing decision 152 refuses.
	void SyncPointer(SceneStore& scene, Instant now);

private:
	// Everything a keyboard needs to know to address the focused client, or nothing where focus is on a
	// window no client is behind — which is every window gyro authors for itself.
	[[nodiscard]] Wayland::Server::WlSurface FocusedSurface() const noexcept;

	void SendEnter();
	void SendLeave();

	// The surface behind an entity, which is what an `enter` names and a `leave` is owed to. Null for a
	// window gyro authored for itself, which is every node the splash and the recovery console draw.
	[[nodiscard]] Wayland::Server::WlSurface SurfaceFor(EntityId id) const noexcept;

	// Where the pointer is and what it is on, resolved against the grab. The grab wins whatever is in
	// front of it, which is the protocol's implicit grab: a button held is a surface that keeps every
	// event until it comes up.
	struct PointerTarget
	{
		EntityId Node{};
		Point<SurfaceSpace> Local{};
	};

	[[nodiscard]] PointerTarget Resolve(const SceneStore& scene) const;

	// Move the clients' idea of where the pointer is onto `target`, sending the leave, the enter and the
	// motion that difference is worth — and nothing where it is worth nothing.
	void Refocus(const PointerTarget& target, Instant now);

	// Hand the queue to whoever the pointer is on, as one `frame` group: a press and the scroll that
	// came with it are one physical event and a toolkit is entitled to see them that way.
	void Deliver();

	void SendPointerEnter(Point<SurfaceSpace> local);
	void SendPointerLeave();

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

	// Borrowed, and each one deregisters itself, exactly as the keyboards are.
	std::vector<ClientPointer*> m_Pointers;

	// **What the clients have been told the pointer is on**, which is the same distinction `m_Focused`
	// draws: the world's answer can be a node no client is behind, and this is the entity a `leave` is
	// still owed to.
	EntityId m_Pointed{};

	// Where on it, so that a motion that moved nothing sends nothing. A window a person is not touching
	// republishes constantly — a client's own animation is a commit — and a `motion` per publication is
	// traffic that says the pointer moved when it did not.
	Point<SurfaceSpace> m_Local{};

	// The implicit grab: the entity every event goes to until the last button comes up, whatever the
	// pointer is over. Null when nothing is held.
	//
	// **It is the protocol's rule rather than a policy of gyro's**, and the behaviour it buys is the one
	// every drag depends on: a person who presses on a scrollbar and slides off the window is still
	// scrolling. Without it the client gets a leave mid-drag and the thumb sticks where the pointer left
	// the frame.
	EntityId m_Grab{};

	// How many buttons are down, which is what opens and closes the grab. A count rather than a flag
	// because a person may press a second button without releasing the first, and a grab that ended on
	// the first release would drop the rest of the gesture.
	std::uint32_t m_ButtonsDown = 0;

	// When the hand last moved, or nothing where nothing moved this iteration — in which case a motion
	// forced by the *window* moving under a still pointer is stamped with dispatch's own now, since
	// there is no device instant to carry and the alternative is a timestamp from an earlier wakeup.
	std::optional<Instant> m_MovedAt;

	// What the devices said, in the order they said it. Cleared rather than freed after every delivery,
	// so the steady state allocates nothing.
	std::vector<SeatPointerEvent> m_Queue;
};
