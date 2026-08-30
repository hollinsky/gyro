#include "Scene/Density.h"

#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Testing/Test.h"

// Docs/Decisions.md decision 164: one angular preference, and every output derives its scale from it
// and its own geometry.
//
// The entry argues from a table of five panels, and that table is what this file checks — it is the
// only external statement of what the derivation is supposed to produce, and every constant in
// Scene/Density.h was chosen to reproduce it.

namespace
{
constexpr AngularPreference Reference{};

// The rounded 120th a derivation lands on, or zero where it refused.
[[nodiscard]] std::int32_t
Derived(PanelSize size, PixelSize<DeviceSpace> grid, std::int32_t distanceMm, AngularPreference preference = Reference)
{
	const std::optional<Scale> density = DensityFromGeometry(size, grid, distanceMm, preference);

	return density ? density->Numerator() : 0;
}
} // namespace

GYRO_TEST(Density, TheDeskMonitorsDecision164TabulatesLandWhereItSaysTheyDo)
{
	// A 24-inch 1080p panel at 600 mm derives 0.96 and is taken at 1: the ordinary monitor, which is
	// exactly what the reference was calibrated on and therefore the row that would show a factor
	// error immediately.
	GYRO_CHECK_EQ(Derived({ 531, 299 }, { 1920, 1080 }, 600), 120);

	// A 27-inch 1440p panel derives 1.13, which is the row that decides how wide the snapping band is:
	// it is taken at 1, and the alternative is putting every window on the machine through decision
	// 56's minification path for an angular difference nobody can see.
	GYRO_CHECK_EQ(Derived({ 597, 336 }, { 2560, 1440 }, 600), 120);

	// A 13.3-inch laptop panel at 500 mm derives 1.97 and is taken at 2, which is the answer every
	// desktop ships as a hand-written table entry.
	GYRO_CHECK_EQ(Derived({ 286, 179 }, { 2560, 1600 }, 500), 240);
}

GYRO_TEST(Density, TheTwoPanelsWithNoGoodIntegerKeepTheirExactRational)
{
	// A 27-inch 4K panel derives 1.70 and stays there. This is the panel fractional scaling exists
	// for: 18% away from 2 and 70% away from 1 by the band's own measure, so snapping either way is
	// visibly the wrong text size, and decision 53's exact rational is what it takes instead.
	GYRO_CHECK_EQ(Derived({ 597, 336 }, { 3840, 2160 }, 600), 204);

	// A 55-inch 4K television at 2.5 m derives 3.48 and stays there — which is the row that refuses
	// decision 164's own 15% as a band, since it is 13.7% from 3 and the entry tabulates it as taken
	// at 3.48 rather than snapped.
	GYRO_CHECK_EQ(Derived({ 1219, 685 }, { 3840, 2160 }, 2500), 417);
}

GYRO_TEST(Density, MovingAPanelChangesItsScaleAndNothingElseHasTo)
{
	// The whole point of the axis: one panel, one preference, three distances. A 4K 27-inch monitor
	// pushed further away takes a larger scale, because it subtends less and a glyph has to grow to
	// hold its angle — and the person said nothing.
	//
	// The near one is the same 1.13 the 1440p panel derives at arm's length and snaps for the same
	// reason, which is worth seeing here: the band is about the scale rather than about the panel.
	GYRO_CHECK_EQ(Derived({ 597, 336 }, { 3840, 2160 }, 400), 120);
	GYRO_CHECK_EQ(Derived({ 597, 336 }, { 3840, 2160 }, 600), 204);
	GYRO_CHECK_EQ(Derived({ 597, 336 }, { 3840, 2160 }, 900), 306);
}

GYRO_TEST(Density, ThePreferenceScalesEveryOutputAndTwoPeopleDisagreeOnlyInSize)
{
	// The preference multiplies in exactly: a finer angle per logical pixel is a smaller scale, which
	// is smaller text, and 204 x 1263 / 1516 is 170. This is the operation a person actually performs,
	// and it must not require touching a single output. 1263 is the reference asked for a fifth
	// smaller and 758 is half of it, written as milliarcminutes off the reference rather than as a
	// round taste because what these two rows check is the proportionality.
	GYRO_CHECK_EQ(Derived({ 597, 336 }, { 3840, 2160 }, 600, AngularPreference{ 1263 }), 170);
	GYRO_CHECK_EQ(Derived({ 597, 336 }, { 3840, 2160 }, 600, AngularPreference{ 758 }), 102);

	// And the snap is what makes two people's layouts differ in *shape* rather than only in size,
	// which is why decision 164 stores a setup's arrangement in the reference preference's units
	// rather than in logical pixels. The 1440p panel is snapped to 1 at the reference and lands on an
	// exact rational two thirds of a reference along (1011 milliarcminutes) — and 91 is not 120 over 1.5, because what
	// scaled uniformly was the derivation and the snap only caught one of them. Two people at this
	// desk therefore have logical layouts that are not the same rectangles in different units.
	GYRO_CHECK_EQ(Derived({ 597, 336 }, { 2560, 1440 }, 600), 120);
	GYRO_CHECK_EQ(Derived({ 597, 336 }, { 2560, 1440 }, 600, AngularPreference{ 1011 }), 91);
}

