#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "Core/Clock.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Core/Trace.h"

// The rings, the thread that writes them out, and the policy Core/Trace.h refuses to hold.
//
// **A snapshot is rare and the recording is continuous, which is the inversion this whole module
// exists for.** A profiler that has to be started records the run after the interesting one. So the
// memory is spent for the length of the session — one ring per recorded thread, sized in bytes because
// how many seconds that buys depends on what the machine is doing — and the expensive part, which is
// interning, encoding and writing a file, happens on a thread of its own when somebody asks.
//
// **Asking is a signal, and that is what sets the shape of the waiting.** `SIGUSR1` is what a person
// types when the stutter just happened, and a handler may do nothing but store to an atomic — it may
// not take the lock a condition variable needs. So the writer thread wakes on a timer, and the interval
// is the delay between the flag and the file rather than anything on a frame path. Nothing here runs at
// real-time priority and nothing here is on either loop.
//
// **The file is written beside its destination and renamed onto it**, so that a reader who opened the
// directory during the write sees either the previous snapshot or this one and never half of this one.

// Sized rather than measured, and generously: gyro has two threads that record, the device workers
// will be the third kind, and a spare costs a pointer.
inline constexpr std::size_t MaxTraceSources = 4;

// How much of a log message survives. Long enough for every line gyro writes today — the longest in
// the tree is a little over a hundred characters — and a message past it is cut rather than refused,
// because a truncated sentence still says which frame it landed beside.
inline constexpr std::size_t TraceMessageLimit = 192;

// One line the process logged, held until somebody asks for a snapshot.
struct TraceLogRecord
{
	Instant Stamp{};

	// A literal, so it is a pointer: spdlog's levels are a fixed set and the sink maps them to one of
	// six strings that outlive the process.
	const char* Level = nullptr;

	std::array<char, TraceMessageLimit> Message{};
	std::uint16_t Length = 0;
};

// What spdlog wrote, kept beside the rings so that "the log said the panel came back" and the frame
// that was on the screen when it did are read off one timeline.
//
// **A second store rather than a second ring, because a log message is a runtime string.**
// `Core/Trace.h`'s record is a pointer to a string literal and a `uint64` — the property that makes
// emitting four relaxed stores — and there is no way to put `output 2 lost its panel at 41.203s`
// through it. So this one copies bytes and takes a lock.
//
// **That is legitimate here and would not be in the ring, because spdlog already allocates and locks.**
// Logging is therefore already forbidden on the frame path — nothing inside a `Core/FrameSection.h`
// guard may call it, and `Core/DebugAllocator.cpp` aborts if anything does — so a sink that takes one
// more mutex adds no prohibition that was not already in force. Nothing here is reachable from a
// frame section that was not already an abort.
//
// **Oldest dropped rather than newest**, which is the ring's own rule and for the ring's own reason: a
// snapshot is asked for just after the thing being chased, so the end of the window is the half worth
// keeping. A store that refused writes once full would answer a stutter with the log of the startup
// that preceded it by an hour.
class TraceLogStore
{
public:
	// Reserves the whole window up front, for `Recorder::Arm`'s reason: this is the last allocation it
	// makes, and everything after it happens while a thread that must not allocate is running.
	void Arm(std::size_t records, const IClock& clock);

	[[nodiscard]] bool IsArmed() const noexcept { return !m_Records.empty(); }

	// Any thread, under the lock. Silently does nothing where nothing was armed, so that
	// `--trace-buffer=0` needs no second flag to mean *record nothing*.
	void Write(const char* level, std::string_view message) noexcept;

	// Oldest first, which is the order the encoder wants and the order the store keeps.
	[[nodiscard]] std::vector<TraceLogRecord> Copy() const;

private:
	mutable std::mutex m_Lock;

	std::vector<TraceLogRecord> m_Records;
	std::size_t m_Next = 0;
	std::size_t m_Held = 0;

	const IClock* m_Clock = nullptr;
};

struct TracePolicy
{
	// Per recorded thread. Sixteen mebibytes is half a million records, which is a minute of a
	// two-panel machine and something like half of one at four panels and 144 Hz — the figure the report
	// line prints is what a person should size this against, since the rate is what the compositor is
	// doing rather than anything this can predict.
	std::size_t Bytes = 16U * 1024U * 1024U;

	// Where a snapshot lands. A signal-triggered one takes the same name with a number in it, so that
	// asking twice during one session does not overwrite the first answer.
	std::filesystem::path Path{ "gyro.pftrace" };

	std::int32_t Pid = 0;

	// How much of the ring's byte budget the log store gets. A sixty-fourth: at the default sixteen
	// mebibytes that is a little over nine hundred lines, which at the rate gyro actually logs — a
	// handful a second at the noisiest — covers a longer stretch than the ring itself holds. Sized
	// against the ring rather than fixed so that `--trace-buffer` moves both halves of the picture
	// together, which is the whole point of the two windows lining up.
	static constexpr std::size_t LogShare = 64;

	// How long the writer thread sleeps between looks at the request flag.
	Duration Poll = std::chrono::milliseconds{ 200 };

