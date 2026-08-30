#include "Protocol/Positioner.h"

#include <algorithm>

namespace
{
using Anchor = Wayland::Server::XdgPositionerAnchor;
using Gravity = Wayland::Server::XdgPositionerGravity;
using Adjustment = Wayland::Server::XdgPositionerConstraintAdjustment;

// One axis of the placement, so that the whole of the algorithm below is written once and applied
// twice. Everything the protocol says about x it says about y with the words changed, and a version
// of this file with both spelled out is one where the two drift.
struct Axis
{
	// Where the anchor point sits along this axis: the near edge, the far edge, or the middle.
	//
	// `-1`, `+1` and `0` rather than the enumerations, because the anchor and the gravity are the same
	// three positions asked in two different vocabularies — `top_left` is *near* on both axes — and
	// reducing them here is what lets the arithmetic below be four lines instead of two switches.
	int Anchor = 0;
	int Gravity = 0;
};

[[nodiscard]] Axis Horizontal(Anchor anchor, Gravity gravity) noexcept
{
	Axis axis;

	switch (anchor)
	{
		case Anchor::Left:
		case Anchor::TopLeft:
		case Anchor::BottomLeft:
			axis.Anchor = -1;
			break;
		case Anchor::Right:
		case Anchor::TopRight:
		case Anchor::BottomRight:
			axis.Anchor = 1;
			break;
		default:
			break;
	}

	switch (gravity)
	{
		case Gravity::Left:
		case Gravity::TopLeft:
		case Gravity::BottomLeft:
			axis.Gravity = -1;
			break;
		case Gravity::Right:
		case Gravity::TopRight:
		case Gravity::BottomRight:
			axis.Gravity = 1;
			break;
		default:
			break;
	}

	return axis;
}

[[nodiscard]] Axis Vertical(Anchor anchor, Gravity gravity) noexcept
{
	Axis axis;

	switch (anchor)
	{
		case Anchor::Top:
		case Anchor::TopLeft:
		case Anchor::TopRight:
			axis.Anchor = -1;
			break;
		case Anchor::Bottom:
		case Anchor::BottomLeft:
		case Anchor::BottomRight:
			axis.Anchor = 1;
			break;
		default:
			break;
	}

	switch (gravity)
	{
		case Gravity::Top:
		case Gravity::TopLeft:
		case Gravity::TopRight:
			axis.Gravity = -1;
			break;
		case Gravity::Bottom:
		case Gravity::BottomLeft:
		case Gravity::BottomRight:
			axis.Gravity = 1;
			break;
		default:
			break;
	}

	return axis;
}

// The near edge of the popup along one axis, before anything is done about the screen.
//
// The halving is integer division and matches every other compositor's, which matters more than it
// looks: a centred menu on an odd-width anchor lands one device pixel to one side, and a compositor
// that rounded the other way would put a toolkit's own arrow a pixel off the control it points at.
[[nodiscard]] std::int32_t
Place(Axis axis, std::int32_t near, std::int32_t span, std::int32_t extent, std::int32_t offset) noexcept
{
	const std::int32_t anchor = axis.Anchor < 0 ? near : (axis.Anchor > 0 ? near + span : near + span / 2);
	const std::int32_t gravity = axis.Gravity < 0 ? -extent : (axis.Gravity > 0 ? 0 : -(extent / 2));

	return anchor + gravity + offset;
}

// How much of `extent` from `near` lies inside `[low, high)`. Zero where the two do not meet at all.
[[nodiscard]] std::int32_t Overlap(std::int32_t near, std::int32_t extent, std::int32_t low, std::int32_t high) noexcept
{
	return std::max(0, std::min(near + extent, high) - std::max(near, low));
}

// The axis with its anchor and its gravity mirrored, which is the whole of what a flip is. The offset
// is deliberately untouched — see the header.
[[nodiscard]] Axis Flipped(Axis axis) noexcept
{
	return { .Anchor = -axis.Anchor, .Gravity = -axis.Gravity };
}

// One axis resolved against one span of screen. `near` and `extent` are updated in place, because the
// resize adjustment changes the size as well as the position and the two have to move together.
void Constrain(
	Axis axis,
	std::int32_t anchorNear,
	std::int32_t anchorSpan,
	std::int32_t offset,
	std::int32_t low,
	std::int32_t high,
	bool flip,
	bool slide,
	bool resize,
	std::int32_t& near,
	std::int32_t& extent
) noexcept
{
	const auto fits = [&](std::int32_t at, std::int32_t size) noexcept { return at >= low && at + size <= high; };

	if (fits(near, extent))
	{
		return;
	}

	// **Flip first, and it is accepted on the weaker of two tests.** A flipped placement that fits is
	// obviously right; one that merely fits *better* is taken too, which is Mutter's behaviour and is
	// what a person experiences as a menu opening upwards near the bottom of a screen even where it is
	// too tall to fit either way. Taking only the perfect fit leaves that menu hanging off the bottom
	// edge with its first item off-screen.
	if (flip)
	{
		const std::int32_t flipped = Place(Flipped(axis), anchorNear, anchorSpan, extent, offset);

		if (fits(flipped, extent) || Overlap(flipped, extent, low, high) > Overlap(near, extent, low, high))
		{
			near = flipped;
		}
	}

	if (!fits(near, extent) && slide)
	{
		// The far edge first: a popup pushed off the right of a screen slides left until it fits, but
		// never past the left edge, because a popup wider than the screen has to lose one end and the end
		// a person reads from is the near one.
		if (near + extent > high)
		{
			near = std::max(low, high - extent);
		}
		else if (near < low)
		{
			near = low;
		}
	}

	if (!fits(near, extent) && resize)
	{
		const std::int32_t left = std::max(near, low);

		extent = std::max(0, std::min(near + extent, high) - left);
		near = left;
	}
}
} // namespace

