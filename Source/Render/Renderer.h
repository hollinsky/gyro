#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "Core/Clock.h"
#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Trace.h"
#include "Geometry/Space.h"
#include "Render/Backdrop.h"
#include "Render/Deadline.h"
#include "Render/Device.h"
#include "Render/Pipeline.h"
#include "Render/Textures.h"
#include "Render/Unfused.h"
#include "Render/Vulkan.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Seam/SyncPoint.h"

// The Vulkan renderer: `Seam/Renderer.h`'s `Record` half, against images the presenter allocated.
//
// **It draws solids, it samples images, it dresses them, and it lifts them — and it still refuses
// what it cannot express.** Render/Pipeline.h is the quad pipeline and it covers two alternatives of
// `DrawContent`: a fill or a read from an image Render/Textures.h holds, over a projected quad, with
// a corner radius and a per-node opacity. Beside it are the two dressings: Render/Backdrop.h's chain
// for a material, and the shadow pipeline for decision 104's elevation, which reads nothing and costs
// one draw. A `DrawGroup` is still `EINVAL` — Seam/Renderer.h's *an item the renderer cannot express*
// — rather than a menu whose overlap shows through mid-fade, and so is a shadow asked of a turned
// quad, whose direction Docs/Open.md has not settled. The refusal is the half of this file worth
// keeping as the pipeline set grows: the alternative failure mode is a screen that is subtly wrong
// with nothing in any log saying why, which nobody can report and nobody can bisect.
//
// **A texture whose id resolves to nothing is skipped rather than refused**, which is the one place
// this file answers differently from the rest. Core/Texture.h fixes that answer: a client destroying
// a buffer while a published snapshot still names it resolves to nothing, and *what a renderer does
// with a null or stale id is draw nothing and say nothing* — a frame is not where a lifetime bug gets
// reported, and the frame after it would report the same one again.
//
// **The colour-state conversion is the next thing owed, and it is why a mismatch is refused.**
// Decision 47 composites in linear light at wide primaries; nothing here converts anything yet, so an
// item whose light differs from the target's is refused rather than written through as if the numbers
// meant the same thing. What that leaves standing is a blend performed in the target's own encoding,
// which is wrong wherever an item is not opaque and which the conversion element of decision 62's
// pointwise chain is what fixes.
//
// **What it exercises beyond the drawing is the three things that actually break**: a dmabuf the
// renderer did not allocate becoming a `VkImage` under an explicit DRM modifier, the damage region
// reaching the driver as a scissor rather than as a whole-target repaint, and the frame's completion
// being something the caller can act on. Those were each measured against lavapipe and against a real
// driver before this file was written.
//
// **The device is held by reference and never owned**, which is Docs/Structure.md#ownership on
// device migration: `Render` tears down and rebuilds whole while `Frame` keeps running, so the
// composition root destroys the renderer, then the device, then constructs both again. A renderer
// that owned its device would make that ordering a thing somebody has to remember instead of a thing
// the types state.

// SPEC: how many images one output's ring may hand over at once. Four is what both existing
// presenters cap themselves at — Seam/RenderTarget.h's `MaxTargets`, a flip queue deeper than which is
// headroom nobody has needed — and this is one doubling of slack so that a presenter growing a cursor
// plane's target does not meet this number first. Written as that doubling rather than as an eight
// *(2026-08-23)*, because the slack is the whole of the argument and a literal would stop being slack
// the moment the ceiling moved. Fixed because `BindTargets` must not allocate per target set on a path
// that runs at every mode set.
inline constexpr std::uint32_t MaxRenderTargets = 2 * MaxTargets;

// SPEC: how many timestamps one submission may write, which is the same thing as how many named spans
// its GPU track can be cut into.
//
// **A mark costs nothing where it sits on a barrier that was already there, and that is the rule this
// number is sized against rather than a budget.** Every mark this renderer writes goes at a point the
// command buffer already synchronises at — the boundary between the extract and the blur chain, the
// boundary between the chain and the resumed composite — so the pipeline was going to drain there
// anyway and the timestamp is a register write. A mark placed between two draws inside one render pass
// would not be free: it would order two things the hardware was overlapping, and the instrument would
// be reporting the cost it had just created. So there are none, and the inside of a composite is
// decomposed by counting fragments instead.
//
// Thirty-two is eight glass panels' worth — a triple of extract, blur and resumed composite each, plus
// the opening and closing pair. A submission with more dressed items than that stops marking and keeps
// measuring: the closing timestamp is always written, so what overflow costs is detail rather than the
// frame's total.
inline constexpr std::uint32_t MaxStamps = 32;

