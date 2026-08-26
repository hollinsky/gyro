#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <variant>

#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"

// Which of a frame's draw items go on planes, and which are left for the GPU.
//
// **A partition of the evaluated draw list with no memory, recomputed every frame, rather than state a
// surface enters and leaves.** That is decision 152, and the whole of what this file is. Nothing here
// is sticky: an item is promoted this frame and composited the next, and the two frames are the same
// picture. Treating *scanout candidate* as per-surface state is what every other compositor does, and
// every glitch it produces — a video hitching as it goes fullscreen, hitching again as it is dragged a
// pixel off the edge — is a transition it got wrong. A partition with no state has no transitions.
//
// **The promoted set is a suffix of the list, and that is what makes the composite need no hole.** The
// draw list is bottom-first painter's order, and gyro's composite goes on the primary plane with
// promoted layers above it, so an item can only be promoted if everything drawn over it is promoted
// too. Taking a suffix makes that true by construction — and it means the composite is simply the
// prefix, drawn exactly as it would have been, with nothing cut out of it. The alternative, promoting
// an item out of the middle, requires the composite to be transparent where the promoted layer lands,
// which means an alpha channel on a scanout target and a renderer that can be told to leave a hole.
// Both are real and neither is needed for the arrangement that pays: a fullscreen window under nothing,
// and a pointer over everything.
//
// **The frame thread runs this and it allocates nothing.** Fixed arrays sized from `MaxLayers`, one
// pass over the list from the top, and no ioctl — asking the hardware is `IPresenter::TestLayers` and
// is the caller's business, because it is the expensive half and the one worth caching.

namespace Frame
{
// What one frame's draw list was split into.
struct Partition
{
	// The draw items promoted onto planes, bottom-first, as indices into the list handed in. This is a
	// suffix of that list in the same order, so `Items[0]` is the lowest promoted item.
	std::array<std::uint32_t, MaxLayers> Items{};
	std::uint32_t Count = 0;

	// How many items are left for the GPU: the prefix `[0, Composited)` of the list.
	std::uint32_t Composited = 0;

	// Whether a composite has to happen at all.
	//
	// **False is the arrangement the whole mechanism exists for.** A screen whose every item promoted
	// needs no render pass, no target acquired and no queue submission — which is the tablet playing
	// video with the GPU asleep, and it is worth the branch it costs the frame loop. The bottom-most
	// promoted layer lands on the primary plane in that case, which is what keeps the CRTC showing
	// something.
	[[nodiscard]] constexpr bool NeedsComposite() const noexcept { return Composited != 0; }

	// The layers a presenter is handed: the composite where there is one, then the promoted items.
	[[nodiscard]] constexpr std::uint32_t Layers() const noexcept { return Count + (NeedsComposite() ? 1U : 0U); }

	friend constexpr bool operator==(const Partition&, const Partition&) noexcept = default;
};

// Whether this item could be handed to a plane at all, before any hardware is asked.
//
// **Two clauses, and both of them are the display engine's rather than gyro's taste.** A plane has no
// corner radius, no material and no shadow, and it samples its source through a fixed-function path
// that is not decision 56's mip chain. So the set of promotable items is not a policy this function
// picks — it is what Seam/Dressing.h can express that a plane cannot, which is nearly all of it.
//
// **The scale clause is gyro's, and it is the one that is still argued.**
// `TransformClass::IsPlaneExpressible` deliberately admits a fractional scale, on the reading that
// decision 56 makes minification ordinary and that refusing it forecloses promotion on every
// fractionally scaled output. Decision 152 takes the other side for now — a hardware scaler is a
// visible sharpness change on the frame a window is handed over — so the geometric question and the
// sharpness question are asked separately here rather than folded into one predicate. When there is a
// panel to measure the scaler against, this is the one line that moves.
[[nodiscard]] constexpr bool IsPromotable(const DrawItem& item) noexcept
{
	// Only a texture has anything a plane could scan out. A solid could be a plane's background colour
	// on hardware that has one, a group is an offscreen the GPU has to produce, and a dressing draws
	// nothing but its own decoration.
	if (!std::holds_alternative<DrawTexture>(item.Content))
	{
		return false;
	}

	// A shadow is separable in principle — it is drawn around an opaque quad, so it could stay in the
	// composite while the quad goes to a plane — and doing that means re-emitting the item as a
	// `DrawDressing` with no content, which is a list this function is not allowed to rewrite. So it is
	// refused here and the door is named rather than left implicit.
	if (item.Lift.Draws())
	{
		return false;
	}

	// A radius is not separable, and it is the reason why: cutting the corner is what reveals what is
	// underneath, and a plane scans out its whole rectangle.
	if (item.Radius != 0.0F || item.Dress != Material::None || item.Opacity != 1.0F)
	{
		return false;
	}

	return item.Sampling.IsPlaneExpressible() && item.Sampling.IsResampleFree();
}

// Split a frame's draw list. `ceiling` is `IPresenter::LayerCeiling` — the planes this output has —
// and nothing wider than it is ever proposed.
[[nodiscard]] constexpr Partition Assign(std::span<const DrawItem> items, std::uint32_t ceiling) noexcept
{
	Partition partition{};
	partition.Composited = static_cast<std::uint32_t>(items.size());

	if (items.empty() || ceiling == 0)
	{
		return partition;
	}

	const std::uint32_t most = ceiling < MaxLayers ? ceiling : MaxLayers;

	// From the top down, because the promoted set is a suffix. The first item that cannot be promoted
	// stops the walk: anything below it is drawn under something the composite is responsible for, and
	// putting it on a plane above the composite would draw it over that instead.
	std::uint32_t taken = 0;

	while (taken < most && taken < items.size())
	{
		const std::uint32_t index = static_cast<std::uint32_t>(items.size()) - 1 - taken;

		if (!IsPromotable(items[index]))
		{
			break;
		}

		++taken;
	}

	// One layer has to be left for the composite unless the composite has become empty — the whole list
	// promoted, which is the arrangement worth having and the one case where the count may reach the
	// ceiling exactly.
	if (taken < items.size() && taken == most)
	{
		--taken;
	}

	partition.Count = taken;
	partition.Composited = static_cast<std::uint32_t>(items.size()) - taken;

	for (std::uint32_t slot = 0; slot < taken; ++slot)
	{
		partition.Items[slot] = partition.Composited + slot;
	}

	return partition;
}
} // namespace Frame
