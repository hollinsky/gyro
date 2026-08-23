#pragma once

#include <string_view>

#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Seam/Buffer.h"
#include "Seam/RenderTarget.h"

// Where a presenter's images come from.
//
// **Two implementations and two consumers in different modules, which is what puts it at the waist.**
// It used to live in `Virtual`, and the argument for keeping it out of `Seam` was a statement of fact
// rather than a principle: nothing outside that module ever named an allocator. Decision 120 ended
// that — a nested output has no GBM device and no swapchain, so the only thing on the machine that
// can produce its targets is the Vulkan device that is about to draw into them. `Render` implements
// this and `Nested` consumes it; neither may name the other; and the composition root is the only
// thing that knows both sides, which is what Docs/Structure.md#orchestration says a seam is for.
//
// **The rule working rather than eroding.** Docs/Structure.md's test for the control waist is *every
// interface with more than one implementation and the data crossing it*, and both clauses now hold
// where before only the first did. What came up with the interface is Seam/Buffer.h — a descriptor, a
// mapping, and a description — and nothing else; constructing one is still a platform module's
// business, because only a platform module implements this.
//
// **The three providers differ in what they can produce, not in how they are called.** `udmabuf`
// turns a sealed `memfd` into a dmabuf with no device at all, which makes it linear-only — and linear
// is the only modifier lavapipe imports, so the pairing is exact rather than lucky (Docs/Decisions.md
// decision 40). The heap is that with the ioctl removed, so that everything above the allocator is
// testable on a machine with no `/dev/udmabuf`. The Vulkan export produces whatever the device tiles,
// which is the whole reason a nested output can offer a parent compositor a modifier from its own
// feedback rather than the one thing gyro knows how to allocate. GBM on a render node is deferred
// with the dependency it costs; see decision 102.
//
// **A caller that has candidates offers them one at a time, in its own preference order.** There is
// no list-taking verb here and there does not need to be: `zwp_linux_dmabuf_v1` hands a nested output
// a tranche of format-and-modifier pairs already ranked by the host against its own hardware, and the
// output walks that order calling `Allocate` until one is accepted. The device contributes the veto
// and the host contributes the ranking, which is where each of them belongs — and a verb that took
// the whole list would have to define what the two providers with one modifier each do with it.
//
// **Allocation is unbounded and allocating by contract**, which is not a hedge: it opens a
// descriptor, maps pages, and may talk to a driver. It runs where `IRenderer::BindTargets` runs — at
// a target invalidation, never inside the frame section — and the presenter that calls it is
// obliged to do so from the same place.
class IDmabufAllocator
{
public:
	IDmabufAllocator() = default;

	virtual ~IDmabufAllocator() = default;

	// Neither copied nor moved, for `IPresenter`'s reason one layer down: a provider holds a device
	// descriptor, and the presenter that uses one holds its address for as long as its targets live.
	IDmabufAllocator(const IDmabufAllocator&) = delete;
	IDmabufAllocator& operator=(const IDmabufAllocator&) = delete;
	IDmabufAllocator(IDmabufAllocator&&) = delete;
	IDmabufAllocator& operator=(IDmabufAllocator&&) = delete;

	// One image, under exactly the format and modifier named. `EINVAL` for a size or format this
	// provider cannot express, `ENOMEM` where the allocation failed, and whatever the device reported
	// otherwise — the same vocabulary `IRenderer::BindTargets` answers with, because a caller that
	// cannot allocate a target set and a renderer that cannot bind one are in the same position.
	//
	// **`EINVAL` is an ordinary answer to a candidate rather than the end of a negotiation**, and a
	// caller walking a list of them treats it that way. What must not vary is the modifier: a provider
	// that quietly substituted one would hand a parent compositor a buffer laid out differently from
	// the one it agreed to import.
	[[nodiscard]] virtual Result<DmabufBuffer> Allocate(PixelSize<DeviceSpace> size, PixelFormat format) = 0;

	// Whether `Allocate` would accept this format, without allocating to find out. A presenter asks
	// before it tears down a working target set for a reconfiguration it cannot honour.
	[[nodiscard]] virtual bool Supports(PixelFormat format) const noexcept = 0;

	// For a log line naming which provider a target set came from. Two providers that both worked on
	// the developer's machine and one that does not exist in CI is the failure this makes legible.
	[[nodiscard]] virtual std::string_view Name() const noexcept = 0;
};