// Which of decision 62's two executions this renderer draws.
//
// **It is a property of a renderer rather than of a frame, and that is what keeps the oracle
// honest.** The comparison decision 62 asks for is one scene drawn twice, so the two executions have
// to be able to exist at the same moment against the same device and the same targets — two
// renderers, not one renderer told something different between two frames. A per-frame switch would
// also be a lie about the production rule: decision 62 binds a variant when an animation begins and
// holds it for the duration, precisely so that a path change never lands under a moving picture.
enum class Fusion : std::uint8_t
{
	// Render/Pipeline.h's lattice where it has the variant, separate passes where it does not. What
	// every renderer outside a test is built with.
	Selected,

	// Never fused: every element of every pointwise chain is a pass of its own through an
	// intermediate that rounds. Render/Unfused.h, and decision 62's reference.
	Separate,
};

class VulkanRenderer final : public IRenderer, public ITextureFence
{
public:
	// **Construction cannot fail, and `Status()` is where the reason lives.** Same shape as
	// `VirtualOutput`, for the same reason one layer up: a machine whose driver refused a command
	// pool should come up with an output it cannot draw rather than abort before anything reaches
	// the screen. Decision 41 has the composition root building one of these on every boot, and a
	// throwing constructor there is a boot that ends in the dark.
	//
	// The clock is held for `SceneEvaluator`'s reason and it is decision 94's: the party that knows
	// where the work started and stopped is the party that did it, so `Submission::RecordCost` is
	// measured here rather than bracketed by a caller that cannot see the submission boundary.
	//
	// **`fusion` is the last argument and it has a default, which is the direction it should fail
	// in.** A composition root that says nothing gets the fused path; the reference is something a
	// caller asks for by name. It is fixed at construction rather than settable because the images
	// it needs are reserved at a binding, and a renderer that could change its mind between two
	// binds would be one whose two paths were never both available at once — which is the one
	// arrangement the oracle cannot be written against.
	// `textures` is the device's importer, shared by every output's renderer and outliving all of
	// them. Held rather than owned for the device's own reason one paragraph up: the composition root
	// destroys the renderers, then the importer, then the device, and a renderer that owned the table
	// would make a two-output machine hold two of every window. Registering for retirement and
	// unregistering happen here rather than at the root, because the pairing is what makes it correct
	// and a root that forgot the second half would free an image mid-composite.
	VulkanRenderer(
		const IClock& clock,
		VulkanDevice& device,
		VulkanTextures& textures,
		Fusion fusion = Fusion::Selected
	);

	~VulkanRenderer() override;

	[[nodiscard]] Result<void> BindTargets(std::span<const RenderTarget> targets, ColorState output) override;

	void ReleaseTargets() noexcept override;

	[[nodiscard]] Result<Submission> Record(const RecordRequest& request) override;

	[[nodiscard]] bool IsComplete(SyncPoint point) const override;

	// Decision 29's `C`, GPU half: every submission whose timestamps have resolved since the last
	// call, oldest first.
	//
	// **A poll and never a wait, twice over.** The timeline is asked first — a submission the queue
	// has not signalled cannot have written its second timestamp — and only then is the query pool
	// read, without `VK_QUERY_RESULT_WAIT_BIT`. Both are the reason `IsComplete` is a poll: a block
	// here would put the GPU's schedule on the `SCHED_FIFO` frame thread.
	//
	// **What the pair measures is elapsed GPU time, not gyro's own occupancy.** The opening timestamp
	// is written when the GPU *reaches* this command buffer, so time spent queued behind another
	// client's batch is already outside the bracket; what remains inside it is genuine mid-batch
	// preemption, which inflates the figure with work that is not gyro's. Vulkan exposes no counter
	// that separates the two — `VK_KHR_performance_query` is not present on the Intel driver this was
	// measured against — so elapsed is what there is, and it is also what the deadline cares about:
	// a frame is not on glass until it is on glass, whoever the GPU spent the interval serving.
	//
	// Zero forever on a device whose graphics family cannot timestamp, which is the same honest
	// report `Blit` makes rather than a stub.
	[[nodiscard]] std::size_t CollectCosts(std::span<GpuCost> into) override;

