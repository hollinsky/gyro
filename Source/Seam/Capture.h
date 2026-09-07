#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "Core/Texture.h"
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

// One client's image read back beside the frame, which is the other half of Scene/Capture.h.
//
// **A `wl_shm` buffer is captured on the dispatch thread and a `zwp_linux_dmabuf_v1` one is captured
// here, and the split is a lifetime rather than a preference.** Protocol/Shm.cpp copies at commit and
// hands the buffer straight back, so those pixels stop being the client's frame the instant `Adopt`
// returns — the only moment they can be read is the commit itself, which is why that half is held
// continuously and costs a copy of every software window. A descriptor is the inverse: gyro borrows
// it and keeps borrowing it until the watermark says nobody is reading, so the pixels a client
// committed are *still there* when somebody presses the chord. Nothing has to be kept, and the whole
// mechanism costs nothing until the key is pressed.
//
// **The read is here, on the frame thread, in the frame that is already stalling.** A dmabuf's bytes
// are tiled under a modifier and are not a picture; the one party obliged to know the layout is the
// driver, which is Render/Readback.h's argument for a scanout target applied to the other end of the
// same pipe. So the copy is the driver's, off the image Render/Textures.h already imported, on the
// thread that has just waited on the composite's fence for `TargetReadback` anyway.
//
// The alternative was a `VK_EXT_host_image_copy` on the dispatch thread, which would have cost the
// frame nothing at all, and it is dead for a reason the layout comment in Render/Textures.cpp states:
// an imported image lives in `VK_IMAGE_LAYOUT_GENERAL` and moves between `VK_QUEUE_FAMILY_FOREIGN_EXT`
// and gyro's queue on every frame that samples it. A host copy issued from the other thread races an
// ownership it does not hold, and what that produces is a file of plausible garbage rather than an
// error — the worst possible output for an instrument whose whole job is to be believed.
//
// **Only the textures this frame actually drew.** The walk is over the draw list, so a window that is
// occluded, off-screen or on another output is not read back. That is a real gap and it is the honest
// one: the frame side can only photograph what the frame sampled, and a texture no item names is one
// the composite never touched.

// A slab to read one client image into, or nothing.
//
// **The sink sizes it rather than the frame loop, because the sink is the party that knows the
// shape.** Scene/Capture.h's offer already told it the extent, the stride and what the top byte means,
// at the commit that produced the texture — so the loop asks for an id and is handed somewhere to put
// it. What the renderer then checks is that the image it actually holds fits, which is the one place
// the client's claim and the device's record are compared.
struct TextureSlab
{
	std::span<std::byte> Into;

	// Bytes per row of `Into`. The sink's own, for `TargetReadback::Stride`'s reason.
	std::uint32_t Stride = 0;

	[[nodiscard]] bool IsValid() const noexcept { return !Into.empty() && Stride != 0; }
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

	// Arm this one output from the frame thread, for a capture no person asked for.
	//
	// **`Wanted` answers a press and this answers a moment only the frame thread can see.** Decision
	// 20's picture of a closing window is drawn on exactly one frame per exit, and the frame worth
	// photographing is the one *after* it — the first drawn from the copy rather than from the window.
	// That frame is a third of a second wide and arrives whenever somebody closes something, so a
	// person with a keyboard cannot reliably be inside it, and the chord's own answer is the frame the
	// key was pressed on. Nothing else on the machine knows the moment has come.
	//
	// **It is the output slab alone and never the texture table**, which is what makes it legal here.
	// The implementation's per-surface entries are filled by the dispatch thread inside a commit and
	// are not touched again until the frame side has given them all back; arming one of those from
	// this thread would race that handover. An output's slot is an atomic exchange out of idle and
	// races nothing, so this is the whole of the request that can cross.
	//
	// False where the output has no slab or already owes a capture, both of which are ordinary. The
	// caller has done nothing to undo — the frame is not forced until `Wanted` says so on the next
	// pass — so a refusal is a reason to say nothing rather than to retry.
	[[nodiscard]] virtual bool RequestOutput([[maybe_unused]] std::uint32_t output) noexcept { return false; }

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

	// The three above again for a client's image rather than a panel's, and defaulted to *no* for the
	// reason `IRenderer::ReadTarget` is defaulted to a refusal: the frame loop asks these of every
	// drawn item on a captured frame, and a sink that captures only the glass should answer without
	// having to say so three times.
	//
	// **Asked once per drawn item, so it must be cheap and must tolerate being asked twice.** A texture
	// two items name — a window and its own thumbnail — is offered twice on one frame, and the second
	// ask has to come back empty rather than reading the same image into the same slab again.
	//
	// **Two things the frame loop owes before it may ask at all, and both are decision 181's.** The
	// read acquires a client's dmabuf from `VK_QUEUE_FAMILY_FOREIGN_EXT`, which asserts the foreign
	// user is done with it — so no commit still on its way to the glass may have that buffer on a
	// plane, or the acquire is taken against a display engine mid-scanout and the panel does not come
	// back. And the copy is ordered by the composite's own fence and by nothing else, so the target
	// read that waits on that fence has to have *succeeded* first: two of its refusals come back before
	// it waits, and a texture read after one of those is racing the GPU. A loop that cannot promise
	// both reads nothing and writes no buffers, which is the same empty answer a run with no clients
	// gives and is why `Buffers()` is counted apart from `Written()`.
	[[nodiscard]] virtual bool WantsTexture([[maybe_unused]] TextureId texture) const noexcept { return false; }

	[[nodiscard]] virtual TextureSlab ReserveTexture([[maybe_unused]] TextureId texture) noexcept { return {}; }

	// `complete` is false where the read was refused after the reserve — a device with no readback
	// usage, a copy that timed out, an image whose extent does not match what the commit claimed. The
	// slab goes back unwritten, for the reason a half-read target does.
	virtual void PublishTexture([[maybe_unused]] TextureId texture, [[maybe_unused]] bool complete) noexcept {}
};
