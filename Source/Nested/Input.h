#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "Core/Clock.h"
#include "Core/Fd.h"
#include "Core/Input.h"
#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Nested/Host.h"
#include "Seam/Input.h"
#include "Wayland/Wayland.h"
#include "Wire/Message.h"

// Where a keystroke and a mouse come from when gyro is a client of another compositor.
//
// **gyro takes input the way every other application does, and that is the whole of the design.**
// The alternative is libinput's udev backend, which takes every device on the seat — so a nested gyro
// would be reading the host session's keyboard behind its back, every keystroke a person types
// whatever has focus, while the host is still moving its own pointer with the same mouse. Decision
// 173 has the argument; `Compositor/Compositor.cpp` is where the choice is made, and it is made by
// *which backend was built* rather than by a flag somebody has to remember.
//
// **A host window is a piece of glass with a pointer on it, so it is an absolute device.** The host
// says where the pointer is inside a surface, not how far it moved, and decision 167 already has the
// vocabulary for a device that states a position: a fraction of its own active area, bound to one
// output, landed on that output's grid by the composition root. What is different here is only that
// the binding is exact rather than inferred — the device *is* the window — so a device is minted per
// window and named for it, and the root's `GYRO_OUTPUT` rung matches by construction.
//
// **A keyboard is the connection's rather than a window's, because the host says so.** There is one
// `wl_keyboard` on the seat and its focus is a fact about the whole client, so unlike the pointer
// there is nothing to bind and nothing to name an output with — a keystroke means the same thing
// whichever window gyro opened is in front. What it does need is the thing an absolute device does
// not: a record of what is held down, so that focus leaving gyro's window releases it. A person who
// alt-tabs out while holding `Alt` never sends gyro the release, and a modifier stuck down inside a
// compositor is a machine that types nothing anybody meant.
//
// **The host's keymap is read and discarded, and gyro's own is what its clients get.** A key crosses
// as the kernel numbered it — `Seam/Input.h` forbids a layout in this path in as many words — so the
// layer that turns a keycode into a letter is `Protocol/Keymap.h` at the far end, exactly as it is on
// a real seat. The cost is that a person who selected Dvorak in the surrounding session types QWERTY
// inside a nested gyro unless `XKB_DEFAULT_LAYOUT` says otherwise, which Open.md carries.
//
// **The events are read on the frame thread and emitted on the dispatch thread**, which is the
// handoff decision 81 recorded as nested's price for one connection. Reading is
// `NestedHost::Drain`'s, because the socket carrying `wl_pointer.motion` is the socket carrying
// `wp_presentation_feedback` and one socket has one reader; emitting is this object's `Drain`, which
// the dispatch thread calls. In between is a fixed ring and an eventfd, which is decision 83's shape
// pointed the other way — the direction with no deadline on it, so a late handoff is a late click
// rather than a missed frame.
//
// **Nothing here allocates or logs.** The producer half runs inside the frame loop's step, where
// `Core/FrameSection.h` aborts on an allocation; a queue that overflows drops and counts, and the
// count is said out loud from the consumer side where saying it is legal.

