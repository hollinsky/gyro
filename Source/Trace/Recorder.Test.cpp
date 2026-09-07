#include "Trace/Recorder.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "Core/Clock.h"
#include "Core/Trace.h"
#include "Testing/Test.h"

namespace
{

// A path nobody else is using, removed on the way out whatever the test did.
class Scratch
{
public:
	explicit Scratch(std::string_view name)
		: m_Path{ std::filesystem::temp_directory_path() / std::filesystem::path{ name } }
	{
		Remove();
	}

	~Scratch() { Remove(); }

	Scratch(const Scratch&) = delete;
	Scratch& operator=(const Scratch&) = delete;

	[[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_Path; }

	[[nodiscard]] std::uintmax_t Size() const
	{
		std::error_code failed;
		const std::uintmax_t size = std::filesystem::file_size(m_Path, failed);

		return failed ? 0 : size;
	}

private:
	void Remove() const
	{
		std::error_code failed;

		std::filesystem::remove(m_Path, failed);
		std::filesystem::remove(NumberedTrace(m_Path, 1), failed);
	}

	std::filesystem::path m_Path;
};

} // namespace

GYRO_TEST(Recorder, ANumberedSnapshotKeepsTheStem)
{
	const std::filesystem::path numbered = NumberedTrace(std::filesystem::path{ "gyro.pftrace" }, 3);

	GYRO_CHECK_EQ(numbered.filename().string(), std::string{ "gyro-3.pftrace" });
}

GYRO_TEST(Recorder, ANumberedSnapshotKeepsTheDirectory)
{
	const std::filesystem::path numbered = NumberedTrace(std::filesystem::path{ "/var/log/gyro.pftrace" }, 12);

	GYRO_CHECK_EQ(numbered.string(), std::string{ "/var/log/gyro-12.pftrace" });
}

GYRO_TEST(Recorder, ARingIsSizedDownToAPowerOfTwo)
{
	const ManualClock clock;

	// Twenty thousand bytes is six hundred and twenty-five records, so the ring is five hundred and
	// twelve. Rounding up would hand a machine chosen for its memory more than it agreed to.
	Recorder recorder{ TracePolicy{ .Bytes = 20'000 } };

	GYRO_REQUIRE(recorder.Arm("frame", clock) != nullptr);
	GYRO_CHECK_EQ(recorder.Capacity(), std::uint64_t{ 512 });
}

GYRO_TEST(Recorder, ABufferTooSmallToHoldAnythingIsRefused)
{
	const ManualClock clock;
	Recorder recorder{ TracePolicy{ .Bytes = 8 } };

	GYRO_CHECK(recorder.Arm("frame", clock) == nullptr);
	GYRO_CHECK(!recorder.IsRecording());
}

GYRO_TEST(Recorder, SourcesAreBounded)
{
	const ManualClock clock;
	Recorder recorder{ TracePolicy{ .Bytes = 4'096 } };

	for (std::size_t index = 0; index < MaxTraceSources; ++index)
	{
		GYRO_CHECK(recorder.Arm("thread", clock) != nullptr);
	}

	GYRO_CHECK(recorder.Arm("one too many", clock) == nullptr);
}

GYRO_TEST(Recorder, ASnapshotWritesWhatBothThreadsSaid)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1'000'000) };
	Scratch scratch{ "gyro-recorder-test.pftrace" };

	Recorder recorder{ TracePolicy{ .Bytes = 64 * 1024, .Path = scratch.Path(), .Pid = 4321 } };

	TraceBuffer* const frame = recorder.Arm("frame", clock);
	TraceBuffer* const dispatch = recorder.Arm("dispatch", clock);

	GYRO_REQUIRE(frame != nullptr && dispatch != nullptr);

	recorder.Join(*frame, 4322);

	{
		const TraceSpan iteration{ "iteration" };

		clock.Advance(std::chrono::milliseconds{ 4 });
		TraceCount("held", 3);
	}

	std::thread other{ [&recorder, dispatch, &clock] {
		recorder.Join(*dispatch, 4323);

		clock.Advance(std::chrono::milliseconds{ 1 });

		const TraceSpan step{ "step" };
		TraceMark("published", TraceThread, TraceFlow(TraceDomain::Scene, 12));
	} };

	other.join();
	EnrollTracing(nullptr);

	const Result<TraceSummary> written = recorder.Snapshot(scratch.Path());

	GYRO_REQUIRE(written.has_value());

	// Two slices, a counter, an instant: three from the frame thread and three from the other one.
	GYRO_CHECK_EQ(written->Events, std::size_t{ 6 });
	GYRO_CHECK_EQ(written->Covered, Duration{ std::chrono::milliseconds{ 5 } });
	GYRO_CHECK(written->Bytes != 0);

	GYRO_CHECK_EQ(scratch.Size(), static_cast<std::uintmax_t>(written->Bytes));
	GYRO_CHECK_EQ(recorder.Snapshots(), std::uint64_t{ 1 });

	// The partial file is the one a reader must never find, because a trace that is half written opens
	// as a trace that is half true.
	std::filesystem::path partial = scratch.Path();
	partial += ".partial";

	GYRO_CHECK(!std::filesystem::exists(partial));
}

GYRO_TEST(Recorder, RecordingNothingIsRefusedRatherThanWritingAnEmptyFile)
{
	Scratch scratch{ "gyro-recorder-empty.pftrace" };
	Recorder recorder{ TracePolicy{ .Path = scratch.Path() } };

	GYRO_CHECK(!recorder.Snapshot(scratch.Path()).has_value());
	GYRO_CHECK(!std::filesystem::exists(scratch.Path()));
}

GYRO_TEST(Recorder, ARequestIsAnsweredByTheWriterThread)
{
	const MonotonicClock clock;
	Scratch scratch{ "gyro-recorder-request.pftrace" };

	Recorder recorder{ TracePolicy{
		.Bytes = 64 * 1024, .Path = scratch.Path(), .Poll = std::chrono::milliseconds{ 5 } } };

	TraceBuffer* const frame = recorder.Arm("frame", clock);

	GYRO_REQUIRE(frame != nullptr);

	recorder.Join(*frame, 0);
	TraceMark("something happened");

	recorder.Start();
	recorder.Request();

	// The signal handler stores a flag and the writer thread notices it on its own schedule, so what is
	// being waited for here is the interval rather than a handshake.
	for (int attempt = 0; attempt < 400 && recorder.Snapshots() == 0; ++attempt)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
	}

	recorder.Stop();
	EnrollTracing(nullptr);

	GYRO_CHECK_EQ(recorder.Snapshots(), std::uint64_t{ 1 });
	GYRO_CHECK(std::filesystem::exists(NumberedTrace(scratch.Path(), 1)));
	GYRO_CHECK(recorder.Outcome().has_value());
}

// The trigger is the process asking for the file when the anomaly is too rare for a person to ask in
// time. Answered once and then disarmed, because the conditions worth arming it for recur — and a
// trigger that stayed armed would answer a busy afternoon with a directory of near-identical files.
GYRO_TEST(Recorder, ATriggerIsAnsweredOnceAndThenDisarmed)
{
	const MonotonicClock clock;
	Scratch scratch{ "gyro-recorder-trigger.pftrace" };

	Recorder recorder{ TracePolicy{
		.Bytes = 64 * 1024, .Path = scratch.Path(), .Poll = std::chrono::milliseconds{ 5 }, .OnTrigger = true } };

	TraceBuffer* const frame = recorder.Arm("frame", clock);

	GYRO_REQUIRE(frame != nullptr);

	recorder.Join(*frame, 0);
	TraceMark("landed late");

	recorder.Start();
	TraceTrigger();

	for (int attempt = 0; attempt < 400 && recorder.Snapshots() == 0; ++attempt)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
	}

