#pragma once

#include <cstdint>

#include "Core/Time.h"

// What a keyboard produced, as everything downstream of a device sees it.
//
// **It is here rather than at the waist because two modules that may not name `Seam` both need it.**
// `Seam/Input.h` declares the interface that emits one and `Protocol` is what turns it into a
// `wl_keyboard.key` — and decision 87 forbids `Protocol` the waist, for the same reason it moved
// `Core/Buffer.h` down: a type crossing between two parties is not the same thing as the interface
// between them, and putting it at the waist is what drags a module across a layer it has no business
// naming. `Seam/Input.h` includes this and carries the argument for the shape.

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
