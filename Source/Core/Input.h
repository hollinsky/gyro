#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "Core/Handle.h"
#include "Core/Time.h"

// What a device produced, as everything downstream of one sees it.
//
// **It is here rather than at the waist because two modules that may not name `Seam` both need it.**
// `Seam/Input.h` declares the interface that emits one and `Protocol` is what turns it into a
// `wl_keyboard.key` — and decision 87 forbids `Protocol` the waist, for the same reason it moved
// `Core/Buffer.h` down: a type crossing between two parties is not the same thing as the interface
// between them, and putting it at the waist is what drags a module across a layer it has no business
// naming. `Seam/Input.h` includes this and carries the argument for the shape.
//
// **The vocabulary is the device's, and every projection out of it loses something.** A `wl_pointer`
// has no tilt, no proximity and no idea which of two styluses is in the hand; a `wl_touch` cannot say
// that a sequence was one of five. The rule this header exists to state is that *ingest keeps what
// the device said and `Protocol` is where it is thrown away*. The inverse — narrowing at the device
// because the protocol in front of it today is narrow — cannot be undone later without reopening the
// path a keystroke takes, and it forecloses the things gyro would use the extra fields for itself: a
// gesture recognizer, an ink path, a stylus that hovers over a control and lights it up without
// touching it.
//
// **Where the device says nothing, this says nothing, and `TouchEvent` is where that bites.** The
// rule cuts both ways: a field nothing can fill is a claim about a capability gyro does not have.
//
// **A code is the kernel's, a coordinate is nobody's.** Keys and buttons are reported in evdev's
// numbering for `KeyEvent`'s reason below. Positions are *not* reported in a coordinate space, and
// cannot be: an absolute device states where a contact is on its own surface, and turning that into a
// place on a screen is a question about outputs that only the world can answer. `Geometry` is above
// this header in any case — it depends on `Core` — so a `Point<GlobalSpace>` here would be a layering
// inversion as well as a lie. What crosses is the device's own fraction, and `Scene/Pointer.h` is
// where it lands on a screen.

struct InputDeviceTag
{
	static constexpr std::string_view Name = "InputDevice";
};

// Which device an event came from, generational for decision 15's reason rather than a weaker one: a
// touchscreen is unplugged and the next device takes the slot, and a touch sequence still keyed to the
// old one would deliver a finger the person lifted before the cable came out. It also carries the only
// scope a touch point or a tablet tool has — a slot number is unique within its device and nowhere
// else, so two touchscreens both report a point 0 and the pair is what separates them.
using InputDeviceId = Handle<InputDeviceTag>;

// How big a device's active surface is, in millimetres of real glass.
//
// It is here and not in `Geometry` because it is not a coordinate: nothing is measured against it and
// nothing is transformed by it. It is a physical fact about a slab, and the one thing it is for is
// recognizing which panel that slab is bonded to — decision 167's third rung, where a digitizer and
// the screen behind it agree because they are the same piece of glass.
struct InputDeviceSize
{
	double WidthMm = 0.0;
	double HeightMm = 0.0;

	friend constexpr bool operator==(const InputDeviceSize&, const InputDeviceSize&) noexcept = default;
};

// A device arrived, and what the device itself can say about what it is.
//
// **This is the half `IInput` was missing, and it exists for one caller and one question**: an
// absolute device reports a fraction of its own glass, and turning that into a place on a screen
// needs to know which screen. Neither half of the process can answer it — `Input` may not name an
// output and `Scene` may not name a libinput device — so the record crosses to the composition root,
// which is the only party that sees both. Decision 167 has the argument and the three rungs the root
// resolves it with.
//
// **What crosses is the device's vocabulary and nothing else.** An identity, a name for the log, the
// millimetres where the device has them, and the property where somebody set one. No coordinate
// space, for the reason at the top of this header — a device record that carried one would be making
// the claim the whole path exists to avoid.
//
// **The strings are borrowed for the duration of the emit**, because they are the device set's own
// and it is holding them open across the call. An observer that keeps one keeps a copy. That is the
// allocation-free shape on the thread a keystroke travels, and device arrival is rare enough that the
// copy an observer does make is invisible.
struct InputDevice
{
	InputDeviceId Id{};