	GYRO_CHECK_EQ(recorder.Snapshots(), std::uint64_t{ 1 });

	// Disarmed by its own firing: a second trigger is counted by Core and answered by nobody.
	TraceTrigger();
	std::this_thread::sleep_for(std::chrono::milliseconds{ 50 });

	recorder.Stop();
	EnrollTracing(nullptr);

	GYRO_CHECK_EQ(recorder.Snapshots(), std::uint64_t{ 1 });
	GYRO_CHECK(std::filesystem::exists(NumberedTrace(scratch.Path(), 1)));
	GYRO_CHECK(recorder.Outcome().has_value());
}

// A recorder whose policy did not ask treats the counter as somebody else's — the same firing, no
// file. The baseline is `Start`'s, so a trigger left over from an earlier arming is not answered
// either, which is what keeps two recorders in one process from stealing each other's requests.
GYRO_TEST(Recorder, ATriggerIsIgnoredWhereThePolicyDidNotAsk)
{
	const MonotonicClock clock;
	Scratch scratch{ "gyro-recorder-unarmed.pftrace" };

	Recorder recorder{ TracePolicy{
		.Bytes = 64 * 1024, .Path = scratch.Path(), .Poll = std::chrono::milliseconds{ 5 } } };

	TraceBuffer* const frame = recorder.Arm("frame", clock);

	GYRO_REQUIRE(frame != nullptr);

	recorder.Join(*frame, 0);
	TraceMark("something happened");

	recorder.Start();
	TraceTrigger();

	std::this_thread::sleep_for(std::chrono::milliseconds{ 50 });

	recorder.Stop();
	EnrollTracing(nullptr);

	GYRO_CHECK_EQ(recorder.Snapshots(), std::uint64_t{ 0 });
	GYRO_CHECK(!std::filesystem::exists(NumberedTrace(scratch.Path(), 1)));
}

