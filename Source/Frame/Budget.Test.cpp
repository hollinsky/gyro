#include "Frame/Budget.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <string>

#include "Core/Time.h"
#include "Testing/Test.h"

// What is worth testing here is not that a maximum is a maximum. It is the four ways this record can
// be wrong without anything downstream noticing: a cost filed against the wrong population, a
// measurement outliving the configuration it was taken under, a seed that never leaves, and a mark
// that rises and cannot come back down. Each one produces a compositor that schedules confidently
// against a number describing work nobody is doing.

namespace
{
using namespace std::chrono_literals;

// A window short enough to walk an eviction by hand.
BudgetPolicy Windowed(std::int64_t window)
{
	return BudgetPolicy{ .Window = window };
}
} // namespace

GYRO_TEST(Budget, ReservesNothingBeforeTheFirstFrame)
{
	Budget budget;

	GYRO_CHECK_EQ(budget.PlannedCpu(), Duration::zero());
	GYRO_CHECK_EQ(budget.PlannedGpu(), Duration::zero());
	GYRO_CHECK_EQ(budget.MeasuredFloorCpu(), Duration::zero());
	GYRO_CHECK(!budget.FloorExceedsTarget());
	GYRO_CHECK_EQ(budget.Window(), Budget::WindowCapacity);
}

// The seed is a sample, which is the whole of what bounds its life. A seed held as a floor under the
// mark instead would never age out, and a probe's figure taken on a synthetic scene would reserve
// time for the machine's whole run.
GYRO_TEST(Budget, TheSeedLivesExactlyOneWindow)
{
	Budget budget{ BudgetPolicy{ .InitialCpu = 5ms, .Window = 3 } };

	GYRO_CHECK_EQ(budget.PlannedCpu(), 5ms);

	budget.ObserveCpu(RenderMode::Planned, 1ms);
	budget.ObserveCpu(RenderMode::Planned, 1ms);

	GYRO_CHECK_EQ(budget.PlannedCpu(), 5ms);

	// The third real frame is the one that pushes the seed out of a window of three.
	GYRO_CHECK(budget.ObserveCpu(RenderMode::Planned, 1ms));
	GYRO_CHECK_EQ(budget.PlannedCpu(), 1ms);
}

// A mark that only rises reserves the worst moment the machine has ever had for as long as it runs.
GYRO_TEST(Budget, TheMarkComesBackDownWhenTheWorstSampleLeaves)
{
	Budget budget{ Windowed(4) };

	GYRO_CHECK(budget.ObserveCpu(RenderMode::Planned, 6ms));
	GYRO_CHECK(!budget.ObserveCpu(RenderMode::Planned, 2ms));
	GYRO_CHECK(!budget.ObserveCpu(RenderMode::Planned, 2ms));
	GYRO_CHECK_EQ(budget.PlannedCpu(), 6ms);

	// The window holds the seed and three samples; the next one evicts the seed rather than the peak.
	GYRO_CHECK(!budget.ObserveCpu(RenderMode::Planned, 2ms));
	GYRO_CHECK_EQ(budget.PlannedCpu(), 6ms);

	GYRO_CHECK(budget.ObserveCpu(RenderMode::Planned, 2ms));
	GYRO_CHECK_EQ(budget.PlannedCpu(), 2ms);
}

// The rescan has to find a second copy of the maximum rather than assuming the evicted one was the
// only one, which is the case a guard on uniqueness would get wrong.
GYRO_TEST(Budget, ADuplicatedMaximumSurvivesItsOwnEviction)
{
	Budget budget{ Windowed(3) };

	budget.ObserveCpu(RenderMode::Planned, 6ms);
	budget.ObserveCpu(RenderMode::Planned, 6ms);
	GYRO_CHECK_EQ(budget.PlannedCpu(), 6ms);

	// The window holds the seed and the two peaks, so the first sample evicts the seed.
	GYRO_CHECK(!budget.ObserveCpu(RenderMode::Planned, 1ms));

	// This one evicts the first 6ms, and the mark holds because the rescan finds the second.
	GYRO_CHECK(!budget.ObserveCpu(RenderMode::Planned, 1ms));
	GYRO_CHECK_EQ(budget.PlannedCpu(), 6ms);

	GYRO_CHECK(budget.ObserveCpu(RenderMode::Planned, 1ms));
	GYRO_CHECK_EQ(budget.PlannedCpu(), 1ms);
}

