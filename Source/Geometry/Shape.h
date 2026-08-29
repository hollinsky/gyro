#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "Geometry/Space.h"

// A surface-local shape: the sequence of adds and subtracts a client hands the compositor to say
// where it is opaque and where it accepts input.
//
// **This is not `Geometry/Region.h` and could not be.** That one is the damage set — it has no
// subtract, and when it runs out of rectangles it *collapses to its bounding box*, which is exactly
// right for damage (repainting more than was dirtied costs time and is never wrong) and exactly wrong
// here. An input shape that collapsed outward would swallow the clicks belonging to the window behind
// it, and a person would experience that as a dead strip beside a window that is not there. The two
// types are neighbours in this module so that each can say what the other is for; they share nothing
// else, which is why this one is not called a region.
//
// **It is here rather than in `Protocol` because the hit test is `Scene`'s.** A client mints one of
// these over the wire and `Scene/Hit.h` is what asks it a question, and decision 87 forbids the two
// meeting at a waist — the same argument that put `Core/Buffer.h` below `Protocol` rather than beside
// the interface that carries it. Nothing here names a surface, a client, or a connection: it is a set
// of integer rectangles and one predicate over them.
//
// **The shape is kept as the client's own sequence, and never reduced.** The obvious alternative is
// to maintain a canonical disjoint rectangle set on every request, which is where a compositor grows
// a region algebra — and the reason not to is that the only question ever asked of an input shape is
// *is this pointer inside it*, and the op list answers that exactly, in one backward pass, with no
// coalescing to get wrong. The last op whose rectangle covers the point decides; if none does, the
// point is outside. Subtraction needs no representation at all.
//
// What that costs is a linear scan per hit test rather than a logarithmic one, against a list that is
// a handful of entries for every real client — GTK sets a rounded-corner input shape as a few dozen
// rows, and a shaped window is the outlier rather than the norm. If a client ever appears for which
// that is not true, the fix is a bounding-box reject in front of the scan, which is why `Bounds` is
// here.

// SPEC: how many rectangles one shape may hold. It bounds a client's ability to make gyro allocate,
// rather than estimating a working set — decision 27's rule, an order of magnitude above anything
// real. A rounded rectangle at 4K is a few hundred rows at worst.
//
// A client past it is ended with `no_memory`, which is the truth: gyro declined to allocate on its
// behalf. The alternative is to drop the surplus, and both directions of that are worse than saying
// so — a dropped add is a window with a hole in it that swallows nothing, a dropped subtract is a
// window that swallows a neighbour's clicks, and neither reaches the client as anything but a bug in
// the compositor.
inline constexpr std::uint32_t MaxShapeRects = 4096;

// One request's worth of shape.
struct ShapeRect
{
	PixelRect<SurfaceSpace> Rect;

	// Whether this rectangle was taken away rather than added. The field is the whole of subtraction's
	// representation.
	bool Subtract = false;

	friend constexpr bool operator==(const ShapeRect&, const ShapeRect&) noexcept = default;
};

// The shape itself, as a value.
//
// **A value rather than the protocol object**, because `wl_surface.set_input_region` copies: the
// protocol says a later change to the `wl_region` does not affect a surface already given it, so a
// surface holds one of these and not a pointer to the client's object. That is also what makes a
// shape outlive its `wl_region` being destroyed, which every real toolkit does immediately after
// setting it.
class SurfaceShape
{
public:
	SurfaceShape() = default;

	// Whether there is room for one more. The caller answers a full shape, because what it does about
	// it is end the client and only the caller holds the resource to end.
	[[nodiscard]] bool IsFull() const noexcept { return m_Rects.size() >= MaxShapeRects; }

	void Add(PixelRect<SurfaceSpace> rect) { m_Rects.push_back(ShapeRect{ .Rect = rect, .Subtract = false }); }

