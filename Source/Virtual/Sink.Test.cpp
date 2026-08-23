#include "Virtual/Sink.h"

#include <cstdint>
#include <optional>

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

// The consumer end, driven by hand.
//
// The output is driven directly rather than through `VirtualDevice`, because what is under test here
// is what a sink does with a frame — the delivery rule, the completion gate, and the ordering across
// outputs belong to the device and are asserted in Device.Test.cpp. Both run over `HeapAllocator`,
// so both run on a machine with no `/dev/udmabuf`.

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

} // namespace

GYRO_TEST(Sink, ACapturedFrameHoldsWhatWasDrawn)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };
	GYRO_REQUIRE(output.Status().has_value());

	CapturingSink sink{ Resolution, Linear };

	const std::optional<std::uint32_t> target = output.AcquireTarget();
	GYRO_REQUIRE(target.has_value());

	const DmabufBuffer* buffer = output.Buffer(*target);
	GYRO_REQUIRE(buffer != nullptr);
	GYRO_REQUIRE(buffer->IsMapped());

	const Result<MutableImageView> canvas =
		MutableImageView::Over(buffer->Pixels(), buffer->Size(), buffer->Stride(), buffer->Format());
	GYRO_REQUIRE_EQ(canvas.has_value(), true);

	canvas->Fill(canvas->Read().Extent(), Rgb8(0, 0, 0));
	canvas->Fill(PixelRect<DeviceSpace>{ { 2, 1 }, { 3, 2 } }, Rgb8(255, 0, 0));

	const PresentLayer layer = Whole(*target);
	GYRO_REQUIRE_EQ(output.Present({ &layer, 1 }).has_value(), true);

	clock.Advance(Period * 2);
	output.Advance(clock.Now());

	const std::optional<VirtualFrame> presented = output.PresentedFrame();
	GYRO_REQUIRE(presented.has_value());

	sink.OnFrame(output, *presented);

	GYRO_CHECK_EQ(sink.Copied(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(sink.Skipped(), std::uint64_t{ 0 });
	GYRO_REQUIRE_EQ(sink.Retained(), std::size_t{ 1 });

	// The picture, read out of the sink's own copy rather than out of the target — which is the whole
	// point of capturing, since the target is about to be drawn into again.
	const CapturingSink::Capture capture = sink.Newest();
	GYRO_CHECK_EQ(capture.Frame.Target, *target);
	GYRO_CHECK(capture.Image.IsUniform(PixelRect<DeviceSpace>{ { 2, 1 }, { 3, 2 } }, Rgb8(255, 0, 0)));

	const std::optional<PixelRect<DeviceSpace>> drawn = capture.Image.BoundsOfDiffering(Rgb8(0, 0, 0));
	GYRO_REQUIRE_EQ(drawn.has_value(), true);
	GYRO_CHECK_EQ(*drawn, (PixelRect<DeviceSpace>{ { 2, 1 }, { 3, 2 } }));

	// Released inside the notification, so the ring is whole again.
	GYRO_CHECK(!output.PresentedFrame().has_value());
	GYRO_CHECK_EQ(output.FreeTargets(), DefaultVirtualTargets);
}

// The copy is the sink's, so a target drawn into afterwards does not change what was captured. This
// is the property a buffer-age test rests on and it is worth stating on its own.
GYRO_TEST(Sink, ACaptureSurvivesTheTargetBeingRedrawn)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };
	CapturingSink sink{ Resolution, Linear };

	const std::optional<std::uint32_t> target = output.AcquireTarget();
	GYRO_REQUIRE(target.has_value());

	const DmabufBuffer* buffer = output.Buffer(*target);
	const Result<MutableImageView> canvas =
		MutableImageView::Over(buffer->Pixels(), buffer->Size(), buffer->Stride(), buffer->Format());
	GYRO_REQUIRE_EQ(canvas.has_value(), true);

	canvas->Fill(canvas->Read().Extent(), Rgb8(10, 20, 30));

	const PresentLayer layer = Whole(*target);
	GYRO_REQUIRE_EQ(output.Present({ &layer, 1 }).has_value(), true);

	clock.Advance(Period * 2);
	output.Advance(clock.Now());
	sink.OnFrame(output, *output.PresentedFrame());

	canvas->Fill(canvas->Read().Extent(), Rgb8(200, 200, 200));

	GYRO_CHECK(sink.Newest().Image.IsUniform(PixelRect<DeviceSpace>{ {}, Resolution }, Rgb8(10, 20, 30)));
}