	// Render/Textures.h's half, answered on the dispatch thread while this thread composites.
	//
	// **`m_Submitted` is atomic for exactly these two readers and nothing else.** The frame thread
	// stores it once per submission, which is a release of everything that submission recorded; the
	// dispatch thread loads it to stamp a doomed image. It is not on the sampling path, not on the
	// per-item path, and not read inside the frame section at all — `Record` uses the plain value it
	// just computed.
	[[nodiscard]] std::uint64_t Submitted() const noexcept override
	{
		return m_Submitted.load(std::memory_order_acquire);
	}

	[[nodiscard]] bool Reached(std::uint64_t value) const noexcept override;

	// Why the renderer is not usable, where it is not. Success once construction completed.
	[[nodiscard]] const Result<void>& Status() const noexcept { return m_Status; }

	// How many images are currently imported.
	[[nodiscard]] std::uint32_t BoundTargets() const noexcept { return m_TargetCount; }

	// Whether this renderer hands out a waitable point at all, which is decision 108's device
	// property read back. False means every `Submission` is `Immediate` because the frame was
	// finished before `Record` returned.
	[[nodiscard]] bool ExportsTimeline() const noexcept { return m_TimelineFd.IsValid(); }

	// Which execution this renderer was built for. Read back so that a test can say in one line
	// which of the two it is holding, rather than inferring it from what it passed.
	[[nodiscard]] Fusion Fuses() const noexcept { return m_Fusion; }

private:
	// One imported image. The descriptor is not here: `RenderTarget`'s is borrowed and belongs to the
	// presenter, and what this owns is the `dup` that `vkAllocateMemory` consumed — which the driver
	// closes with the memory, so there is nothing here to close either.
	struct Slot
	{
		VkImage Image = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		VkImageView View = VK_NULL_HANDLE;
		PixelSize<DeviceSpace> Size{};

		// The set the chain samples this target through, allocated once at the binding and never
		// updated — Render/Backdrop.h's whole descriptor argument. Null where the target's modifier
		// cannot be sampled, which falls this output's materials to their tint.
		VkDescriptorSet Backdrop = VK_NULL_HANDLE;

		// Whether this target's modifier lists `SAMPLED_IMAGE`, which is what the set above needs of
		// it. Held per slot rather than per renderer because a target set is one format in practice
		// and nothing says it has to be.
		bool Samplable = false;

		// What the pipeline for this target was built against. Kept here rather than re-derived from
		// the `RenderTarget` at record time because the target set is the presenter's and this slot
		// outlives the span `BindTargets` was handed.
		VkFormat Format = VK_FORMAT_UNDEFINED;

		// The timeline value the last submission against this image signals. A record into an image
		// whose previous submission has not landed is `EBUSY` rather than a wait, because a wait
		// here would put the GPU's schedule on the `SCHED_FIFO` thread — which is the inversion
		// Seam/Renderer.h's `IsComplete` is a poll to avoid.
		std::uint64_t LastSubmit = 0;
	};

	// One submission's GPU cost, between the frame that wrote the timestamps and the iteration that
	// reads them back.
	//
	// **Held per target rather than in a queue of its own, because the target already bounds it.**
	// `LastSubmit` above refuses a second record into a target whose first has not landed, so a
	// target carries at most one outstanding submission and a query pair per target can never be
	// contended. That is also what lets the pool be indexed by target instead of by a cursor two
	// structures would have to keep in agreement.
	//
	// The mode and the generation are copied out of the request rather than looked up at collection,
	// because by then this renderer has drawn other frames in other modes against an output that may
	// have been reconfigured underneath both — which is the whole of what `GpuCost` carries them for.
	struct PendingCost
	{
		// The timeline value this submission signals, and zero where the target has nothing
		// outstanding. Zero is never a real submission value: `Record` increments before it submits.
		std::uint64_t Submit = 0;

		std::uint32_t Generation = 0;
		RenderMode Mode = RenderMode::Planned;

		// The GPU's clock pair when this submission was recorded, in MHz — the actual point and the
		// commanded one — read at submit rather than at collection for the same reason the mode and the
		// generation are copied here: by the time the timestamps resolve the clock has moved, so the
		// operating point the span was measured at is only knowable near the work that produced it.
		//
		// **And collection is not the other end of the span, which is why there is no closing read.**
		// `CollectCosts` runs at the top of a later frame's iteration, after the GPU has been parked
		// through the gap, so a second sample there would be another reading of the idle clock rather
		// than the one the work finished at. Nothing on this path is ever inside the span — the whole
		// design is not to wait for the batch — so the pair is taken once, near the submission, and
		// `RequestedMhz` is what carries what a parked `ClockMhz` could not say. Zero where the device
		// has no clock reader.
		std::uint32_t ClockMhz = 0;
		std::uint32_t RequestedMhz = 0;

