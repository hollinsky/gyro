#include "Trace/Recorder.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

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
		TraceMark("published", TraceThread, 12);
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
