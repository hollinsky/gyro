#include "Virtual/Dump.h"

#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "Core/Clock.h"
#include "Core/Time.h"
#include "Geometry/Space.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Seam/SyncPoint.h"
#include "Testing/Test.h"
#include "Virtual/Heap.h"
#include "Virtual/Output.h"
#include "Virtual/Pixels.h"

// The sink that writes, driven by hand against a real output.
//
// **What is asserted is the handoff and not the picture.** Pam.Test.cpp already reads a file back
// and checks that the header and the samples say what the image was; repeating that here would be
// testing the writer twice and the queue not at all. What has never been checked before this file is
// the part that is new: that a frame copied on one thread reaches a file on another, that the image
// goes back to the ring inside `OnFrame` rather than being held until the disk is finished with it,
// and that every frame offered is accounted for exactly once.
//
// **A drop is provoked rather than staged.** Offering frames flat out, with no wall clock between
// them and a one-deep queue, fills the ring faster than any filesystem empties it — which is what
// `EveryFrameOfferedIsAccountedForExactlyOnce` does. What it can assert is the invariant rather than
// a number: a frame offered is queued, dropped, or skipped, and a frame queued is written or failed.
// A drop that went uncounted breaks that sum wherever it happened, which is what the counter is for.
//
// **The output is driven directly rather than through `VirtualDevice`**, for Sink.Test.cpp's reason:
// delivery rules and the completion gate belong to the device and are asserted there. Everything
// here runs over `HeapAllocator`, so it runs on a machine with no `/dev/udmabuf`.

namespace
{
constexpr PixelFormat Linear{ FormatXrgb8888, 0, ModifierLinear };
constexpr PixelSize<DeviceSpace> Resolution{ 8, 4 };
constexpr Duration Period = PeriodFromHertz(60.0);

[[nodiscard]] OutputConfiguration Configured()
{
	return OutputConfiguration{ .Resolution = Resolution, .Period = Period, .Format = Linear };
}

[[nodiscard]] PresentLayer Whole(std::uint32_t target)
{
	return PresentLayer{ .Target = target,
		                 .Blend = BlendMode::Opaque,
		                 .Acquire = SyncPoint::Immediate(),
		                 .Source = {},
		                 .Destination = { {}, Resolution },
		                 .Damage = {} };
}

// Pam.Test.cpp's helper, kept separate rather than shared: a test fixture reached from two files is
// a third thing to keep working, and this one is fifteen lines.
class Scratch
{
public:
	explicit Scratch(std::string_view name)
		: m_Path{ std::format("/tmp/gyro-{}-{}", name, static_cast<long>(::getpid())) }
	{}

	~Scratch()
	{
		for (const std::string& file : m_Written)
		{
			::unlink(file.c_str());
		}

		::rmdir(m_Path.c_str());
	}

	Scratch(const Scratch&) = delete;
	Scratch& operator=(const Scratch&) = delete;
	Scratch(Scratch&&) = delete;
	Scratch& operator=(Scratch&&) = delete;

	[[nodiscard]] const std::string& Path() const noexcept { return m_Path; }

	// Whether `frame-<sequence>.pam` is there, remembering it so that it is removed either way.
	[[nodiscard]] bool Holds(std::uint64_t sequence)
	{
		std::string file = std::format("{}/frame-{:08}.pam", m_Path, sequence);
		m_Written.push_back(file);

		std::FILE* const opened = std::fopen(file.c_str(), "rb");

		if (opened == nullptr)
		{
			return false;
		}

		std::fclose(opened);

		return true;
	}

private:
	std::string m_Path;
	std::vector<std::string> m_Written;
};

// Paint a target, present it, let the period elapse, and hand what came out to the sink. The body of
// every test below, so that each one is about what it asserts afterwards.
[[nodiscard]] std::optional<VirtualFrame> Draw(ManualClock& clock, VirtualOutput& output, Rgba16 colour)
{
	const std::optional<std::uint32_t> target = output.AcquireTarget();

	if (!target)
	{
		return {};
	}

	const DmabufBuffer* const buffer = output.Buffer(*target);
	const Result<MutableImageView> canvas =
		MutableImageView::Over(buffer->Pixels(), buffer->Size(), buffer->Stride(), buffer->Format());

	if (!canvas)
	{
		return {};
	}

	canvas->Fill(canvas->Read().Extent(), colour);

	const PresentLayer layer = Whole(*target);

	if (!output.Present({ &layer, 1 }))
	{
		return {};
	}

	clock.Advance(Period * 2);
	output.Advance(clock.Now());

	return output.PresentedFrame();
}
} // namespace