GYRO_TEST(Density, ARotatedPanelDerivesTheScaleItHasWhenItIsUpright)
{
	// EDID's millimetres are the panel's own and a mode may be turned, so the pitch pairs the longer
	// physical axis with the longer pixel one. Getting this wrong is not subtle — width over height
	// on a 16:9 panel is off by 1.78 — but it only ever appears on a monitor somebody stood on end.
	GYRO_CHECK_EQ(Derived({ 597, 336 }, { 3840, 2160 }, 600), Derived({ 336, 597 }, { 2160, 3840 }, 600));
}

GYRO_TEST(Density, APanelThatWillNotSayHowBigItIsFallsToThePixelCount)
{
	// Most projectors and every virtual connector, per Drm/Device.h. The derivation refuses rather
	// than inventing a pitch, and the caller falls back to the coarser prior.
	GYRO_CHECK(!DensityFromGeometry({}, { 3840, 2160 }, 600, Reference).has_value());
	GYRO_CHECK(!DensityFromGeometry({ 597, 0 }, { 3840, 2160 }, 600, Reference).has_value());

	// The prior is an integer on purpose: a display gyro knows nothing about is the worst place to
	// spend a resample.
	GYRO_CHECK_EQ(DensityFromResolution({ 1920, 1080 }), Scale::FromInteger(1));
	GYRO_CHECK_EQ(DensityFromResolution({ 3840, 2160 }), Scale::FromInteger(2));
	GYRO_CHECK_EQ(DensityFromResolution({ 7680, 4320 }), Scale::FromInteger(3));
	GYRO_CHECK_EQ(DensityFromResolution({ 2160, 3840 }), Scale::FromInteger(2));
}

GYRO_TEST(Density, TheDistancePriorIsFormFactorAndNothingElse)
{
	// An internal panel is at arm's length because it is attached to the keyboard.
	GYRO_CHECK_EQ(SeededDistance(PanelKind::Internal, { 286, 179 }), 500);

	// Over 40 inches diagonal is a television and is across the room; a 32-inch monitor is the
	// largest thing that goes on a desk and stays there.
	GYRO_CHECK_EQ(SeededDistance(PanelKind::External, { 1219, 685 }), 2500);
	GYRO_CHECK_EQ(SeededDistance(PanelKind::External, { 708, 399 }), 600);

	// No millimetres is no claim: the diagonal cannot be tested, so the desk is the guess.
	GYRO_CHECK_EQ(SeededDistance(PanelKind::Unknown, {}), 600);
}

GYRO_TEST(Density, ATelevisionAndAMonitorWithTheSamePitchTakeCompletelyDifferentScales)
{
	// The sentence decision 164 opens with, as an assertion. Both panels are 0.317 mm to the pixel —
	// identical DPI, which is the quantity every other compositor decides on — and they are three
	// metres and sixty centimetres away, so they want 4x and 0.83x. Any derivation that reads a
	// density and not an angle gives them the same answer and is wrong about one of them.
	const std::int32_t television = Derived({ 1219, 685 }, { 3840, 2160 }, 3000);
	const std::int32_t monitor = Derived({ 609, 342 }, { 1920, 1080 }, 600);

	GYRO_CHECK_EQ(television, 480);
	GYRO_CHECK_EQ(monitor, 100);
}

GYRO_TEST(Density, ThePreferenceIsAnAngleAndReadsTheWayAPersonExpects)
{
	// The unit is arcminutes per logical pixel, so a bigger number is bigger text — the direction the
	// reciprocal unit this replaced had to warn about in a comment. Decision 164's revision is that a
	// preference is a physical quantity rather than a multiple of a 1987 logical inch, and this is the
	// whole of what that buys at a call site.
	GYRO_CHECK(
		Derived({ 597, 336 }, { 3840, 2160 }, 600, AngularPreference{ 1800 }) >
		Derived({ 597, 336 }, { 3840, 2160 }, 600, AngularPreference{ 1200 })
	);

	// The default is the reference and the reference is 1.516 arcminutes, which is 96 DPI at 600 mm
	// restated as the angle it always was.
	GYRO_CHECK_EQ(AngularPreference{}.MilliArcminutes, AngularPreference::Reference);
	GYRO_CHECK_EQ(AngularPreference::FromArcminutes(1.516), Reference);

	// What a person writes is a decimal, and the one division in the story happens here rather than in
	// the derivation. The reader decision 164 tabulates sits at 1.10.
	GYRO_CHECK_EQ(AngularPreference::FromArcminutes(1.10).MilliArcminutes, 1100);
	GYRO_CHECK_EQ(AngularPreference::FromArcminutes(1.1004).MilliArcminutes, 1100);

	// Refused rather than wrapped, at both ends. The bound is arithmetic — this term multiplies into
	// the derivation's numerator — so a value past it is not a scale nobody wanted but an overflow.
	GYRO_CHECK_EQ(AngularPreference::FromArcminutes(0.0).MilliArcminutes, 1);
	GYRO_CHECK_EQ(AngularPreference::FromArcminutes(-3.0).MilliArcminutes, 1);
	GYRO_CHECK_EQ(AngularPreference::FromArcminutes(1.0e9).MilliArcminutes, AngularPreference::MaximumMilliArcminutes);
	GYRO_CHECK_EQ(Derived({ 597, 336 }, { 3840, 2160 }, 600, AngularPreference{ 0 }), 0);
	GYRO_CHECK_EQ(
		Derived({ 597, 336 }, { 3840, 2160 }, 600, AngularPreference{ AngularPreference::MaximumMilliArcminutes + 1 }),
		0
	);
}
