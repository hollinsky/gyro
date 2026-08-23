#include "Virtual/Device.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Core/Clock.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Geometry/Space.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Seam/SyncPoint.h"
#include "Testing/Test.h"
#include "Virtual/Heap.h"
#include "Virtual/Sink.h"

// The device: what a drain does, in what order, and what it refuses to hand over yet.

namespace
{
constexpr PixelFormat Linear{ FormatXrgb8888, 0, ModifierLinear };
constexpr PixelSize<DeviceSpace> Resolution{ 8, 4 };
constexpr Duration Fast = PeriodFromHertz(120.0);
constexpr Duration Slow = PeriodFromHertz(60.0);

[[nodiscard]] OutputConfiguration Configured(Duration period)
{
	return OutputConfiguration{ .Resolution = Resolution, .Period = period, .Format = Linear };
}

[[nodiscard]] PresentLayer Whole(std::uint32_t target, SyncPoint acquire)
{
	return PresentLayer{ .Target = target,
		                 .Blend = BlendMode::Opaque,
		                 .Acquire = acquire,
		                 .Source = {},
		                 .Destination = { {}, Resolution },
		                 .Damage = {} };
}

// A renderer that submits nothing and answers whatever the test says about completion.
//
// It is only ever asked `IsComplete`, which is the one verb the device uses — everything else is
// present so that this is an `IRenderer` rather than a second interface invented for the device to
// hold. Seam/Renderer.h's argument for `IsComplete` being a poll is exactly why the device can hold
// one at all.
class ScriptedRenderer final : public IRenderer
{
public:
	[[nodiscard]] Result<void> BindTargets(std::span<const RenderTarget>, ColorState) override { return {}; }

	void ReleaseTargets() noexcept override {}

	[[nodiscard]] Result<Submission> Record(const RecordRequest&) override
	{
		return Submission{ .Point = SyncPoint::Immediate(), .RecordCost = {} };
	}

	[[nodiscard]] bool IsComplete(SyncPoint) const override { return Finished; }

	[[nodiscard]] std::size_t CollectCosts(std::span<GpuCost>) override { return 0; }

	bool Finished = true;
};

// A sink that records the order frames arrived in across every output it is attached to, which is
// the property the device's ordering sort is for.
class OrderSink final : public IFrameSink
{
public:
	struct Arrival
	{
		const VirtualOutput* From = nullptr;
		Instant At{};
	};

	void OnFrame(VirtualOutput& output, const VirtualFrame& frame) override
	{
		Arrivals.push_back({ &output, frame.At });
		output.Release(frame.Target);
	}

	std::vector<Arrival> Arrivals;
};

// Acquire, present, and leave it to the device to advance and deliver.
void Submit(VirtualOutput& output, SyncPoint acquire = SyncPoint::Immediate())
{
	const std::optional<std::uint32_t> target = output.AcquireTarget();

	GYRO_REQUIRE(target.has_value());

	const PresentLayer layer = Whole(*target, acquire);
	GYRO_REQUIRE_EQ(output.Present({ &layer, 1 }).has_value(), true);
}
} // namespace

GYRO_TEST(VirtualDevice, ADrainAdvancesTheOutputsAndFeedsTheirConsumers)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualDevice device{ clock };
	DiscardingSink sink;

	VirtualOutput* const output = device.Add(Configured(Slow), allocator, sink);
	GYRO_REQUIRE(output != nullptr);
	GYRO_REQUIRE(output->Status().has_value());

	Submit(*output);

	// Before the boundary, nothing has happened — a drain reports what the clock says, and the clock
	// has not moved.
	GYRO_REQUIRE_EQ(device.Drain().has_value(), true);
	GYRO_CHECK_EQ(sink.Frames(), std::uint64_t{ 0 });

	clock.Advance(Slow * 2);
	GYRO_REQUIRE_EQ(device.Drain().has_value(), true);

	GYRO_CHECK_EQ(sink.Frames(), std::uint64_t{ 1 });

	// And a second drain does not deliver it again, which is what keeps a holding consumer from being
	// handed the same image on every iteration.
	GYRO_REQUIRE_EQ(device.Drain().has_value(), true);
	GYRO_CHECK_EQ(sink.Frames(), std::uint64_t{ 1 });
}