	// Answer `Core/Trace.h`'s `TraceTrigger` with a snapshot — the frame loop asking for the file when
	// it watches the anomaly a person could never ask for in time. **Once, and the first trigger
	// disarms it**: the conditions worth arming this for also recur — under a parallel build one commit
	// in forty lands late — and a trigger that stayed armed would answer a busy afternoon with a
	// directory of near-identical files. The person hunting a rare event wants the first one; asking
	// again is a restart, which is what a hunt session is anyway.
	bool OnTrigger = false;
};

// What the composition root knows about the run and Trace may not go and find out. See
// `Trace/Perfetto.h`'s `TraceFact` for why the split is where it is.
struct TraceRun
{
	std::vector<std::string> CommandLine;

	std::string Sysname;
	std::string Release;
	std::string Version;
	std::string Machine;

	std::vector<std::pair<std::string, std::string>> Facts;
};

struct TraceSummary
{
	std::size_t Events = 0;
	std::size_t Bytes = 0;

	// The oldest record in the snapshot against the newest, which is the honest answer to *how far back
	// does this go* — it is what the ring actually held rather than what it was sized for.
	Duration Covered{};
};

class Recorder
{
public:
	explicit Recorder(const TracePolicy& policy) : m_Policy{ policy } {}

	~Recorder() { Stop(); }

	Recorder(const Recorder&) = delete;
	Recorder& operator=(const Recorder&) = delete;

	// Reserves a ring for a thread that does not exist yet, which is the order the composition root
	// builds in. Returns null when the policy asks for nothing or there is no source left.
	[[nodiscard]] TraceBuffer* Arm(std::string_view name, const IClock& clock);

	// The same, for the log. Null where the policy asks for nothing, which is what keeps
	// `--trace-buffer=0` from starting a sink as well as from starting a ring.
	[[nodiscard]] TraceLogStore* ArmLog(const IClock& clock);

	// What produced this trace, handed over once by the composition root before the threads start.
	//
	// **Owned here rather than pointed at, because a snapshot happens seconds later on another
	// thread.** The root gathers these from `uname`, `/sys`, the generated version header and the
	// device it opened, all of which are the platform tier's; what crosses is strings.
	void Describe(TraceRun run);

	// One more fact, learned after the run was described. There is exactly one: whether the frame
	// thread actually got `SCHED_FIFO`, which is not knowable until that thread exists and asks — and
	// which explains a whole capture when the answer is no.
	void Note(std::string_view name, std::string_view value);

	// Called by the recorded thread as its first act, because the thread id is the thread's to know and
	// the thread-local pointer is the thread's to set.
	void Join(TraceBuffer& buffer, std::int32_t tid) noexcept;

	void Start();

	void Stop() noexcept;

	// Async-signal-safe by construction: one relaxed store and nothing else. Everything the request
	// leads to happens on the writer thread.
	void Request() noexcept { m_Requested.store(true, std::memory_order_relaxed); }

	[[nodiscard]] Result<TraceSummary> Snapshot(const std::filesystem::path& path);

	[[nodiscard]] bool IsRecording() const noexcept { return m_Count != 0; }

	// Records taken since the run began, and how many the rings can hold between them. The difference is
	// what has already been overwritten, which is the number that says whether the buffer is big enough
	// for the thing being chased.
	[[nodiscard]] std::uint64_t Written() const noexcept;

	[[nodiscard]] std::uint64_t Capacity() const noexcept;

	[[nodiscard]] std::uint64_t Snapshots() const noexcept { return m_Snapshots.load(std::memory_order_relaxed); }

	// The failure the writer thread had nowhere to report, kept for whoever joins it — the same route
	// Compositor.cpp already carries a dispatch failure out on.
	[[nodiscard]] const Result<void>& Outcome() const noexcept { return m_Outcome; }

private:
	void Watch();

	// One thread's ring, its storage, and what the trace calls it.
	struct Source
	{
		std::string Name;
		std::unique_ptr<TraceRecord[]> Records;
		std::size_t Capacity = 0;
		std::atomic<std::int32_t> Tid{ 0 };
		TraceBuffer Buffer;
	};

	TracePolicy m_Policy;

	std::array<Source, MaxTraceSources> m_Sources{};
	std::size_t m_Count = 0;

	std::atomic<bool> m_Requested{ false };
	std::atomic<bool> m_Stopping{ false };
	std::atomic<std::uint64_t> m_Snapshots{ 0 };

	// The trigger count at `Start`, which is where this recorder's answerability begins. Written before
	// the writer thread exists and read only by it, so the thread's creation is the ordering.
	std::uint64_t m_Seen = 0;

	TraceLogStore m_Log;

	// The facts and the lock that lets the writer thread read them while the frame thread is still
	// adding the last one. A mutex of its own rather than the condition variable's: that one is the
	// writer thread's sleep, and taking it to say a sentence about scheduling would be two unrelated
	// things behind one name.
	mutable std::mutex m_Described;
	TraceRun m_Run;

	std::mutex m_Mutex;
	std::condition_variable m_Wake;
	std::thread m_Writer;

	Result<void> m_Outcome{};
};

// `gyro.pftrace` and 3 make `gyro-3.pftrace`. Exposed because the composition root's log line names the
// file it just wrote, and computing the name twice is how the two stop agreeing.
[[nodiscard]] std::filesystem::path NumberedTrace(const std::filesystem::path& path, std::uint64_t number);
