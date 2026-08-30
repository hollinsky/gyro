#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "Geometry/Space.h"
#include "Seam/Pixel.h"
#include "Seam/SyncPoint.h"

// The debug capture: reading a target back off the GPU so a person can look at the frame that was on
// the glass.
//
// **This is not a screencapture and the two must not become one.** Decision 64 already settles what a
// day-to-day capture is — a second instance of the scene, rendered under its own transform into a
// target the consumer owns, at the consumer's own size and its own clock, with the panel's plane
// promotion left intact. That mechanism can capture a single window, capture at a rate the panel is
// not running at, and serve two recorders at once, none of which this can do. What it cannot do is
// prove that its pixels are the pixels the display engine scanned out, because it is a second
// computation of the same picture rather than the picture itself.
//
// That proof is the whole of what this is for. The frame loop forces the captured frame to composite
// everything — `Assign` with a ceiling of zero — and then reads back the target it just committed, so
// the bytes on disk are the bytes that went to the panel. The costs are real and are the reason this
// verb lives behind Input/Chord.h's leader rather than on a key anybody presses:
//
//   - **The captured frame is drawn differently from its neighbours.** Where the display engine would
//     have taken a layer onto a plane, the GPU draws it instead. If those two paths ever disagree,
//     the person watching sees a flash at the instant of capture — which is exactly the bug this verb
//     exists to photograph, and exactly the reason a shipping capture path may not work this way.
//   - **The frame thread blocks on the composite's fence.** Reading a target means waiting for the
//     work that filled it, and this waits rather than deferring to a later iteration. On a
//     `SCHED_FIFO` thread that is a frame missed, and it is accepted here because a debug capture is
//     one frame at a moment a person chose: it perturbs the run it is measuring either way, and a
//     deferred readback would buy back one dropped frame in exchange for a slab held across
//     iterations and a target the ring could not reuse.
//
// **What it still cannot see.** A CRTC's gamma and degamma LUTs, its colour transform matrix, panel
// dithering, and anything PSR or FBC does to the picture after the framebuffer is latched all happen
// downstream of this and are invisible to it. The DRM mechanism that *would* see them is a writeback
// connector, which i915 does not implement; on hardware that does, a writeback capture is the honest
// version of this file and this one becomes the fallback.

// One target read back into linear rows.
//
// **Linear rows rather than the target's own memory**, because a scanout image is tiled under a
// modifier and its bytes are not a picture. Whoever implements this owns the detile — for the Vulkan
// renderer that is the copy itself, which is the one party on the machine that already knows the
// image's layout.
struct TargetReadback
{
	// Index into the set last handed to `BindTargets`, which is `IRenderer::Record`'s numbering.
	std::uint32_t Target = 0;

	// The work that filled the target. Waited for — see the header above for why the wait is here
	// rather than on a later iteration.
	SyncPoint After;

	// Where the rows land. At least `Size.Height * Stride` bytes, and the caller's rather than the
	// renderer's for `CollectCosts`' reason one file over: the frame loop already holds a slab that
	// was reserved off the frame path, and a renderer that allocated one would be allocating inside
	// Core/FrameSection.h.
	std::span<std::byte> Into;

	// Bytes per row of `Into`, which the caller sizes and the renderer does not choose. Tight is the
	// only value anything asks for, and saying it explicitly is what lets a reader of the slab know
	// the stride without asking the format how wide a sample is.
	std::uint32_t Stride = 0;
};

// Where a captured frame goes, and the reason the frame loop can hand one over at all.
//
// **Three verbs rather than one, because the frame thread may neither allocate nor open a file.** The
// slab is reserved by whoever implements this, off the frame path; the frame thread copies into it
// and says the copy happened; and the write to disk is that implementation's own thread. Virtual/Dump.h
// is the same split for the same reason and is what the composition root's implementation is built out
// of — Docs/Open.md's *spdlog async sink* entry is the hazard, and a capture whose own cost showed up
// in the frame it captured would be an instrument that reads its own weight.
class ICaptureSink
{
public:
	ICaptureSink() = default;
	ICaptureSink(const ICaptureSink&) = delete;
	ICaptureSink& operator=(const ICaptureSink&) = delete;

	virtual ~ICaptureSink() = default;

	// Is this output owed a capture?
	//
	// **Asked before the partition is assigned**, which is why it is separate from `Reserve`: the
	// ceiling that forces a full composite has to be chosen before the draw list is split, and the
	// target this frame will draw into is not known until after. Const and cheap — it is read once per
	// output per frame on every frame, capture or not.
	[[nodiscard]] virtual bool Wanted(std::uint32_t output) const noexcept = 0;

	// A slab to read the target into, or empty where the capture cannot be held — a shape the sink was
	// not built for, a writer still busy with the last one, a `Wanted` that has already been answered.
	//
	// An empty span is an ordinary answer rather than a fault: the frame it was asked on has already
	// been forced to composite, so the honest thing is to draw it and say nothing, which is what an
	// empty span makes the loop do.
	[[nodiscard]] virtual std::span<std::byte>
	Reserve(std::uint32_t output, PixelSize<DeviceSpace> size, PixelFormat format, std::uint32_t stride) noexcept = 0;

	// The slab is finished with. `complete` is false where the readback was refused after the reserve,
	// which hands the slab back without writing a file — a truncated PAM in a directory somebody is
	// watching reads as a compositor that drew half a frame.
	virtual void Publish(std::uint32_t output, std::uint64_t sequence, bool complete) noexcept = 0;
};
