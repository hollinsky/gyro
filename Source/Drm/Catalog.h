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

// The vertical blanking interval those same timings describe: `(vtotal - vdisplay) * htotal / clock`.
//
// **This is the distance between the last instant a commit still makes a frame and the instant the
// kernel timestamps that frame at**, which is why a presenter owes it to `Seam/OutputConfiguration.h`.
// `drm_calc_vbltimestamp_from_scanoutpos` computes its answer as the "end of vblank" — the start of
// scanout of the first active line — while a commit has to be latched before the vblank *begins*, and
// `drm_mode_set_crtcinfo` puts that boundary at `vdisplay` exactly (`crtc_vblank_start =
// min(vsync_start, vdisplay)`). So the two ends of this interval are the two things gyro has to hold
// apart, and neither is a guess: measured across five modes of two panels the latch threshold tracks
// this number with a slope of one.
//
// **Not the sync pulse and not the back porch**, both of which are the same shape and the wrong
// answer. A capture that mistook one for the other agreed with a 60 Hz panel to within five
// microseconds and was out by a factor of two at 40 Hz on that same panel.
[[nodiscard]] Duration BlankingOf(const drmModeModeInfo& mode) noexcept;

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

// What a plane is for, which is the one thing about it that cannot be discovered by trying.
//
// **Primary is not *the best plane*; it is the plane a modeset means something on.** An overlay bound
// to a CRTC with no primary is a legal atomic state on some drivers and a blank screen on others, and
// the failure arrives at the first commit with nothing to say why. Cursor is named because the kernel
// names it and because some drivers accept nothing else on it, not because gyro treats a pointer
// specially — decision 152 makes the cursor one promotable node among others.
enum class PlaneKind : std::uint8_t
{
	Primary,
	Overlay,
	Cursor,
};

// One format a plane accepts, with the modifiers it accepts it under.
struct PlaneFormat
{
	std::uint32_t Code = 0;
	std::vector<std::uint64_t> Modifiers;
};

// What `type` means, as the kernel's enumerator. Anything the kernel adds later is an overlay, which
// is the reading that stays correct: a plane gyro does not recognise is one it may put a layer on only
// if the atomic test agrees, and that is exactly the contract an overlay already has.
[[nodiscard]] PlaneKind KindOf(std::uint64_t type) noexcept;

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

// Whether a plane advertises that layout at all: `ModifiersFor` asked as the yes-or-no a partition
// wants.
//
// **The cheap half of not proposing a layer the display engine will refuse.** An atomic test is
// all-or-nothing and costs a couple of hundred microseconds on the frame thread, so a layer whose
// fourcc a plane has never heard of is worth dropping before the ioctl rather than after it. It is
// not the authority and cannot be — a plane that takes `AR24` may still refuse a particular extent,
// which only `DRM_MODE_ATOMIC_TEST_ONLY` can say — so this is a filter in front of that call and
// never a substitute for it.
//
// **Two silences are read as yes rather than as no.** An empty catalog is a driver that did not
// answer `IN_FORMATS`, and inventing a refusal from that would disable promotion on hardware that
// works; an invalid modifier is a framebuffer created without `DRM_MODE_FB_MODIFIERS`, where the
// driver chose the layout and is the only party that knows it, so there is nothing to compare and the
// fourcc alone is the answer. Both leave the decision with the atomic test, which is where a question
// this file cannot answer belongs.
[[nodiscard]] bool Advertised(std::span<const PlaneFormat> catalog, PixelFormat format) noexcept;
} // namespace Drm
