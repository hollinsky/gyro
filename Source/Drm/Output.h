#pragma once

#include <xf86drmMode.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <span>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Drm/Commit.h"
#include "Drm/Device.h"
#include "Drm/Fence.h"
#include "Drm/Scanout.h"
#include "Drm/Watchdog.h"
#include "Seam/Allocator.h"
#include "Seam/Buffer.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"

// One panel, as an `IPresenter`: a connector, the CRTC driving it, and a primary plane the composite
// is flipped onto.
//
// **The ring is gyro's own and the flip is what retires it**, which is Seam/Presenter.h's framing of
// KMS as a swapchain taken literally. Targets are allocated from the rendering device — decision 120,
// the same allocator a nested output takes — under a modifier the *plane* said it can scan out, and
// wrapped as framebuffers once. A commit names one of them; the page flip that answers says the
// previous one is off the screen and free again.
//
// **The atomic commit is issued as a bare ioctl rather than through libdrm, and that is not a style
// choice.** `drmModeAtomicCommit` allocates four arrays per call, and `Present` runs inside
// Core/FrameSection.h's guard, where an allocation is an abort — decision 36, and the one check that
// is not a build rule. Everything off the frame path is libdrm's: enumeration, property blobs,
// `AddFB2`, and the mode set itself. What is left inside the frame is a fixed set of property ids and
// values that were resolved when the output was opened, written into arrays that were sized then.
//
// **And it is issued *blocking*, on a thread of this output's own.** A non-blocking commit is finished
// by an `SCHED_OTHER` kworker whose priority gyro cannot raise, which under load misses vblanks in
// bursts and lands on a person as a pointer that stutters whenever the machine is busy. The blocking
// form does the same work on the calling thread, so it runs at a priority gyro chose — and because that
// call does not return until the flip is done, the caller cannot be the frame thread. Drm/Commit.h is
// the thread, the mailbox and the whole of the argument. What stays here is that `Present` returns when
// the commit was *handed over* rather than when the kernel took it, which is why a refused commit comes
// back as `Missed` rather than as a failure from `Present`.
//
// **The commit carries an `IN_FENCE_FD` where the device can produce one, and is held where it
// cannot.** Drm/Fence.h is the translation from the renderer's timeline point; the fallback is
// Nested/Output.h's held commit — the frame waits until `IRenderer::IsComplete` says the pixels exist,
// which costs the overlap between drawing a frame and handing it over and is said out loud in the log
// once, because every pacing figure the session then reports has gyro's own polling in it.
//
// **A mode set is not built here yet, and the header says so rather than pretending.** `Reconfigure`
// is defined by Seam/Presenter.h as initiated on the frame thread and *performed elsewhere*, because
// the kernel runs the driver's `atomic_check` synchronously on the caller and amdgpu takes every
// modeset lock on the device inside it. That is a thread and a completion path. What this output does
// today is adopt at `Open` — one blocking commit before the frame thread exists — and answer a later
// request by reporting the configuration it still has, which `OutputConfiguration::SatisfiedBy` reads
// as *not honoured*. Nothing lies about having changed a mode it did not change.

namespace Drm
{
// Three, for Nested/Output.h's reason one layer down: the panel is scanning one out while gyro draws
// into another, and two makes a stall the ordinary case every time a flip is late.
inline constexpr std::uint32_t DefaultDrmTargets = 3;

class DrmOutput final : public IPresenter, public IScanoutHold
{
public:
	// The device and the allocator are references because neither is copyable and both outlive every
	// output — one card and one rendering device serve every panel on the machine.
	//
	// `completion` is the renderer whose submissions land in these targets, asked `IsComplete` only on
	// the held path. Null means a commit is never held, which is right for a CPU blitter and wrong for
	// anything with a queue.
	DrmOutput(
		DrmDevice& device,
		const Pipeline& pipeline,
		IDmabufAllocator& allocator,
		const IRenderer* completion,
		std::uint32_t targets = DefaultDrmTargets
	);

	~DrmOutput() override;