	// libinput's name for it, which is the string a person will be looking for in the log line that
	// tells them a touchscreen is bound to nothing.
	std::string_view Name;

	// The device states a position on its own surface rather than a displacement, so it is meaningless
	// until it is bound to an output.
	//
	// **It is a touchscreen or a tablet and it is asked of the capability rather than of the events**,
	// which is what can honestly be established at the moment a device arrives — the events have not
	// happened yet. A pointer that turns out to report absolute positions is not distinguishable here:
	// libinput has no capability for one, and `LIBINPUT_DEVICE_CAP_POINTER` is what a mouse has too. So
	// this is false for a virtual pointer behind a remote session, which is correct in the only sense
	// that matters today — there is nothing on this machine that produces one — and is the paragraph to
	// re-read on the day there is.
	bool Absolute = false;

	// The active area, where libinput has it. Absent is an ordinary answer and is *no evidence* rather
	// than a size of zero: a device without the data fails the query outright, and decision 167's size
	// match refuses a candidate it cannot measure instead of matching it against nothing.
	std::optional<InputDeviceSize> Size;

	// `GYRO_OUTPUT` off the udev device, naming the connector this one is glued to, or empty where
	// nobody set it. It is the rung that always wins because it is the only one that is correct by
	// construction — decision 167 puts it in the same file the rule admitting the device already lives
	// in, rather than inventing a second configuration channel with nowhere to keep it.
	std::string_view Output;
};

// One key transition, which was the whole of what the first input path carried.
//
// **The instant is the device's own**, converted at ingest by whichever implementation read it —
// libinput reports `CLOCK_MONOTONIC` microseconds, which Core/Time.h's `Monotonic::FromMicroseconds`
// is the named door for. It is carried rather than left for the reader to stamp with `now` because it
// is the `t₀` decision 26 promises an animation starts from: a keystroke that spent four milliseconds
// behind a scene walk should begin its motion four milliseconds in, not begin it late. Every event
// below carries one for the same reason and it is always the same clock.
struct KeyEvent
{
	// evdev's numbering, `linux/input-event-codes.h`. Reproduced as a number rather than included, for
	// the reason Seam/Pixel.h reproduces a fourcc: it is stable kernel ABI, and naming the header here
	// would put a Linux include at the portable waist.
	std::uint32_t Code = 0;

	bool Pressed = false;

	Instant When{};

	InputDeviceId Device{};

	friend constexpr bool operator==(const KeyEvent&, const KeyEvent&) noexcept = default;
};

// A pointer moved by a displacement, which is what a mouse and a touchpad produce.
//
// **Both the accelerated and the raw displacement are carried, and neither is derivable from the
// other.** The accelerated one is what moves the cursor; the raw one is what
// `zwp_relative_pointer_v1` is obliged to send beside it, so a game reading the mouse directly is not
// reading a curve applied twice. Dropping either costs a protocol gyro would then have to reopen this
// for.
//
// **Acceleration is applied per event, before anything is summed.** The curve is nonlinear in
// velocity, so accelerating a coalesced displacement is not the same number as accelerating each and
// adding — the difference shows up as a pointer that feels right when it is flung and mushy when it is
// eased, which is the hardest class of input bug to attribute after the fact. Coalescing is
// `Scene/Pointer.h`'s and happens on the far side of this type.
struct PointerMotion
{
	// Logical pixels at scale 1, which is the unit `GlobalSpace` is stated in — the one thing about a
	// displacement that does not need an output to be meaningful.
	double DeltaX = 0.0;
	double DeltaY = 0.0;

	double UnacceleratedX = 0.0;
	double UnacceleratedY = 0.0;

	Instant When{};

	InputDeviceId Device{};
};

