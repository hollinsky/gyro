#include "Compositor/Capture.h"

#include <sys/stat.h>

#include <format>
#include <memory>
#include <mutex>
#include <utility>

#include "Seam/Pixel.h"
#include "Virtual/Pam.h"
#include "Virtual/Pixels.h"

PamCapture::PamCapture(std::string directory, std::size_t outputs)
	: m_Directory{ std::move(directory) }, m_Count{ std::min(outputs, MaxOutputs) }
{}

PamCapture::~PamCapture()
{
	Close();
}

Result<void> PamCapture::Open()
{
	if (m_Writer.joinable())
	{
		return Failure(EALREADY, "this capture is already open");
	}

	if (m_Directory.empty())
	{
		return Failure(EINVAL, "a capture wants somewhere to write");
	}

	m_Stopping.store(false, std::memory_order_relaxed);
	m_Writer = std::thread{ [this] { Write(); } };

	return {};
}

void PamCapture::Close() noexcept
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

Result<void> PamCapture::Remember(std::size_t index, PixelSize<DeviceSpace> size, PixelFormat format)
{
	if (index >= m_Count)
	{
		return Failure(EINVAL, "remembering a capture shape for an output the capture does not have");
	}

	const std::uint32_t bytesPerPixel = DecodableBytesPerPixel(format.Code);

	if (bytesPerPixel == 0 || !size.IsValid() || size.IsEmpty())
	{
		return Failure(EINVAL, "a capture wants a real extent and a decodable sample width");
	}

	Output& output = m_Outputs[index];

	// **Refused rather than resized while a capture is outstanding.** The frame thread may be copying
	// into this slab right now, and a mode set that reallocated it underneath would be a use-after-free
	// on the one thread that cannot afford one. A reconfigure during a capture is rare enough that
	// losing the picture is the right answer.
	if (output.State.load(std::memory_order_acquire) != Slot::Idle)
	{
		return Failure(EBUSY, "a capture of this output is still outstanding");
	}

	output.Size = size;
	output.Format = format;
	output.Stride = static_cast<std::uint32_t>(size.Width) * bytesPerPixel;
	output.Pixels.assign(static_cast<std::size_t>(output.Stride) * static_cast<std::size_t>(size.Height), std::byte{});

	return {};
}

void PamCapture::Offer(const SurfaceCapture& buffer) noexcept
{
	const auto rows = static_cast<std::size_t>(buffer.Stride) * static_cast<std::size_t>(buffer.Size.Height);

	if (rows == 0 || buffer.Pixels.size() < rows)
	{
		return;
	}

	// **Newest wins, per surface.** A window that redraws sixty times a second replaces its entry sixty
	// times and costs one copy each; what is kept is the buffer a press would find, which is the only
	// one a diff against the glass can use.
	const auto matches = [&buffer](const std::shared_ptr<const Buffer>& held) noexcept {
		return held->Surface == buffer.Surface;
	};
	const auto at = std::find_if(m_Held.begin(), m_Held.end(), matches);

	if (at == m_Held.end() && m_Held.size() >= MaxBuffers)
	{
		return;
	}

	// Allocating on the dispatch thread, which owes no deadline — Core/FrameSection.h's rule is the
	// frame thread's and this side of the boundary is where the copies already happen.
	auto held = std::make_shared<Buffer>();

	held->Surface = buffer.Surface;
	held->Pixels.assign(buffer.Pixels.begin(), buffer.Pixels.begin() + static_cast<std::ptrdiff_t>(rows));
	held->Size = buffer.Size;
	held->Stride = buffer.Stride;
	held->Alpha = buffer.Alpha;
	held->Damage.assign(buffer.Damage.begin(), buffer.Damage.end());
	held->Commit = ++m_Commits;

	if (at == m_Held.end())
	{
		m_Held.push_back(std::move(held));
	}
	else
	{
		*at = std::move(held);
	}
}

