#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Core/Signal.h"
#include "Core/Trace.h"
#include "Geometry/Region.h"
#include "Geometry/Space.h"
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
// **The source is a target index or a texture id**, which is decision 153 and is what gives the
// partition something to promote. A composite is one of the presenter's own images; a promoted layer
// is a client's buffer, named by the id the draw item was already carrying and turned into a scanout
// framebuffer on the dispatch thread by `Seam/Scanout.h`, because that is a kernel allocation and the
// presenter is frame-side.
struct PresentLayer
{
	// One of this presenter's targets, or a texture id a display engine scans out directly. See
	// Seam/RenderTarget.h's `LayerSource` and Seam/Scanout.h for who resolves the second.
	LayerSource Target{};

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
	// The space follows the source: the device grid for a composite, and a promoted client buffer's
	// own grid for a layer naming a texture id, which are the same numbers only where the promotion
	// was unscaled — and decision 152's predicate is that it was.
	Region<DeviceSpace> Damage;

	ColorState Color = ColorState::Srgb();
};

// Where a commit's journey is drawn, and what the drawings are named. `Core/Trace.h`'s
// `TraceCommit(output)` and the frame's number, travelling per call for Seam/Renderer.h's
// `RecordRequest` reason: a backend serves one output and does not know which, and everything the
// records need to *label* what happens after the ioctl has to be in hand when it happens — a flip
// lands, a held commit is abandoned, and the loop is not on the stack for either. `frame 142` on the
// commit row is the same words as on every other row, which is how a reader joins them.
struct PresentTrace
{
	std::uint16_t Trace = TraceThread;

	// Zero from a caller that has no frame to name.
	std::uint64_t Frame = 0;
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

	// How many commits may be outstanding at once — accepted by the backend and not yet answered by
	// `Presented` or `Missed`.
	//
	// **One is the answer for hardware and it is the default, because it is the answer that is never
	// wrong.** KMS refuses a second nonblocking commit on a CRTC that has not flipped, so a backend
	// that says nothing here is held to the rule the kernel would hold it to anyway. A backend that
	// wants more has to say so.
	//
	// **The reason this is at the seam at all is that the rule is the hardware's and the frame loop is
	// not on hardware.** A nested output's completion is `wp_presentation_feedback`, which the host
	// sends once the frame is already on the glass — so a loop that waits for it before starting the
	// next frame has spent a whole refresh waiting for news, and presents on every *other* vblank. That
	// is a compositor running at half rate on a machine with the headroom to run at full rate, and
	// nothing anywhere reports a miss, because nothing missed: every frame gyro drew arrived on time
	// and it simply drew half as many. Measured on the materials gym nested under a 60 Hz host: 38 fps,
	// with the loop's own *nothing changed, skip it* path firing once in forty seconds.
	//
	// **What it may not be is a queue depth.** A host applies at most one commit per refresh and
	// discards whatever it superseded, so a backend that answered four here would have three of every
	// four frames thrown away and would invalidate its clock on each one. The number is *how far ahead
	// of the last completion this output may work*, which is one frame ahead of hardware wherever the
	// completion arrives a frame late — never a licence to run open loop.
	//
	// Bounded in practice by the target ring, since every outstanding commit is holding one: a
	// presenter answering more than `Targets().size() - 1` will simply find `AcquireTarget` empty.
	[[nodiscard]] virtual std::uint32_t CommitDepth() const noexcept { return 1; }