// A pointer placed at a position on the device's own surface: a graphics tablet in absolute mode, a
// touchscreen standing in for a mouse, a virtual pointer behind a remote session.
//
// A separate event rather than a flag on the one above, because *moved by* and *is at* are different
// facts and a struct that carries both with a discriminator has a representable state where they
// disagree. The fraction is of the device's active area, so mapping it onto a screen needs to know
// which output the device is bound to — see the coordinate paragraph at the top of this header.
struct PointerPosition
{
	double NormalizedX = 0.0;
	double NormalizedY = 0.0;

	Instant When{};

	InputDeviceId Device{};
};

// A button transition on a pointer, a touchpad, or the barrel of a stylus.
//
// One type for all three because evdev already numbers them in one space — `BTN_LEFT`, `BTN_STYLUS`,
// `BTN_TOOL_PEN` — and a seat that has to ask which kind of device a button came from can read
// `Device`. The alternative, a per-device-class button type, buys nothing and makes the routing
// table three tables.
struct PointerButton
{
	// evdev's numbering again, `BTN_*`. `KeyEvent::Code`'s paragraph is the argument.
	std::uint32_t Code = 0;

	bool Pressed = false;

	Instant When{};

	InputDeviceId Device{};
};

// Which way a scroll went.
enum class ScrollAxis : std::uint8_t
{
	Vertical,
	Horizontal,
};

// What did the scrolling, which is not a detail and is the reason `wl_pointer` version 5 exists.
//
// A toolkit implements kinetic scrolling — the flick that keeps going and decays — only for `Finger`,
// because a wheel has no release to start the deceleration from. Reporting a touchpad as a wheel
// turns every two-finger flick into a dead stop, and reporting a wheel as a finger makes a page drift
// after every notch.
enum class ScrollSource : std::uint8_t
{
	// A detented wheel: the only source that has a discrete step, reported below in 120ths of one.
	Wheel,
	// Two fingers on a touchpad. The only source that can *stop*.
	Finger,
	// A scroll ball or an edge strip: continuous, but with no lift to report.
	Continuous,
};

// One scroll increment on one axis.
//
// **An axis at a time, because that is what libinput reports and what the wire carries.** A diagonal
// two-finger scroll is two of these with the same instant, and grouping them back into one gesture is
// `wl_pointer.frame`'s job on the far side rather than a pair of fields here.
struct PointerScroll
{
	ScrollAxis Axis = ScrollAxis::Vertical;

	ScrollSource Source = ScrollSource::Wheel;

	// Logical pixels at scale 1. For a detented wheel it is the device's own step scaled by the
	// fraction below, so a caller that does not care about detents can use this field alone.
	double Distance = 0.0;

	// 120ths of a detent, which is the high-resolution wheel's unit both in the kernel's `REL_WHEEL_HI_RES`
	// and in `wl_pointer.axis_value120`. Zero for every source that has no detent, which is how a
	// reader tells a free-running scroll from a notched one without consulting `Source`.
	double Clicks120 = 0.0;

	// The fingers left the touchpad. It carries no distance and exists so a toolkit knows the gesture
	// ended rather than merely paused — `wl_pointer.axis_stop`, and the thing kinetic scrolling starts
	// its decay from. Only ever true for `Finger`.
	bool Stop = false;

	Instant When{};

	InputDeviceId Device{};
};

// What happened to one contact.
enum class TouchPhase : std::uint8_t
{
	Down,
	Motion,
	Up,
	// The sequence is over and did not end where it was going. It is a first-class phase rather than a
	// variety of `Up` because the two mean opposite things to whatever was tracking the finger: an up
	// commits the gesture and a cancel unwinds it. It is also what gyro says when it takes a sequence
	// over for a system gesture, so a path that cannot express one is a path where a stolen swipe
	// leaves a finger stuck down in the client forever.
	Cancel,
};

