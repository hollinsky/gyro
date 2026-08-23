#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "Core/Clock.h"
#include "Core/Fd.h"
#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Render/Device.h"
#include "Render/Pipeline.h"
#include "Render/Vulkan.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Seam/SyncPoint.h"

// The Vulkan renderer: `Seam/Renderer.h`'s `Record` half, against images the presenter allocated.
//
// **It draws solids, and it still refuses what it cannot express.** Render/Pipeline.h is the quad
// pipeline and it covers one alternative of `DrawContent`: a fill, over a projected quad, with a
// corner radius and a per-node opacity. A `DrawTexture`, a `DrawGroup`, a node dressed in a material
// or lifted to an elevation is `EINVAL` — Seam/Renderer.h's *an item the renderer cannot express* —
// rather than a window that silently comes out as a flat rectangle or vanishes. The refusal is the
// half of this file worth keeping as the pipeline set grows: the alternative failure mode is a
// screen that is subtly wrong with nothing in any log saying why, which nobody can report and nobody
// can bisect.
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
	VulkanRenderer(const IClock& clock, VulkanDevice& device);

	~VulkanRenderer() override;

	[[nodiscard]] Result<void> BindTargets(std::span<const RenderTarget> targets) override;

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

	[[nodiscard]] Result<void> Import(const RenderTarget& target, Slot& slot);

	// Put every freshly imported image into `VK_IMAGE_LAYOUT_GENERAL` once, so that every later
	// barrier is a queue-family transfer with matching layouts on both halves rather than a layout
	// change that would be an unmatched pair. It is here rather than in `Record` because the
	// transition out of `VK_IMAGE_LAYOUT_UNDEFINED` is the one the driver is permitted to discard
	// contents across, and a target's contents are gyro's the moment it has been bound.
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
