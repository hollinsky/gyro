#include "Virtual/Dump.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include "Virtual/Buffer.h"
#include "Virtual/Pam.h"
#include "Virtual/Pixels.h"

FrameDump::FrameDump(std::string directory, PixelSize<DeviceSpace> size, PixelFormat format, std::size_t depth)
	: m_Directory{ std::move(directory) }, m_Size{ size }, m_Format{ format },
	  m_Depth{ std::max<std::size_t>(depth, 1) }
{}

FrameDump::~FrameDump()
{
	Close();
}

Result<void> FrameDump::Open()
{
	if (m_Writer.joinable())
	{
		return Failure(EALREADY, "this frame dump is already writing");
	}

	const std::uint32_t bytesPerPixel = DecodableBytesPerPixel(m_Format.Code);

	if (bytesPerPixel == 0)
	{
		return Failure(EINVAL, "a frame dump wants a format with a decodable sample width");
	}

	if (!m_Size.IsValid() || m_Size.IsEmpty())
	{
		return Failure(EINVAL, "a frame dump wants an output with a real extent");
	}

	m_Stride = static_cast<std::uint32_t>(m_Size.Width) * bytesPerPixel;

	// Every allocation this object ever makes, here — which is what lets `OnFrame` run on the frame
	// thread at all, and what decision 36's allocator would catch if it were anywhere else.
	m_Slabs.resize(static_cast<std::size_t>(m_Stride) * static_cast<std::size_t>(m_Size.Height) * m_Depth);
	m_Slots.resize(m_Depth);

	m_Writer = std::thread{ [this] { Write(); } };

	return {};
}

void FrameDump::Close() noexcept
{
	if (!m_Writer.joinable())
	{
		return;
	}

	m_Stopping.store(true, std::memory_order_release);
	m_Signal.fetch_add(1, std::memory_order_release);
	m_Signal.notify_one();

	m_Writer.join();
}

void FrameDump::Flush() noexcept
{
	if (!m_Writer.joinable())
	{
		return;
	}

	for (;;)
	{
		const std::uint64_t tail = m_Tail.load(std::memory_order_acquire);

		if (tail == m_Head.load(std::memory_order_acquire))
		{
			return;
		}

		m_Tail.wait(tail, std::memory_order_acquire);
	}
}

std::span<std::byte> FrameDump::Pixels(std::size_t slot) noexcept
{
	const std::size_t image = static_cast<std::size_t>(m_Stride) * static_cast<std::size_t>(m_Size.Height);

	return { m_Slabs.data() + slot * image, image };
}

void FrameDump::OnFrame(VirtualOutput& output, const VirtualFrame& frame)
{
	const DmabufBuffer* const buffer = output.Buffer(frame.Target);

	if (buffer == nullptr || m_Slabs.empty())
	{
		m_Skipped.fetch_add(1, std::memory_order_relaxed);
		output.Release(frame.Target);

		return;
	}

	// Held across the copy and released before the image goes back, which is `CapturingSink`'s
	// bracket for `CapturingSink`'s reason: an exporter with real cache maintenance needs both ends,
	// and the end is the half that gets forgotten.
	const BufferReader reader{ *buffer };

	if (!reader.IsValid() || reader.Image().Size() != m_Size || reader.Image().Format() != m_Format)
	{
		m_Skipped.fetch_add(1, std::memory_order_relaxed);
		output.Release(frame.Target);

		return;
	}

	const std::uint64_t head = m_Head.load(std::memory_order_relaxed);
	const std::uint64_t tail = m_Tail.load(std::memory_order_acquire);

	// Monotone counts rather than wrapped indices, so a full ring is a subtraction rather than a
	// spare slot kept empty to tell full from empty apart.
	if (head - tail >= m_Depth)
	{
		m_Dropped.fetch_add(1, std::memory_order_relaxed);
		output.Release(frame.Target);

		return;
	}

	const std::size_t slot = head % m_Depth;
	const ImageView& image = reader.Image();
	std::byte* const into = Pixels(slot).data();

	for (std::int32_t y = 0; y < m_Size.Height; ++y)
	{
		std::memcpy(
			into + static_cast<std::size_t>(y) * m_Stride,
			image.Bytes().data() + static_cast<std::size_t>(y) * image.Stride(),
			m_Stride
		);
	}

	m_Slots[slot].Sequence = frame.Sequence;

	// The release that publishes the slot. Everything above it — the pixels and the sequence — is
	// visible to the writer's acquire load below, and nothing above it may be reordered past it.
	m_Head.store(head + 1, std::memory_order_release);
	m_Queued.fetch_add(1, std::memory_order_relaxed);

	m_Signal.fetch_add(1, std::memory_order_release);
	m_Signal.notify_one();

	// After the publish rather than before, so the writer has the longest possible head start on a
	// frame the ring is about to hand back out.
	output.Release(frame.Target);
}

void FrameDump::Write()
{
	for (;;)
	{
		// Read the signal *before* looking at the queue. A push that lands between the look and the
		// wait bumps this, so the wait returns at once rather than sleeping on a frame that has
		// already arrived — which is the whole reason the wait is on a counter and not on the queue.
		const std::uint32_t observed = m_Signal.load(std::memory_order_acquire);
		const bool stopping = m_Stopping.load(std::memory_order_acquire);

		std::uint64_t tail = m_Tail.load(std::memory_order_relaxed);

		while (tail != m_Head.load(std::memory_order_acquire))
		{
			const std::size_t slot = tail % m_Depth;
			const Result<ImageView> image = ImageView::Over(Pixels(slot), m_Size, m_Stride, m_Format);

			// A view over a slab this object sized itself cannot fail, but the failure is carried
			// rather than asserted: this thread aborting would take a compositor down over a
			// diagnostic, which is exactly the wrong trade for the one component nobody depends on.
			const Result<void> written = image ? DumpFrame(*image, m_Directory, m_Slots[slot].Sequence) :
			                                     Result<void>{ std::unexpected{ image.error() } };

			if (written)
			{
				m_Written.fetch_add(1, std::memory_order_relaxed);
			}
			else
			{
				m_Failed.fetch_add(1, std::memory_order_relaxed);

				if (!m_FirstFailure)
				{
					m_FirstFailure = written.error();
				}
			}

			// The release that frees the slot for the producer to fill again, and the wake for a
			// `Flush` parked on it. `notify_all` rather than `notify_one` because a flush is not the
			// only thing that may be waiting and this is off the frame path entirely.
			++tail;
			m_Tail.store(tail, std::memory_order_release);
			m_Tail.notify_all();
		}

		// Checked after the drain rather than before it, so a stop always flushes what was accepted.
		// The read is the one taken at the top: a stop that lands during the drain bumps the signal,
		// so the wait below returns and the next pass sees it.
		if (stopping)
		{
			return;
		}

		m_Signal.wait(observed, std::memory_order_acquire);
	}
}