// A GPU span arrives in the ring long after the frame-thread records around it, so what the writer
// copies is not in time order. `Covered` is the cheapest place that shows: it is the front of the
// snapshot against the back, which is only the window if the sort happened.
GYRO_TEST(Recorder, ALateRecordIsSortedBackIntoItsPlace)
{
	const MonotonicClock clock;
	Scratch scratch{ "gyro-recorder-late.pftrace" };

	Recorder recorder{ TracePolicy{ .Bytes = 64 * 1024, .Path = scratch.Path() } };

	TraceBuffer* const frame = recorder.Arm("frame", clock);

	GYRO_REQUIRE(frame != nullptr);

	recorder.Join(*frame, 0);

	// Emitted second and stamped first, which is the shape `Core/Trace.h`'s `EmitAt` exists for.
	TraceMark("now");
	TraceSpanAt("composite", Monotonic::FromNanoseconds(1), Monotonic::FromNanoseconds(2), TraceGpu(0));

	const Result<TraceSummary> written = recorder.Snapshot(scratch.Path());

	EnrollTracing(nullptr);

	GYRO_REQUIRE(written.has_value());
	GYRO_CHECK_EQ(written->Events, std::size_t{ 3 });

	// Unsorted this is the distance from *now* back to the epoch, which is negative. Sorted it is the
	// distance from the epoch to now, which is the whole age of the machine and is at least positive.
	GYRO_CHECK(written->Covered > Duration::zero());
}

// **The store keeps the end of the window rather than the beginning**, which is the ring's own rule:
// a snapshot is asked for just after the thing being chased, so the lines worth having are the last
// ones written rather than the first.
GYRO_TEST(Recorder, TheLogStoreDropsTheOldest)
{
	const ManualClock clock;
	TraceLogStore store;

	store.Arm(2, clock);

	store.Write("info", "first");
	store.Write("info", "second");
	store.Write("warn", "third");

	const std::vector<TraceLogRecord> kept = store.Copy();

	GYRO_REQUIRE_EQ(kept.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(std::string(kept[0].Message.data(), kept[0].Length), std::string{ "second" });
	GYRO_CHECK_EQ(std::string(kept[1].Message.data(), kept[1].Length), std::string{ "third" });
	GYRO_CHECK_EQ(std::string{ kept[1].Level }, std::string{ "warn" });
}

// A message is whatever a caller formatted — a client's own title can be in it — so its length is
// somebody else's to decide, and the buffer is fixed.
GYRO_TEST(Recorder, ALongMessageIsCutRatherThanOverrunning)
{
	const ManualClock clock;
	TraceLogStore store;

	store.Arm(4, clock);

	const std::string long_message(TraceMessageLimit + 64, 'x');

	store.Write("info", long_message);

	const std::vector<TraceLogRecord> kept = store.Copy();

	GYRO_REQUIRE_EQ(kept.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(std::size_t{ kept[0].Length }, TraceMessageLimit);
}

// `--trace-buffer=0` means record nothing, and that has to include the log: a sink armed against a
// recorder that holds no rings is memory spent on a picture nobody will ever be handed.
GYRO_TEST(Recorder, AZeroBufferArmsNoLogEither)
{
	const ManualClock clock;
	Recorder recorder{ TracePolicy{ .Bytes = 0 } };

	GYRO_CHECK(recorder.Arm("frame", clock) == nullptr);
	GYRO_CHECK(recorder.ArmLog(clock) == nullptr);
}

// The plumbing rather than the encoding: Trace/Perfetto.Test.cpp proves the bytes are shaped right,
// and what this proves is that what the composition root said reaches them.
GYRO_TEST(Recorder, ASnapshotCarriesWhatTheRootDescribed)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1'000'000) };
	Scratch scratch{ "gyro-recorder-identity.pftrace" };

	Recorder recorder{ TracePolicy{ .Bytes = 64 * 1024, .Path = scratch.Path(), .Pid = 4321 } };

	TraceBuffer* const frame = recorder.Arm("frame", clock);
	TraceLogStore* const log = recorder.ArmLog(clock);

	GYRO_REQUIRE(frame != nullptr && log != nullptr);

	recorder.Join(*frame, 4322);

	TraceRun run;
	run.CommandLine = { "gyro", "--backend=headless" };
	run.Facts = { { "backend", "headless" } };

	recorder.Describe(std::move(run));
	recorder.Note("scheduling", "SCHED_FIFO at 20");

	log->Write("info", "the panel came back");

	{
		const TraceSpan iteration{ "iteration" };

		clock.Advance(std::chrono::milliseconds{ 4 });
	}

	GYRO_REQUIRE(recorder.Snapshot(scratch.Path()));

	std::ifstream file{ scratch.Path(), std::ios::binary };
	const std::string bytes{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };

	GYRO_CHECK(bytes.find("backend=headless") != std::string::npos);
	GYRO_CHECK(bytes.find("scheduling=SCHED_FIFO at 20") != std::string::npos);
	GYRO_CHECK(bytes.find("--backend=headless") != std::string::npos);
	GYRO_CHECK(bytes.find("the panel came back") != std::string::npos);
}
