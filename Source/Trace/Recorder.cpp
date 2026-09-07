#include "Trace/Recorder.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
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

void TraceLogStore::Arm(std::size_t records, const IClock& clock)
{
	const std::lock_guard held{ m_Lock };

	m_Records.assign(records, TraceLogRecord{});
	m_Next = 0;
	m_Held = 0;
	m_Clock = &clock;
}

void TraceLogStore::Write(const char* level, std::string_view message) noexcept
{
	const std::lock_guard held{ m_Lock };

	if (m_Records.empty() || m_Clock == nullptr)
	{
		return;
	}

	TraceLogRecord& record = m_Records[m_Next];

	record.Stamp = m_Clock->Now();
	record.Level = level;

	// Truncated rather than overrun, and the cut is at the buffer rather than at the message: a sink is
	// handed whatever a caller formatted, and a format string with a client's title in it is a message
	// whose length is somebody else's to decide.
	const std::size_t kept = std::min(message.size(), TraceMessageLimit);

	std::memcpy(record.Message.data(), message.data(), kept);
	record.Length = static_cast<std::uint16_t>(kept);

	m_Next = (m_Next + 1) % m_Records.size();
	m_Held = std::min(m_Held + 1, m_Records.size());
}

std::vector<TraceLogRecord> TraceLogStore::Copy() const
{
	const std::lock_guard held{ m_Lock };

	std::vector<TraceLogRecord> taken;

	if (m_Held == 0)
	{
		return taken;
	}

	taken.reserve(m_Held);

	// Where the oldest one is: the write cursor once the store has wrapped, and the front of it before
	// that. The same arithmetic either way, since `m_Held` is the count and `m_Next` is one past the
	// newest.
	const std::size_t oldest = (m_Next + m_Records.size() - m_Held) % m_Records.size();

	for (std::size_t at = 0; at < m_Held; ++at)
	{
		taken.push_back(m_Records[(oldest + at) % m_Records.size()]);
	}

	return taken;
}

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

TraceLogStore* Recorder::ArmLog(const IClock& clock)
{
	// The same question the rings ask, asked once: a policy that buys no ring buys no log window
	// either, which is what `--trace-buffer=0` means without a second flag saying so.
	if (RecordsWithin(m_Policy.Bytes) == 0 || m_Log.IsArmed())
	{
		return nullptr;
	}

	const std::size_t records =
		std::max<std::size_t>(32, m_Policy.Bytes / TracePolicy::LogShare / sizeof(TraceLogRecord));

	m_Log.Arm(records, clock);

	return &m_Log;
}

void Recorder::Describe(TraceRun run)
{
	const std::lock_guard held{ m_Described };

	m_Run = std::move(run);
}

void Recorder::Note(std::string_view name, std::string_view value)
{
	const std::lock_guard held{ m_Described };

	m_Run.Facts.emplace_back(name, value);
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

	// The log store is read here and not in the loop above, because it is not one thread's: whichever
	// thread logged took the store's lock, and what comes back is already in time order.
	const std::vector<TraceLogRecord> logged = m_Log.Copy();
	std::vector<TraceLine> lines;

	lines.reserve(logged.size());

	for (const TraceLogRecord& record : logged)
	{
		lines.push_back(
			TraceLine{ .Stamp = record.Stamp,
		               .Level = record.Level,
		               .Message = std::string_view{ record.Message.data(), record.Length } }
		);
	}

	// **Copied under the lock rather than viewed through it**, because the frame thread may still be
	// adding the one fact that is not knowable at startup and a `string_view` onto a vector that grew
	// is a view onto freed memory.
	TraceRun run;

	{
		const std::lock_guard held{ m_Described };

		run = m_Run;
	}

	std::vector<std::string_view> invocation;
	std::vector<TraceFact> facts;

	invocation.reserve(run.CommandLine.size());
	facts.reserve(run.Facts.size());

	for (const std::string& argument : run.CommandLine)
	{
		invocation.push_back(argument);
	}

	for (const auto& [name, value] : run.Facts)
	{
		facts.push_back(TraceFact{ .Name = name, .Value = value });
	}

	// Read here rather than at startup, because what separates the two domains is time spent suspended
	// and a session that suspended after the anchor was taken would place every record in this file at
	// the wrong moment of the system trace beside it. Everything beside it *is* read at startup, and
	// says so: a version and a kernel that changed mid-run would be a different process.
	const TraceIdentity identity{ .Pid = m_Policy.Pid,
		                          .Name = "gyro",
		                          .Anchor = ReadClockAnchor(),
		                          .CommandLine = invocation,
		                          .Machine = TraceMachine{ .Sysname = run.Sysname,
		                                                   .Release = run.Release,
		                                                   .Version = run.Version,
		                                                   .Machine = run.Machine },
		                          .Facts = facts };

	const std::vector<std::byte> encoded = EncodeTrace(identity, sources, lines);

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
