#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "Core/Handle.h"
#include "Core/Input.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Geometry/Space.h"
#include "Protocol/Context.h"
#include "Protocol/Drag.h"
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
// **The touch capability is advertised whether or not a touchscreen is plugged in**, which is the
// choice already made for the keyboard and the pointer one line above it: gyro says what the seat is
// for rather than what is currently attached, so a machine whose panel is unplugged mid-session does
// not have every toolkit tear down its `wl_touch` and rebuild it on the way back. What a client is
// entitled to conclude from a capability is that it may ask for the object, and the object is honest —
// it says nothing when nothing is happening, which is also what it does on a machine with a touchscreen
// nobody is using.
//
// **A sequence is bound to what it went down on and there is no `enter`.** The pointer is somewhere
// even when nobody is touching anything, so it needs an enter and a leave to say where; a finger has no
// position between sequences, and the protocol reflects that by routing a whole sequence to the surface
// the `down` landed on. So the implicit grab a pointer opens on a button is what touch does *always*,
// and the table below is one entry per finger rather than one entity for the seat.
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

// What became of a request to take the pointer, and why where it was refused.
//
// **A refusal is silent at both ends otherwise, which is the same argument `Server::Admit` makes about
// a client of the wrong uid.** `xdg_toplevel.move` and `xdg_toplevel.resize` have no reply and no
// error the protocol would have gyro send — a client learns what it got from what happens next, and
// what happens next is nothing. So a person drags a corner, the window does not move, the application
// is behaving correctly, and there is no record anywhere of the request having been made. This is that
// record, and the reason it is an enumeration rather than a bool is that *which* check refused is the
// whole of the diagnosis.
enum class GestureRefusal : std::uint8_t
{
	None,        // it started
	Unmapped,    // there is no window in the world to act on, or no dispatch around the request
	NoSeat,      // the id the client named is not a wl_seat gyro made
	NoEdges,     // xdg_toplevel.resize with `none`, which is legal and names no direction
	Busy,        // a gesture is already running, and there is one pointer
	NoButton,    // nothing is held, so there is no grab to ride
	WrongSerial, // the serial is not the one gyro sent with the press that opened the grab
	OtherWindow, // that press landed somewhere else
	Gone,        // the window went away between the press and the request
};

// One clause, for the log line. Present tense and about the *world* rather than about the code, so
// that the line reads as a description of what a person did rather than as an internal state name.
[[nodiscard]] std::string_view Describe(GestureRefusal refusal) noexcept;

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

	Wayland::Server::WlPointerHandler* OnGetPointer() override;

	// Answered like the two above it, and there is nothing left in this interface gyro refuses by name.
	Wayland::Server::WlTouchHandler* OnGetTouch() override;

	// The seat behind this object, which is how a request that *names* a seat reaches the one thing on
	// the machine that knows what the pointer is doing. `xdg_toplevel.move` is the caller: the protocol
	// makes the seat an argument precisely because a grab belongs to one, so the shell resolves the id
	// the client sent rather than reaching for a seat it assumed.
	[[nodiscard]] SeatGlobal& Seat() const noexcept { return *m_Seat; }

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

// One client's `wl_touch`.
//
// **A list rather than one per client, for `ClientKeyboard`'s reason**, and with no version gate on
// anything it sends: `down`, `up`, `motion`, `frame` and `cancel` are all version 1, so unlike the
// pointer there is no event here a client can be entitled to and unable to parse. The two that are
// not — `shape` and `orientation`, at version 6 — carry a contact ellipse libinput exports for a
// tablet tool and for nothing else, which [Core/Input.h](../Core/Input.h) records as the reason
// `TouchEvent` has no such field to send.
//
// **Nothing is owed at bind, which is where this differs from both objects above it.** A keyboard
// bound while its client has focus is told so at once or it is deaf forever, and a pointer is resolved
// from scratch on the next iteration. A finger already down when the object is created is neither: the
// sequence began with a `down` this object could not have received, and inventing one would hand a
// toolkit a contact at a position it never travelled to. The next finger is the first this object
// hears about, and that is one sequence long rather than forever.
class ClientTouch final : public Wayland::Server::WlTouchIgnoring
{
public:
	explicit ClientTouch(SeatGlobal& seat) noexcept : m_Seat{ &seat } {}

	// Deregisters before it dies, because the seat holds a borrowed pointer and the next contact would
	// find it.
	void OnGone() override;

	[[nodiscard]] bool BelongsTo(wl_client* client) const noexcept;

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

