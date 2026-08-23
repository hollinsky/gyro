#include "Virtual/Sink.h"

#include <algorithm>
#include <cstring>

CapturingSink::CapturingSink(
	PixelSize<DeviceSpace> size,
	PixelFormat format,
	std::size_t depth,
	FrameRetention retention
)
	: m_Size{ size }, m_Format{ format }, m_Depth{ std::max<std::size_t>(depth, 1) }, m_Retention{ retention }
{
	const std::uint32_t bytesPerPixel = DecodableBytesPerPixel(format.Code);

	if (bytesPerPixel == 0 || !size.IsValid() || size.IsEmpty())
	{
		// Nothing is allocated and every frame will be skipped, which is the honest outcome for a
		// shape this sink cannot hold. Reporting it through `Skipped()` rather than through a
		// constructor that fails keeps the sink a member the way `VirtualOutput` is one.
		m_Depth = 0;

		return;
	}

	m_Stride = static_cast<std::uint32_t>(size.Width) * bytesPerPixel;

	// The whole capture window, once, here — which is the only allocation this object ever makes and
	// is why `OnFrame` may run where it does.
	m_Pixels.resize(m_Stride * static_cast<std::size_t>(size.Height) * m_Depth);
	m_Frames.reserve(m_Depth);
	m_Held.reserve(MaxVirtualTargets);
}

void CapturingSink::OnFrame(VirtualOutput& output, const VirtualFrame& frame)
{
	const DmabufBuffer* buffer = output.Buffer(frame.Target);

	if (buffer == nullptr || m_Depth == 0)
	{
		++m_Skipped;
		Retire(output, frame);

		return;
	}

	// The sync bracket is held across the copy and released before the frame is given back, which is
	// what `DmabufBuffer::CpuRead` is for — an exporter with real cache maintenance needs both ends,
	// and the end is the half that gets forgotten.
	const BufferReader reader{ *buffer };

	if (!reader.IsValid() || reader.Image().Size() != m_Size || reader.Image().Format() != m_Format)
	{
		++m_Skipped;
		Retire(output, frame);

		return;
	}

	// Oldest evicted, which is a rotation of the window rather than a shuffle of the pixels: the slot
	// index is the frame count modulo the depth, so nothing is ever moved.
	const std::size_t slot = m_Copied % m_Depth;
	const ImageView& image = reader.Image();

	for (std::int32_t y = 0; y < m_Size.Height; ++y)
	{
		std::byte* into = m_Pixels.data() + slot * m_Stride * static_cast<std::size_t>(m_Size.Height) +
		                  static_cast<std::size_t>(y) * m_Stride;

		std::memcpy(into, image.Bytes().data() + static_cast<std::size_t>(y) * image.Stride(), m_Stride);
	}

	if (m_Frames.size() < m_Depth)
	{
		m_Frames.push_back(frame);
	}
	else
	{
		m_Frames[slot] = frame;
	}

	++m_Copied;
	Retire(output, frame);
}

void CapturingSink::Retire(VirtualOutput& output, const VirtualFrame& frame)
{
	if (m_Retention == FrameRetention::Release)
	{
		output.Release(frame.Target);

		return;
	}

	// Recorded rather than released, so that the ring runs out of images and the frame loop meets the
	// refusal `AcquireTarget` is documented to answer with.
	if (std::ranges::find(m_Held, frame.Target) == m_Held.end())
	{
		m_Held.push_back(frame.Target);
	}
}

std::size_t CapturingSink::Retained() const noexcept
{
	return static_cast<std::size_t>(std::min<std::uint64_t>(m_Copied, m_Depth));
}

std::span<const std::byte> CapturingSink::Slot(std::size_t index) const noexcept
{
	const std::size_t bytes = m_Stride * static_cast<std::size_t>(m_Size.Height);

	return { m_Pixels.data() + index * bytes, bytes };
}

CapturingSink::Capture CapturingSink::At(std::size_t index) const noexcept
{
	const std::size_t retained = Retained();

	if (index >= retained)
	{
		return {};
	}

	// `index` is oldest-first over the window, and the window's oldest is the slot after the newest
	// once it has wrapped. Before it wraps, slot order and arrival order are the same.
	const std::size_t oldest = (m_Copied - retained) % m_Depth;
	const std::size_t slot = (oldest + index) % m_Depth;

	Result<ImageView> image = ImageView::Over(Slot(slot), m_Size, m_Stride, m_Format);

	if (!image)
	{
		return {};
	}

	return { m_Frames[slot], *image };
}

CapturingSink::Capture CapturingSink::Newest() const noexcept
{
	const std::size_t retained = Retained();

	return retained == 0 ? Capture{} : At(retained - 1);
}

void CapturingSink::ReleaseHeld(VirtualOutput& output) noexcept
{
	for (const std::uint32_t target : m_Held)
	{
		output.Release(target);
	}

	m_Held.clear();
}