// The failure the split exists to prevent. A system in trouble renders the floor tier often, and a
// floor frame diluting the planned window would make admission optimistic at exactly the moment the
// cascade decision 35's third branch stops is already running.
GYRO_TEST(Budget, FloorFramesNeverSizeThePlannedMark)
{
	Budget budget{ Windowed(2) };

	budget.ObserveCpu(RenderMode::Planned, 8ms);
	budget.ObserveGpu(RenderMode::Planned, budget.Generation(), 9ms);

	for (int frame = 0; frame < 8; ++frame)
	{
		GYRO_CHECK(!budget.ObserveCpu(RenderMode::Floor, 1ms));
		GYRO_CHECK(!budget.ObserveGpu(RenderMode::Floor, budget.Generation(), 1ms));
	}

	GYRO_CHECK_EQ(budget.PlannedCpu(), 8ms);
	GYRO_CHECK_EQ(budget.PlannedGpu(), 9ms);
	GYRO_CHECK_EQ(budget.MeasuredFloorCpu(), 1ms);
	GYRO_CHECK_EQ(budget.MeasuredFloorGpu(), 1ms);
}

// Two devices, two populations, two windows. A thermal event moves one and a scheduling disturbance
// moves the other, and a shared window would let either mask the other's recovery.
GYRO_TEST(Budget, TheTwoDevicesAreIndependent)
{
	Budget budget{ Windowed(2) };

	budget.ObserveCpu(RenderMode::Planned, 3ms);

	GYRO_CHECK_EQ(budget.PlannedCpu(), 3ms);
	GYRO_CHECK_EQ(budget.PlannedGpu(), Duration::zero());

	budget.ObserveGpu(RenderMode::Planned, budget.Generation(), 7ms);
	budget.ObserveCpu(RenderMode::Planned, 1ms);
	budget.ObserveCpu(RenderMode::Planned, 1ms);

	// The CPU window has turned over twice while the GPU window has seen one sample.
	GYRO_CHECK_EQ(budget.PlannedCpu(), 1ms);
	GYRO_CHECK_EQ(budget.PlannedGpu(), 7ms);
}

// A 4K frame's execution filed into a 1080p output's mark reserves time nothing will ask for, for a
// whole window. The generation is what stops it, and the readback latency is what makes it possible.
GYRO_TEST(Budget, AMeasurementFromASupersededConfigurationIsDropped)
{
	Budget budget;

	const std::uint32_t inFlight = budget.Generation();
	budget.ObserveCpu(RenderMode::Planned, 4ms);

	budget.Invalidate();

	GYRO_CHECK(budget.Generation() != inFlight);
	GYRO_CHECK(!budget.ObserveGpu(RenderMode::Planned, inFlight, 40ms));
	GYRO_CHECK_EQ(budget.PlannedGpu(), Duration::zero());

	GYRO_CHECK(budget.ObserveGpu(RenderMode::Planned, budget.Generation(), 4ms));
	GYRO_CHECK_EQ(budget.PlannedGpu(), 4ms);
}

// Within a generation nothing is policed, because nothing needs to be: order and duplication cannot
// change a maximum, and rejecting them would only discard evidence.
GYRO_TEST(Budget, LateAndDuplicatedSamplesWithinAGenerationAreKept)
{
	Budget budget{ Windowed(8) };

	const std::uint32_t generation = budget.Generation();

	GYRO_CHECK(budget.ObserveGpu(RenderMode::Planned, generation, 5ms));
	GYRO_CHECK(!budget.ObserveGpu(RenderMode::Planned, generation, 5ms));
	GYRO_CHECK(!budget.ObserveGpu(RenderMode::Planned, generation, 2ms));

	GYRO_CHECK_EQ(budget.PlannedGpu(), 5ms);
}

// The record is cleared and the seed is put back, so an output that has just changed mode is in the
// same state as one that has just been constructed rather than in a third one.
GYRO_TEST(Budget, InvalidationClearsTheRecordAndReseeds)
{
	Budget budget{ BudgetPolicy{ .FloorCpu = 2ms, .InitialCpu = 5ms, .Window = 4 } };

	budget.ObserveCpu(RenderMode::Planned, 9ms);
	budget.ObserveCpu(RenderMode::Floor, 3ms);
	GYRO_CHECK(budget.FloorExceedsTarget());

	budget.Invalidate();

	GYRO_CHECK_EQ(budget.PlannedCpu(), 5ms);
	GYRO_CHECK_EQ(budget.MeasuredFloorCpu(), Duration::zero());
	GYRO_CHECK(!budget.FloorExceedsTarget());

	// The target is policy and survives, where everything measured did not.
	GYRO_CHECK_EQ(budget.FloorCpu(), 2ms);
}

