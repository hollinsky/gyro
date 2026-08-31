#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
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
	};

	Kind What = Kind::Added;

	// Which window, which is which device and which output.
	std::uint32_t Window = 0;

	// `Position`: the fraction of that window's own surface.
	double NormalizedX = 0.0;
	double NormalizedY = 0.0;

	// `Button`: evdev's numbering, which is what `wl_pointer.button` already carries.
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
	// The seat, which exists to answer one question: does this host have a pointer.
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

	[[nodiscard]] std::size_t Find(Wire::ObjectId surface) const noexcept;

	const IClock* m_Clock = nullptr;

	Fd m_Wake;

	Seat m_Seat{ *this };
	Pointer m_Pointer{ *this };

	Wayland::WlSeat m_SeatObject;
	Wayland::WlPointer m_PointerObject;

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

	// Frame thread. Whether anything has been queued since the doorbell was last rung.
	bool m_Queued = false;
};
} // namespace Nested