		// Which GPU track this submission's spans belong on, copied off the request for the same
		// reason as the two fields above: by the time the timestamps resolve, this renderer has drawn
		// for whichever outputs it serves and cannot be asked which one this was.
		std::uint16_t Trace = TraceThread;

		// The frame the submission was made for, carried so the run of spans can be named for it when
		// the timestamps resolve. See `RecordRequest::Frame`.
		std::uint64_t Frame = 0;

		// The host's clock immediately after `vkQueueSubmit`, and the anchor the run is drawn from on a
		// device that cannot calibrate.
		//
		// **A bound rather than a guess, which is the whole of why it is honest to draw from.** The
		// opening timestamp is written when the GPU *reaches* the batch, so the run cannot have begun
		// before the submission that queued it — placing it here understates queue wait and never moves
		// the work earlier than it truly ran. On a device driving one composite per refresh with the
		// part otherwise idle that wait is a few microseconds, and the alternative was the row this
		// left empty: a `.pftrace` from a turnip laptop carried eleven hundred `uncalibrated` marks and
		// not one number, while the schedule those frames were being admitted against turned entirely
		// on the cost none of them reported.
		//
		// Unused where `VK_EXT_calibrated_timestamps` answers, because a real anchor places both ends.
		Instant SubmittedAt{};

		// How many timestamps the submission actually wrote, and what each of them opens. `Stamps`
		// is zero on a frame that was not being traced, one more than the number of spans otherwise —
		// the last stamp closes the one before it and names nothing.
		std::uint32_t Stamps = 0;

		std::array<const char*, MaxStamps> Names{};
	};

	// Where the marks a recording has written so far are counted.
	//
	// **A member rather than a parameter threaded through `Dress`, because the marks are not on one
	// call path.** `Record` opens and closes the pair, `Dress` cuts the chain out of the middle of it,
	// and a counter passed by reference through five signatures would be five signatures documenting
	// an instrument. `Record` is not reentrant — one command buffer per target, and a target with a
	// submission outstanding is refused before anything is recorded — so this is as single-owner as a
	// local would be.
	struct Stamping
	{
		std::uint32_t Target = 0;
		std::uint32_t Count = 0;
		bool Active = false;
	};

	[[nodiscard]] Result<void> Import(const RenderTarget& target, ColorState output, Slot& slot);

	// Put every freshly imported image into `VK_IMAGE_LAYOUT_GENERAL` once, so that every later
	// barrier is a queue-family transfer with matching layouts on both halves rather than a layout
	// change that would be an unmatched pair. It is here rather than in `Record` because the
	// transition out of `VK_IMAGE_LAYOUT_UNDEFINED` is the one the driver is permitted to discard
	// contents across, and a target's contents are gyro's the moment it has been bound.
	// Everything a gather needs, taken at the binding and never inside a frame. Returns nothing
	// because it cannot fail in a way a caller should act on: what a failure costs is decision 34's
	// third rung on this output, not a bind.
	void Reserve(std::span<const RenderTarget> targets, ColorState output);

	// One item's shadow: decision 104's closed form, over the item's rect grown by the light's reach.
	//
	// **Drawn immediately before the item's own first write to the target, which is not the same as
	// first.** For a plain item those are the same moment. For a dressed one they are not: the chain
	// reads the target back, so a shadow composited before the extract would be blurred and pulled in
	// under the panel — a dark fringe inside every piece of glass, which decision 104 forbids in as
	// many words and which nobody would attribute to elevation. So `Dress` calls this after its
	// extract, and the loop calls it only for items that have no chain to wait for.
	//
	// Nothing to fail: the pipeline's existence is checked with every other item's before recording
	// begins, for the reason `Record` states there.
	void Shade(
		VkCommandBuffer command,
		const Slot& slot,
		const DrawItem& item,
		std::span<const VkClearRect> rects,
		VkPipeline& bound
	) const noexcept;

