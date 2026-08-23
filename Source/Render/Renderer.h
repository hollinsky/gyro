#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "Core/Clock.h"
#include "Core/Fd.h"
#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Render/Backdrop.h"
#include "Render/Device.h"
#include "Render/Pipeline.h"
#include "Render/Unfused.h"
#include "Render/Vulkan.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Seam/SyncPoint.h"

// The Vulkan renderer: `Seam/Renderer.h`'s `Record` half, against images the presenter allocated.
//
// **It draws solids, it dresses them, and it lifts them — and it still refuses what it cannot
// express.** Render/Pipeline.h is the quad pipeline and it covers one alternative of `DrawContent`: a
// fill, over a projected quad, with a corner radius and a per-node opacity. Beside it are the two
// dressings: Render/Backdrop.h's chain for a material, and the shadow pipeline for decision 104's
// elevation, which reads nothing and costs one draw. A `DrawTexture` and a `DrawGroup` are `EINVAL` —
// Seam/Renderer.h's *an item the renderer cannot express* — rather than a window that silently comes
// out as a flat rectangle or vanishes, and so is a shadow asked of a turned quad, whose direction
// Docs/Open.md has not settled. The refusal is the half of this file worth keeping as the pipeline
// set grows: the alternative failure mode is a screen that is subtly wrong with nothing in any log
// saying why, which nobody can report and nobody can bisect.
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
// presenters cap themselves at — a flip queue deeper than that is headroom nobody has needed — and
// eight is one doubling of slack so that a presenter growing a cursor plane's target does not meet
// this number first. Fixed because `BindTargets` must not allocate per target set on a path that
// runs at every mode set.
inline constexpr std::uint32_t MaxRenderTargets = 8;

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

class VulkanRenderer final : public IRenderer
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
	VulkanRenderer(const IClock& clock, VulkanDevice& device, Fusion fusion = Fusion::Selected);

	~VulkanRenderer() override;

	[[nodiscard]] Result<void> BindTargets(std::span<const RenderTarget> targets, ColorState output) override;

	void ReleaseTargets() noexcept override;

	[[nodiscard]] Result<Submission> Record(const RecordRequest& request) override;

	[[nodiscard]] bool IsComplete(SyncPoint point) const override;

	// Nothing yet, and this is a gap rather than a floor.
	//
	// `SimulatedRenderer` and `Blit` return zero because their work has no second device to cost;
	// this one has a real queue whose occupancy is exactly what decision 29's `C` wants, and
	// measuring it means a timestamp query pool written at submission and resolved some frames
	// later. Until that lands, `Budget` sees the CPU half of `C` and nothing else — which understates
	// the composite on an accelerated device and is very nearly right on the floor tier, where the
	// wait below folds the GPU half into the CPU figure anyway.
	[[nodiscard]] std::size_t CollectCosts(std::span<GpuCost> into) override;

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

	[[nodiscard]] bool Reached(std::uint64_t value) const noexcept;

	void Destroy() noexcept;

	const IClock* m_Clock = nullptr;
	VulkanDevice* m_Device = nullptr;

	VkCommandPool m_Pool = VK_NULL_HANDLE;
	std::array<VkCommandBuffer, MaxRenderTargets> m_Commands{};

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
	std::uint64_t m_Submitted = 0;

	std::array<Slot, MaxRenderTargets> m_Slots{};
	std::uint32_t m_TargetCount = 0;

	Result<void> m_Status{};
};
