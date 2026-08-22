#include "Headless/Device.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "Core/Clock.h"
#include "Core/Signal.h"
#include "Core/Time.h"
#include "Headless/Output.h"
#include "Seam/EventSource.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/PresentationInfo.h"
#include "Seam/Presenter.h"
#include "Seam/SyncPoint.h"
#include "Testing/Test.h"

// One source, N outputs, which is decision 80's granularity rather than an economy. What is worth
// checking is exactly that: that the source is the device's, that a drain reports every output's
// completions in the order the panels produced them, and that a descriptor nobody can wait on is an
// ordinary answer rather than a failure.

namespace
{
using namespace std::chrono_literals;

constexpr Instant At(std::int64_t milliseconds) noexcept
{
	return Monotonic::FromNanoseconds(milliseconds * 1'000'000);
}

OutputConfiguration Panel(Duration period)
{
	OutputConfiguration configuration;
	configuration.Resolution = { 640, 480 };
	configuration.Period = period;

	return configuration;
}

// Records which output spoke and when, which is the only thing these cases assert about.
class Listener
{
public:
	void Watch(HeadlessOutput& output, int identity)
	{
		m_Identity = identity;
		m_Connection.ConnectTo<&Listener::Presented>(output.Presented, *this);
	}

	std::vector<std::pair<int, std::uint64_t>>* Log = nullptr;

private:
	void Presented(const PresentationInfo& info) { Log->emplace_back(m_Identity, info.Sequence); }

	int m_Identity = 0;
	Connection<const PresentationInfo&> m_Connection;
};

bool Commit(HeadlessOutput& output)
{
	const std::optional<std::uint32_t> target = output.AcquireTarget();

	if (!target)
	{
		return false;
	}

	const PixelSize<DeviceSpace> size = output.Configuration().Resolution;
	const PresentLayer layer{ .Target = *target,
		                      .Acquire = SyncPoint::Immediate(),
		                      .Source = { {}, { static_cast<float>(size.Width), static_cast<float>(size.Height) } },
		                      .Destination = { {}, size },
		                      .Damage = {} };

	return output.Present({ &layer, 1 }).has_value();
}
} // namespace

// Seam/EventSource.h calls this an ordinary answer, and it is the second argument in that file against
// handing the loop a readiness set: a source with no descriptor could never appear in one, so headless
// would never present.
GYRO_TEST(HeadlessDevice, TheSourceHasNoDescriptorAndIsStillDrained)
{
	ManualClock clock{ At(1000) };
	HeadlessDevice device{ clock };
	HeadlessOutput* panel = device.Add(Panel(10ms));

	GYRO_REQUIRE(panel != nullptr);
	GYRO_CHECK(!device.Descriptor().IsValid());

	// It is an IEventSource to whoever holds it, and nothing about the drain depends on the descriptor.
	IEventSource& source = device;
	GYRO_CHECK(Commit(*panel));
	clock.Set(At(1011));
	GYRO_CHECK(source.Drain().has_value());
	GYRO_CHECK(!panel->IsFlipPending());
}

// The granularity claim: one drain serves every output on the device.
GYRO_TEST(HeadlessDevice, OneDrainServesEveryOutput)
{
	ManualClock clock{ At(1000) };
	HeadlessDevice device{ clock };

	HeadlessOutput* fast = device.Add(Panel(7ms));
	HeadlessOutput* slow = device.Add(Panel(16ms));
	GYRO_REQUIRE(fast != nullptr && slow != nullptr);

	GYRO_CHECK(Commit(*fast));
	GYRO_CHECK(Commit(*slow));

	clock.Set(At(1020));
	(void)device.Drain();

	GYRO_CHECK(!fast->IsFlipPending());
	GYRO_CHECK(!slow->IsFlipPending());
	GYRO_CHECK_EQ(device.Count(), std::size_t{ 2 });
}