	// Whether this is the press that opened the implicit grab, marked when the grab is settled and read
	// when the event goes out. **It is here rather than recomputed at delivery because the serial only
	// exists at delivery**: the grab is decided against the queue before anything is sent, and the number
	// a client will quote back at gyro is minted on the way out. One bit on an event that already exists
	// is what joins the two.
	bool OpensGrab = false;
};

// One contact, held until the world is in hand.
//
// **The global point travels beside the event rather than inside it**, which is decision 167's rule
// arriving at its one consumer: `TouchEvent` carries a fraction of the device's own glass and no
// coordinate space at all, and the composition root is the party that knows which output that glass is
// in front of. So what is queued is what the device said and where on the screen the root resolved it
// to, as two things — and an event from a device bound to no output never arrives here, rather than
// arriving with a position meaning *nowhere*.
//
// **Queued for `SeatPointerEvent`'s reason and routed in the same call**: hit-testing needs the store,
// and the store is only in hand inside `Advance`.
struct SeatTouchEvent
{
	TouchEvent Contact{};

	// Where the contact is, in the world. Meaningless on an up and a cancel, which report no position —
	// the point's own last position is what those are delivered against, and it is not needed: an `up`
	// carries no coordinate on the wire either.
	Point<GlobalSpace> At{};
};

// One finger, for as long as it is down.
//
// **The node is resolved once, at the `down`, and never asked again.** That is the protocol's rule
// rather than a shortcut — a touch sequence has no `enter` and no `leave`, so there is nowhere to say
// *the finger is over something else now* — and it is what makes dragging off the edge of a window
// work for a finger the way an implicit grab makes it work for a button.
struct SeatTouchPoint
{
	// The pair that identifies a contact: libinput's slot is unique within its device and meaningless
	// without it, which is what `Core/Input.h` mints an `InputDeviceId` generationally for. Two
	// touchscreens both report a point 0.
	InputDeviceId Device{};
	std::int32_t Slot = 0;

	// What the client calls this finger, which is *not* the slot. The wire's id has to be unique among
	// the points currently down on the seat, and two devices' slots collide by construction — so the
	// seat mints its own, smallest free first, and hands it back when the finger lifts. The protocol
	// says an id may be reused after an up, which is exactly what that is.
	std::int32_t Wire = 0;

	// What the `down` landed on, or null where it landed on gyro's own floor or on nothing. A point
	// with no node is still tracked: its motion and its up have to be recognized and dropped, and it
	// still counts as a finger being down.
	EntityId Node{};
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

	// Route everything the devices said into the world it happened in. Called from `Advance` after the
	// dispatch — the commit that mapped a window is what makes it something the pointer can be on — and
	// **before `SyncFocus`, because a press routed in here can move focus** (162). The other order sends
	// the `wl_keyboard.enter` an iteration late, and what is in that gap is a keystroke: keys are
	// delivered as the devices are drained, ahead of the step, so the first thing typed after a click
	// would go to the window the person had just clicked away from.
	//
	// **The pointer is at one place for the whole iteration**, and that is a real limit rather than an
	// implementation detail. A drain carrying a move, a press and another move delivers the press at the
	// position the second move reached — so a press and a release that straddle two different windows
	// inside one wakeup both land on the second. The iteration is one input event long whenever the
	// dispatch thread is keeping up, which is the case this is chosen for; when it is not, everything in
	// that wakeup is late together and the alternative — replaying positions the store no longer holds —
	// would mean a second copy of the pointer, which is the thing decision 152 refuses.
	void SyncPointer(SceneStore& scene, Instant now);

	// A touch object came or went, exactly as the pointers do.
	void Add(ClientTouch& touch);
	void Remove(ClientTouch& touch) noexcept;

	// One contact, at a place on the screen the composition root resolved (167). Queues rather than
	// sends, for the buttons' reason.
	void Touch(const TouchEvent& event, Point<GlobalSpace> at);

	// Route the contacts into the world they happened in, and close the group. Called from `Advance`
	// beside `SyncPointer` and before `SyncFocus`, because a finger going down moves focus the way a
	// press does.
	//
	// **It takes no `now`, and the pointer's taking one is the difference worth naming.** A pointer can
	// be moved by the *window* sliding under a hand that did not move, which is a motion with no device
	// instant behind it; a contact only ever exists because a finger did something, so every event
	// routed here carries the instant libinput stamped it with.
	void SyncTouch(SceneStore& scene);

