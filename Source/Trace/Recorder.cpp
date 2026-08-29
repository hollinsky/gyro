#include "Trace/Recorder.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <fstream>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "Trace/Perfetto.h"

namespace
{

// A ring's slot arithmetic is a mask, so the byte figure a person types becomes the largest power of
// two that fits inside it rather than the nearest one. Asking for twenty megabytes and being given
// thirty-two would be a surprise on a machine that was chosen for having sixteen.
[[nodiscard]] std::size_t RecordsWithin(std::size_t bytes) noexcept
{
	const std::size_t records = bytes / sizeof(TraceRecord);

	if (records < 2)
	{
		return 0;
	}

	return std::bit_floor(records);
}

} // namespace

std::filesystem::path NumberedTrace(const std::filesystem::path& path, std::uint64_t number)
{
	std::filesystem::path numbered = path;

	numbered.replace_filename(path.stem().string() + "-" + std::to_string(number) + path.extension().string());

	return numbered;
}

TraceBuffer* Recorder::Arm(std::string_view name, const IClock& clock)
{
	const std::size_t records = RecordsWithin(m_Policy.Bytes);

	if (records == 0 || m_Count >= m_Sources.size())
	{
		return nullptr;
	}

	Source& source = m_Sources[m_Count];

	source.Name = name;
	source.Records = std::make_unique<TraceRecord[]>(records);
	source.Capacity = records;
	source.Buffer.Arm({ source.Records.get(), records }, clock);

	++m_Count;

	return &source.Buffer;
}

void Recorder::Join(TraceBuffer& buffer, std::int32_t tid) noexcept
{
	for (std::size_t index = 0; index < m_Count; ++index)
	{
		if (&m_Sources[index].Buffer == &buffer)
		{
			m_Sources[index].Tid.store(tid, std::memory_order_relaxed);
		}
	}

	EnrollTracing(&buffer);
}

void Recorder::Start()
{
	if (m_Count == 0 || m_Writer.joinable())
	{
		return;
	}

	// The trigger baseline, read *before* the writer exists so the ordering is the caller's: anything
	// tripped after `Start` returns is this recorder's to answer, and anything before it — a test's, a
	// previous arming's — is not. Read on the writer thread instead, a trigger fired into the gap
	// between the spawn and the thread's first instruction would be absorbed into the baseline and the
	// one snapshot the hunt was armed for would never be written.
	m_Seen = TraceTriggers();

	m_Writer = std::thread{ [this] { Watch(); } };
}

void Recorder::Stop() noexcept
{
	if (!m_Writer.joinable())
	{
		return;
	}

	{
		const std::lock_guard<std::mutex> held{ m_Mutex };
		m_Stopping.store(true, std::memory_order_relaxed);
	}

	m_Wake.notify_all();
	m_Writer.join();
}

void Recorder::Watch()
{
	// Not enrolled, deliberately. A thread that recorded its own waiting would fill a ring with the fact
	// that nothing was happening, which is the one thing a trace never needs to be told.

	bool armed = m_Policy.OnTrigger;

	// `Start` read the baseline; from here the counter is this thread's alone.
	std::uint64_t seen = m_Seen;

	while (!m_Stopping.load(std::memory_order_relaxed))
	{
		{
			std::unique_lock<std::mutex> held{ m_Mutex };

			m_Wake.wait_for(held, m_Policy.Poll, [this] { return m_Stopping.load(std::memory_order_relaxed); });
		}

		bool requested = m_Requested.exchange(false, std::memory_order_relaxed);

		// A changed count is the same request `SIGUSR1` makes, taken once — `TracePolicy::OnTrigger`
		// says why once — and any triggers that fired between looks collapse into the one snapshot,
		// which already holds all of them.
		if (const std::uint64_t fired = TraceTriggers(); fired != seen)
		{
			seen = fired;

			if (armed)
			{
				requested = true;
				armed = false;
			}
		}

		if (!requested)
		{
			continue;
		}

		const std::uint64_t number = m_Snapshots.load(std::memory_order_relaxed) + 1;

		// The first failure is kept and the rest are not, for `FrameOutput::FirstRefusal`'s reason: a
		// snapshot that cannot be written is a standing condition — a directory that is not there, a
		// filesystem that is full — and the hundredth says nothing the first did not.
		if (const Result<TraceSummary> written = Snapshot(NumberedTrace(m_Policy.Path, number)); !written)
		{
			if (m_Outcome)
			{
				m_Outcome = std::unexpected{ written.error() };
			}
		}
	}
}