	// Pick the mode, build the ring, and put the first frame's worth of state on the hardware.
	//
	// **It blocks, and this is the one place in this module that may.** The initial commit carries
	// `ALLOW_MODESET`, which the contract puts outside the frame thread's budget entirely; it runs on
	// whichever thread the composition root builds outputs on, before the frame thread exists — exactly
	// where `NestedOutput::Open` does its roundtrip.
	//
	// **Adoption is not a second entry point**, per Seam/Presenter.h: the commit is tried bare first,
	// and `-EINVAL` is the kernel saying this was not an adoption — that the state actually differs and
	// a modeset flag is required. gyro never reads hardware state back to decide.
	[[nodiscard]] Result<void> Open(const OutputConfiguration& wanted);

	// Seam/Presenter.h.
	[[nodiscard]] std::span<const RenderTarget> Targets() const override;

	[[nodiscard]] std::optional<std::uint32_t> AcquireTarget() override;

	using IPresenter::Present;

	[[nodiscard]] Result<void> Present(std::span<const PresentLayer> layers, PresentTrace trace) override;

	// `DRM_MODE_ATOMIC_TEST_ONLY` over exactly the commit `Present` would make. This is the party
	// Seam/Presenter.h says knows the answer, and it is the only one: what a display engine refuses is a
	// combination, bounded by bandwidth and by scaler units shared between pipes.
	[[nodiscard]] Result<void> TestLayers(std::span<const PresentLayer> layers) override;

	// The planes this output may put a layer on. Fixed at `Open`, because the inventory is the device's
	// and the device does not grow planes.
	[[nodiscard]] std::uint32_t LayerCeiling() const noexcept override { return m_PlaneCount; }

	void Reconfigure(const OutputConfiguration& wanted) override;

	// Everything owed once the device's events have been read to empty: a held commit whose composite
	// has landed, and a reconfiguration that has been answered.
	//
	// Public for `VirtualOutput::Advance`'s reason — the device above owns the drain — and carrying its
	// obligation: every signal on this output emits from here or from the completion the drain
	// produced, so whoever calls this is the frame thread.
	void Settle();

	// When this output next wants looking at. `Duration::max()` where it is quiet, which is the
	// ordinary answer — a flip completion is a real event on a real descriptor. A *held* commit is the
	// exception, because no file becomes readable when the GPU finishes.
	[[nodiscard]] Instant NextEvent() const noexcept;

	[[nodiscard]] std::uint32_t Crtc() const noexcept { return m_Crtc; }

	[[nodiscard]] const OutputConfiguration& Configuration() const noexcept { return m_Configuration; }

	[[nodiscard]] const Pipeline& Pipe() const noexcept { return *m_Pipeline; }

	// Whether commits carry a fence rather than waiting for the composite. False is the held path, and
	// what it costs is a frame of overlap on every frame.
	[[nodiscard]] bool IsExplicitlySynchronized() const noexcept { return m_Fenced; }

	// How many commits were held for a composite rather than going out at once, and how many this
	// output has accepted. The pair is the line at shutdown that says whether the pacing figures mean
	// anything.
	[[nodiscard]] std::uint64_t Held() const noexcept { return m_HeldCommits; }

	std::uint64_t Commits = 0;

	// A page flip completed. Called by the device's drain, on the frame thread, and the only producer
	// of `Presented`.
	void OnPresented(Instant at, std::uint32_t sequence, bool hardwareClock);

	// Drm/Scanout.h's question, answered on the dispatch thread about writes the frame thread made.
	//
	// **The last two commits, which is exactly the set the panel could be reading.** A framebuffer is on
	// the glass until another commit replaces it, and this backend never has more than one commit
	// outstanding — `Present` refuses a second — so the image being scanned out is always in the newer
	// pair. Holding a framebuffer one commit longer than strictly necessary costs a client one buffer of
	// its own cycle for one frame; releasing one early disables the plane it is on.
	[[nodiscard]] bool Holds(std::uint32_t framebuffer) const noexcept override;

private:
	enum class TargetState : std::uint8_t
	{
		Free,
		Acquired,
		Committed,

		// On the glass. It stays there until another flip completes, which is what makes a ring of two
		// on this backend a ring of one usable image.
		Scanout,
	};

	struct Target
	{
		DmabufBuffer Buffer;

		// The `AddFB2` handle, which is what the commit actually names. Zero where the framebuffer could
		// not be built, which leaves the ring shorter rather than the output broken.
		std::uint32_t Framebuffer = 0;