// Oldest first, so the flips a drain reports arrive in the order the panels produced them rather than
// in the order the outputs were constructed. The slow panel is added first and flips second.
GYRO_TEST(HeadlessDevice, CompletionsAreOrderedByWhenTheyHappened)
{
	ManualClock clock{ At(1000) };
	HeadlessDevice device{ clock };

	HeadlessOutput* slow = device.Add(Panel(16ms));
	HeadlessOutput* fast = device.Add(Panel(7ms));
	GYRO_REQUIRE(slow != nullptr && fast != nullptr);

	std::vector<std::pair<int, std::uint64_t>> log;
	Listener first;
	Listener second;
	first.Log = &log;
	second.Log = &log;
	first.Watch(*slow, 0);
	second.Watch(*fast, 1);

	GYRO_CHECK(Commit(*slow));
	GYRO_CHECK(Commit(*fast));

	clock.Set(At(1020));
	(void)device.Drain();

	GYRO_REQUIRE_EQ(log.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(log[0].first, 1);
	GYRO_CHECK_EQ(log[1].first, 0);
}

// Nothing due is success and reports nothing, which Seam/EventSource.h names as the ordinary result of
// a wakeup some other source caused.
GYRO_TEST(HeadlessDevice, ADrainWithNothingDueIsSuccess)
{
	ManualClock clock{ At(1000) };
	HeadlessDevice device{ clock };
	HeadlessOutput* panel = device.Add(Panel(10ms));

	GYRO_REQUIRE(panel != nullptr);
	GYRO_CHECK(Commit(*panel));

	GYRO_CHECK(device.Drain().has_value());
	GYRO_CHECK(panel->IsFlipPending());
}

// What the simulated hardware does next, which is not what gyro is owed next. See the header for why
// this is not a Wake.
GYRO_TEST(HeadlessDevice, NextEventIsTheEarliestOutstandingFlip)
{
	ManualClock clock{ At(1000) };
	HeadlessDevice device{ clock };

	HeadlessOutput* slow = device.Add(Panel(16ms));
	HeadlessOutput* fast = device.Add(Panel(7ms));
	GYRO_REQUIRE(slow != nullptr && fast != nullptr);

	// Nothing outstanding is the identity of the fold rather than a case to test for.
	GYRO_CHECK_EQ(device.NextEvent(), Instant{ Duration::max() });

	GYRO_CHECK(Commit(*slow));
	GYRO_CHECK_EQ(device.NextEvent(), At(1016));

	GYRO_CHECK(Commit(*fast));
	GYRO_CHECK_EQ(device.NextEvent(), At(1007));
}

// A full device says so rather than writing past the end of its slots, because past capacity is a
// miswired composition root rather than a runtime condition.
GYRO_TEST(HeadlessDevice, AFullDeviceRefusesAnotherOutput)
{
	ManualClock clock{ At(1000) };
	HeadlessDevice device{ clock };

	for (std::size_t index = 0; index < MaxHeadlessOutputs; ++index)
	{
		GYRO_REQUIRE(device.Add(Panel(10ms)) != nullptr);
	}

	GYRO_CHECK(device.Add(Panel(10ms)) == nullptr);
	GYRO_CHECK_EQ(device.Count(), MaxHeadlessOutputs);
}

// The phase is the parameter a schedulability sweep varies, and it is what makes two outputs at the
// same rate genuinely different task sets.
GYRO_TEST(HeadlessDevice, PhaseSeparatesTwoOutputsAtTheSameRate)
{
	ManualClock clock{ At(1000) };
	HeadlessDevice device{ clock };

	HeadlessOutput* aligned = device.Add(Panel(10ms));
	HeadlessOutput* offset = device.Add(Panel(10ms), HeadlessOutputPolicy{ .Phase = 3ms });
	GYRO_REQUIRE(aligned != nullptr && offset != nullptr);

	GYRO_CHECK(Commit(*aligned));
	GYRO_CHECK(Commit(*offset));

	GYRO_CHECK_EQ(aligned->Vblank().At(1), At(1010));
	GYRO_CHECK_EQ(offset->Vblank().At(1), At(1013));
	GYRO_CHECK_EQ(device.NextEvent(), At(1003));
}