// One contact on a touchscreen, at one instant.
//
// **Per point, not per frame.** libinput reports a contact at a time and a `TOUCH_FRAME` after them;
// the frame is a grouping of these and belongs to whatever regroups them, for `PointerScroll`'s
// reason.
//
// **There is no pressure here and no contact ellipse, and that is a fact about libinput rather than
// about the panels.** The kernel reports `ABS_MT_PRESSURE` and `ABS_MT_TOUCH_MAJOR` on most
// touchscreens; libinput consumes both for its own palm and thumb detection and exports neither —
// `libinput.h` has `libinput_event_tablet_tool_get_size_major` and nothing of the kind for a touch.
// So a field for them here would be one nothing could ever fill, which is worse than their absence:
// it reads as a capability. Getting them means reading evdev underneath libinput, which means
// reimplementing palm rejection, and that trade is Open.md's to record rather than this header's to
// pre-empt with four unfilled members.
struct TouchEvent
{
	// libinput's slot. Unique within `Device` and meaningless without it, which is the second thing
	// `InputDeviceId` is for.
	std::int32_t Point = 0;

	TouchPhase Phase = TouchPhase::Down;

	// Of the device's active area. Unset on `Up` and `Cancel`, where the kernel reports no position and
	// the last known one is the caller's to remember — carried as zero rather than as the previous
	// value, because a lift that silently repeats a stale coordinate is indistinguishable from one that
	// moved.
	double NormalizedX = 0.0;
	double NormalizedY = 0.0;

	Instant When{};

	InputDeviceId Device{};
};

// Which implement is near the tablet. The eraser is a *tool* and not a button, which is the thing
// this enumeration exists to say: flipping a stylus over ends one proximity and begins another, with
// its own pressure curve and its own serial.
enum class ToolKind : std::uint8_t
{
	Unknown,
	Pen,
	Eraser,
	Brush,
	Pencil,
	Airbrush,
	Mouse,
	Lens,
	Totem,
};

// What the tool did.
enum class ToolPhase : std::uint8_t
{
	// The tool came within sensing range. There is no contact and there may never be one: a hover is a
	// complete interaction, and it is the reason a tablet cannot be modelled as a mouse that is
	// sometimes pressed.
	ProximityIn,
	Motion,
	// The tip touched down or lifted, which `TipDown` below distinguishes.
	Tip,
	Button,
	// The tool left sensing range. Whatever it was hovering over stops being hovered, and anything
	// tracking this tool's serial should forget it — the next proximity may be a different stylus.
	ProximityOut,
};

// A tablet tool, at one instant.
//
// **A hovering tool has a position and no contact, which no pointer vocabulary can express.** That is
// the whole reason this is its own type: `wl_pointer` has one state, *where it is*, and a tablet has
// three — out of range, in range, and touching — with different meanings for a client that draws.
struct ToolEvent
{
	ToolKind Kind = ToolKind::Unknown;

	// The tool's own hardware serial where it has one, and zero where it does not. It is what makes
	// *this stylus* a thing that can be recognized across proximity cycles, which is what a per-tool
	// pressure curve or a two-artist tablet needs. A device is not enough: one tablet, several tools.
	std::uint64_t Serial = 0;

	ToolPhase Phase = ToolPhase::ProximityIn;

	// Whether the tip is down *now*, carried on every event rather than only on the transition. A
	// motion while drawing and a motion while hovering are the same phase and different facts, and a
	// reader that had to remember which one it was in would be keeping a copy of state the device is
	// already reporting.
	bool TipDown = false;

	// Set on `Phase == Button`, in evdev's numbering. Zero otherwise.
	std::uint32_t ButtonCode = 0;
	bool ButtonPressed = false;

	// Of the tablet's active area.
	double NormalizedX = 0.0;
	double NormalizedY = 0.0;

	// Normalized where the tool reports them, absent where it does not — which is per tool rather than
	// per tablet, since a puck on the same tablet as a pen reports neither pressure nor tilt.
	std::optional<double> Pressure;

	// How far off the surface, normalized. It is what a hover interaction is drawn from and it is
	// distinct from `Pressure` being zero: a tip resting with no force still has a distance of nothing.
	std::optional<double> Distance;

	// Degrees from vertical, along each axis.
	std::optional<double> TiltXDegrees;
	std::optional<double> TiltYDegrees;

	// Degrees clockwise. An airbrush's barrel and an art pen's twist both land here.
	std::optional<double> RotationDegrees;

	// The airbrush wheel, normalized to its own travel.
	std::optional<double> Slider;

	Instant When{};

	InputDeviceId Device{};
};
