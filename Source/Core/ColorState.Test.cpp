#include "Core/ColorState.h"

#include <format>
#include <string>

#include "Testing/Test.h"

// The rules this file carries are all comparisons: untagged content is sRGB, the composite space is
// linear at wide primaries, and two states that differ in any field describe different light. The
// last one is what a promotion decision rests on, so a comparison that quietly ignored a field would
// report a plane as free when it changes the picture.

GYRO_TEST(ColorState, UntaggedIsSrgbByRule)
{
	GYRO_CHECK_EQ(ColorState{}, ColorState::Srgb());
	GYRO_CHECK(ColorState::Srgb().Primaries == ColorPrimaries::Bt709);
	GYRO_CHECK(ColorState::Srgb().Transfer == TransferFunction::Srgb);
	GYRO_CHECK(ColorState::Srgb().Alpha == AlphaMode::Premultiplied);
}

GYRO_TEST(ColorState, TheCompositeSpaceIsLinearAtWidePrimaries)
{
	const ColorState composite = ColorState::Composite();

	GYRO_CHECK(composite.Transfer == TransferFunction::Linear);
	GYRO_CHECK(composite.Primaries == ColorPrimaries::Bt2020);
	GYRO_CHECK(composite != ColorState::Srgb());
}

GYRO_TEST(ColorState, EveryFieldIsPartOfTheIdentity)
{
	ColorState state = ColorState::Srgb();

	state.Alpha = AlphaMode::Straight;
	GYRO_CHECK(state != ColorState::Srgb());

	state = ColorState::Srgb();
	state.ReferenceLuminance = 100.0F;
	GYRO_CHECK(state != ColorState::Srgb());

	state = ColorState::Srgb();
	state.Primaries = ColorPrimaries::DciP3;
	GYRO_CHECK(state != ColorState::Srgb());
}

GYRO_TEST(ColorState, FormatsForALog)
{
	GYRO_CHECK_EQ(std::format("{}", ColorState::Srgb()), std::string{ "bt709/srgb premultiplied @203nit" });
	GYRO_CHECK_EQ(std::format("{}", ColorState::Composite()), std::string{ "bt2020/linear premultiplied @203nit" });

	ColorState hdr{ ColorPrimaries::Bt2020, TransferFunction::Pq, AlphaMode::Straight, 0, 1000.0F };

	GYRO_CHECK_EQ(std::format("{}", hdr), std::string{ "bt2020/pq straight @1000nit" });
}
