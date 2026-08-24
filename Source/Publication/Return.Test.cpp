#include "Publication/Return.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Core/Time.h"
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

[[nodiscard]] Instant At(std::int64_t nanoseconds) noexcept
{
	return Monotonic::FromNanoseconds(nanoseconds);
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

GYRO_TEST(ReturnChannel, ThePresentedRunCrossesBesideTheWatermarkAndTheTwoAreNotTheSameNumber)
{
	ReturnChannel channel;

	// The frame thread is reading sequence 9 and output zero last put sequence 7 on the glass, which is
	// the ordinary state of a pipelined output rather than a corner: a commit is in flight and the loop
	// has already acquired newer bytes. A dispatch side that derived frame callbacks from the watermark
	// would announce a frame two scenes ahead of what anybody has seen.
	const std::array<PresentedFrame, 2> presented{ PresentedFrame{ .Sequence = 7, .At = At(1000) }, PresentedFrame{} };

	GYRO_CHECK_EQ(channel.Post(9, {}, presented), std::size_t{ 0 });

	FrameReport report{};
	GYRO_REQUIRE(channel.Take(report));

	GYRO_CHECK_EQ(report.Watermark, std::uint64_t{ 9 });
	GYRO_REQUIRE_EQ(report.Presented().size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(report.Presented()[0].Sequence, std::uint64_t{ 7 });
	GYRO_CHECK(report.Presented()[0].At == At(1000));

	// The second output presented nothing, and says so with a zero rather than by being absent — the run
	// is positional, so a short run would misalign every index after it.
	GYRO_CHECK_EQ(report.Presented()[1].Sequence, std::uint64_t{ 0 });
}

GYRO_TEST(ReturnChannel, AMergedPresentationKeepsTheLatestFrameRatherThanTheFirst)
{
	ReturnChannel channel;
	FillTheQueue(channel);

	const std::array<PresentedFrame, 1> first{ PresentedFrame{ .Sequence = 4, .At = At(1000) } };
	const std::array<PresentedFrame, 1> second{ PresentedFrame{ .Sequence = 5, .At = At(2000) } };

	GYRO_CHECK_EQ(channel.Post(20, {}, first), std::size_t{ 0 });
	GYRO_CHECK_EQ(channel.Post(21, {}, second), std::size_t{ 0 });
	GYRO_CHECK(channel.HasStagedReport());

	const std::vector<FrameReport> reports = DrainAll(channel);
	GYRO_REQUIRE_EQ(reports.size(), ReportQueueDepth);

	// Drain to empty and post again, which is what lets the staged report out.
	GYRO_CHECK_EQ(channel.Post(21), std::size_t{ 0 });

	FrameReport merged{};
	GYRO_REQUIRE(channel.Take(merged));

	// Maximum by sequence, for the watermark's reason: the dispatch side derives *everything up to and
	// including P has been shown*, so the older value carries nothing the newer one does not. What is
	// lost is the exactness of one timestamp, and only while dispatch is already sixteen frames behind.
	GYRO_REQUIRE_EQ(merged.Presented().size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(merged.Presented()[0].Sequence, std::uint64_t{ 5 });
	GYRO_CHECK(merged.Presented()[0].At == At(2000));
}

GYRO_TEST(ReturnChannel, AQuietOutputNeverOverwritesAFlipTheStagedReportIsStillHolding)
{
	ReturnChannel channel;
	FillTheQueue(channel);

	const std::array<PresentedFrame, 1> flipped{ PresentedFrame{ .Sequence = 6, .At = At(3000) } };
	const std::array<PresentedFrame, 1> quiet{ PresentedFrame{} };

	GYRO_CHECK_EQ(channel.Post(30, {}, flipped), std::size_t{ 0 });
	GYRO_CHECK_EQ(channel.Post(31, {}, quiet), std::size_t{ 0 });

	(void)DrainAll(channel);
	GYRO_CHECK_EQ(channel.Post(31), std::size_t{ 0 });

	FrameReport merged{};
	GYRO_REQUIRE(channel.Take(merged));

	// A report goes out every frame whether or not anything flipped, so most posts carry zeroes. If a
	// zero could overwrite, the one flip in sixteen quiet frames would be the one that vanishes — and a
	// vanished flip is a client that never gets its callback and never draws again.
	GYRO_REQUIRE_EQ(merged.Presented().size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(merged.Presented()[0].Sequence, std::uint64_t{ 6 });
}

GYRO_TEST(ReturnChannel, ARunOfADifferentLengthReplacesTheStagedOneRatherThanBlendingIntoIt)
{
	ReturnChannel channel;
	FillTheQueue(channel);

	const std::array<PresentedFrame, 2> before{ PresentedFrame{ .Sequence = 8, .At = At(1000) },
		                                        PresentedFrame{ .Sequence = 8, .At = At(1000) } };
	const std::array<PresentedFrame, 1> after{ PresentedFrame{ .Sequence = 2, .At = At(9000) } };

	GYRO_CHECK_EQ(channel.Post(40, {}, before), std::size_t{ 0 });

	// A monitor was unplugged between the two posts. The indices no longer name the same panels, so the
	// maximum-per-index merge would be comparing one output's sequence against another's — and here the
	// surviving panel's number is *lower*, which the merge would silently discard.
	GYRO_CHECK_EQ(channel.Post(41, {}, after), std::size_t{ 0 });

	(void)DrainAll(channel);
	GYRO_CHECK_EQ(channel.Post(41), std::size_t{ 0 });

	FrameReport merged{};
	GYRO_REQUIRE(channel.Take(merged));

	GYRO_REQUIRE_EQ(merged.Presented().size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(merged.Presented()[0].Sequence, std::uint64_t{ 2 });
	GYRO_CHECK(merged.Presented()[0].At == At(9000));
}
