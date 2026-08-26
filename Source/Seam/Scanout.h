#pragma once

#include <cstdint>

#include "Core/Result.h"
#include "Core/Texture.h"
#include "Seam/Importer.h"

// How a texture comes to be scannable: the display device's dispatch half.
//
// Seam/Importer.h makes an id name pixels a fragment shader can sample. This makes the *same* id name
// a framebuffer a display engine can scan out, so that decision 152's per-frame partition has
// something to put on a plane — the draw item it promotes already carries a `TextureId` and nothing
// else, and Seam/Presenter.h's `LayerSource` is that id reaching the backend unchanged. See
// Docs/Decisions.md decision 153.
//
// **One id space, two questions, two devices.** A machine can composite on one card and scan out on
// another, so this is a second interface rather than two more verbs on `ITextureImporter`: a single
// interface would put the framebuffer on whichever device the renderer happens to be. What the shared
// id space buys is that the composition root drives both from one place, in one pass, across the
// device rebuild decision 41 performs — which is the property decision 131 went to the waist for,
// arriving a second time for a second reason.
//
// **It belongs to the device rather than to the presenter**, for Render/Textures.h's reason exactly. A
// presenter is per output; `drmModeAddFB2` is an object on a card. A table hung off a presenter would
// hold a framebuffer per monitor for every window on the machine and re-import the same client buffer
// once per panel it is visible on.
//
// **Only descriptors, never a mapping.** A `wl_shm` buffer's pixels are copied into gyro's own memory
// at commit and there is no dmabuf underneath to hand a display engine, so a mapped source is
// `EINVAL` here rather than a copy this layer invents.
//
// **Offering is not promotion.** Whether a buffer is worth importing for scanout at all is the slow
// loop's answer — a `zwp_linux_dmabuf_v1` feedback tranche and a client round trip — and whether to
// promote it on a given frame is the assigner's. The two must not be conflated: a buffer laid out for
// sampling may carry a modifier no plane scans out, which is a refusal here and a surface that simply
// never promotes, rather than anything the frame path learns about.

class IScanoutImporter
{
public:
	IScanoutImporter() = default;

	virtual ~IScanoutImporter() = default;

	IScanoutImporter(const IScanoutImporter&) = delete;
	IScanoutImporter& operator=(const IScanoutImporter&) = delete;
	IScanoutImporter(IScanoutImporter&&) = delete;
	IScanoutImporter& operator=(IScanoutImporter&&) = delete;

	// Make `id` name a framebuffer on this device.
	//
	// **Unbounded and allocating, on the dispatch thread**, for `ITextureImporter::Adopt`'s reason: the
	// frame thread never waits on this and never triggers one. It names an id, and a partition naming
	// an id that did not resolve is refused whole by `IPresenter::TestLayers`, which costs decision
	// 35's one frame and no more.
	//
	// **The descriptors stay the caller's for the duration of the call and no longer.** Unlike a mapped
	// texture, nothing here reads the caller's memory afterwards: a framebuffer holds GEM handles the
	// device imported, and the client's descriptor may be closed the moment this returns.
	//
	// **A refusal is ordinary.** `EINVAL` for a null id, a source that does not describe the image it
	// claims, a mapped source, or a format and modifier no plane on this device scans out; `ENOMEM`
	// where the table has no room. All of them mean the same thing to the caller — this buffer is
	// sampled and never promoted — which is why none of them reaches a frame.
	[[nodiscard]] virtual Result<void> Adopt(TextureId id, const TextureSource& source) = 0;

	// Give up an id, and say nothing about one this does not hold.
	//
	// **Called below Publication/Return.h's watermark**, the same half of the rule `ITextureImporter`
	// states: the scene has stopped naming the id.
	//
	// **And the completion that releases it is the page flip, not the queue.** This is the one place
	// the two importers differ and the difference is not a detail. The last commit that named a
	// framebuffer keeps scanning it out until the *next* flip replaces it, which is after the sequence
	// that named it went below the watermark — so the display engine is still reading an image the
	// scene has finished with. Worse, `drm_mode_rmfb` disables every plane still using the framebuffer,
	// which is a modeset: releasing early blanks a plane and takes decision 73's unbounded path on the
	// `SCHED_FIFO` thread. So this returns having promised to release, and the implementation reclaims
	// at a frame boundary once the panel has stopped scanning the image out.
	virtual void Forget(TextureId id) noexcept = 0;
};