// A window of captures is a rotation, oldest first, which is what a test asking "what did frame one
// look like" needs from a sink that has since seen three more.
GYRO_TEST(Sink, TheWindowKeepsTheNewestAndOrdersThemOldestFirst)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };
	CapturingSink sink{ Resolution, Linear, 2 };

	for (std::uint8_t frame = 1; frame <= 3; ++frame)
	{
		const std::optional<std::uint32_t> target = output.AcquireTarget();
		GYRO_REQUIRE(target.has_value());

		const DmabufBuffer* buffer = output.Buffer(*target);
		const Result<MutableImageView> canvas =
			MutableImageView::Over(buffer->Pixels(), buffer->Size(), buffer->Stride(), buffer->Format());
		GYRO_REQUIRE_EQ(canvas.has_value(), true);
		canvas->Fill(canvas->Read().Extent(), Rgb8(frame, 0, 0));

		const PresentLayer layer = Whole(*target);
		GYRO_REQUIRE_EQ(output.Present({ &layer, 1 }).has_value(), true);

		clock.Advance(Period * 2);
		output.Advance(clock.Now());
		sink.OnFrame(output, *output.PresentedFrame());
	}

	GYRO_CHECK_EQ(sink.Copied(), std::uint64_t{ 3 });
	GYRO_REQUIRE_EQ(sink.Retained(), std::size_t{ 2 });

	// The first frame has been evicted; what is left is two and three, in that order.
	GYRO_CHECK(sink.At(0).Image.IsUniform(PixelRect<DeviceSpace>{ {}, Resolution }, Rgb8(2, 0, 0)));
	GYRO_CHECK(sink.At(1).Image.IsUniform(PixelRect<DeviceSpace>{ {}, Resolution }, Rgb8(3, 0, 0)));
	GYRO_CHECK_EQ(sink.Newest().Frame.Sequence, sink.At(1).Frame.Sequence);

	// Past the window is nothing rather than a read off the end, and the empty view's predicates all
	// answer false.
	GYRO_CHECK(!sink.At(2).Image.IsValid());
}

// A consumer that keeps its images stalls the output, which Virtual/Output.h says is the correct
// answer rather than a dropped frame. Nothing in the tree exercised it before this.
GYRO_TEST(Sink, AHoldingConsumerStallsTheRing)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured(), { .Targets = 2 } };
	CapturingSink sink{ Resolution, Linear, 2, FrameRetention::Hold };

	GYRO_REQUIRE_EQ(output.FreeTargets(), 2U);

	for (int frame = 0; frame < 2; ++frame)
	{
		const std::optional<std::uint32_t> target = output.AcquireTarget();
		GYRO_REQUIRE(target.has_value());

		const PresentLayer layer = Whole(*target);
		GYRO_REQUIRE_EQ(output.Present({ &layer, 1 }).has_value(), true);

		clock.Advance(Period * 2);
		output.Advance(clock.Now());
		sink.OnFrame(output, *output.PresentedFrame());
	}

	// Both images are with the consumer, so the frame loop's next acquisition finds nothing — the
	// refusal `AcquireTarget` documents, and the reason the loop skips an output rather than
	// treating it as an error.
	GYRO_CHECK_EQ(sink.Held(), std::size_t{ 2 });
	GYRO_CHECK_EQ(output.FreeTargets(), 0U);
	GYRO_CHECK(!output.AcquireTarget().has_value());

	sink.ReleaseHeld(output);

	GYRO_CHECK_EQ(output.FreeTargets(), 2U);
	GYRO_CHECK(output.AcquireTarget().has_value());
}

// A frame the sink cannot hold is counted and given back, rather than resized around or kept. A sink
// that dropped a frame *and* held its image would stall the output for a reason nothing reported.
GYRO_TEST(Sink, AFrameOfTheWrongShapeIsSkippedAndStillReleased)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };

	// Sized for a different output entirely.
	CapturingSink sink{ PixelSize<DeviceSpace>{ 16, 16 }, Linear };

	const std::optional<std::uint32_t> target = output.AcquireTarget();
	GYRO_REQUIRE(target.has_value());

	const PresentLayer layer = Whole(*target);
	GYRO_REQUIRE_EQ(output.Present({ &layer, 1 }).has_value(), true);

	clock.Advance(Period * 2);
	output.Advance(clock.Now());
	sink.OnFrame(output, *output.PresentedFrame());

	GYRO_CHECK_EQ(sink.Copied(), std::uint64_t{ 0 });
	GYRO_CHECK_EQ(sink.Skipped(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(output.FreeTargets(), DefaultVirtualTargets);
}

// The consumer that looks at nothing is still a consumer, and the difference from having no sink at
// all is that the ring keeps turning.
GYRO_TEST(Sink, TheDiscardingConsumerKeepsTheRingTurning)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured(), { .Targets = 1 } };
	DiscardingSink sink;

	for (int frame = 0; frame < 4; ++frame)
	{
		const std::optional<std::uint32_t> target = output.AcquireTarget();
		GYRO_REQUIRE(target.has_value());

		const PresentLayer layer = Whole(*target);
		GYRO_REQUIRE_EQ(output.Present({ &layer, 1 }).has_value(), true);

		clock.Advance(Period * 2);
		output.Advance(clock.Now());
		sink.OnFrame(output, *output.PresentedFrame());
	}

	// One image, four frames: without a consumer this output would have stopped after the first.
	GYRO_CHECK_EQ(sink.Frames(), std::uint64_t{ 4 });
	GYRO_CHECK_EQ(output.FreeTargets(), 1U);
}
