#include "Render/Governor.h"

#include <cstdint>

#include "Testing/Test.h"

// The rule the probe's two numbers are turned into a decision by, on a machine with no GPU.
//
// **What is testable here is the sign and the margin, and that is most of what can go wrong.** Whether
// a driver moves the clock is a fact about a kernel and a part, and the only instrument for it is the
// probe beside this running against real hardware. What does not need hardware is that gyro reads the
// answer in the recoverable direction: decision 142 says an unclear probe takes the floor, so every
// case that is not a clear step up has to come out `Ignored` — and a rule that got that backwards
// would drop frames on every machine gyro has ever run on while passing any test that only checked
// the obvious case.

// The part this was written against: parked at the efficient frequency with the deadline stated and
// parked at the efficient frequency without it, because i915 has no plumbing to carry the number.
GYRO_TEST(Governor, AClockThatDidNotMoveIsADeadlineGoingNowhere)
{
	GYRO_CHECK(GpuGovernor::Judge(300, 300) == DeadlineResponse::Ignored);
}

// msm's answer is `get_freq() * 2`, which is the shape the margin was chosen against.
GYRO_TEST(Governor, ADoublingIsADriverAnswering)
{
	GYRO_CHECK(GpuGovernor::Judge(300, 600) == DeadlineResponse::Honoured);
}

// The margin is exactly a tenth, and the boundary belongs to the honoured side: a driver that raised
// the clock by precisely a tenth did raise it.
GYRO_TEST(Governor, TheMarginIsATenthAndTheBoundaryCounts)
{
	GYRO_CHECK(GpuGovernor::Judge(1'000, 1'100) == DeadlineResponse::Honoured);
	GYRO_CHECK(GpuGovernor::Judge(1'000, 1'099) == DeadlineResponse::Ignored);
}

// Two batches of identical work disagreeing by a few percent is the noise the margin exists to sit
// above, and reading it as a response is how a machine that needs the floor fails to get one.
GYRO_TEST(Governor, NoiseIsNotAResponse)
{
	GYRO_CHECK(GpuGovernor::Judge(700, 720) == DeadlineResponse::Ignored);
	GYRO_CHECK(GpuGovernor::Judge(700, 660) == DeadlineResponse::Ignored);
}

// A probe that ran and read nothing back has still answered, in the direction that costs power rather
// than frames. `Unprobed` is reserved for a probe that never ran at all, which is a state `Judge`
// cannot produce because it is only reached before any work is submitted.
GYRO_TEST(Governor, AnUnreadableClockTakesTheFloorRatherThanReportingNothing)
{
	GYRO_CHECK(GpuGovernor::Judge(0, 0) == DeadlineResponse::Ignored);
	GYRO_CHECK(GpuGovernor::Judge(0, 900) == DeadlineResponse::Ignored);
	GYRO_CHECK(GpuGovernor::Judge(900, 0) == DeadlineResponse::Ignored);
}

// The arithmetic is done wide, so a part whose range is stated in a unit nobody expected does not
// wrap the comparison into the wrong answer.
GYRO_TEST(Governor, TheComparisonDoesNotOverflowOnALargeUnit)
{
	constexpr std::uint32_t Large = 4'000'000'000U;

	GYRO_CHECK(GpuGovernor::Judge(Large, Large) == DeadlineResponse::Ignored);
	GYRO_CHECK(GpuGovernor::Judge(Large / 2U, Large) == DeadlineResponse::Honoured);
}

// A governor nobody probed holds no floor and has commanded nothing, which is the state a machine
// gyro cannot read a minimum-clock node off stays in for the whole session.
GYRO_TEST(Governor, ADefaultGovernorHoldsNothing)
{
	const GpuGovernor governor;

	GYRO_CHECK(governor.Reading().Response == DeadlineResponse::Unprobed);
	GYRO_CHECK(!governor.Floor().IsValid());
	GYRO_CHECK(governor.Floor().Commanded() == 0);
}
