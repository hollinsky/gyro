#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>

#include "Geometry/Space.h"

// Damage: the part of an image that changed, as a bounded set of integer rectangles.
//
// **Here rather than in Seam, and reachability is why.** It reads as seam data — it is what
// `IPresenter::Present` takes, it becomes `FB_DAMAGE_CLIPS` on KMS and `wl_surface.damage_buffer`
// nested — and every one of those arguments is about the *consumer*. The producers settle it the
// other way: a client's damage rectangles are wire knowledge, so `Protocol` receives them and
// `Scene` stores them, and neither module may name `Seam` by the graph. A type both sides must name
// lives in `Core`, `Geometry`, or a waist both can reach; `Geometry` is where this one lands because
// it is rect arithmetic over the coordinate spaces and `Rect<S, T>` is already here. See
// Docs/Structure.md#region-is-in-geometry-and-reachability-is-why.
//
// **Bounded, because it is frame-side, and the bound has a correct answer rather than a policy.**
// Decision 36 forbids allocation inside the frame section, so the capacity is fixed and something has
// to happen when a seventeenth rectangle arrives. Here — unlike Publication/Return.h, where a dropped
// release is a client that never draws again — overflow has an answer that cannot be wrong: **damage
// is conservative, so a superset is always correct.** A region that has lost track of its detail
// collapses to its own bounding rectangle and costs bandwidth. A region that lost a rectangle would
// leave stale pixels on the glass, which is the one thing this type must not do. So the policy is not
// a tradeoff between two failures; it is the only direction that is not a defect.
//
// **Which makes the capacity a tuning number and says so.** Sixteen is enough for the shapes damage
// actually takes — a cursor, a caret, a handful of dirty tiles, a moving window's leading and
// trailing edges — and past it the collapse costs a larger composite rather than a wrong one. Moving
// it changes bandwidth and nothing else, which is exactly the kind of constant that should be easy to
// move.
//
// **Templated on the space, because damage is not always in one.** A layer's damage is in the
// coordinates of the buffer being scanned out — that is what `FB_DAMAGE_CLIPS` and
// `wl_surface.damage_buffer` both take — while the renderer's scissor is in the composite target's
// device grid. Those are different spaces in the sense Geometry/Space.h means, and the whole point of
// that file is that a value in the wrong one does not compile. Producing damage in the space it is
// exactly known in also keeps a rounding out of the backend: mapping a device-space rectangle back
// through a crop and a scale would round three times in three implementations, and rounding damage
// inward is how a moving edge leaves a one-pixel trail.
//
// **What this is deliberately not.** There is no subtraction, no exact union, and no sorted
// canonical form — that is a region algebra, and every caller here wants a conservative cover rather
// than a minimal one. The merging Add does is the cheap half that pays for itself: a rectangle
// already covered is dropped, and rectangles a new one covers are dropped, which is what keeps a
// caret blinking in one place from filling the set.

template<SpaceTag S>
class Region
{
public:
	// Rectangles before the set collapses to its bounds. See above for why this is a bandwidth number
	// rather than a correctness one.
	static constexpr std::size_t Capacity = 16;

	constexpr Region() = default;

	constexpr explicit Region(PixelRect<S> rect) { Add(rect); }

	constexpr void Clear() noexcept
	{
		m_Count = 0;
		m_Collapsed = false;
	}

	// An empty rectangle is not damage and is dropped rather than stored, so that a caller which
	// computes an intersection does not have to check before adding it.
	constexpr void Add(PixelRect<S> rect) noexcept
	{
		if (rect.IsEmpty())
		{
			return;
		}

		if (m_Collapsed)
		{
			m_Rects[0] = Union(m_Rects[0], rect);
			return;
		}

		std::size_t kept = 0;

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			if (Contains(m_Rects[index], rect))
			{
				return;
			}

			// Kept in place rather than swapped to the end, because emission order is the order damage
			// was reported and a backend that walks the array should see that rather than a shuffle.
			if (!Contains(rect, m_Rects[index]))
			{
				m_Rects[kept] = m_Rects[index];
				++kept;
			}
		}

		m_Count = kept;

		if (m_Count == Capacity)
		{
			Collapse(rect);
			return;
		}

