#include "Seam/OutputConfiguration.h"

#include <format>
#include <string>

#include "Core/Time.h"
#include "Seam/ColorState.h"
#include "Seam/RenderTarget.h"
#include "Testing/Test.h"

// One type carries both directions — what was asked for and what was achieved — so every test here is
// about keeping those two readings apart. The learned range must not make a satisfied request look
// unsatisfied, and a silent fallback must not look satisfied.

namespace
{
[[nodiscard]] OutputConfiguration Wanted()
{
	return {
		.Generation = 4,
		.Resolution = { 2560, 1440 },
		.Period = PeriodFromHertz(144.0),
		.Refresh = { .Enabled = true },
		.Format = { FormatXrgb8888, 0, ModifierLinear },
	};
}
} // namespace

GYRO_TEST(OutputConfiguration, ARangeLearnedAfterTheModeStillSatisfiesTheRequest)
{
	const OutputConfiguration wanted = Wanted();

	OutputConfiguration achieved = wanted;
	achieved.Refresh = { true, PeriodFromHertz(144.0), PeriodFromHertz(48.0) };

	GYRO_CHECK(wanted.SatisfiedBy(achieved));
	GYRO_CHECK(wanted != achieved);
	GYRO_CHECK(!achieved.Refresh.IsDegenerate());
}

GYRO_TEST(OutputConfiguration, AFallbackModeDoesNotSatisfyTheRequest)
{
	const OutputConfiguration wanted = Wanted();

	OutputConfiguration resolution = wanted;
	resolution.Resolution = { 1920, 1080 };
	GYRO_CHECK(!wanted.SatisfiedBy(resolution));

	OutputConfiguration period = wanted;
	period.Period = PeriodFromHertz(60.0);
	GYRO_CHECK(!wanted.SatisfiedBy(period));

	OutputConfiguration refused = wanted;
	refused.Refresh.Enabled = false;
	GYRO_CHECK(!wanted.SatisfiedBy(refused));

	OutputConfiguration rotated = wanted;
	rotated.Transform = OutputTransform::Rotate90;
	GYRO_CHECK(!wanted.SatisfiedBy(rotated));

	OutputConfiguration darker = wanted;
	darker.Color = ColorState::Composite();
	GYRO_CHECK(!wanted.SatisfiedBy(darker));
}

// The generation is an echo rather than a request, so it takes no part in the comparison — and it is
// the thing a caller compares separately to know *which* request completed.
GYRO_TEST(OutputConfiguration, TheGenerationIsEchoedAndNotCompared)
{
	const OutputConfiguration wanted = Wanted();

	OutputConfiguration superseded = wanted;
	superseded.Generation = 3;

	GYRO_CHECK(wanted.SatisfiedBy(superseded));
	GYRO_CHECK(superseded.Generation != wanted.Generation);
}

GYRO_TEST(OutputConfiguration, AFixedOutputHasADegenerateRangeRatherThanNone)
{
	OutputConfiguration fixed;
	fixed.Period = PeriodFromHertz(60.0);
	fixed.Refresh = { false, fixed.Period, fixed.Period };

	GYRO_CHECK(fixed.Refresh.IsDegenerate());
	GYRO_CHECK(!fixed.Refresh.Enabled);
}

// Power is part of the configuration because DPMS is in the same class as a mode set: too expensive
// to do inline, therefore the deferred verb's business. An output that is off says nothing else about
// itself.
GYRO_TEST(OutputConfiguration, AnUnpoweredOutputIsAConfigurationRatherThanAnAbsence)
{
	OutputConfiguration off = Wanted();
	off.Powered = false;

	GYRO_CHECK(!Wanted().SatisfiedBy(off));
	GYRO_CHECK_EQ(std::format("{}", off), std::string{ "gen 4 off" });
}

GYRO_TEST(OutputConfiguration, FormatsForALog)
{
	OutputConfiguration achieved = Wanted();
	achieved.Refresh = { true, PeriodFromHertz(144.0), PeriodFromHertz(48.0) };

	GYRO_CHECK_EQ(
		std::format("{}", achieved),
		std::string{ "gen 4 device(2560x1440) @6944444ns vrr[6944444ns, 20833333ns] normal XR24 mod 0x0 "
	                 "bt709/srgb premultiplied @203nit" }
	);
}