// The direction a broken timestamp breaks in is the one that matters: a negative reservation admits a
// frame with less time than it takes.
GYRO_TEST(Budget, ANegativeMeasurementCannotShortenAReservation)
{
	Budget budget{ Windowed(2) };

	budget.ObserveCpu(RenderMode::Planned, -4ms);
	budget.ObserveCpu(RenderMode::Floor, -4ms);

	GYRO_CHECK_EQ(budget.PlannedCpu(), Duration::zero());
	GYRO_CHECK_EQ(budget.MeasuredFloorCpu(), Duration::zero());
}

// Zero means not yet chosen rather than free, so nothing reports a defect against a figure nobody set.
GYRO_TEST(Budget, AnUnchosenFloorTargetCannotBeExceeded)
{
	Budget budget;

	budget.ObserveCpu(RenderMode::Floor, 50ms);
	budget.ObserveGpu(RenderMode::Floor, budget.Generation(), 50ms);

	GYRO_CHECK_EQ(budget.MeasuredFloorCpu(), 50ms);
	GYRO_CHECK(!budget.FloorExceedsTarget());
}

// The falsification path. Unwindowed on purpose: a figure whose job is to contradict a target must not
// forget the contradiction the way a figure whose job is to size a reservation must.
GYRO_TEST(Budget, AFloorFrameOverTheTargetIsRememberedIndefinitely)
{
	Budget budget{ BudgetPolicy{ .FloorGpu = 2ms, .Window = 2 } };

	budget.ObserveGpu(RenderMode::Floor, budget.Generation(), 3ms);
	GYRO_CHECK(budget.FloorExceedsTarget());

	for (int frame = 0; frame < 16; ++frame)
	{
		budget.ObserveGpu(RenderMode::Floor, budget.Generation(), 1ms);
	}

	GYRO_CHECK_EQ(budget.MeasuredFloorGpu(), 3ms);
	GYRO_CHECK(budget.FloorExceedsTarget());
}

// The length is data a test varies, and the capacity is what bounds the storage. A policy naming
// neither a usable length nor a representable one gets the nearest one that is.
GYRO_TEST(Budget, TheWindowLengthIsClampedIntoTheCapacity)
{
	GYRO_CHECK_EQ(Budget{ Windowed(0) }.Window(), std::size_t{ 1 });
	GYRO_CHECK_EQ(Budget{ Windowed(-9) }.Window(), std::size_t{ 1 });
	GYRO_CHECK_EQ(Budget{ Windowed(1'000'000) }.Window(), Budget::WindowCapacity);
}

// A full window turning over several times, which is the only place the ring's wrap is exercised
// against a maximum that moves through it rather than sitting at one end.
GYRO_TEST(Budget, TheMarkTracksAPeakMovingThroughAFullWindow)
{
	constexpr std::int64_t Window = 4;

	Budget budget{ Windowed(Window) };

	// Fills the window past the seed, then walks a single peak across three turnovers. After each
	// batch the mark must be the maximum of the last four samples and nothing older.
	for (std::int64_t frame = 0; frame < 32; ++frame)
	{
		const Duration cost = (frame % 7 == 0) ? 9ms : 1ms;
		budget.ObserveCpu(RenderMode::Planned, cost);

		const bool peakInWindow = (frame % 7) < Window;

		GYRO_CHECK_EQ(budget.PlannedCpu(), peakInWindow ? 9ms : 1ms);
	}
}

GYRO_TEST(Budget, ThePrintedFormReadsAsABudget)
{
	Budget budget{ BudgetPolicy{ .FloorCpu = 1ms, .FloorGpu = 2ms, .Window = 4 } };
	budget.ObserveCpu(RenderMode::Planned, 3ms);
	budget.ObserveGpu(RenderMode::Planned, budget.Generation(), 4ms);

	GYRO_CHECK_EQ(
		std::format("{}", budget),
		std::string{ "budget gen 0 planned cpu 3000000ns gpu 4000000ns floor cpu 1000000ns gpu 2000000ns" }
	);

	budget.ObserveGpu(RenderMode::Floor, budget.Generation(), 5ms);

	GYRO_CHECK_EQ(
		std::format("{}", budget),
		std::string{ "budget gen 0 planned cpu 3000000ns gpu 4000000ns floor cpu 1000000ns gpu 2000000ns "
	                 "over-floor cpu 0ns gpu 5000000ns" }
	);
}