		TargetState State = TargetState::Free;
	};

	// The commit that is waiting for something: the partition, and the images it names.
	struct Pending
	{
		std::array<PresentLayer, MaxLayers> Layers{};
		std::uint32_t Count = 0;
		bool Waiting = false;

		// The name the hold's slice was opened under, kept so the flip that ends the wait says the
		// same words.
		std::uint64_t Frame = 0;
	};

	// Where one plane's properties sit in the preallocated commit.
	//
	// **Every usable plane has a block whether or not a layer is on it**, because a partition that
	// shrinks has to turn off the plane the vanished layer was on. A commit that simply stopped
	// mentioning it would leave the kernel's last state in place, which is the promoted window still on
	// the screen after the composite stopped drawing a hole for it — a stale copy of a window that has
	// moved, sitting over the top of everything, until something else happens to touch that plane.
	struct PlaneCommit
	{
		std::uint32_t Object = 0;
		std::uint32_t Offset = 0;
		std::uint32_t Count = 0;
		bool Fenced = false;

		// What this plane says it will scan out, borrowed from the pipeline's own inventory.
		//
		// **A borrow rather than a copy, and it is safe because `Pipe()` is already borrowed the same
		// way.** The pipeline outlives every output built from it — `m_Pipeline` beside this is the
		// same lifetime — and the catalog is a few dozen fourccs with a modifier list each, which is
		// not a thing to duplicate per plane per output.
		std::span<const PlaneFormat> Formats;
	};

	[[nodiscard]] Result<void> BuildTargets();

	void DropTargets() noexcept;

	// The frame commit: plane properties only, asking for a page-flip event, and handed to the commit
	// thread rather than issued here. No libdrm and no allocation; see the header comment. `frame` is
	// what the records it writes are named for.
	//
	// The fences are taken by mutable span because they are *moved* into the request: a raw descriptor
	// in the property values is only valid while something holds it open, and once the ioctl left this
	// thread the caller's stack stopped being that something.
	[[nodiscard]] Result<void> Flip(std::span<const PresentLayer> layers, std::span<Fd> fences, std::uint64_t frame);

	// The commit thread's ioctl. Static because it is what `CommitIssuer` is, and it runs on that thread
	// — it may touch nothing here but the device's descriptor, which is fixed at construction.
	static int Issue(void* context, const CommitRequest& request) noexcept;

	// Take the outcome of a commit that has finished, and unwind the frame where it failed.
	//
	// **A failure cannot arrive as a `Present` refusal any more**, because `Present` returned when the
	// commit was handed over rather than when the kernel took it. It arrives as `Missed`, which
	// Seam/Presenter.h built for exactly this — a frame the backend accepted and will never show — and
	// which the loop already answers by dropping what was in flight, invalidating the clock rather than
	// predicting from a cadence with a hole in it, and re-damaging the output.
	void Reap();

	// Give up on a page flip event that is not coming, which is the watchdog firing.
	//
	// **The targets are moved as though the flip happened, because it almost certainly did.** The
	// kernel's wait returned before the commit thread reported success, so what was lost is the
	// notification rather than the flip — and of the two ways to be wrong here, believing a committed
	// image is on the glass costs one target held out of the ring, while disbelieving it hands the
	// renderer an image a display engine is scanning out. That second one is the fault decision 182 was
	// written for, in a different file.
	//
	// **What does not happen is a `Presented`.** There is no instant and no vblank sequence to report,
	// and Seam/PresentationInfo.h forbids inventing either, so the loop is told `Missed` — which is the
	// honest statement that this output cannot vouch for the frame — and re-damages and re-anchors.
	void AbandonFlip();

	// Whether this output could program that partition at all, which is the half of the question that
	// needs no ioctl: a layer per plane, and every image one this output owns.
	[[nodiscard]] Result<void> Expressible(std::span<const PresentLayer> layers) const noexcept;

	// Fill the preallocated blocks for this partition: every usable plane written, the ones past the
	// partition's end turned off. No allocation and no property lookup — see the header comment.
	void Program(std::span<const PresentLayer> layers, std::span<const Fd> fences) noexcept;