	// Whether this partition of the draw list is one the hardware will actually take.
	//
	// **The assigner proposes and the backend answers, rather than the backend describing itself and
	// the assigner reasoning.** A portable capability struct — scaler ratios, plane counts, format
	// pairs — is the obvious alternative and it cannot be written truthfully: what a display engine
	// refuses is a *combination*, bounded by memory bandwidth and by scaler units shared between
	// pipes, and no per-plane description gyro could publish would predict it. `DRM_MODE_ATOMIC_TEST_ONLY`
	// is the only party that knows, so the question is asked in the form the kernel already answers.
	//
	// **The cost is an ioctl, which is why decision 152 makes the answer cacheable rather than the
	// question cheap.** The caller is expected to ask when the partition's *shape* changes — an item
	// crossing the promotion predicate, the draw list's membership changing — and not once per frame:
	// a promoted layer that merely moved is a partition already tested, since a plane's position is two
	// integers in a commit that was happening anyway.
	//
	// The vocabulary is `Present`'s, and `EINVAL` here is the ordinary answer rather than a bug: the
	// whole point is to find out. A backend that has not implemented promotion inherits the default,
	// which is the same rule its `Present` already holds, so an assigner running against it proposes
	// once, is refused, and composites everything.
	//
	// **Both halves of that rule, and the second one shipped missing.** A backend with no planes refuses
	// a layer count above one *and* a layer that is a client's texture, because it has nothing to scan a
	// client's buffer out on — which is what `Nested` and `Virtual` both say in `Present`. Answering only
	// the first half here is a partition the test accepts and the commit then refuses, and the one that
	// reaches is the commonest screen there is: a single window with nothing over it promotes whole,
	// leaving no composite, so the frame is refused every refresh and the panel keeps the last picture it
	// had. Under the nested backend that picture is the host window's initial black, and the window in it
	// never redraws either — no frame ever reaches the glass, so no frame callback is ever answered.
	[[nodiscard]] virtual Result<void> TestLayers(std::span<const PresentLayer> layers)
	{
		if (layers.size() != 1)
		{
			return Failure(EINVAL, "this output scans out one layer");
		}

		if (layers.front().Target.IsTexture())
		{
			return Failure(EINVAL, "this output has no scanout for a promoted texture");
		}

		return {};
	}

	// How many layers this output could take at most, which is the ceiling the assigner stops at
	// before it proposes anything.
	//
	// **A bound rather than a promise.** It is what the hardware has — planes on this CRTC, one of them
	// the composite's — and says nothing about whether any particular partition of that width will be
	// accepted, which is `TestLayers`' business. Its purpose is to stop an assigner enumerating
	// partitions of forty windows on a device with two planes.
	[[nodiscard]] virtual std::uint32_t LayerCeiling() const noexcept { return 1; }

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
	virtual Result<void> Present(std::span<const PresentLayer> layers, PresentTrace trace) = 0;

	// The spelling for a caller with no row to draw on and no frame to name — a test driving one
	// backend, never the loop. Backends re-expose it with `using IPresenter::Present`.
	Result<void> Present(std::span<const PresentLayer> layers) { return Present(layers, {}); }

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

	// The frame that was accepted will never reach the glass.
	//
	// **Present() returning success is a promise about the commit and never about the picture**, and
	// this is what happens when the two come apart. A nested output learns it from
	// `wp_presentation_feedback.discarded` — a host that superseded the commit, moved the window to
	// another output, or occluded it entirely — and the answer at the seam has to be a *third* one,
	// because both existing signals would lie. Presented would hand FrameClock an observation with no
	// timestamp behind it, which is a prediction built on a frame that never happened;
	// TargetsInvalidated says the images are gone, and they are not — the host is still holding this
	// one and will release it in its own time.
	//
	// **Silence is the thing that must not happen.** The loop marks an output flip-pending at the
	// commit and will not serve it again until something clears that, which is the hardware condition
	// KMS imposes and not an arithmetic one. So an unanswered commit is an output that stops drawing
	// for good, from a frame the host quietly dropped — visible as one window in a multi-output
	// session freezing while the others carry on, with nothing anywhere reporting a failure. What the
	// loop does instead is drop what was in flight, invalidate the clock rather than let it predict
	// from a cadence that has a hole in it, and re-damage the output, because the pixels that were
	// drawn are not on any screen.
	//
	// It carries nothing. There is nothing to say beyond *not that one*: no instant, since none
	// happened, and no sequence, since the loop already knows which frame it committed.
	Signal<> Missed;

	// The images are gone: a resize, a mode set, a modifier renegotiation. Everything the renderer
	// imported from Targets() is invalid from this point and the set must be re-read.
	//
	// It is not implied by Reconfigure() and it is not exclusive to it — a nested backend's window
	// resize invalidates targets with no reconfiguration at all — so the two are separate signals
	// rather than one with an argument.
	Signal<> TargetsInvalidated;

	// All four emit on the frame thread. Docs/Structure.md has signals intra-thread only: one
	// crossing the publication boundary would be a third channel where the design turns on there being
	// two, and Core/Signal.h makes that a runtime abort rather than a convention. Reconfigured is the
	// one worth checking twice — completion arrives as an event the frame loop already polls for, so
	// it is emitted from the loop's own drain and never from whatever thread the driver finished on.
};