Result<TraceSummary> Recorder::Snapshot(const std::filesystem::path& path)
{
	if (m_Count == 0)
	{
		return Failure(ENODATA, "nothing is being recorded");
	}

	// Allocated here rather than held for the session, because holding it would double what tracing
	// costs a machine that never asks for a snapshot. This runs on the writer thread, which is
	// allowed to do exactly the things the frame thread is not.
	std::vector<std::vector<TraceEvent>> collected;
	std::vector<TraceSource> sources;

	collected.reserve(m_Count);
	sources.reserve(m_Count);

	TraceSummary summary{};
	Instant oldest{};
	Instant newest{};
	bool any = false;

	for (std::size_t index = 0; index < m_Count; ++index)
	{
		Source& source = m_Sources[index];

		std::vector<TraceEvent> events(source.Capacity);
		const std::size_t count = source.Buffer.Copy(events);

		events.resize(count);

		// **The copy is in emission order, which is not the same as time order, and Perfetto wants
		// time.** Core/Trace.h's `EmitAt` is why: a GPU span is stamped with the device clock and
		// emitted two frames after it happened, so it lands in the ring behind records that are newer
		// than it. A stable sort is what reconciles the two, and it belongs here rather than in the
		// ring because this is the thread that is allowed to allocate and take milliseconds — the
		// whole reason the ring hands out a copy at all.
		//
		// **Stable, and that is load-bearing rather than a default.** Two records with the same stamp
		// are the ordinary case at the boundary between one span and the next, and their order is the
		// order they were emitted in — an `End` before the `Begin` that abuts it. A sort free to swap
		// them would draw a slice that opens before its predecessor closed, on a track where nesting
		// is what a slice means.
		std::ranges::stable_sort(events, {}, &TraceEvent::Stamp);

		if (count != 0)
		{
			oldest = any ? (events.front().Stamp < oldest ? events.front().Stamp : oldest) : events.front().Stamp;
			newest = any ? (events.back().Stamp > newest ? events.back().Stamp : newest) : events.back().Stamp;
			any = true;
		}

		summary.Events += count;
		collected.push_back(std::move(events));

		sources.push_back(
			TraceSource{
				.Name = source.Name, .Tid = source.Tid.load(std::memory_order_relaxed), .Events = collected.back() }
		);
	}

	if (any)
	{
		summary.Covered = Elapsed(oldest, newest);
	}

	// Read here rather than at startup, because what separates the two domains is time spent suspended
	// and a session that suspended after the anchor was taken would place every record in this file at
	// the wrong moment of the system trace beside it.
	const std::vector<std::byte> encoded =
		EncodeTrace(TraceIdentity{ .Pid = m_Policy.Pid, .Name = "gyro", .Anchor = ReadClockAnchor() }, sources);

	summary.Bytes = encoded.size();

	std::filesystem::path partial = path;
	partial += ".partial";

	{
		std::ofstream file{ partial, std::ios::binary | std::ios::trunc };

		if (!file)
		{
			return Failure(errno != 0 ? errno : EIO, "opening a trace snapshot");
		}

		file.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
		file.close();

		if (!file)
		{
			return Failure(errno != 0 ? errno : EIO, "writing a trace snapshot");
		}
	}

	std::error_code failed;
	std::filesystem::rename(partial, path, failed);

	if (failed)
	{
		return Failure(failed.value(), "renaming a trace snapshot into place");
	}

	m_Snapshots.fetch_add(1, std::memory_order_relaxed);

	return summary;
}

std::uint64_t Recorder::Written() const noexcept
{
	std::uint64_t total = 0;

	for (std::size_t index = 0; index < m_Count; ++index)
	{
		total += m_Sources[index].Buffer.Written();
	}

	return total;
}

std::uint64_t Recorder::Capacity() const noexcept
{
	std::uint64_t total = 0;

	for (std::size_t index = 0; index < m_Count; ++index)
	{
		total += m_Sources[index].Capacity;
	}

	return total;
}