	// One atomic commit against the blocks `Program` filled, issued on the calling thread. `TestLayers`
	// is the only caller left — the frame commit goes to the commit thread instead, and takes a copy of
	// the blocks with it — and it is a bare ioctl for the reason the header gives.
	[[nodiscard]] Result<void> Commit(std::uint32_t flags) noexcept;

	// Copy the proposal the kernel just refused out of the commit blocks, on the frame thread. Allocates
	// nothing and asks nothing: it is `std::memcpy` in a loop, which is what makes it legal where it is
	// called from.
	void KeepRefusal(std::span<const PresentLayer> layers, int code) noexcept;

	// Say it out loud. Called from `Settle`, which the device's drain runs outside the frame section.
	void ReportRefusal();

	// What a layer's pixels are on this card: one of this output's own targets, or a framebuffer the
	// device's scanout table holds for a promoted client buffer. Zero for neither, which `Expressible`
	// turns into a refusal of the whole partition.
	[[nodiscard]] std::uint32_t Framebuffer(const PresentLayer& layer) const noexcept;

	// How those pixels are laid out — the same two sources `Framebuffer` reads, asked the other
	// question. An invalid format where the layer names neither, which `Advertised` reads as *nothing
	// to compare* rather than as a refusal, because `Framebuffer` has already refused that layer.
	[[nodiscard]] PixelFormat Layout(const PresentLayer& layer) const noexcept;

	// The image's own extent, which a layer with an empty source rectangle means the whole of.
	[[nodiscard]] PixelSize<DeviceSpace> Extent(const PresentLayer& layer) const noexcept;

	// Publish the framebuffers this commit named, so that the dispatch thread can tell whether it is
	// safe to remove one. See `Holds`.
	void Record(std::span<const PresentLayer> layers) noexcept;

	// The commit that carries a mode. libdrm's, blocking, and off the frame thread by contract.
	[[nodiscard]] Result<void> Modeset(bool allowModeset);

	DrmDevice* m_Device = nullptr;
	const Pipeline* m_Pipeline = nullptr;
	IDmabufAllocator* m_Allocator = nullptr;
	const IRenderer* m_Completion = nullptr;

	std::uint32_t m_Crtc = 0;
	std::uint32_t m_ModeBlob = 0;
	drmModeModeInfo m_Mode{};

	OutputConfiguration m_Configuration{};
	FenceExporter m_Fences;

	std::array<Target, MaxTargets> m_Targets{};
	std::array<RenderTarget, MaxTargets> m_Descriptions{};
	std::uint32_t m_TargetCount = 0;
	std::uint32_t m_Wanted = DefaultDrmTargets;
	std::uint32_t m_Next = 0;

	// The property ids and values one flip writes, sized and filled at `Open`. `Present` overwrites the
	// values and issues the ioctl; nothing here grows.
	//
	// **Eleven per plane, which is what `PlaneProperties::IsComplete` already guarantees plus the
	// fence.** The order inside a block is fixed — framebuffer, CRTC, the four source edges, the four
	// destination edges, then the fence where the plane has one — so `Program` writes by offset rather
	// than searching, which is what keeps a per-frame commit free of string comparisons and of
	// branches on which property is which.
	static constexpr std::size_t PropertiesPerPlane = 11;
	static constexpr std::size_t MaxCommitProperties = MaxLayers * PropertiesPerPlane;

	std::array<PlaneCommit, MaxLayers> m_Planes{};

	// The framebuffers of the last two commits, written by the frame thread inside `Flip` and read by
	// the dispatch thread inside `Holds`. Relaxed stores under one release, because the only thing that
	// has to be ordered is *these values before this index*, and a reader that sees the older index
	// reads a superset of what the panel is showing.
	std::array<std::array<std::atomic<std::uint32_t>, MaxLayers>, 2> m_Committed{};
	std::atomic<std::uint32_t> m_Recording{ 0 };
	std::array<std::uint32_t, MaxLayers> m_Objects{};
	std::array<std::uint32_t, MaxLayers> m_Counts{};
	std::array<std::uint32_t, MaxCommitProperties> m_Properties{};
	std::array<std::uint64_t, MaxCommitProperties> m_Values{};

	// How many planes this output may put a layer on: the primary and everything above it, capped.
	std::uint32_t m_PlaneCount = 0;