	// One dressed item: the chain, and then the quad that composites its result.
	//
	// **It splits the render pass, and that is what a gather costs structurally.** Vulkan cannot
	// sample the colour attachment it is rendering into with a neighbourhood read — an input
	// attachment reads one fragment's own coordinate and a blur reads its neighbours — so this ends
	// the pass, barriers the target to a shader read, runs the chain, barriers back, and resumes with
	// `LOAD_OP_LOAD`. One split per dressed item. Decision 116 said this pass-based form was not built
	// because no gather was expressible; this is the gather that made it expressible.
	//
	// **At `RenderMode::Floor`, or where the chain could not be reserved, it draws the tint alone.**
	// That is decision 34's third rung and it is why this returns without a chain rather than
	// refusing: a floored frame keeps every item and gives up the blur behind a panel, which is the
	// degradation Docs/Experience.md#how-it-degrades promises.
	[[nodiscard]] Result<void> Dress(
		VkCommandBuffer command,
		const Slot& slot,
		const DrawItem& item,
		const RecordRequest& request,
		std::span<const VkClearRect> rects,
		VkPipeline& bound
	);

	// One pass of the chain: a barrier onto the destination, a rendering instance over the region in
	// use, and the full-viewport triangle Render/Shaders/Backdrop.vert draws.
	//
	// `read` is the image this pass samples, and null for the extract — whose source is the composite
	// target, which the caller has already made legible and must not transition per pass.
	void Pass(
		VkCommandBuffer command,
		VkPipeline pipeline,
		VkDescriptorSet source,
		std::uint32_t destination,
		VkRect2D used,
		const VkViewport& pane,
		const PassConstants& constants,
		VkImage read = VK_NULL_HANDLE
	) const noexcept;

	// Begin a rendering instance against one target, loading what is already there. Two callers: the
	// start of a recording, and `Dress` resuming after a split.
	void BeginTarget(VkCommandBuffer command, const Slot& slot, const RecordRequest& request) const noexcept;

	// Close whatever span is open and open one called `name`, at a point where the pipeline has
	// already drained. A no-op when the frame is not being traced or the stamp budget is spent, which
	// is what lets the call sites read as ordinary statements rather than as guarded ones.
	void Mark(VkCommandBuffer command, const char* name) noexcept;

	// One item drawn as decision 62's separate passes: the fill into an intermediate, then one pass
	// per element of the chain, then the result composited `over` the target.
	//
	// **It splits the render pass exactly as `Dress` does, and for a weaker reason.** A gather has to
	// materialise because it reads a neighbourhood; this materialises because it was told to. The
	// split is the same shape either way — end the target's rendering, run the chain against
	// offscreens, resume with `LOAD_OP_LOAD` and composite — which is what makes the two paths
	// comparable at all: whatever the split costs in rounding, it costs it in the same places.
	//
	// **The chain runs once over the item's own bound and the composite runs once per damage
	// rectangle.** The elements are pointwise, so a pixel's value does not depend on which damage
	// rectangle it fell in; the composite is where damage is honoured, and it draws the same six
	// vertices under the same scissor the fused path would have used.
	//
	// **It is total rather than fallible** because `Record`'s pre-flight walk is what makes it so: an
	// unfused renderer whose intermediates were not reserved fails at `BindTargets`, and every
	// pipeline this reaches for was built there.
	void Separate(
		VkCommandBuffer command,
		const Slot& slot,
		const DrawItem& item,
		DrawSolid solid,
		const RecordRequest& request,
		std::span<const VkClearRect> rects,
		VkPipeline& bound
	) const noexcept;

	// One element of that chain: the barriers onto both images, a rendering instance over the item's
	// region, and the quad. `source` is ignored by `Element::Emit`, which reads nothing.
	void Apply(
		VkCommandBuffer command,
		Element element,
		std::uint32_t source,
		std::uint32_t destination,
		VkRect2D region,
		const VkViewport& pane,
		const ElementConstants& constants
	) const noexcept;

	[[nodiscard]] Result<void> Settle();

	// Every distinct client image the list samples, brought back from `VK_QUEUE_FAMILY_FOREIGN_EXT`
	// before the render pass opens and handed back after it closes.
	//
	// **A pair per frame rather than once at import, and the ownership rule is why.** A dmabuf's
	// pixels are written by something that is not this queue, so what the acquire buys is that the
	// device's caches see them; doing it once would need a submission on the dispatch thread, which
	// Render/Textures.h declines for the reason stated there. Doing it per frame needs no shared state
	// between the two threads at all — the barrier is derived from the item list this thread is
	// already walking.
	//
	// **Only the imported arm.** An image this device filled itself never left, so it has no ownership
	// to reacquire and appears here as a null handle that is skipped.
	[[nodiscard]] std::uint32_t GatherSampled(std::span<const DrawItem> items) noexcept;

	void TransferSampled(VkCommandBuffer command, std::uint32_t count, bool acquiring) const noexcept;