PixelRect<SurfaceSpace> PlacePopup(const PopupPlacement& rules) noexcept
{
	const Axis horizontal = Horizontal(rules.Anchor, rules.Gravity);
	const Axis vertical = Vertical(rules.Anchor, rules.Gravity);

	const std::int32_t x =
		Place(horizontal, rules.AnchorRect.Left(), rules.AnchorRect.Extent.Width, rules.Extent.Width, rules.Offset.X);
	const std::int32_t y =
		Place(vertical, rules.AnchorRect.Top(), rules.AnchorRect.Extent.Height, rules.Extent.Height, rules.Offset.Y);

	return { { x, y }, rules.Extent };
}

PixelRect<SurfaceSpace> PlacePopup(const PopupPlacement& rules, PixelRect<SurfaceSpace> bounds) noexcept
{
	PixelRect<SurfaceSpace> placed = PlacePopup(rules);

	if (bounds.IsEmpty())
	{
		return placed;
	}

	const Adjustment adjustment = rules.Adjustment;

	Constrain(
		Horizontal(rules.Anchor, rules.Gravity),
		rules.AnchorRect.Left(),
		rules.AnchorRect.Extent.Width,
		rules.Offset.X,
		bounds.Left(),
		bounds.Right(),
		Any(adjustment & Adjustment::FlipX),
		Any(adjustment & Adjustment::SlideX),
		Any(adjustment & Adjustment::ResizeX),
		placed.Origin.X,
		placed.Extent.Width
	);

	Constrain(
		Vertical(rules.Anchor, rules.Gravity),
		rules.AnchorRect.Top(),
		rules.AnchorRect.Extent.Height,
		rules.Offset.Y,
		bounds.Top(),
		bounds.Bottom(),
		Any(adjustment & Adjustment::FlipY),
		Any(adjustment & Adjustment::SlideY),
		Any(adjustment & Adjustment::ResizeY),
		placed.Origin.Y,
		placed.Extent.Height
	);

	return placed;
}
