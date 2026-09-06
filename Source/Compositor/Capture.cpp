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

	// **A descriptor, which arrives with no rows and is recorded anyway.** Scene/Capture.h says why the
	// offer is made at all: the damage rectangles and the extent exist on this stack and nowhere else,
	// and the pixels come later off the device. What is refused is an offer with neither — no rows and
	// no id is a commit that adopted nothing, and there is no picture behind it to go and fetch.
	const bool borrowed = buffer.Pixels.empty();

	if (borrowed)
	{
		if (buffer.Texture.IsNull() || !buffer.Size.IsValid() || buffer.Size.IsEmpty())
		{
			return;
		}
	}
	else if (rows == 0 || buffer.Pixels.size() < rows)
	{
		return;
	}

	// **Newest wins, per surface — and a surface is a client and an id together.** A window that
	// redraws sixty times a second replaces its entry sixty times and costs one copy each; what is kept
	// is the buffer a press would find, which is the only one a diff against the glass can use. Keying
	// on the wire id alone made that replacement happen *across* clients, which Scene/Capture.h
	// records: two toolkits both own a `wl_surface@18`, so a terminal's repaint threw away the
	// browser's window and every press wrote one of the two at random.
	const auto matches = [&buffer](const std::shared_ptr<const Buffer>& held) noexcept {
		return held->Surface == buffer.Surface && held->Client == buffer.Client;
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
	held->Client = buffer.Client;

	if (!borrowed)
	{
		held->Pixels.assign(buffer.Pixels.begin(), buffer.Pixels.begin() + static_cast<std::ptrdiff_t>(rows));
	}

	held->Size = buffer.Size;

	// Tight for a descriptor, because the rows a readback produces were laid out by the copy rather
	// than by the client — and four bytes a sample, which is every format `Render/Readback.cpp` can
	// come back with and every one Virtual/Pam.h can write.
	held->Stride = borrowed ? static_cast<std::uint32_t>(buffer.Size.Width) * 4 : buffer.Stride;
	held->Alpha = buffer.Alpha;
	held->Texture = borrowed ? buffer.Texture : TextureId{};
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

		// **Only the ones whose pixels are already here.** A `wl_shm` record was copied at its commit
		// and can go to the writer now; a descriptor record has an extent and a damage list and no
		// bytes, and what fills it is the frame thread on the frame this press is about to force.
		{
			const std::lock_guard<std::mutex> guard{ m_Lock };

			for (const std::shared_ptr<const Buffer>& held : m_Held)
			{
				if (held->Texture.IsNull())
				{
					m_Queued.emplace_back(press, held);
				}
			}
		}

		// **Armed here and not before, and the slab is allocated here too.** This is the dispatch
		// thread, which owes no deadline; the frame thread that fills these may not allocate at all.
		//
		// Skipped entirely while any entry from the last press is still outstanding, which is the
		// output slabs' rule read across the whole table: a press whose readbacks have not landed is
		// one whose slabs the writer may still be reading out of.
		{
			// **Everything a previous press left armed is abandoned first.** Those are the textures the
			// frame never drew — a window occluded, minimised, or on a panel this capture did not
			// reach — and they are safe to take back precisely because they are still in `Armed`: an
			// entry the frame thread is working on has already moved to `Reserved`.
			for (Pending& slot : m_Pending)
			{
				Slot stale = Slot::Armed;

				(void)slot.State.compare_exchange_strong(stale, Slot::Idle, std::memory_order_acq_rel);
			}

			std::size_t armed = 0;

			for (const std::shared_ptr<const Buffer>& held : m_Held)
			{
				if (held->Texture.IsNull())
				{
					continue;
				}

				// Past the entries a `Reserved` or `Filled` from this press or the last is still using.
				// Running out is the ordinary answer and is the same one `MaxBuffers` gives: the picture
				// is on disk and the buffers are a supplement to it.
				while (armed < m_Pending.size() && m_Pending[armed].State.load(std::memory_order_acquire) != Slot::Idle)
				{
					++armed;
				}

				if (armed >= m_Pending.size())
				{
					break;
				}

				// A copy of the record rather than the record itself, because the frame thread is about
				// to write into `Pixels` and the dispatch thread may replace `m_Held`'s entry with the
				// client's next commit while it does.
				auto owed = std::make_shared<Buffer>(*held);

				owed->Pixels.assign(
					static_cast<std::size_t>(owed->Stride) * static_cast<std::size_t>(owed->Size.Height), std::byte{}
				);

				Pending& slot = m_Pending[armed];

				slot.Held = std::move(owed);
				slot.Press = press;
				slot.Stride = slot.Held->Stride;
				slot.State.store(Slot::Armed, std::memory_order_release);

				++armed;
			}

			// **The bound the other two threads scan to, computed here because this is the only thread
			// that ever puts an entry into the table.** It was a high-water mark the writer reset when
			// the table emptied, which is a check-then-act split across two threads: the writer decided
			// *nothing is outstanding* from a scan bounded by a value it had already read, and a press
			// that armed an entry in between had its bound overwritten with zero. The window was a few
			// instructions wide and it cost a keystroke its buffers — the frame thread read a bound of
			// nothing, so no window was ever offered for readback.
			//
			// A full pass rather than `armed`, because entries this press did not touch may still be
			// owed: a `Filled` one the writer has not taken yet sits above whatever this press armed,
			// and a bound that stopped at `armed` would strand it unwritten.
			std::size_t bound = 0;

			for (std::size_t index = 0; index < m_Pending.size(); ++index)
			{
				if (m_Pending[index].State.load(std::memory_order_acquire) != Slot::Idle)
				{
					bound = index + 1;
				}
			}

			// Racing the writer here is harmless in the one direction it can go: an entry it retires
			// after this read leaves the bound a little long, and a scan of an `Idle` entry costs a
			// load. Nothing can appear above it, because appearing is this thread's own work.
			m_PendingCount.store(bound, std::memory_order_release);
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

bool PamCapture::WantsTexture(TextureId texture) const noexcept
{
	if (texture.IsNull())
	{
		return false;
	}

	const std::size_t count = m_PendingCount.load(std::memory_order_acquire);

	for (std::size_t index = 0; index < count; ++index)
	{
		const Pending& slot = m_Pending[index];

		if (slot.State.load(std::memory_order_acquire) == Slot::Armed && slot.Held != nullptr &&
		    slot.Held->Texture == texture)
		{
			return true;
		}
	}

	return false;
}

TextureSlab PamCapture::ReserveTexture(TextureId texture) noexcept
{
	const std::size_t count = m_PendingCount.load(std::memory_order_acquire);

	for (std::size_t index = 0; index < count; ++index)
	{
		Pending& slot = m_Pending[index];

		if (slot.Held == nullptr || slot.Held->Texture != texture)
		{
			continue;
		}

		// **The claim, and the header on `Slot::Reserved` is the whole argument for it.** Failing the
		// exchange is the ordinary case rather than a fault: a texture two draw items name is offered
		// twice on one frame, and the second offer finds the entry already taken and reads nothing.
		Slot expected = Slot::Armed;

		if (!slot.State.compare_exchange_strong(expected, Slot::Reserved, std::memory_order_acq_rel))
		{
			return {};
		}

		return { .Into = { slot.Held->Pixels.data(), slot.Held->Pixels.size() }, .Stride = slot.Stride };
	}

	return {};
}

void PamCapture::PublishTexture(TextureId texture, bool complete) noexcept
{
	const std::size_t count = m_PendingCount.load(std::memory_order_acquire);

	for (std::size_t index = 0; index < count; ++index)
	{
		Pending& slot = m_Pending[index];

		if (slot.Held == nullptr || slot.Held->Texture != texture ||
		    slot.State.load(std::memory_order_acquire) != Slot::Reserved)
		{
			continue;
		}

		if (!complete)
		{
			// Handed back unwritten, per Seam/Capture.h. A file of zeroes beside a picture of a window
			// that is plainly drawn is the worst answer available: it reads as a client that committed
			// nothing, which is precisely the bug somebody would be here to diagnose.
			//
			// **Labelled rather than released**, for the reason on `Slot::Dropped`: this is the frame
			// thread and the record is its own to free, so a `reset()` here is a client's rows going
			// back to the allocator inside a frame section. The writer takes it from `Dropped` to
			// `Idle`, so it is signalled exactly as a filled one is — an entry nobody woke would hold
			// the table shut until the next press abandoned it.
			slot.State.store(Slot::Dropped, std::memory_order_release);

			m_Signal.fetch_add(1, std::memory_order_release);
			m_Signal.notify_one();
		}
		else
		{
			// The release that hands the rows to the writer, matching the output slabs' above.
			slot.State.store(Slot::Filled, std::memory_order_release);

			m_Signal.fetch_add(1, std::memory_order_release);
			m_Signal.notify_one();
		}

		return;
	}
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
	// **The readbacks the frame thread finished, folded into the same queue as the copied ones**, so
	// everything below this line writes one kind of record. A dmabuf capture and a `wl_shm` capture
	// differ in when their bytes were taken and in nothing a reader of the directory can see.
	//
	// The entry is cleared as it is taken and only then handed back to `Idle`, which is what lets the
	// next press arm this table again — the same *last, so the slab is not reused under a write that
	// has not happened* rule the output slabs follow.
	{
		const std::size_t count = m_PendingCount.load(std::memory_order_acquire);

		for (std::size_t index = 0; index < count; ++index)
		{
			Pending& slot = m_Pending[index];

			switch (slot.State.load(std::memory_order_acquire))
			{
				case Slot::Filled:
				{
					{
						const std::lock_guard<std::mutex> guard{ m_Lock };

						m_Queued.emplace_back(slot.Press, std::shared_ptr<const Buffer>{ std::move(slot.Held) });
					}

					slot.Held.reset();
					slot.State.store(Slot::Idle, std::memory_order_release);

					break;
				}
				case Slot::Dropped:
				{
					// The frame thread refused this readback and may not free what it holds. Nothing is
					// written and nothing is counted — a refusal is not a failure to write, and
					// `Failed` is the writer's own tally.
					slot.Held.reset();
					slot.State.store(Slot::Idle, std::memory_order_release);

					break;
				}
				case Slot::Armed:
				case Slot::Reserved:
				{
					// Still owed a frame, or being read right now. An entry stuck in `Armed` is a window
					// the composite never sampled, and the next press takes it back rather than anything
					// here waiting on it.
					break;
				}
				case Slot::Idle:
					break;
			}
		}
	}

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
			"gyro client {} surface {} commit {} {}x{} stride {} {}",
			held->Client,
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

		// The client before the id, so a directory sorts into one block per application: a person
		// reading a press wants a window's buffers together far more often than they want the same wire
		// id across two of them.
		const std::string path =
			std::format("{}/surface-{:08}-{:08}-{:08}.pam", m_Directory, press, held->Client, held->Surface);

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
		// **Read before the scan below and not after it, which is the whole of the promise made to
		// `Close`.** The scan is what finds a published capture, so a stop observed afterwards cannot
		// say whether the pass that just ran happened before or after the publish it is racing. Taken
		// first, a false here means the scan that follows it ran no earlier than this read, and the
		// stop that arrives during it is one the *next* pass is guaranteed to scan for.
		const bool stopping = m_Stopping.load(std::memory_order_acquire);

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

		if (stopping)
		{
			// Nothing was found by a scan that began after the stop was visible, so nothing is left to
			// find: every publish `Close` promised to write happened before the flag it set, and this
			// pass looked after reading it. A pass that did write goes round again, because a capture
			// may have landed while it was writing the one before it.
			if (!wrote)
			{
				return;
			}

			continue;
		}

		if (!wrote)
		{
			// Safe against a stop that lands here rather than a wait nothing will wake: `Close` sets the
			// flag and then bumps the counter, so a signal this misses is one whose value `seen` does not
			// carry — and a signal it does carry is one whose stop the read at the top of the next pass
			// is ordered to see.
			m_Signal.wait(seen, std::memory_order_acquire);
		}

		seen = m_Signal.load(std::memory_order_acquire);
	}
}