namespace Nested
{
// How many events one host drain may leave for the dispatch thread.
//
// **Sized for a burst rather than for a rate.** A mouse reporting at 1 kHz against a 60 Hz host
// produces around seventeen motions per drain, and a drain that fell behind by ten refreshes is still
// inside this. What overflowing costs is a dropped event rather than a broken queue, and the events
// most likely to be dropped are motions, where the next one supersedes what was lost.
inline constexpr std::size_t NestedInputQueue = 512;

// The pointer is over no window gyro opened, which is where it starts and where it returns whenever a
// person moves the mouse onto the rest of their desktop. A sentinel rather than a count comparison,
// because a window can be tracked after the fact and *no focus* must not become *window zero*.
inline constexpr std::size_t NoFocus = MaxNestedOutputs;

// The slot the keyboard's device id is minted from, one past the windows. A keyboard is the
// connection's rather than a window's — the host has one focus for the whole client — so it needs an
// identity that no window can collide with and that binds to no output.
inline constexpr std::size_t KeyboardDevice = MaxNestedOutputs;

// How many keycodes the down-set covers. `KEY_MAX` is 767 and is stable kernel ABI, so the set is 96
// bytes and a host that sends something past it is out of contract rather than a case to grow for —
// which is checked, because the alternative is a stray write on the frame thread.
inline constexpr std::size_t MaxKeycode = 768;

// One thing the host said, in the vocabulary it will be emitted in. A flat record rather than a
// variant: it crosses a thread boundary in a fixed array, so it is trivially copyable by
// construction and the ring holds no indirection at all.
struct NestedInputEvent
{
	enum class Kind : std::uint8_t
	{
		// A window's pointer device exists. Queued rather than emitted where it is learned, so that the
		// arrival reaches the root through the same ordering every event after it does.
		Added,
		Position,
		Button,
		Scroll,
		Key,
	};

	Kind What = Kind::Added;

	// Which window, which is which device and which output — or `KeyboardDevice`, which is neither.
	std::uint32_t Window = 0;

	// `Position`: the fraction of that window's own surface.
	double NormalizedX = 0.0;
	double NormalizedY = 0.0;

	// `Button` and `Key`: evdev's numbering, which is what `wl_pointer.button` and `wl_keyboard.key`
	// both already carry.
	std::uint32_t Code = 0;
	bool Pressed = false;

	// `Scroll`.
	ScrollAxis Axis = ScrollAxis::Vertical;
	ScrollSource Source = ScrollSource::Wheel;
	double Distance = 0.0;
	double Clicks120 = 0.0;
	bool Stop = false;

	Instant When{};
};

// The host's seat, as gyro's own device set.
class NestedInput final : public IInput
{
public:
	explicit NestedInput(const IClock& clock) noexcept : m_Clock{ &clock } {}

	// Open the doorbell. Separate from the constructor because it can fail and a member cannot.
	[[nodiscard]] Result<void> Open();

	// Bind the host's seat, if it has one.
	//
	// **A host with no seat is an ordinary host**, not a refusal: gyro still opens its window and still
	// shows pictures, and the person is told once. That is the same stance `wp_linux_drm_syncobj_v1`
	// gets, and for a stronger reason — a compositor that will not start because there is no mouse is
	// useless on the machine where you most want to look at what it drew.
	void Bind(const Wayland::WlRegistry& registry, std::uint32_t name, std::uint32_t version);

	// Learn what surface window `index` is and how big it is, so that an enter can be attributed and a
	// position can be a fraction of something. Called once when the window takes its role and again on
	// every resize, both from `NestedHost` — which is why it takes the two facts rather than the output
	// they come from: an input path holding a presenter would be this module's own layering inversion.
	void Track(std::size_t index, Wire::ObjectId surface, PixelSize<DeviceSpace> size);

	// The name this window answers to as a connector, which is what binds its pointer to its output.
	//
	// **A host window has no connector and gyro gives it one anyway**, because the string is how
	// `Compositor/Binding.h`'s first rung works and this is the one case where the answer is not an
	// inference: the device and the output are the same object. Owned here rather than in the backend
	// so that the name the pointer carries and the name the output answers to cannot drift apart.
	[[nodiscard]] std::string_view Connector(std::size_t index) const noexcept;

	// Seam/EventSource.h's two verbs, both on the dispatch thread.
	[[nodiscard]] RawFd Descriptor() const noexcept override { return RawFd{ m_Wake.Get() }; }

	[[nodiscard]] Result<void> Drain() override;

	// Frame thread. Ring the doorbell if anything was queued since the last one, called once at the
	// end of a host drain rather than once per event — a mouse at 1 kHz would otherwise be a syscall
	// per report on the thread that owes a frame.
	void Notify() noexcept;

private:
	// The seat, which exists to answer one question: what does this host have on it.
	class Seat final : public Wayland::WlSeatListener
	{
	public:
		explicit Seat(NestedInput& input) noexcept : m_Input{ &input } {}