	// Every sequence on `device` is over without an ending, and whoever was tracking one has to be told.
	//
	// **Two callers, one verb.** A touchscreen unplugged with a finger on it produces no up and no
	// cancel ([Seam/Input.h](../Seam/Input.h) argues `IInput::Removed` into existence for exactly this),
	// and a device whose output has gone away is the same fact arriving from the other side — decision
	// 167 leaves that one to the seat, and this is where it lands. Neither can be discovered by waiting:
	// the events simply stop.
	void Forget(InputDeviceId device);

	// Start moving `window` with the pointer, per decision 51: **the client asks once and gyro runs the
	// gesture.** Called from `xdg_toplevel.move`, and the answer is whether the drag took.
	//
	// **Three things have to be true, and each of them is a way a client could otherwise take the
	// pointer without a person having touched it.** A button must be down; the serial must be the one
	// gyro sent with the press that put it down, which is the protocol's own defence and the ledger
	// [Popup.h](Popup.h)'s grab has no way to check against; and the window the client named must be the
	// window that press landed in, so that an application cannot ride somebody else's click.
	//
	// **What is deliberately not checked is whether the client is allowed to move its own window**, which
	// is a shell's constraint to declare and there is no shell (51). See [Drag.h](Drag.h).
	GestureRefusal BeginMove(SceneStore& scene, EntityId window, std::uint32_t serial);

	// The same for `xdg_toplevel.resize`, checked against the same three things and refusing one more:
	// edges naming nothing, which is a gesture with no direction to run in.
	GestureRefusal BeginResize(SceneStore& scene, EntityId window, std::uint32_t serial, ResizeEdges edges);

private:
	// Whether a request quoting `serial` is entitled to take the pointer for `window`, and which check
	// says otherwise. The three `BeginMove` describes, shared because a resize asks exactly the same
	// questions.
	[[nodiscard]] GestureRefusal MayGrab(const SceneStore& scene, EntityId window, std::uint32_t serial) const;

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

	// The four phases, each with the store in hand. Separate functions rather than a switch body because
	// a `down` is the only one that resolves anything and the other three are lookups against what it
	// decided.
	void TouchBegan(SceneStore& scene, const SeatTouchEvent& queued);
	void TouchMoved(const SceneStore& scene, const SeatTouchEvent& queued);
	void TouchEnded(const SeatTouchEvent& queued);

	[[nodiscard]] SeatTouchPoint* FindPoint(InputDeviceId device, std::int32_t slot) noexcept;

	// The smallest id no finger currently down is using. Linear in the fingers on the glass, which is
	// ten on the most extravagant panel anybody makes.
	[[nodiscard]] std::int32_t MintPointId() const noexcept;

	// Everything this client was tracking is over, and it is told once.
	//
	// **The wire's cancel is per client rather than per point, and that coarseness is the protocol's**:
	// `wl_touch.cancel` applies to every contact currently active on that client's surfaces. So a device
	// that cancels one finger cancels every finger that client had, including ones from another
	// touchscreen — which is worth stating out loud because it is a real if rare loss, and the
	// alternative is inventing an `up` for a finger that is still down.
	void CancelClient(wl_client* client);

	// This client heard something and is owed a `frame`, recorded rather than sent so that a `down` and
	// the two motions behind it close as one group.
	void Framed(wl_client* client);

	void SendTouchFrames();

	// Which client is behind an entity, or null for a node gyro authored for itself.
	[[nodiscard]] wl_client* ClientOf(EntityId id) const noexcept;

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

	// The serial gyro sent with the press that opened the grab, or nothing where no press has gone out
	// under it — the pointer was over gyro's own background, or over nothing. **It is one entry rather
	// than a history, and that is enough for the only question asked of it**: a request naming a grab
	// names the live one, so a serial that is not this number is not a grab this client is in.
	std::optional<std::uint32_t> m_GrabSerial;

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

	// Borrowed, and each one deregisters itself, exactly as the pointers are.
	std::vector<ClientTouch*> m_Touches;

	// Every finger currently down, whoever it belongs to. **One table for the seat rather than one per
	// client**, because the wire's ids have to be unique across it and because the question asked most
	// often — is this the first finger down — is about the machine rather than about a client.
	std::vector<SeatTouchPoint> m_Points;

	// What the touchscreens said, in the order they said it. `m_Queue`'s twin and cleared the same way.
	std::vector<SeatTouchEvent> m_TouchQueue;

	// The clients owed a `frame` at the end of this iteration. A vector for `m_Keyboards`' reason: it is
	// the clients one wakeup's contacts landed on, which is one on every iteration that is not a person
	// dragging two windows at once with two hands.
	std::vector<wl_client*> m_Framed;
};
