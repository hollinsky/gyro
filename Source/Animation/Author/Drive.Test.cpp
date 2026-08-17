#include "Animation/Author/Drive.h"

#include <format>
#include <limits>

#include "Testing/Test.h"

// The runtime half of Drive.h's contract. The compile-time half is the static_assert block at the
// foot of that header — the projection's three properties, both ends of the band, the hard clamp, and
// the slope at the join — and is not repeated here.
//
// What is left is the two properties that are about the whole range rather than about any point in
// it. Both are swept rather than sampled, because a point assertion cannot distinguish a curve that
// behaves from one that happens to behave at the points somebody chose.

namespace
{
constexpr DriveMapping Swipe{ .Travel = { 1200.0, 0.0 }, .RubberBand = 0.1 };

// Three travels either side, so the sweep spends most of its length outside the ends where the
// interesting arithmetic is, and crosses both boundaries rather than approaching them.
constexpr double Extent = 3600.0;
constexpr int Steps = 4000;

[[nodiscard]] Offset<GlobalSpace> At(int step) noexcept
{
	return { -Extent + 2.0 * Extent * static_cast<double>(step) / static_cast<double>(Steps), 0.0 };
}
} // namespace

// A finger moving one way never moves the content the other way.
//
// The property that makes the driven regime worth having, and the one a rubber band is the obvious
// way to break: a curve that overshot and came back would put the content briefly in reverse under a
// finger that never reversed, which reads as the picture twitching and is attributed to anything but
// the mapping. Non-strict, because the band is asymptotic and far enough past the end two adjacent
// samples round to the same progress — that is the curve flattening, which is the intent, rather than
// the curve turning.
GYRO_TEST(Drive, ProgressNeverGoesBackwardsUnderAFingerThatDoesNot)
{
	double previous = Progress(Swipe, At(0));

	for (int step = 1; step <= Steps; ++step)
	{
		const Offset<GlobalSpace> displacement = At(step);
		const double current = Progress(Swipe, displacement);

		if (current < previous)
		{
			GYRO_FAIL(std::format("progress fell from {} to {} at {}", previous, current, displacement));
		}

		previous = current;
	}
}

// However hard the gesture is pulled, the content stays within a band of the end.
//
// The bound is what lets everything downstream treat progress as very nearly a fraction: a channel
// reading it does not need its own guard, and a bundle's designer can reason about what the value at
// each end looks like without knowing how far somebody swiped.
GYRO_TEST(Drive, ProgressStaysInsideTheBandHoweverFarTheGestureGoes)
{
	for (int step = 0; step <= Steps; ++step)
	{
		const double progress = Progress(Swipe, At(step));

		GYRO_CHECK(progress >= -Swipe.RubberBand);
		GYRO_CHECK(progress <= 1.0 + Swipe.RubberBand);
	}

	// Past the sweep entirely, at a displacement no touchpad could produce, because the bound is
	// asymptotic rather than a range the sweep happened to stay inside.
	GYRO_CHECK(Progress(Swipe, { 1.0e12, 0.0 }) < 1.0 + Swipe.RubberBand);
	GYRO_CHECK(Progress(Swipe, { -1.0e12, 0.0 }) > -Swipe.RubberBand);
}

// A displacement that is finite and whose projection is not.
//
// Here rather than in the header's assertion block because producing an infinity from finite operands
// is not a constant expression, and this is the case the infinity guard actually exists for: nobody
// hands the mapping an infinity, but a number one multiplication away from one is what arrives after
// something upstream has already gone wrong. What must not happen is the arithmetic answering with a
// value that is not a number — inf*b/(inf+b) is inf/inf — because that value reaches a spring, and a
// spring holding one never comes to rest, so the compositor never idles.
GYRO_TEST(Drive, AProjectionThatOverflowsLandsOnTheBandRatherThanOnNonsense)
{
	constexpr double Enormous = std::numeric_limits<double>::max();

	const double forwards = Progress(Swipe, { Enormous, 0.0 });
	const double backwards = Progress(Swipe, { -Enormous, 0.0 });

	GYRO_CHECK_EQ(forwards, 1.0 + Swipe.RubberBand);
	GYRO_CHECK_EQ(backwards, -Swipe.RubberBand);

	// Self-comparison rather than std::isnan, as the header spells it. Stated separately from the
	// equalities above because it is the property that matters and the one a future curve could lose
	// while still landing near the right number.
	GYRO_CHECK(forwards == forwards);
	GYRO_CHECK(backwards == backwards);
}

// The mapping does not care which way a transition is authored to run. A vertical swipe is the
// horizontal one turned ninety degrees, and both project the same way — worth a test rather than an
// argument, because it is a claim about the arithmetic being direction-agnostic and the alternative
// is an axis assumption nobody wrote down.
GYRO_TEST(Drive, DirectionIsTheTravelsAndNotTheAxis)
{
	constexpr DriveMapping Vertical{ .Travel = { 0.0, 800.0 }, .RubberBand = 0.1 };
	constexpr DriveMapping Diagonal{ .Travel = { 600.0, 800.0 }, .RubberBand = 0.1 };

	GYRO_CHECK_EQ(Progress(Vertical, { 0.0, 400.0 }), 0.5);
	GYRO_CHECK_EQ(Progress(Vertical, { 5000.0, 400.0 }), 0.5);
	GYRO_CHECK_EQ(Progress(Diagonal, { 300.0, 400.0 }), 0.5);

	// A gesture along the travel's perpendicular is no progress at all, in every orientation.
	GYRO_CHECK_EQ(Progress(Vertical, { 900.0, 0.0 }), 0.0);
	GYRO_CHECK_EQ(Progress(Diagonal, { 800.0, -600.0 }), 0.0);
}