GYRO_TEST(Dump, AFrameReachesAFileNamedForItsSequence)
{
	Scratch scratch{ "dump-writes" };

	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };
	GYRO_REQUIRE(output.Status().has_value());

	FrameDump dump{ scratch.Path(), Resolution, Linear };
	GYRO_REQUIRE_EQ(dump.Open().has_value(), true);

	const std::optional<VirtualFrame> frame = Draw(clock, output, Rgb8(255, 0, 0));
	GYRO_REQUIRE(frame.has_value());

	dump.OnFrame(output, *frame);

	// The release happens inside the notification, so the ring is whole again before the file exists.
	// That is the property the whole two-thread split is for: the frame loop is not waiting on a disk.
	GYRO_CHECK_EQ(output.FreeTargets(), DefaultVirtualTargets);
	GYRO_CHECK_EQ(dump.Queued(), std::uint64_t{ 1 });

	dump.Close();

	GYRO_CHECK_EQ(dump.Written(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(dump.Dropped(), std::uint64_t{ 0 });
	GYRO_CHECK_EQ(dump.Skipped(), std::uint64_t{ 0 });
	GYRO_CHECK_EQ(dump.Failed(), std::uint64_t{ 0 });
	GYRO_CHECK(scratch.Holds(frame->Sequence));
}

// Several frames, so that the ring wraps and the sequence in a name is the frame's own rather than a
// count of what has been written. A run that drops has gaps, and the gaps have to be findable.
GYRO_TEST(Dump, EveryPresentedFrameIsWrittenUnderItsOwnSequence)
{
	Scratch scratch{ "dump-many" };

	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };

	FrameDump dump{ scratch.Path(), Resolution, Linear };
	GYRO_REQUIRE_EQ(dump.Open().has_value(), true);

	std::vector<std::uint64_t> sequences;

	for (std::uint32_t index = 0; index < 6; ++index)
	{
		const std::optional<VirtualFrame> frame =
			Draw(clock, output, Rgb8(static_cast<std::uint8_t>(index * 40), 0, 0));
		GYRO_REQUIRE(frame.has_value());

		sequences.push_back(frame->Sequence);
		dump.OnFrame(output, *frame);

		// A barrier per frame, because this test is about *which* file each frame becomes and not
		// about the queue absorbing a burst. Offering six frames in the microseconds a loop with no
		// wall clock in it takes is nothing a real output does — a 60 Hz one leaves 16 ms between
		// them — so without this the ring fills and the assertion below would be measuring the test
		// harness. `EveryFrameOfferedIsAccountedForExactlyOnce` is the one that runs it flat out.
		dump.Flush();
	}

	dump.Close();

	GYRO_CHECK_EQ(dump.Queued(), std::uint64_t{ 6 });
	GYRO_CHECK_EQ(dump.Written(), std::uint64_t{ 6 });
	GYRO_CHECK_EQ(dump.Dropped(), std::uint64_t{ 0 });

	for (const std::uint64_t sequence : sequences)
	{
		GYRO_CHECK(scratch.Holds(sequence));
	}
}

