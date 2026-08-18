#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "Core/Result.h"
#include "Core/Signal.h"
#include "Geometry/Region.h"
#include "Geometry/Space.h"
#include "Seam/ColorState.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/PresentationInfo.h"
#include "Seam/RenderTarget.h"
#include "Seam/SyncPoint.h"

// The presentation half of the seam: how a frame reaches the glass, on all three backends.
//
// The framing that makes one interface serve KMS, a nested Wayland window, and a headless dump:
// **KMS is a swapchain.** A small ring of images the backend owns, presented in order, with an
// unusually precise clock. Everything below follows from that plus decision 73's split of the two
// verbs.
//
// **The renderer never sees a `VkSwapchainKHR`.** It renders into images described by RenderTarget
// and produces a SyncPoint. That is exactly what KMS needs, and it is achievable nested by driving
// `zwp_linux_dmabuf_v1` directly rather than through Vulkan WSI — which is decision 1, and which is
// also what makes a virtual output another presenter rather than a fork of the render path.

// How a layer's alpha is treated when the hardware composites it.
//
// Only two values, because the third thing a KMS blend mode can say — whether alpha is premultiplied
// — is already said by the layer's ColorState, and a fact with two encodings is a fact that can
// disagree with itself. A backend combines the two: Blended plus AlphaMode::Premultiplied is KMS's
// pre-multiplied mode, Blended plus Straight is coverage, and Opaque is None whatever the alpha says.
enum class BlendMode : std::uint8_t
{
	Opaque,
	Blended,
};

// One image on its way to the screen this frame.
//
// **A list rather than a target index, and the list is the part that cannot be widened later.** The
// arrangement worth having is not one client filling the screen — it is several layers on several
// planes with the GPU never waking, which is what a tablet spends most of its life able to do. The
// composited remainder is one more layer in the list rather than a privileged concept, so promotion
// is a partition rather than a fullscreen special case. Widening a target index into this afterwards
// would touch all three backends, the frame loop, damage accounting, and admission control's cost
// model at once. See Docs/Architecture.md#what-to-build-before-it-is-needed.
//
// **Z is the list order**, bottom first, for the same reason blending is not spelled twice: a z field
// beside an ordered list is a second encoding of one fact.
//
// **The source is a target index today, and that is the honest limit of what this can express.** A
// promoted *client* buffer needs a name at this seam, and naming one is not answerable yet: turning a
// dmabuf into a scanout framebuffer is a kernel allocation that must not happen inside the frame
// section, while the presenter itself is frame-side, so the import has no home until plane assignment
// says where it lives. What that costs is recorded in [Open.md](../../Docs/Open.md); what it does not
// cost is this shape, because a cursor image and a virtual output's imported buffers are both targets
// and the multi-layer path is exercised by them.
struct PresentLayer
{
	// Index into Targets(), valid until TargetsInvalidated.
	std::uint32_t Target = 0;

	BlendMode Blend = BlendMode::Opaque;
	std::uint8_t Reserved[3] = {};

	// What the GPU must finish before these pixels may be read. Immediate where the content was
	// complete before the call — a CPU blit, a static logo.
	SyncPoint Acquire;

	// The region of the source image that is shown, real-valued because a plane's source rectangle is
	// 16.16 fixed point and a viewport crop samples between texels. Empty means the whole image.
	Rect<DeviceSpace> Source{};

	// Where it lands, integral because `CRTC_W` and `CRTC_H` are integers — which is why a 1.1× scale
	// of a 101-pixel source has no whole destination, and why the plane assigner has to intersect the
	// transform's facts with the plane's own capabilities rather than deciding from the transform
	// alone.
	PixelRect<DeviceSpace> Destination{};

	// What changed since this layer was last presented, in the source image's coordinates.
	//
	// Per layer rather than per output, because `FB_DAMAGE_CLIPS` is a plane property: offloaded
	// layers accumulate separately even though they retire together at the flip. Modelled per frame it
	// would be a correctness bug the first time a frame is skipped — damage accumulates since the last
	// *successful* present, never since the last iteration.
	//
	// The space follows the source, and today the source is always one of the output's own targets, so
	// it is the device grid. A client buffer promoted to a plane would carry its damage in that
	// buffer's space, which is part of what the deferred item above has to answer.
	Region<DeviceSpace> Damage;

	ColorState Color = ColorState::Srgb();
};

class IPresenter
{
public:
	IPresenter() = default;

	virtual ~IPresenter() = default;

	// Neither copied nor moved: it owns signals, which are neither, and observers hold its address.
	// Decision 41 replaces a presenter by destroying it and constructing another, which the
	// composition root sequences.
	IPresenter(const IPresenter&) = delete;
	IPresenter& operator=(const IPresenter&) = delete;
	IPresenter(IPresenter&&) = delete;
	IPresenter& operator=(IPresenter&&) = delete;

