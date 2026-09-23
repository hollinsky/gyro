#include "Seam/PresentationInfo.h"

#include <format>
#include <string>

#include "Core/Time.h"
#include "Testing/Test.h"

// This is the only input to every deadline in the system, so the properties worth asserting are about
// what it refuses to imply: a default carries no observation, an unknown period is zero rather than a
// guess, and the flags say what the backend knows rather than what it hopes.

GYRO_TEST(PresentationInfo, TheDefaultIsTheAbsenceOfAnObservation)
{
	const PresentationInfo info;

	GYRO_CHECK_EQ(info.Period, Duration::zero());
	GYRO_CHECK_EQ(info.Sequence, std::uint64_t{ 0 });
	GYRO_CHECK(!info.HardwareClock);
	GYRO_CHECK(!info.Vsync);
	GYRO_CHECK(!info.ZeroCopy);
}

GYRO_TEST(PresentationInfo, AFrameCarriesTheTimebaseRatherThanARawNumber)
{
	const PresentationInfo info{
		.PresentedAt = Monotonic::FromMicroseconds(12'500),
		.Period = PeriodFromHertz(120.0),
		.Sequence = 4210,
		.Vsync = true,
		.HardwareClock = true,
	};

	// The subtraction Architecture.md's timebase section promises: two instants in one domain.
	GYRO_CHECK_EQ(Elapsed(Monotonic::FromMicroseconds(10'000), info.PresentedAt), Duration{ 2'500'000 });
	GYRO_CHECK_EQ(info.Period, Duration{ 8'333'333 });
}

GYRO_TEST(PresentationInfo, FormatsForALog)
{
	const PresentationInfo bare{ .PresentedAt = Monotonic::FromMicroseconds(12'500), .Period = PeriodFromHertz(120.0) };

	GYRO_CHECK_EQ(std::format("{}", bare), std::string{ "presented 12500000ns seq 0 period 8333333ns" });

	PresentationInfo full = bare;
	full.Vsync = true;
	full.HardwareClock = true;
	full.HardwareCompletion = true;
	full.ZeroCopy = true;

	GYRO_CHECK_EQ(
		std::format("{}", full),
		std::string{ "presented 12500000ns seq 0 period 8333333ns vsync hw-clock hw-completion zero-copy" }
	);
}