// A dump that was never opened accepts frames and keeps the ring turning. This is the state a
// composition root is in if it built the sink and the start failed, and the wrong answer would be an
// output that stalls because nobody releases.
GYRO_TEST(Dump, AnUnopenedDumpStillGivesTheImageBack)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };

	FrameDump dump{ "/nonexistent/gyro", Resolution, Linear };

	const std::optional<VirtualFrame> frame = Draw(clock, output, Rgb8(0, 255, 0));
	GYRO_REQUIRE(frame.has_value());

	dump.OnFrame(output, *frame);

	GYRO_CHECK_EQ(dump.Skipped(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(dump.Queued(), std::uint64_t{ 0 });
	GYRO_CHECK_EQ(output.FreeTargets(), DefaultVirtualTargets);
}

// A frame whose shape is not the one the dump was built for is counted rather than written, and the
// image still goes back. A mode change is the real case: the output reallocates and the sink's slab
// is the old extent until the root builds a new one.
GYRO_TEST(Dump, AFrameOfTheWrongShapeIsSkippedRatherThanWritten)
{
	Scratch scratch{ "dump-shape" };

	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };

	FrameDump dump{ scratch.Path(), PixelSize<DeviceSpace>{ 16, 8 }, Linear };
	GYRO_REQUIRE_EQ(dump.Open().has_value(), true);

	const std::optional<VirtualFrame> frame = Draw(clock, output, Rgb8(0, 0, 255));
	GYRO_REQUIRE(frame.has_value());

	dump.OnFrame(output, *frame);
	dump.Close();

	GYRO_CHECK_EQ(dump.Skipped(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(dump.Written(), std::uint64_t{ 0 });
	GYRO_CHECK_EQ(output.FreeTargets(), DefaultVirtualTargets);
	GYRO_CHECK(!scratch.Holds(frame->Sequence));
}

// The refusals. A format with no decodable sample width and an empty extent are both configurations
// a root can arrive at, and neither should start a thread that has nothing to write.
GYRO_TEST(Dump, AShapeThatCannotBeHeldIsRefusedAtOpen)
{
	FrameDump planar{ "/tmp", Resolution, PixelFormat{ FormatNv12, 0, ModifierLinear } };
	const Result<void> refusedFormat = planar.Open();
	GYRO_REQUIRE_EQ(refusedFormat.has_value(), false);
	GYRO_CHECK_EQ(refusedFormat.error().Code(), EINVAL);

	FrameDump empty{ "/tmp", PixelSize<DeviceSpace>{ 0, 0 }, Linear };
	const Result<void> refusedSize = empty.Open();
	GYRO_REQUIRE_EQ(refusedSize.has_value(), false);
	GYRO_CHECK_EQ(refusedSize.error().Code(), EINVAL);
}

// A second `Open` is a miswiring rather than a restart, and the answer says so instead of leaking
// the thread the first one started.
GYRO_TEST(Dump, OpeningTwiceIsRefused)
{
	Scratch scratch{ "dump-twice" };

	FrameDump dump{ scratch.Path(), Resolution, Linear };
	GYRO_REQUIRE_EQ(dump.Open().has_value(), true);

	const Result<void> again = dump.Open();
	GYRO_REQUIRE_EQ(again.has_value(), false);
	GYRO_CHECK_EQ(again.error().Code(), EALREADY);

	dump.Close();
	dump.Close();
}

// The disk refusing is carried out rather than thrown away, because a run that produced no pictures
// has to be able to say why. The directory here is one `mkdir` cannot create.
GYRO_TEST(Dump, AWriteTheFilesystemRefusesIsReported)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };

	FrameDump dump{ "/proc/gyro-cannot-write-here", Resolution, Linear };
	GYRO_REQUIRE_EQ(dump.Open().has_value(), true);

	const std::optional<VirtualFrame> frame = Draw(clock, output, Rgb8(1, 2, 3));
	GYRO_REQUIRE(frame.has_value());

	dump.OnFrame(output, *frame);
	dump.Close();

	GYRO_CHECK_EQ(dump.Written(), std::uint64_t{ 0 });
	GYRO_CHECK_EQ(dump.Failed(), std::uint64_t{ 1 });
	GYRO_CHECK(dump.FirstFailure().has_value());
}

// The accounting is total, which is what makes `Dropped()` trustworthy when it is not zero. Depth
// one against a writer that has to open a file per frame is the arrangement most likely to drop on a
// loaded machine, and the sum holds whether it does or not.
GYRO_TEST(Dump, EveryFrameOfferedIsAccountedForExactlyOnce)
{
	Scratch scratch{ "dump-accounting" };

	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };

	FrameDump dump{ scratch.Path(), Resolution, Linear, 1 };
	GYRO_REQUIRE_EQ(dump.Open().has_value(), true);

	constexpr std::uint64_t Offered = 32;
	std::vector<std::uint64_t> sequences;

	for (std::uint64_t index = 0; index < Offered; ++index)
	{
		const std::optional<VirtualFrame> frame = Draw(clock, output, Rgb8(0, 0, 0));
		GYRO_REQUIRE(frame.has_value());

		sequences.push_back(frame->Sequence);
		dump.OnFrame(output, *frame);
	}

	dump.Close();

	GYRO_CHECK_EQ(dump.Queued() + dump.Dropped() + dump.Skipped(), Offered);
	GYRO_CHECK_EQ(dump.Written() + dump.Failed(), dump.Queued());
	GYRO_CHECK_EQ(dump.Skipped(), std::uint64_t{ 0 });
	GYRO_CHECK_EQ(dump.Failed(), std::uint64_t{ 0 });

	// Whatever survived is on disk, and nothing that did not is. Remembering every name either way is
	// what makes the scratch directory removable.
	std::uint64_t found = 0;

	for (const std::uint64_t sequence : sequences)
	{
		found += scratch.Holds(sequence) ? std::uint64_t{ 1 } : std::uint64_t{ 0 };
	}

	GYRO_CHECK_EQ(found, dump.Written());
}