	// One collected submission's stamps, turned into named spans and a fragment count on decision
	// 139's GPU track.
	//
	// **Separate from `CollectCosts` because they answer to different consumers.** The cost is the
	// frame loop's input and is produced whether or not anybody is looking; this is the picture, and
	// it is produced only when a ring is armed. Keeping the split visible is what makes it obvious
	// that switching tracing on cannot change what the scheduler is fed.
	void Report(const PendingCost& pending, std::span<const std::uint64_t> stamps, std::uint32_t target);

	void Destroy() noexcept;

	const IClock* m_Clock = nullptr;
	VulkanDevice* m_Device = nullptr;

	VkCommandPool m_Pool = VK_NULL_HANDLE;
	std::array<VkCommandBuffer, MaxRenderTargets> m_Commands{};

	// Two queries per target — the frame's opening and closing timestamp — so target `n` owns
	// `2n` and `2n + 1`. Null on a device that cannot timestamp, which is what makes `CollectCosts`
	// answer nothing there rather than branching on a capability at every use.
	VkQueryPool m_Queries = VK_NULL_HANDLE;

	// One pipeline-statistics query per target, wrapping the whole command buffer, of which one
	// counter is read: fragment shader invocations.
	//
	// **It is the only thing that decomposes a composite, and the timestamps cannot.** Everything a
	// floored frame draws — every shadow, every fill, every window — is inside one render pass with no
	// barrier in it, so there is no point to put a mark at that does not also serialise the pass. What
	// there is instead is a count of how many times a fragment shader ran, which turns the one span
	// into two answers: divided by the panel's pixels it is how many times gyro drew over the same
	// pixel, and divided by the span it is the rate the part is achieving, which is the number to hold
	// against what the part is supposed to do.
	//
	// Null where the device does not count, and only ever recorded while a trace ring is armed:
	// Frame/Budget.h has no use for it, so a machine that is not being looked at does not pay for it.
	VkQueryPool m_Statistics = VK_NULL_HANDLE;

	Stamping m_Stamping{};

	// The two clocks read together, refreshed once per collection that has something to report. Held
	// rather than passed because the conversion happens well after the frame that produced the stamps.
	GpuCalibration m_Calibration{};

	std::array<PendingCost, MaxRenderTargets> m_Pending{};

	// Built at construction and populated per format at `BindTargets`, so that nothing inside a
	// recording ever creates one — decision 62's *no frame blocks on compilation*, holding at the one
	// place it is currently possible to break it.
	QuadPipeline m_Pipeline;

	// The gathering half. It is a member rather than a thing constructed per frame for decision 46's
	// reason: the images it holds are reserved when the output is configured, and a transition that
	// allocated a render target at the moment it started would be a stutter exactly where one is most
	// visible.
	class Backdrop m_Backdrop;

	// Decision 62's reference path. Empty and costing nothing under `Fusion::Selected` — `Reserve`
	// is never called, so no image is allocated and no pipeline is built on an output that will
	// never draw this way.
	class Unfused m_Unfused;

	// The device's texture table. Never null after construction — the constructor refuses a null
	// reference by taking one.
	VulkanTextures* m_Textures = nullptr;

	// The images `GatherSampled` found, reused across frames so that the walk allocates nothing.
	// Decision 36's rule, and the reason `MaxSampledImages` is a refusal rather than a resize.
	std::array<VkImage, MaxSampledImages> m_Sampled{};

	Fusion m_Fusion = Fusion::Selected;

	// What the composite is encoded to, from the binding rather than from the frame. Held because
	// every item's variant and its two luminance factors are a function of this and the item's own
	// colour state, and `Record` may only look things up.
	ColorState m_Output = ColorState::Srgb();

	// One timeline for the device's whole life, which is what Seam/SyncPoint.h's borrowed descriptor
	// requires: *the timeline outlives every point on it*. The descriptor is invalid on a device that
	// cannot export one, and decision 108 is what that means for the points handed out.
	VkSemaphore m_Timeline = VK_NULL_HANDLE;
	Fd m_TimelineFd;

	// Decision 142's deadline, imported off the descriptor above and therefore constructed after it.
	// Invalid on a device that exports no timeline, which is the same device that has no queue for a
	// deadline to hurry.
	FenceDeadline m_Deadline;
	std::atomic<std::uint64_t> m_Submitted = 0;

	std::array<Slot, MaxRenderTargets> m_Slots{};
	std::uint32_t m_TargetCount = 0;

	Result<void> m_Status{};
};