		void OnCapabilities(Wayland::WlSeatCapability capabilities) override;

		void OnName(std::string_view) override {}

	private:
		NestedInput* m_Input = nullptr;
	};

	// The pointer itself. Every handler here runs on the frame thread and does nothing but push.
	class Pointer final : public Wayland::WlPointerListener
	{
	public:
		explicit Pointer(NestedInput& input) noexcept : m_Input{ &input } {}

		void
		OnEnter(std::uint32_t serial, Wayland::WlSurface surface, Wire::Fixed surfaceX, Wire::Fixed surfaceY) override;

		void OnLeave(std::uint32_t serial, Wayland::WlSurface surface) override;

		void OnMotion(std::uint32_t time, Wire::Fixed surfaceX, Wire::Fixed surfaceY) override;

		void OnButton(
			std::uint32_t serial,
			std::uint32_t time,
			std::uint32_t button,
			Wayland::WlPointerButtonState state
		) override;

		void OnAxis(std::uint32_t time, Wayland::WlPointerAxis axis, Wire::Fixed value) override;

		// The group boundary, and gyro does not use it. `wl_pointer.frame` says which events were one
		// physical movement, and the party that regroups them for a client is `Protocol/Seat.h` against
		// its own drain — `Seam/Input.h` says so in as many words, and a boundary forwarded from here
		// would be the host's grouping rather than gyro's.
		void OnFrame() override {}

		void OnAxisSource(Wayland::WlPointerAxisSource axisSource) override;

		void OnAxisStop(std::uint32_t time, Wayland::WlPointerAxis axis) override;

		void OnAxisDiscrete(Wayland::WlPointerAxis, std::int32_t) override {}

		void OnAxisValue120(Wayland::WlPointerAxis axis, std::int32_t value120) override;

		void OnAxisRelativeDirection(Wayland::WlPointerAxis, Wayland::WlPointerAxisRelativeDirection) override {}

	private:
		NestedInput* m_Input = nullptr;
	};

	// The keyboard. Frame thread, like the pointer, and it holds the one piece of state a key path
	// needs that a pointer path does not: what is down.
	class Keyboard final : public Wayland::WlKeyboardListener
	{
	public:
		explicit Keyboard(NestedInput& input) noexcept : m_Input{ &input } {}

		// **The host's keymap is taken and dropped**, and the descriptor closes with the argument
		// because `Fd` owns it — which is the whole of the handling and is why there is no body. What a
		// key means is `Protocol/Keymap.h`'s at the far end of gyro's own seat, compiled from
		// `XKB_DEFAULT_*` exactly as it is on a machine with real devices; forwarding the host's would
		// mean carrying a layout through a path `Seam/Input.h` keeps keycodes in on purpose.
		void OnKeymap(Wayland::WlKeyboardKeymapFormat, Fd, std::uint32_t) override {}

		// **The keys already held are *not* replayed as presses.** `wl_keyboard`'s own text says a
		// client must not emulate presses from this list, and the reason is exactly gyro's case: a
		// synthetic press reaches a terminal inside the nested session as a character nobody typed. So
		// focus arriving starts from nothing held, and a modifier a person was holding when they
		// alt-tabbed in is invisible until they let go of it — which the host does send, because it put
		// the key in this list and owes the release.
		void OnEnter(std::uint32_t serial, Wayland::WlSurface surface, std::span<const std::byte> keys) override;

		void OnLeave(std::uint32_t serial, Wayland::WlSurface surface) override;

		void
		OnKey(std::uint32_t serial, std::uint32_t time, std::uint32_t key, Wayland::WlKeyboardKeyState state) override;

