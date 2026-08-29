#include "Protocol/Region.h"

#include <algorithm>

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