		m_Rects[m_Count] = rect;
		++m_Count;
	}

	constexpr void Add(const Region& other) noexcept
	{
		for (std::size_t index = 0; index < other.m_Count; ++index)
		{
			Add(other.m_Rects[index]);
		}
	}

	// Clipping to the image's own extent, which every producer owes its consumer: a client may report
	// damage outside the buffer it attached, and KMS rejects a damage clip that leaves the framebuffer.
	constexpr void Intersect(PixelRect<S> clip) noexcept
	{
		std::size_t kept = 0;

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			const PixelRect<S> clipped = Intersection(m_Rects[index], clip);

			if (!clipped.IsEmpty())
			{
				m_Rects[kept] = clipped;
				++kept;
			}
		}

		m_Count = kept;

		if (m_Count == 0)
		{
			m_Collapsed = false;
		}
	}

	// Outward by a margin, for the resampling filter's support radius. Docs/Architecture.md's *where
	// the integers are* requires it: content that moved is resampled, the filter reads outside the
	// rectangle it writes, and damage that ignores the kernel footprint leaves trails that no
	// screenshot shows.
	//
	// Saturating rather than wrapping. The arithmetic is done at 64 bits and clamped, because a
	// rectangle near the representable edge is a bug somewhere upstream and an overflow here would
	// turn it into damage that reports as empty — the one failure this type refuses.
	constexpr void Expand(std::int32_t margin) noexcept
	{
		if (margin <= 0)
		{
			return;
		}

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			const PixelRect<S>& rect = m_Rects[index];

			const std::int64_t left = static_cast<std::int64_t>(rect.Origin.X) - margin;
			const std::int64_t top = static_cast<std::int64_t>(rect.Origin.Y) - margin;
			const std::int64_t right = static_cast<std::int64_t>(rect.Right()) + margin;
			const std::int64_t bottom = static_cast<std::int64_t>(rect.Bottom()) + margin;

			m_Rects[index] =
				PixelRect<S>::FromEdges({ Saturate(left), Saturate(top) }, { Saturate(right), Saturate(bottom) });
		}
	}

	[[nodiscard]] constexpr bool IsEmpty() const noexcept { return m_Count == 0; }

	// True once detail has been given up. Nothing branches on this — the region is correct either way
	// — and it is here because a frame that composites more than it should is otherwise invisible, and
	// the number worth tuning is the one above.
	[[nodiscard]] constexpr bool IsCollapsed() const noexcept { return m_Collapsed; }

	[[nodiscard]] constexpr std::span<const PixelRect<S>> Rects() const noexcept { return { m_Rects.data(), m_Count }; }

	[[nodiscard]] constexpr PixelRect<S> Bounds() const noexcept
	{
		if (m_Count == 0)
		{
			return {};
		}

		PixelRect<S> bounds = m_Rects[0];

		for (std::size_t index = 1; index < m_Count; ++index)
		{
			bounds = Union(bounds, m_Rects[index]);
		}

		return bounds;
	}

	// Set equality is deliberately not offered. Two regions covering the same pixels can hold
	// different rectangles, and an operator== that answered *the same rectangles in the same order*
	// under the name of equality would be a trap in exactly the tests that would use it. Compare
	// Rects() or Bounds(), which say which question is being asked.

private:
	constexpr void Collapse(PixelRect<S> withRect) noexcept
	{
		PixelRect<S> bounds = Union(Bounds(), withRect);

		m_Rects[0] = bounds;
		m_Count = 1;
		m_Collapsed = true;
	}

	[[nodiscard]] static constexpr std::int32_t Saturate(std::int64_t value) noexcept
	{
		constexpr std::int64_t Low = std::numeric_limits<std::int32_t>::min();
		constexpr std::int64_t High = std::numeric_limits<std::int32_t>::max();

		return static_cast<std::int32_t>(std::clamp(value, Low, High));
	}

	[[nodiscard]] static constexpr PixelRect<S> Union(PixelRect<S> left, PixelRect<S> right) noexcept
	{
		if (left.IsEmpty())
		{
			return right;
		}

		if (right.IsEmpty())
		{
			return left;
		}

		return PixelRect<S>::FromEdges(
			{ std::min(left.Left(), right.Left()), std::min(left.Top(), right.Top()) },
			{ std::max(left.Right(), right.Right()), std::max(left.Bottom(), right.Bottom()) }
		);
	}

	[[nodiscard]] static constexpr PixelRect<S> Intersection(PixelRect<S> left, PixelRect<S> right) noexcept
	{
		const std::int32_t leftEdge = std::max(left.Left(), right.Left());
		const std::int32_t topEdge = std::max(left.Top(), right.Top());
		const std::int32_t rightEdge = std::min(left.Right(), right.Right());
		const std::int32_t bottomEdge = std::min(left.Bottom(), right.Bottom());

		if (rightEdge <= leftEdge || bottomEdge <= topEdge)
		{
			return {};
		}

		return PixelRect<S>::FromEdges({ leftEdge, topEdge }, { rightEdge, bottomEdge });
	}

	[[nodiscard]] static constexpr bool Contains(PixelRect<S> outer, PixelRect<S> inner) noexcept
	{
		return outer.Left() <= inner.Left() && outer.Top() <= inner.Top() && outer.Right() >= inner.Right() &&
		       outer.Bottom() >= inner.Bottom();
	}

	std::array<PixelRect<S>, Capacity> m_Rects{};
	std::size_t m_Count = 0;
	bool m_Collapsed = false;
};

// Prints as device{2: [0, 0 64x64], [100, 100 8x16]}, and a collapsed one says so. The space comes
// from the rectangles, which already carry it.
template<SpaceTag S>
struct std::formatter<Region<S>>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const Region<S>& region, Context& context) const
	{
		if (region.IsEmpty())
		{
			return std::format_to(context.out(), "{}{{empty}}", S::Name);
		}

		auto out = std::format_to(context.out(), "{}{{", S::Name);

		if (region.IsCollapsed())
		{
			out = std::format_to(out, "collapsed");
		}
		else
		{
			out = std::format_to(out, "{}", region.Rects().size());
		}

		const char* separator = ": ";

		for (const PixelRect<S>& rect : region.Rects())
		{
			out = std::format_to(
				out, "{}[{}, {} {}x{}]", separator, rect.Origin.X, rect.Origin.Y, rect.Extent.Width, rect.Extent.Height
			);
			separator = ", ";
		}

		return std::format_to(out, "}}");
	}
};
