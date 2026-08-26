#pragma once

#include <cstdint>

#include "Core/Signal.h"
#include "Core/Time.h"
#include "Seam/EventSource.h"

// Where gyro's input comes from: one device set, drained on the dispatch thread, reporting keys.
//
// **It is an `IEventSource` rather than a second thing shaped like one**, and the reason is that the
// drain contract Seam/EventSource.h writes down is the contract wanted here word for word — one
// descriptor for a whole device set, read to empty rather than once, an invalid descriptor being an
// ordinary answer for an implementation whose events come from somewhere that is not a file. What
// differs is not the source, it is who pumps it: presentation is frame-side and input is dispatch's,
// which is the partition Docs/Structure.md#threads-are-a-second-partition already draws. That is a
// property of the object rather than of the type — decision 81 states the rule per source, not per
// interface — so the composition root registering this one with the dispatch thread's wait instead of
// the frame ring is the whole of the difference.
//
// **A key is reported as the kernel numbers it, with no keymap in front of it.** Translation is
// xkbcommon's and belongs beside the seat that sends a keymap to clients; what a device produces is a
// keycode, and putting a layout in the path here would mean the compositor's own escape chord moved
// when somebody selected Dvorak. Nothing below cares which letter is painted on the key it is being
// told about.

// One key transition, which is the whole of what the first input path carries.
//
// **The instant is the device's own**, converted at ingest by whichever implementation read it —
// libinput reports `CLOCK_MONOTONIC` microseconds, which Core/Time.h's `Monotonic::FromMicroseconds`
// is the named door for. It is carried rather than left for the reader to stamp with `now` because it
// is the `t₀` decision 26 promises an animation starts from: a keystroke that spent four milliseconds
// behind a scene walk should begin its motion four milliseconds in, not begin it late.
struct KeyEvent
{
	// evdev's numbering, `linux/input-event-codes.h`. Reproduced as a number rather than included, for
	// the reason Seam/Pixel.h reproduces a fourcc: it is stable kernel ABI, and naming the header here
	// would put a Linux include at the portable waist.
	std::uint32_t Code = 0;

	bool Pressed = false;

	Instant When{};

	friend constexpr bool operator==(const KeyEvent&, const KeyEvent&) noexcept = default;
};

// A device set and the keys it produces.
class IInput : public IEventSource
{
public:
	// Emitted from `Drain`, on the thread that drains — which is dispatch's, so an observer may author
	// into the store from it and a chain from a keystroke to a commit stays one causal sequence on one
	// thread. Docs/Architecture.md#threads has why that chain may not be split.
	Signal<const KeyEvent&> Key;

protected:
	IInput() = default;
};
