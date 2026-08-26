#pragma once

#include <xf86drmMode.h>

#include <cstdint>
#include <span>
#include <vector>

#include "Core/Time.h"
#include "Geometry/Space.h"
#include "Seam/RenderTarget.h"

// The two questions a KMS output has to answer before it can allocate anything, written as functions
// over data rather than as steps inside the device.
//
// **They are here because they are the only half of this backend a machine with no panel can run.**
// Everything else in `Drm` is an ioctl against hardware that is either present or not; choosing a mode
// from a connector's list and decoding what a plane says it accepts are arithmetic over bytes the
// kernel handed over, and a test can hand over the same bytes. That is what keeps the two mistakes
// this backend is most likely to make — picking the mode whose *name* matched rather than whose
// timings did, and reading a modifier table one entry out — findable without a monitor.
//
// **A period is computed from the timings rather than read from `vrefresh`.** `drmModeModeInfo`
// carries both, and the integer field is rounded to a whole hertz: a 59.94 Hz panel reports 60, which
// is a period sixty-seven microseconds short. Frame/FrameClock.h predicts every deadline from that
// number, so an error there is a frame that is scheduled progressively earlier until it lands on the
// wrong side of a vblank — visible as a stutter once every fifteen seconds on a panel that is
// perfectly regular. The clock arithmetic is exact and costs one division.

namespace Drm
{
// The refresh interval the timings describe: pixel clock over the total blanked-out frame. `clock` is
// in kHz and the totals are in pixels, so the period in nanoseconds is `htotal * vtotal * 1e6 / clock`
// — done in 64-bit because that product overflows 32 bits at 4K.
[[nodiscard]] Duration PeriodOf(const drmModeModeInfo& mode) noexcept;

// The mode closest to what was asked for, or null where the connector offers none at all.
//
// **Resolution is matched exactly and refresh is matched nearest, and the asymmetry is deliberate.** A
// resolution gyro did not ask for is a picture at the wrong size, which nobody wants silently; a
// refresh rate two hundredths off is the same panel under a different name and refusing it would leave
// a monitor dark over a rounding convention. Where the wanted resolution is absent the connector's
// preferred mode is taken instead, which is what an adoption at boot asks for — see
// the preferred-mode fallback below.
//
// `wanted` empty means *the preferred mode*, and a zero period means *whatever that mode runs at*.
[[nodiscard]] const drmModeModeInfo*
ChooseMode(std::span<const drmModeModeInfo> modes, PixelSize<DeviceSpace> wanted, Duration period) noexcept;

// One format a plane accepts, with the modifiers it accepts it under.
struct PlaneFormat
{
	std::uint32_t Code = 0;
	std::vector<std::uint64_t> Modifiers;
};

// `IN_FORMATS` decoded: what this plane will scan out, and under which layouts.
//
// **The blob is a kernel ABI with two variable-length tables and a bitmask joining them**, which is
// three places to be off by one — `formats_offset` into a `__u32` array, `modifiers_offset` into an
// array of `drm_format_modifier`, and each of those carrying a sixty-four-bit mask whose bit *i*
// means "formats[offset + i]". Decoding it wrong does not fail: it produces a plausible pairing that
// `AddFB2` accepts and the display engine renders as garbage, or an allocation under a modifier the
// plane cannot scan out, which fails at the first commit with `EINVAL` and no indication why.
//
// Every bound is checked against the blob's own length, because the blob comes from the kernel but the
// *length* comes from a separate ioctl and gyro is the party that has to keep them consistent. A blob
// that does not describe itself yields an empty catalog rather than a read past the end.
[[nodiscard]] std::vector<PlaneFormat> DecodeFormats(std::span<const std::byte> blob);

// The modifiers this catalog offers for `code`, in the plane's own order, or empty where the plane
// does not take that format at all. What it is for is `IDmabufAllocator::Allocate`, which takes the
// whole candidate set and lets the device lay the image out its own best way — decision 120's
// reversal, and the reason this returns a list rather than picking one.
[[nodiscard]] std::span<const std::uint64_t>
ModifiersFor(std::span<const PlaneFormat> catalog, std::uint32_t code) noexcept;
} // namespace Drm