	// The images the backend owns. Both KMS and Wayland have modifier and device constraints the
	// renderer cannot know, so neither can accept a foreign buffer — the backend allocates or imports,
	// and this is how the renderer learns what it may bind.
	//
	// Empty between TargetsInvalidated and Reconfigured, which is a state the frame loop already
	// tolerates because an output mid-transition is not presenting.
	[[nodiscard]] virtual std::span<const RenderTarget> Targets() const = 0;

	// The next image free to render into, or nothing.
	//
	// **Nothing is an ordinary answer, not a failure.** A ring under backpressure — a virtual output
	// whose consumer has not returned a buffer, a host compositor holding both — has no free image, and
	// the frame loop's response is to skip this output's frame rather than to wait. Waiting here would
	// put a client's flow control on the frame thread, which is the one thing the publication boundary
	// exists to prevent.
	[[nodiscard]] virtual std::optional<std::uint32_t> AcquireTarget() = 0;

	// Put these layers on the screen, bottom first.
	//
	// **May never block.** No device-wide lock, no `ALLOW_MODESET`, no wait on another output's
	// commit. Its cost is bounded by the composite because it *is* decision 29's `B(L)`, the longest
	// non-preemptible chunk on the frame thread — and nothing that is not a composite may enter it.
	//
	// **Returns rather than signals, because the caller can act.** A commit can fail without blocking:
	// the device was removed, master was revoked, the layer set is not expressible on this hardware.
	// The frame loop learns at the call site and falls to the floor tier or skips the output, and a
	// permanently failing output cannot masquerade as one that is merely slow. The vocabulary a
	// backend returns: `EBUSY` for transient refusal including an output mid-reconfiguration, `ENODEV`
	// or `EACCES` where the device is gone or paused — which is the composition root's problem and not
	// the loop's — and `EINVAL` where the layers cannot be expressed, which is a bug in the assigner
	// rather than a condition to retry.
	//
	// Success means the commit was accepted, never that anything reached the glass. That arrives as
	// Presented, or does not arrive at all.
	virtual Result<void> Present(std::span<const PresentLayer> layers) = 0;

	// Program the output to be this.
	//
	// **Initiated on the frame thread, performed elsewhere.** It returns before the hardware is
	// programmed and completion arrives as Reconfigured. Its cost is unbounded by contract even where
	// a given driver happens to be fast: `drm_atomic_nonblocking_commit` runs the driver's
	// `atomic_check` synchronously on the calling thread, and amdgpu uses that latitude to take every
	// modeset lock on the device and wait on every CRTC's outstanding commit. `NONBLOCK` defers
	// commitment, not validation. See decision 73 and KernelWishlist.md.
	//
	// **Adoption is not a second entry point.** Reconfiguring to the mode already set *is* an
	// adoption: the kernel demands `ALLOW_MODESET` only where the committed state actually differs, so
	// the backend commits bare first and reads `-EINVAL` as the kernel saying this was not an
	// adoption. gyro never reads hardware state back to decide. The flag is never a constant — on AMD
	// silicon below `IP_VERSION(3, 2, 0)` any commit carrying it resets every plane on the CRTC.
	//
	// **When to call it is not this interface's business.** Decision 73 puts the predicate on the idle
	// fold — reconfigure when nothing is moving, defer only where quiet is known to arrive and known
	// when — and that lives in the frame loop's policy. What is promised here is that an output
	// mid-reconfiguration holds its last frame on the glass, and that on a driver which serializes
	// device-wide, unrelated outputs hold with it.
	virtual void Reconfigure(const OutputConfiguration& wanted) = 0;

	// A frame reached the glass. The sole input to the prediction every deadline is derived from.
	Signal<const PresentationInfo&> Presented;

	// The transition completed, carrying what was *achieved* — which is how a request the hardware
	// could not honour reports itself, and the only way the variable-refresh range is ever learned.
	// The generation is echoed, so a completion for a superseded request is identifiable rather than
	// being mistaken for the one being waited on.
	Signal<const OutputConfiguration&> Reconfigured;

	// The images are gone: a resize, a mode set, a modifier renegotiation. Everything the renderer
	// imported from Targets() is invalid from this point and the set must be re-read.
	//
	// It is not implied by Reconfigure() and it is not exclusive to it — a nested backend's window
	// resize invalidates targets with no reconfiguration at all — so the two are separate signals
	// rather than one with an argument.
	Signal<> TargetsInvalidated;

	// All three emit on the frame thread. Docs/Structure.md has signals intra-thread only: one
	// crossing the publication boundary would be a third channel where the design turns on there being
	// two, and Core/Signal.h makes that a runtime abort rather than a convention. Reconfigured is the
	// one worth checking twice — completion arrives as an event the frame loop already polls for, so
	// it is emitted from the loop's own drain and never from whatever thread the driver finished on.
};
