#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "Geometry/Space.h"
#include "Wayland/Server/Wayland.h"

// A `wl_region`: the surface-local shape a client hands the compositor to say where it is opaque and
// where it accepts input.
//
// **This is not `Geometry/Region.h` and could not be.** That one is the damage set — it has no
// subtract, and when it runs out of rectangles it *collapses to its bounding box*, which is exactly
// right for damage (repainting more than was dirtied costs time and is never wrong) and exactly wrong
// here. An input region that collapsed outward would swallow the clicks belonging to the window
// behind it, and a person would experience that as a dead strip beside a window that is not there.
// The two types share a word and nothing else.
//
// **The shape is kept as the client's own sequence of adds and subtracts, and never reduced.** The
// obvious alternative is to maintain a canonical disjoint rectangle set on every request, which is
// where a compositor grows a region algebra — and the reason not to is that the only question ever
// asked of an input region is *is this pointer inside it*, and the op list answers that exactly, in
// one backward pass, with no coalescing to get wrong. The last op whose rectangle covers the point
// decides; if none does, the point is outside. Subtraction needs no representation at all.
//
// What that costs is a linear scan per hit test rather than a logarithmic one, against a list that is
// a handful of entries for every real client — GTK sets a rounded-corner input region as a few dozen
// rows, and a shaped window is the outlier rather than the norm. If a client ever appears for which
// that is not true, the fix is a bounding-box reject in front of the scan, which is why `Bounds` is
// here.

// SPEC: how many rectangles one region may hold. It bounds a client's ability to make gyro allocate,
// rather than estimating a working set — decision 27's rule, an order of magnitude above anything
// real. A rounded rectangle at 4K is a few hundred rows at worst.
//
// A client past it is ended with `no_memory`, which is the truth: gyro declined to allocate on its
// behalf. The alternative is to drop the surplus, and both directions of that are worse than saying
// so — a dropped add is a window with a hole in it that swallows nothing, a dropped subtract is a
// window that swallows a neighbour's clicks, and neither reaches the client as anything but a bug in
// the compositor.
inline constexpr std::uint32_t MaxRegionRects = 4096;

// One request's worth of shape.
struct RegionRect
{
	PixelRect<SurfaceSpace> Rect;

	// Whether this rectangle was taken away rather than added. The field is the whole of subtraction's
	// representation.
	bool Subtract = false;

	friend constexpr bool operator==(const RegionRect&, const RegionRect&) noexcept = default;
};

// The shape itself, as a value.
//
// **A value rather than the protocol object**, because `wl_surface.set_input_region` copies: the
// protocol says a later change to the `wl_region` does not affect a surface already given it, so a
// surface holds one of these and not a pointer to the client's object. That is also what makes a
// region outlive its `wl_region` being destroyed, which every real toolkit does immediately after
// setting it.
class SurfaceRegion
{
public:
	SurfaceRegion() = default;

	// Whether there is room for one more. The caller answers a full region, because what it does about
	// it is end the client and only the caller holds the resource to end.
	[[nodiscard]] bool IsFull() const noexcept { return m_Rects.size() >= MaxRegionRects; }

	void Add(PixelRect<SurfaceSpace> rect) { m_Rects.push_back(RegionRect{ .Rect = rect, .Subtract = false }); }

	void Subtract(PixelRect<SurfaceSpace> rect) { m_Rects.push_back(RegionRect{ .Rect = rect, .Subtract = true }); }

	void Clear() noexcept { m_Rects.clear(); }

	// Whether the point is inside the shape. Exact: the last op covering it decides, which is what the
	// sequence of adds and subtracts means.
	[[nodiscard]] bool Contains(PixelPoint<SurfaceSpace> point) const noexcept
	{
		for (auto entry = m_Rects.rbegin(); entry != m_Rects.rend(); ++entry)
		{
			const PixelRect<SurfaceSpace>& rect = entry->Rect;

			if (point.X >= rect.Left() && point.X < rect.Right() && point.Y >= rect.Top() && point.Y < rect.Bottom())
			{
				return !entry->Subtract;
			}
		}

		return false;
	}

	// The union of everything added, which bounds the shape from outside — subtraction only ever
	// removes, so nothing outside this can be inside the region. Empty where nothing was added.
	[[nodiscard]] PixelRect<SurfaceSpace> Bounds() const noexcept;

	// Whether the client has said anything at all. **Not whether the shape is empty**: a region built
	// by adding a rectangle and subtracting the same one covers nothing and is not this. The
	// distinction is deliberate and the name is the narrower claim, because the one caller that wants
	// it — a surface deciding whether the client ever set a region — is asking this question and not
	// the geometric one.
	[[nodiscard]] bool IsUnset() const noexcept { return m_Rects.empty(); }

	[[nodiscard]] std::span<const RegionRect> Rects() const noexcept { return m_Rects; }

	friend bool operator==(const SurfaceRegion&, const SurfaceRegion&) noexcept = default;

private:
	std::vector<RegionRect> m_Rects;
};

// The `wl_region` object: a `SurfaceRegion` a client is still building.
//
// It deletes itself in `OnGone` because its lifetime is the client's object and nothing else refers
// to it — a surface given this region took a copy, per `SurfaceRegion` above.
class ClientRegion final : public Wayland::Server::WlRegionHandler
{
public:
	ClientRegion() = default;

	[[nodiscard]] const SurfaceRegion& Shape() const noexcept { return m_Shape; }

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs and `OnGone` follows immediately, so there is
	// nothing for the destructor request itself to do.
	void OnDestroy() override {}

	void OnAdd(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override;

	void OnSubtract(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override;

private:
	// A rectangle as the wire spells one: an origin and an extent, both surface-local and integral,
	// with a negative extent clamped away rather than carried. The protocol does not forbid a client
	// sending one, and a rect whose right edge is left of its left edge would make `Contains` answer
	// for an inverted interval.
	[[nodiscard]] static PixelRect<SurfaceSpace>
	FromWire(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) noexcept;

	// True once the client has been ended for overrunning `MaxRegionRects`, so the second request
	// after that does not post a second error onto a connection already going away.
	bool m_Overrun = false;

	SurfaceRegion m_Shape;
};