		// **Not forwarded, because gyro derives them itself.** The host states its own modifier state
		// against its own layout, and gyro's clients are told about gyro's — one `xkb_state` fed the
		// keycodes that arrive below, which is the same machinery a real seat runs. Taking the host's
		// numbers instead would be two layouts' idea of `Shift` reaching one client.
		void OnModifiers(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override {}

		// **Not forwarded, because repeat is the client's.** Under Wayland a compositor states a rate
		// and every client repeats for itself, so what a nested gyro's clients need is gyro's number
		// rather than the host's — and gyro is the compositor for them.
		void OnRepeatInfo(std::int32_t, std::int32_t) override {}

	private:
		NestedInput* m_Input = nullptr;
	};

	// One host window, as the input path sees it.
	struct Window
	{
		Wire::ObjectId Surface = Wire::ObjectId::None;

		// What a position is a fraction of. The surface's own size, which is the buffer's while gyro
		// commits at scale 1 with no viewport on the window — the day either changes, this is the number
		// that has to change with it.
		PixelSize<DeviceSpace> Size{};

		// `nested-0`, and the pointer device's name beside it. Both are held rather than formatted per
		// emit, because `InputDevice` borrows its strings for the duration of the call.
		std::string Connector;
		std::string Name;

		// Whether the root has been told this device exists. One `Added` per window, emitted the first
		// time the pointer enters it — before that there is nothing to bind and no evidence the host will
		// ever put a pointer there.
		bool Announced = false;
	};

	// Frame thread. Push one event, or count a drop.
	void Push(const NestedInputEvent& event) noexcept;

	// Frame thread. Push one key transition and remember it, or forget it. `Instant` is the caller's so
	// that a whole release sweep carries the instant the focus left rather than a different one each.
	void Transition(std::uint32_t code, bool pressed, Instant when) noexcept;

	// Frame thread. Release everything still held. What a `wl_keyboard.leave` means in as many words —
	// the protocol says the event resets the logical state — and what a seat losing its keyboard means
	// as well.
	void ReleaseHeld(Instant when) noexcept;

	[[nodiscard]] std::size_t Find(Wire::ObjectId surface) const noexcept;

	const IClock* m_Clock = nullptr;

	Fd m_Wake;

	Seat m_Seat{ *this };
	Pointer m_Pointer{ *this };
	Keyboard m_Keyboard{ *this };

	Wayland::WlSeat m_SeatObject;
	Wayland::WlPointer m_PointerObject;
	Wayland::WlKeyboard m_KeyboardObject;

	std::array<Window, MaxNestedOutputs> m_Windows{};
	std::size_t m_Count = 0;

	// Frame thread. Which window the pointer is in, or `NoFocus`.
	std::size_t m_Focus = NoFocus;

	// Frame thread. The scroll source the host last named, which `wl_pointer.axis_source` states once
	// per group and `Core/Input.h` carries on every increment.
	ScrollSource m_Source = ScrollSource::Wheel;

	// Frame thread. The high-resolution step the host sent for this group, spent by the `axis` that
	// follows it — `value120` arrives *before* the distance it describes.
	double m_Pending120 = 0.0;

	// The ring, single producer and single consumer, which is what lets both indices be plain atomics
	// with no lock between them.
	std::array<NestedInputEvent, NestedInputQueue> m_Queue{};
	std::atomic<std::uint32_t> m_Head{ 0 };
	std::atomic<std::uint32_t> m_Tail{ 0 };

	std::atomic<std::uint64_t> m_Dropped{ 0 };
	std::uint64_t m_Reported = 0;

	// Frame thread. What the host has told gyro is down, so that focus leaving can put it back up.
	//
	// **Kept here rather than left to whatever is downstream**, because the parties that hold key state
	// — the chord and the seat's `xkb_state` — are each holding it for their own purpose and neither is
	// in a position to be told *the keyboard went away, assume nothing*. What they can be told is a
	// release per key, which is a fact they already know how to take.
	std::array<std::uint64_t, MaxKeycode / 64> m_Down{};

	// Frame thread. Whether the root has been told the keyboard exists, which is once per bind rather
	// than once per focus.
	bool m_KeyboardAnnounced = false;

	// Frame thread. Whether anything has been queued since the doorbell was last rung.
	bool m_Queued = false;
};
} // namespace Nested