std::size_t PamCapture::Request() noexcept
{
	// **The buffers go first, so the table cannot move under the press.** Both halves are armed by the
	// same keystroke, but only this one is finished by the time the call returns: the frame side is a
	// request to a thread that has not run yet, and a client that commits between the two ends up with
	// a buffer this press did not write. That is the pairing the `Commit` ordinal exists to expose
	// rather than to hide.
	if (!m_Held.empty())
	{
		const std::uint64_t press = ++m_Press;

		{
			const std::lock_guard<std::mutex> guard{ m_Lock };

			for (const std::shared_ptr<const Buffer>& held : m_Held)
			{
				m_Queued.emplace_back(press, held);
			}
		}

		m_Signal.fetch_add(1, std::memory_order_release);
		m_Signal.notify_one();
	}

	std::size_t armed = 0;

	for (std::size_t index = 0; index < m_Count; ++index)
	{
		Output& output = m_Outputs[index];

		if (output.Pixels.empty())
		{
			continue;
		}

		// **Only from `Idle`, so a second press while one is outstanding does nothing.** The header says
		// why that is the behaviour worth having rather than a queue: a screenshot is one instant a
		// person chose, and a picture that lands three seconds late is a picture of the wrong moment.
		Slot expected = Slot::Idle;

		if (output.State.compare_exchange_strong(expected, Slot::Armed, std::memory_order_acq_rel))
		{
			++armed;
		}
	}

	return armed;
}

bool PamCapture::Wanted(std::uint32_t output) const noexcept
{
	return output < m_Count && m_Outputs[output].State.load(std::memory_order_acquire) == Slot::Armed;
}

std::span<std::byte> PamCapture::Reserve(
	std::uint32_t output,
	PixelSize<DeviceSpace> size,
	PixelFormat format,
	std::uint32_t stride
) noexcept
{
	if (!Wanted(output))
	{
		return {};
	}

	Output& slot = m_Outputs[output];

	// A shape this slab was not sized for — a mode set since the last `Remember`, or a target whose
	// format is not the one the output was configured with. Empty rather than grown, because growing it
	// here is an allocation inside Core/FrameSection.h.
	if (slot.Size != size || slot.Format != format || slot.Stride != stride)
	{
		slot.State.store(Slot::Idle, std::memory_order_release);

		return {};
	}

	return { slot.Pixels.data(), slot.Pixels.size() };
}

void PamCapture::Publish(std::uint32_t output, std::uint64_t sequence, bool complete) noexcept
{
	if (output >= m_Count)
	{
		return;
	}

	Output& slot = m_Outputs[output];

	if (!complete)
	{
		// Back to idle with nothing written. A truncated PAM in a directory somebody is watching reads
		// as a compositor that drew half a frame, which is a worse report than no file at all.
		slot.State.store(Slot::Idle, std::memory_order_release);

		return;
	}

	slot.Sequence = sequence;

	// The release that hands the slab over: the pixels and the sequence above it are visible to the
	// writer's acquire below, and nothing above it may be reordered past it.
	slot.State.store(Slot::Filled, std::memory_order_release);

	m_Signal.fetch_add(1, std::memory_order_release);
	m_Signal.notify_one();
}

std::string PamCapture::Destination(std::size_t index)
{
	// One directory for one output, and a level per output beyond that, which is Virtual/Dump.h's rule
	// for Virtual/Dump.h's reason: `frame-00000042.pam` carries no output identity, so two panels
	// writing into one directory overwrite each other at every sequence they share.
	if (m_Count <= 1)
	{
		return m_Directory;
	}

	(void)::mkdir(m_Directory.c_str(), 0755);

	return std::format("{}/output-{}", m_Directory, index);
}