	void Subtract(PixelRect<SurfaceSpace> rect) { m_Rects.push_back(ShapeRect{ .Rect = rect, .Subtract = true }); }

	void Clear() noexcept { m_Rects.clear(); }

	// Whether the point is inside the shape. Exact: the last op covering it decides, which is what the
	// sequence of adds and subtracts means.
	//
	// **The point is real-valued and nothing rounds it**, which is the overload that matters and the
	// reason there is not one taking a grid coordinate alone. A client states its shape on the integer
	// grid because `wl_region` has no other spelling, and a pointer arrives somewhere between two of
	// those columns — so the two are compared on their own terms and a half-open rectangle from 10 to
	// 11 holds every position from 10 up to 11. Flooring the pointer first would be the same answer
	// written twice, and rounding it would move the edge of every window by half a pixel.
	[[nodiscard]] bool Contains(Point<SurfaceSpace> point) const noexcept
	{
		return Covers(static_cast<double>(point.X), static_cast<double>(point.Y));
	}

	[[nodiscard]] bool Contains(PixelPoint<SurfaceSpace> point) const noexcept
	{
		return Covers(static_cast<double>(point.X), static_cast<double>(point.Y));
	}

	// The union of everything added, which bounds the shape from outside — subtraction only ever
	// removes, so nothing outside this can be inside the shape. Empty where nothing was added.
	//
	// Inline because this module has no translation unit and should not grow one for forty lines: an
	// INTERFACE library is what keeps `Geometry` free of a link order, and the walk below runs when a
	// client sets a shape rather than per hit test.
	[[nodiscard]] PixelRect<SurfaceSpace> Bounds() const noexcept
	{
		bool any = false;
		std::int32_t left = 0;
		std::int32_t top = 0;
		std::int32_t right = 0;
		std::int32_t bottom = 0;

		for (const ShapeRect& entry : m_Rects)
		{
			// Subtraction cannot put a point outside the union of the adds, so the bound is over the adds
			// alone. A subtracted rectangle reaching past them describes nothing.
			if (entry.Subtract || entry.Rect.IsEmpty())
			{
				continue;
			}

			if (!any)
			{
				any = true;
				left = entry.Rect.Left();
				top = entry.Rect.Top();
				right = entry.Rect.Right();
				bottom = entry.Rect.Bottom();

				continue;
			}

			left = std::min(left, entry.Rect.Left());
			top = std::min(top, entry.Rect.Top());
			right = std::max(right, entry.Rect.Right());
			bottom = std::max(bottom, entry.Rect.Bottom());
		}

		if (!any)
		{
			return {};
		}

		return PixelRect<SurfaceSpace>::FromEdges({ left, top }, { right, bottom });
	}

	// Whether the client has said anything at all. **Not whether the shape is empty**: a shape built by
	// adding a rectangle and subtracting the same one covers nothing and is not this. The distinction
	// is deliberate and the name is the narrower claim, because the one caller that wants it — a
	// surface deciding whether the client ever set a shape — is asking this question and not the
	// geometric one.
	[[nodiscard]] bool IsUnset() const noexcept { return m_Rects.empty(); }

	[[nodiscard]] std::span<const ShapeRect> Rects() const noexcept { return m_Rects; }

	friend bool operator==(const SurfaceShape&, const SurfaceShape&) noexcept = default;

private:
	// One scan, backwards, in the widest of the coordinate types involved — so the two overloads above
	// are one implementation and cannot answer differently about a point that is exactly on an edge.
	[[nodiscard]] bool Covers(double x, double y) const noexcept
	{
		for (auto entry = m_Rects.rbegin(); entry != m_Rects.rend(); ++entry)
		{
			const PixelRect<SurfaceSpace>& rect = entry->Rect;

			if (x >= rect.Left() && x < rect.Right() && y >= rect.Top() && y < rect.Bottom())
			{
				return !entry->Subtract;
			}
		}

		return false;
	}

	std::vector<ShapeRect> m_Rects;
};