	// **The proposal the kernel last refused, kept so that somebody outside the frame section can say
	// what was in it.** `TestLayers` runs on the `SCHED_FIFO` frame thread inside Core/FrameSection.h's
	// guard, where a log call is the blocking operation that thread exists to avoid — so the numbers are
	// copied into plain storage here and `Settle` prints them, which is `FrameLoop::m_FirstRefusal`'s
	// route unchanged.
	//
	// **What it is for is that an errno alone cannot be acted on.** A capture of a machine that never
	// promotes reads `committing a page flip 22` on every frame and stops there: `EINVAL` is the driver
	// saying *not this*, and which of a dozen properties it meant is the only thing worth knowing. The
	// format and the modifier are not in here because they are the kernel's — `drmModeGetFB2` answers
	// them from the framebuffer id at report time, off the frame path, where an ioctl is free.
	struct RefusedLayer
	{
		std::uint32_t Plane = 0;
		std::uint32_t Framebuffer = 0;

		// 16.16 fixed point, exactly as they went to the kernel: reporting the numbers that were sent
		// rather than the floats they came from is the point, since a conversion is one of the things
		// that can be wrong.
		std::uint64_t SrcX = 0;
		std::uint64_t SrcY = 0;
		std::uint64_t SrcW = 0;
		std::uint64_t SrcH = 0;

		std::int64_t CrtcX = 0;
		std::int64_t CrtcY = 0;
		std::uint64_t CrtcW = 0;
		std::uint64_t CrtcH = 0;
	};

	// **Compared rather than counted, which is what *once per distinct refusal* means.** A standing
	// refusal is one line for the session; a refusal that changes when a window resizes is a new line,
	// and the pair of them together is the diagnosis.
	struct RefusedProposal
	{
		std::array<RefusedLayer, MaxLayers> Layers{};
		std::uint32_t Count = 0;
		int Code = 0;

		[[nodiscard]] bool SameAs(const RefusedProposal& other) const noexcept;
	};

	RefusedProposal m_Refused{};
	RefusedProposal m_Reported{};
	bool m_RefusalPending = false;

	Pending m_Pending{};

	// Which images the panel is showing, and which ones the kernel has accepted and not yet flipped, as
	// bitmasks over the target ring.
	//
	// **Sets rather than single indices, because a partition names one image per layer and they retire
	// together at the flip.** Freeing the wrong one hands the renderer an image that is currently on
	// screen, which is the tear this pair exists to prevent, and a partition makes that a set
	// difference rather than a comparison.
	std::uint32_t m_ScanoutMask = 0;
	std::uint32_t m_InFlightMask = 0;
	bool m_Flipping = false;

	// The page flip event the kernel owes once its own blocking wait has returned. Armed by `Reap` on a
	// commit the kernel took, disarmed by every path that clears `m_Flipping`, and the only thing that
	// keeps this output on `NextEvent`'s books once the commit thread has gone idle — see Drm/Watchdog.h
	// for what silence there costs.
	FlipWatchdog m_Watchdog;

	// The last completion, for the period this output *actually ran at* — which Seam/PresentationInfo.h
	// insists is measured rather than echoed from the mode. Two flips are needed before there is one,
	// and the first report carries zero, which the clock reads as *the backend does not know*.
	Instant m_LastPresented{};
	std::uint32_t m_LastSequence = 0;
	bool m_HavePrevious = false;

	bool m_Fenced = false;
	std::uint64_t m_HeldCommits = 0;

	// The row this output's records land on and the frame whose flip is outstanding, both from the
	// last `Present`. The row is `PresentTrace`'s and not remembered across a presenter rebuild —
	// decision 41 replaces one by constructing another, and a per-call row needs no re-handshake.
	std::uint16_t m_Trace = TraceThread;
	std::uint64_t m_FlippingFrame = 0;

	// A reconfiguration the drain has not answered yet. Held as the request rather than as a flag,
	// because what is echoed back is its generation.
	std::optional<OutputConfiguration> m_Request;

	// The thread that issues this output's commits, and the slot the frame thread hands one through.
	//
	// **Last, so it is destroyed first**, which is the ordering that matters and the reason the
	// destructor also stops it by hand: `DropTargets` removes framebuffers a commit in flight has the
	// kernel scanning out.
	CommitThread m_Commit;
};
} // namespace Drm