void PamCapture::WriteBuffers()
{
	std::vector<std::pair<std::uint64_t, std::shared_ptr<const Buffer>>> queued;

	{
		const std::lock_guard<std::mutex> guard{ m_Lock };

		queued.swap(m_Queued);
	}

	for (const auto& [press, held] : queued)
	{
		// The client's own fourcc, which is the one thing `TextureAlpha` was carrying instead of: a
		// window that says its top byte means nothing is `xrgb8888`, and decoding it as alpha would
		// write a picture that is transparent where the client meant opaque.
		const PixelFormat format{ .Code = held->Alpha == TextureAlpha::Premultiplied ? FormatArgb8888 : FormatXrgb8888,
			                      .Modifier = ModifierLinear };
		const Result<ImageView> image = ImageView::Over(
			held->Pixels, PixelSize<DeviceSpace>{ held->Size.Width, held->Size.Height }, held->Stride, format
		);

		std::string note = std::format(
			"gyro surface {} commit {} {}x{} stride {} {}",
			held->Surface,
			held->Commit,
			held->Size.Width,
			held->Size.Height,
			held->Stride,
			held->Alpha == TextureAlpha::Premultiplied ? "argb8888" : "xrgb8888"
		);

		// **Every rectangle, in the client's own order, and the count said out loud.** A reader asking
		// whether a row was repainted needs to know that the list is complete — `damage 0` is a client
		// that committed a buffer and claimed nothing changed in it, which is a different report from a
		// capture that lost the list.
		note += std::format("\ndamage {}", held->Damage.size());

		for (const PixelRect<BufferSpace>& rect : held->Damage)
		{
			note += std::format(
				"\ndamage {} {} {} {}", rect.Origin.X, rect.Origin.Y, rect.Extent.Width, rect.Extent.Height
			);
		}

		(void)::mkdir(m_Directory.c_str(), 0755);

		const std::string path = std::format("{}/surface-{:08}-{:08}.pam", m_Directory, press, held->Surface);

		if (!image)
		{
			m_Failed.fetch_add(1, std::memory_order_relaxed);

			if (!m_FirstFailure)
			{
				m_FirstFailure = image.error();
			}
		}
		else if (const Result<void> written = WritePam(*image, path, note); !written)
		{
			m_Failed.fetch_add(1, std::memory_order_relaxed);

			if (!m_FirstFailure)
			{
				m_FirstFailure = written.error();
			}
		}
		else
		{
			m_Buffers.fetch_add(1, std::memory_order_relaxed);
		}
	}
}

void PamCapture::Write()
{
	std::uint32_t seen = m_Signal.load(std::memory_order_acquire);

	while (true)
	{
		bool wrote = false;

		{
			// Ahead of the frames, so a press whose picture is still being read back has already put its
			// buffers on disk: the two halves are one diff, and the half that is ready first is the half
			// that costs nothing to wait for.
			const std::size_t before =
				m_Buffers.load(std::memory_order_relaxed) + m_Failed.load(std::memory_order_relaxed);

			WriteBuffers();

			wrote = m_Buffers.load(std::memory_order_relaxed) + m_Failed.load(std::memory_order_relaxed) != before;
		}

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			Output& output = m_Outputs[index];

			if (output.State.load(std::memory_order_acquire) != Slot::Filled)
			{
				continue;
			}

			const Result<ImageView> image = ImageView::Over(output.Pixels, output.Size, output.Stride, output.Format);

			if (!image)
			{
				m_Failed.fetch_add(1, std::memory_order_relaxed);

				if (!m_FirstFailure)
				{
					m_FirstFailure = image.error();
				}
			}
			else if (const Result<void> written = DumpFrame(*image, Destination(index), output.Sequence); !written)
			{
				m_Failed.fetch_add(1, std::memory_order_relaxed);

				if (!m_FirstFailure)
				{
					m_FirstFailure = written.error();
				}
			}
			else
			{
				m_Written.fetch_add(1, std::memory_order_relaxed);
			}

			wrote = true;

			// Last, so the slab is not handed back until everything read out of it has been written.
			output.State.store(Slot::Idle, std::memory_order_release);
		}

		if (m_Stopping.load(std::memory_order_acquire))
		{
			// One more pass on the way out, because a capture published between the last scan and the
			// stop is one the join promised to write.
			if (!wrote)
			{
				return;
			}

			continue;
		}

		if (!wrote)
		{
			m_Signal.wait(seen, std::memory_order_acquire);
		}

		seen = m_Signal.load(std::memory_order_acquire);
	}
}
