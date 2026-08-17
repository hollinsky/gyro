#include "Publication/Return.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Testing/Test.h"

// The return channel's protocol, driven from one thread.
//
// The properties worth asserting here are the ones that make the channel loss-free without a drop
// policy: the watermark merges by maximum, a full queue costs a merge rather than a record, and a
// release the channel could not take is *refused* rather than swallowed. The concurrent half is proven
// in Source/Integration, under ThreadSanitizer, for the reason Ring.Test.cpp gives.

namespace
{
[[nodiscard]] BufferId Buffer(std::uint32_t index) noexcept
{
	return { index, 1 };
}

// Fill the queue exactly, so that the next post is the one that has to merge.
void FillTheQueue(ReturnChannel& channel)
{
	for (std::size_t frame = 0; frame < ReportQueueDepth; ++frame)
	{
		GYRO_CHECK_EQ(channel.Post(frame + 1), std::size_t{ 0 });
	}
}

[[nodiscard]] std::vector<FrameReport> DrainAll(ReturnChannel& channel)
{
	std::vector<FrameReport> reports;

	FrameReport report;
	while (channel.Take(report))
	{
		reports.push_back(report);
	}

	return reports;
}
} // namespace

GYRO_TEST(ReturnChannel, AnEmptyChannelHasNothingToTake)
{
	ReturnChannel channel;
	FrameReport report;

	GYRO_CHECK(!channel.Take(report));
	GYRO_CHECK(!channel.HasStagedReport());
}

GYRO_TEST(ReturnChannel, AReportCrossesWithItsWatermark)
{
	ReturnChannel channel;

	GYRO_CHECK_EQ(channel.Post(7), std::size_t{ 0 });

	FrameReport report;
	GYRO_REQUIRE(channel.Take(report));
	GYRO_CHECK_EQ(report.Watermark, std::uint64_t{ 7 });
	GYRO_CHECK(report.Released().empty());
	GYRO_CHECK(!channel.Take(report));
}

GYRO_TEST(ReturnChannel, ReportsArriveInTheOrderTheyWerePosted)
{
	ReturnChannel channel;

	GYRO_REQUIRE_EQ(channel.Post(1), std::size_t{ 0 });
	GYRO_REQUIRE_EQ(channel.Post(2), std::size_t{ 0 });
	GYRO_REQUIRE_EQ(channel.Post(3), std::size_t{ 0 });

	const std::vector<FrameReport> reports = DrainAll(channel);

	GYRO_REQUIRE_EQ(reports.size(), std::size_t{ 3 });
	GYRO_CHECK_EQ(reports[0].Watermark, std::uint64_t{ 1 });
	GYRO_CHECK_EQ(reports[1].Watermark, std::uint64_t{ 2 });
	GYRO_CHECK_EQ(reports[2].Watermark, std::uint64_t{ 3 });
}

GYRO_TEST(ReturnChannel, HoldsRideAlongsideTheWatermarkAndArriveIntact)
{
	// The one irreducibly per-object record on this channel: an exit blit has landed, so the client
	// buffer it was reading is the dispatch side's again — which the watermark cannot say, because
	// withholding the watermark to say it would stall every unrelated commit below.
	ReturnChannel channel;

	const std::array<BufferId, 2> released{ Buffer(4), Buffer(9) };
	GYRO_CHECK_EQ(channel.Post(12, released), std::size_t{ 2 });

	FrameReport report;
	GYRO_REQUIRE(channel.Take(report));
	GYRO_CHECK_EQ(report.Watermark, std::uint64_t{ 12 });
	GYRO_REQUIRE_EQ(report.Released().size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(report.Released()[0], Buffer(4));
	GYRO_CHECK_EQ(report.Released()[1], Buffer(9));
}

GYRO_TEST(ReturnChannel, MoreHoldsThanFitAreRefusedRatherThanSwallowed)
{
	// The frame thread keeps holding what the channel could not take. Refusing is what makes the loss
	// policy unnecessary: nothing is dropped, so nothing has to be recovered.
	ReturnChannel channel;

	std::vector<BufferId> released;
	for (std::uint32_t index = 0; index < ReleasesPerReport + 3; ++index)
	{
		released.push_back(Buffer(index));
	}

	GYRO_CHECK_EQ(channel.Post(1, released), ReleasesPerReport);

	FrameReport report;
	GYRO_REQUIRE(channel.Take(report));
	GYRO_CHECK_EQ(report.Released().size(), ReleasesPerReport);

	// And the three that were kept go out the following frame, once there is a report to put them in.
	const std::span<const BufferId> kept = std::span<const BufferId>{ released }.subspan(ReleasesPerReport);
	GYRO_CHECK_EQ(channel.Post(2, kept), std::size_t{ 3 });

	GYRO_REQUIRE(channel.Take(report));
	GYRO_REQUIRE_EQ(report.Released().size(), std::size_t{ 3 });
	GYRO_CHECK_EQ(report.Released()[0], Buffer(ReleasesPerReport));
}

GYRO_TEST(ReturnChannel, AFullQueueMergesRatherThanLosingAnything)
{
	// Dispatch has not drained in ReportQueueDepth frames, which means it has not run. The frame thread
	// must not wait for it, and must not throw the report away either — so it amends its own staged
	// report, which is legal because it is the only writer.
	ReturnChannel channel;
	FillTheQueue(channel);

	GYRO_CHECK(!channel.HasStagedReport());

	GYRO_CHECK_EQ(channel.Post(100, std::array<BufferId, 1>{ Buffer(3) }), std::size_t{ 1 });
	GYRO_CHECK(channel.HasStagedReport());

	// A second frame merges into the same staged report: the watermark takes the maximum, the holds
	// accumulate.
	GYRO_CHECK_EQ(channel.Post(101, std::array<BufferId, 1>{ Buffer(5) }), std::size_t{ 1 });
	GYRO_CHECK(channel.HasStagedReport());

	// Draining makes room, and the merged report goes out with the next frame's post rather than being
	// pulled out by the consumer — staging is the writer's, and the consumer never reaches into it.
	const std::vector<FrameReport> backlog = DrainAll(channel);
	GYRO_REQUIRE_EQ(backlog.size(), ReportQueueDepth);
	GYRO_CHECK_EQ(backlog.back().Watermark, ReportQueueDepth);

	GYRO_CHECK_EQ(channel.Post(102), std::size_t{ 0 });
	GYRO_CHECK(!channel.HasStagedReport());

	FrameReport merged;
	GYRO_REQUIRE(channel.Take(merged));
	GYRO_CHECK_EQ(merged.Watermark, std::uint64_t{ 102 });
	GYRO_REQUIRE_EQ(merged.Released().size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(merged.Released()[0], Buffer(3));
	GYRO_CHECK_EQ(merged.Released()[1], Buffer(5));
	GYRO_CHECK(!channel.Take(merged));
}

GYRO_TEST(ReturnChannel, AStagedReportStillRefusesHoldsItCannotHold)
{
	// The merge does not make the capacity elastic. Once the staged report is full the frame thread
	// keeps holding, exactly as it would have if the queue had been empty.
	ReturnChannel channel;
	FillTheQueue(channel);

	std::vector<BufferId> released;
	for (std::uint32_t index = 0; index < ReleasesPerReport; ++index)
	{
		released.push_back(Buffer(index));
	}

	GYRO_REQUIRE_EQ(channel.Post(50, released), ReleasesPerReport);
	GYRO_CHECK_EQ(channel.Post(51, std::array<BufferId, 1>{ Buffer(99) }), std::size_t{ 0 });
}