// Frames reach their consumers in the order the outputs produced them, which is what a reader
// assumes without checking and what construction order would otherwise decide.
GYRO_TEST(VirtualDevice, FramesArriveInTheOrderTheOutputsProducedThem)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualDevice device{ clock };
	OrderSink sink;

	// Added slowest first, so slot order is the opposite of event order and an unsorted drain would
	// report the wrong one.
	VirtualOutput* const slow = device.Add(Configured(Slow), allocator, sink);
	VirtualOutput* const fast = device.Add(Configured(Fast), allocator, sink);
	GYRO_REQUIRE(slow != nullptr && fast != nullptr);

	Submit(*slow);
	Submit(*fast);

	clock.Advance(Slow * 2);
	GYRO_REQUIRE_EQ(device.Drain().has_value(), true);

	GYRO_REQUIRE_EQ(sink.Arrivals.size(), std::size_t{ 2 });

	// The 120 Hz output's boundary is the earlier one, so it is what arrived first — and it is the
	// output rather than the instant that is asserted, since the instants agreeing would also be
	// satisfied by a drain that reported both in slot order.
	GYRO_CHECK(sink.Arrivals[0].From == fast);
	GYRO_CHECK(sink.Arrivals[1].From == slow);
	GYRO_CHECK(sink.Arrivals[0].At < sink.Arrivals[1].At);
}

// The completion gate: a consumer never reads pixels the composite has not finished writing.
GYRO_TEST(VirtualDevice, AFrameIsWithheldUntilItsCompositeHasFinished)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualDevice device{ clock };
	DiscardingSink sink;
	ScriptedRenderer renderer;

	VirtualOutput* const output = device.Add(Configured(Slow), allocator, sink, &renderer);
	GYRO_REQUIRE(output != nullptr);

	renderer.Finished = false;

	// A point that is not immediate, which is what a device able to export a timeline hands back.
	Submit(*output, SyncPoint{ RawFd{ 3 }, 1 });

	clock.Advance(Slow * 2);
	GYRO_REQUIRE_EQ(device.Drain().has_value(), true);

	// The flip has landed — the presenter reported it and the image is held — but the pixels are not
	// the consumer's yet.
	GYRO_CHECK(output->PresentedFrame().has_value());
	GYRO_CHECK_EQ(sink.Frames(), std::uint64_t{ 0 });
	GYRO_CHECK_EQ(device.Awaiting(), std::size_t{ 1 });

	// And the device asks to be looked at again rather than folding to never, which is the deadlock
	// this would otherwise be: nothing else was going to wake the loop for this output.
	GYRO_CHECK(device.NextEvent() < Instant{ Duration::max() });

	renderer.Finished = true;
	GYRO_REQUIRE_EQ(device.Drain().has_value(), true);

	GYRO_CHECK_EQ(sink.Frames(), std::uint64_t{ 1 });
	GYRO_CHECK_EQ(device.Awaiting(), std::size_t{ 0 });
}

// With no renderer to ask, every point is treated as finished — which is right for a blitter and for
// a device that cannot export a timeline, and is the only honest reading of "nobody can tell me".
GYRO_TEST(VirtualDevice, WithoutARendererAFrameIsDeliveredAtOnce)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualDevice device{ clock };
	DiscardingSink sink;

	VirtualOutput* const output = device.Add(Configured(Slow), allocator, sink);
	GYRO_REQUIRE(output != nullptr);

	Submit(*output, SyncPoint{ RawFd{ 3 }, 1 });

	clock.Advance(Slow * 2);
	GYRO_REQUIRE_EQ(device.Drain().has_value(), true);
	GYRO_CHECK_EQ(sink.Frames(), std::uint64_t{ 1 });
}

// Seam/EventSource.h's ordinary answer: nothing becomes readable when a virtual boundary falls due.
GYRO_TEST(VirtualDevice, TheDescriptorIsInvalidAndThatIsTheAnswer)
{
	ManualClock clock;
	VirtualDevice device{ clock };

	GYRO_CHECK(!device.Descriptor().IsValid());

	// An empty device has nothing to say and folds to never, which is the identity the composition
	// root's wake fold needs rather than a case for it to test.
	GYRO_CHECK_EQ(device.NextEvent(), Instant{ Duration::max() });
	GYRO_CHECK_EQ(device.Drain().has_value(), true);
}

GYRO_TEST(VirtualDevice, AFullDeviceRefusesRatherThanOverruns)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualDevice device{ clock };
	DiscardingSink sink;

	// Unpowered, so a full device costs no memory to arrange.
	OutputConfiguration dark = Configured(Slow);
	dark.Powered = false;

	for (std::size_t index = 0; index < MaxVirtualOutputs; ++index)
	{
		GYRO_REQUIRE(device.Add(dark, allocator, sink) != nullptr);
	}

	GYRO_CHECK_EQ(device.Count(), MaxVirtualOutputs);
	GYRO_CHECK(device.Add(dark, allocator, sink) == nullptr);
}
