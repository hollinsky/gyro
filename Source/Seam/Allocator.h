#pragma once

#include <span>
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
// **A caller offers every candidate at once and is told which one it got.** This used to be the
// opposite — one modifier per call, walked in the caller's order, the host ranking and the device
// vetoing — and the reading that reversed it is that *the host's order is not a ranking of anything
// the device cares about*. `zwp_linux_dmabuf_v1` ranks a tranche by what the parent compositor can
// import and scan out; it says nothing about what the GPU underneath likes to render into, and the
// two disagree in the worst possible direction. A host that lists `DRM_FORMAT_MOD_LINEAR` first — and
// they do, because it is the entry every importer accepts — had gyro compositing every frame into an
// untiled, uncompressed image. Measured on Tiger Lake at 1920x1048 that is 7.4ms of GPU time against
// 2.8ms for the same composite Y-tiled, which is most of a 60Hz frame spent on the layout rather than
// the picture; Frame/Timing.h then correctly dropped the blur to decision 35's floor tier on all but
// five frames in fifteen hundred, so the visible symptom was *the material disappeared* and the cause
// was three layers away.
//
// **What the objection to a list was, and why it does not hold.** The argument against a list-taking
// verb was that the two providers with one modifier each would have to define what they do with one.
// They do: take the first entry they support and answer `EINVAL` if there is none — which is a
// sentence, not a design, and it is what walking the list from outside already did to them. What the
// list buys is that the provider with a *real* preference gets to express it: `VK_EXT_image_drm_
// format_modifier` exists to be handed the whole set so the driver can lay the image out its own
// best way, and `vkGetImageDrmFormatModifierPropertiesEXT` is how it reports back which way that was.
// So the ranking moves to the device, the veto stays there too, and the host keeps the only thing it
// was ever authoritative about — which pairs are importable at all.
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

	// One image, under `code` and *one of* `modifiers` — whichever this provider prefers. `EINVAL` for
	// a size or format this provider cannot express or an empty candidate list, `ENOMEM` where the
	// allocation failed, and whatever the device reported otherwise — the same vocabulary
	// `IRenderer::BindTargets` answers with, because a caller that cannot allocate a target set and a
	// renderer that cannot bind one are in the same position.
	//
	// **Which modifier was chosen is the answer rather than the question**, read back off the returned
	// buffer's own `Format()`. That is the property a parent compositor depends on: it is going to be
	// told a modifier when the buffer is offered to it, and a provider that reported one it had not
	// laid the image out under would hand over pixels the host reads wrong. So substituting freely
	// within the offered set is the whole point, and substituting outside it is still forbidden.
	[[nodiscard]] virtual Result<DmabufBuffer>
	Allocate(PixelSize<DeviceSpace> size, std::uint32_t code, std::span<const std::uint64_t> modifiers) = 0;

	// Whether `Allocate` would accept this format, without allocating to find out. A presenter asks
	// before it tears down a working target set for a reconfiguration it cannot honour.
	[[nodiscard]] virtual bool Supports(PixelFormat format) const noexcept = 0;

	// For a log line naming which provider a target set came from. Two providers that both worked on
	// the developer's machine and one that does not exist in CI is the failure this makes legible.
	[[nodiscard]] virtual std::string_view Name() const noexcept = 0;
};

// The list-taking verb's answer for a provider that produces exactly one modifier: the first entry it
// supports, which is what walking the candidates from outside used to do to it. This is the sentence
// the paragraph above promises, written once rather than in each of the two providers that need it.
[[nodiscard]] inline PixelFormat
FirstSupported(const IDmabufAllocator& allocator, std::uint32_t code, std::span<const std::uint64_t> modifiers) noexcept
{
	for (const std::uint64_t modifier : modifiers)
	{
		if (const PixelFormat candidate{ code, 0, modifier }; allocator.Supports(candidate))
		{
			return candidate;
		}
	}

	return {};
}
