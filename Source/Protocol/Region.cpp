#include "Protocol/Region.h"

#include <algorithm>

PixelRect<SurfaceSpace> SurfaceRegion::Bounds() const noexcept
{
	bool any = false;
	std::int32_t left = 0;
	std::int32_t top = 0;
	std::int32_t right = 0;
	std::int32_t bottom = 0;

	for (const RegionRect& entry : m_Rects)
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

PixelRect<SurfaceSpace>
ClientRegion::FromWire(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) noexcept
{
	return { { x, y }, { std::max(width, 0), std::max(height, 0) } };
}

void ClientRegion::OnAdd(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
	if (m_Overrun)
	{
		return;
	}

	if (m_Shape.IsFull())
	{
		m_Overrun = true;
		Object().PostNoMemory();

		return;
	}

	m_Shape.Add(FromWire(x, y, width, height));
}

void ClientRegion::OnSubtract(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
	if (m_Overrun)
	{
		return;
	}

	if (m_Shape.IsFull())
	{
		m_Overrun = true;
		Object().PostNoMemory();

		return;
	}

	m_Shape.Subtract(FromWire(x, y, width, height));
}
