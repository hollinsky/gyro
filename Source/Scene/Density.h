#pragma once

#include <cstdint>
#include <optional>

#include "Geometry/Scale.h"
#include "Geometry/Space.h"

// Where an output's scale comes from, which is decision 164: gyro stores no per-output scale. It
// stores one angular preference — how big a logical pixel should be, in arcminutes of visual angle —
// and every output derives its own scale from that, its own pixel pitch, and how far away it is. What a person
// perceives is angular size, and a 55-inch television at three metres and a 27-inch monitor at sixty centimetres report
// identical millimetres per pixel and want scales two apart.
//
// **Here rather than in the composition root because it is arithmetic about the world, and here
// rather than in `Seam` for decision 87's reason**: `Scene` declares the output model it wants and
// the root fills it in, so the panel facts that model is *derived from* are declared beside it. The
// root is still the only party that can supply them — a physical size arrives from a backend and the
// distance will arrive from a setup — and it is the only caller.
//
// **What is not here is the setup.** Decision 164 keys the viewing distance on the set of displays
// present and makes that record the seat's, and there is nowhere on this machine to keep one yet. So
// what this file has is `SeededDistance`: the prior a display gets before anybody has said anything,
// which is wrong only in the one term a person can correct in a single gesture.

// Which side of a connector a panel is on, as far as the distance prior cares.
//
// The kind is a fact the backend has and the world does not: a laptop panel is at arm's length
// because it is attached to the keyboard, and that is the only thing distinguishing it from the same
// panel on a desk. `Unknown` is a nested window, a headless sweep and a file — no connector, so no
// claim.
enum class PanelKind : std::uint8_t
{
	Unknown,
	Internal,
	External,
};

// The panel's own extent, in millimetres as EDID states it. Zero in either axis means the connector
// did not say, which `Drm/Device.h` notes is most projectors and every virtual connector — so it is
// an ordinary answer rather than a failure, and `DensityFromGeometry` refuses rather than inventing
// a pitch out of it.
struct PanelSize
{
	std::uint32_t WidthMm = 0;
	std::uint32_t HeightMm = 0;

	[[nodiscard]] constexpr bool IsKnown() const noexcept { return WidthMm != 0 && HeightMm != 0; }

	friend constexpr bool operator==(PanelSize, PanelSize) noexcept = default;
};

// The person's one preference: how big a logical pixel should be, as an angle.
//
// **The angle is a logical pixel's, not a glyph's.** That is the unit every extent on the machine is
// in — a window's size, a margin, an icon, and the font size a toolkit asks for — so text follows from
// it rather than being what it measures. It cannot be a character height: gyro does not own the font,
// and a number defined against somebody's default face and x-height ratio would be the 96 DPI mistake
// again, wearing typography. The intuition is still available and worth writing down — ordinary body
// text is about 16 logical pixels to the em, so at the default an em subtends 21 arcminutes and an
// x-height about 10.7, a little under the 12 where reading speed stops improving. At the reference it
// is 12.1, which is to say 96 DPI at 600 mm put ordinary text exactly on that threshold and the
// default sits 11% inside it.
//
// **The unit is arcminutes per logical pixel, stored in thousandths.** A physical quantity declared
// as one, so a bigger number is bigger text — the direction a person expects, needing no warning —
// and so the value means something without a reference beside it, which matters because decision
// 164's setup will write it to a file that outlives this codebase.
//
// **The reference is 1516**, which is 1.516 arcminutes. Decision 164 has where that came from: 96 DPI
// at 600 mm, the pitch and distance every logical unit in the ecosystem descends from. Provenance
// rather than definition — the reference is the *angle*, and 1516 is exact by declaration against
// the 1515.89 that pitch and distance actually give, a difference two hundred times below the
// rounding `DensityFromGeometry` finishes with.
//
// **Not 120ths, and not a `Scale`.** 120 is `wp_fractional_scale_v1`'s denominator and lives in
// gyro's Wayland implementation; a preference never crosses a wire. Borrowing the wire's unit bought
// steps of 0.83% in a quantity nobody resolves better than about 5%, and cost the thing this comment
// used to apologise for — a person's number and an output's number as the same rational in the same
// denominator.
//
// The useful range is about 950 to 1900. 1000 is where 20/20 acuity puts the smallest a logical pixel
// can usefully be; 1600 is near the critical print size, below which reading speed falls off. Every
// ladder anybody ships lands inside it — Apple's densest rung is 1140 to 1170 across their whole
// lineup, and an iPhone's point is 1900 because a 44-point tap target has to stay wider than a
// fingertip rather than because of anything about eyes.
struct AngularPreference
{
	// **The basis, not the default.** 1.516 arcminutes per logical pixel, and the two are different
	// jobs: this is the unit global space is measured in and the unit decision 164 has a setup store
	// its arrangement in, so it is arbitrary but frozen — the day the first setup reaches a disk, it
	// can never move again. `Default` below is a guess at taste and is expected to move.
	static constexpr std::int32_t Reference = 1516;

	// **What a person gets before they have said anything: 1.340 arcminutes.**
	//
	// Three independent pairings people actually run land here — Apple's default rung on a 27-inch 5K,
	// a 27-inch 1440p panel at 1x, and a 27-inch 4K at 1.5x, which is the fraction every desktop
	// offering one picks. That is a far better evidenced claim about taste than the reference, whose
	// support is a 1987 logical inch.
	//
	// **It is not the reference, and it costs two snaps to not be.** At 1516 a 13.3-inch 2560x1600
	// laptop lands exactly on 2x and a 24-inch 1080p monitor exactly on 1x; at 1340 they derive 1.74
	// and 0.84 and take them. The band edges are 1368 and 1404, so a default of 1404 would have been
	// free. It is not taken, because choosing the default for what the snapping band does with it is
	// letting the mechanism pick the taste — and the retina laptop's actual cost is a 1.15:1
	// minification, milder than the 1.60 decision 56 already accepts at a scale of 1.25.
	static constexpr std::int32_t Default = 1340;

	// A thousand arcminutes is sixteen degrees to the logical pixel, which is past absurd in the one
	// direction anybody could reach by typo. It is a bound on the arithmetic rather than on taste:
	// this term multiplies into `DensityFromGeometry`'s numerator, so without it a value nobody meant
	// is an int64 overflow instead of a scale nobody wanted.
	static constexpr std::int32_t MaximumMilliArcminutes = 1'000'000;

	std::int32_t MilliArcminutes = Default;

	// What a person writes, which is a decimal number of arcminutes. The one conversion in the story
	// that needs a real division, kept here — at the boundary, once per preference — rather than in the
	// derivation, which stays integer for decision 53's reason.
	[[nodiscard]] static constexpr AngularPreference FromArcminutes(double arcminutes) noexcept
	{
		constexpr double Smallest = 1.0;
		constexpr double Largest = static_cast<double>(MaximumMilliArcminutes);

		const double thousandths = arcminutes * 1000.0;

		if (!(thousandths >= Smallest))
		{
			return AngularPreference{ static_cast<std::int32_t>(Smallest) };
		}

		return AngularPreference{ static_cast<std::int32_t>(thousandths > Largest ? Largest : thousandths + 0.5) };
	}

	[[nodiscard]] constexpr double ToArcminutes() const noexcept
	{
		return static_cast<double>(MilliArcminutes) / 1000.0;
	}

	friend constexpr bool operator==(AngularPreference, AngularPreference) noexcept = default;
};

// How far away a display is before anybody has said, in millimetres.
//
// **A prior and labelled as one.** The distance is the term that is genuinely not on the connector —
// the same laptop panel is at 350 mm on a lap, 550 mm docked under an external monitor, and 700 mm
// shoved aside as a third screen — so form factor is all there is to seed from: an internal panel is
// at arm's length, a display over 40 inches across the diagonal is a television and is across the
// room, and everything else is a monitor on a desk.
[[nodiscard]] constexpr std::int32_t SeededDistance(PanelKind kind, PanelSize size) noexcept
{
	if (kind == PanelKind::Internal)
	{
		return 500;
	}

	// 40 inches is 1016 mm, compared squared so the diagonal needs no root. A 55-inch panel is 1219
	// mm and a 32-inch monitor — the largest thing people put on a desk — is 813, so the threshold has
	// most of a foot of clearance either side of anything common.
	const std::int64_t width = size.WidthMm;
	const std::int64_t height = size.HeightMm;

	if (size.IsKnown() && (width * width) + (height * height) >= 1016 * 1016)
	{
		return 2500;
	}

	return 600;
}

// The band a derived scale snaps to an integer inside, as a fraction of the derived value: one
// eighth.
//
// **1x and 2x are the only scales that resample nothing**, and decision 54 exists because half a
// device pixel of offset is the most reported complaint about fractional scaling anywhere — while
// nobody can see a tenth of a stop of angular size at all. The asymmetry is enormous and a
// derivation that ignored it would throw away the best outcome available on most of the monitors
// people own.
//
// **The number is Open.md's and this is narrower than the prose that asked it.** Decision 164 offers
// 15% as the error a person cannot see, and its own table refuses that as a band: a 55-inch 4K
// television at 2.5 m derives 3.50 and is *taken* at 3.50, which a 15% band would snap to 3, while a
// 27-inch 1440p panel derives 1.13 and is taken at 1. So the table pins the band between 11.8% and
// 13.7% — narrower than the prose and on the other side of it — and an eighth is the round number
// inside it. It is still a
// person in front of two panels rather than an argument; what this constant buys is that the table
// decision 164 argues from is the thing the code reproduces.
inline constexpr std::int64_t DensitySnapNumerator = 1;
inline constexpr std::int64_t DensitySnapDenominator = 8;

// The scale an output takes, from its own geometry and one person's preference. Nothing where the
// panel did not report a size, because a pitch invented out of no millimetres is a scale nobody
// asked for.
//
// **The derivation is exact in rationals and the 120th is a rounding at the end of it.** The
// reference's tangent cancels against the output's, leaving
//
//     scale = (distance / 600 mm) x (0.2646 mm / pitch) / k
//
// where `k` is the preference over the reference — a pure ratio of two angles, which is what makes
// both tangents cancel — and the pitch is millimetres over a pixel count. A ratio of distances times a ratio of
// pitches, with nothing transcendental left in it — so it is integer arithmetic for decision 53's own reason. What
// falls out is *not* a 120th: the denominator is a panel's millimetres times a person's distance and nothing makes that
// divide 120. So the last step is a rounding to the nearest 120th, worth under 0.4% of angular size, and decision 53's
// exact rational is what comes out of that step rather than what goes into it.
//
// **The pitch is taken along the longer axis of each.** A panel's millimetres are the panel's and
// the grid may be turned, so pairing width with width is wrong by the aspect ratio on a rotated
// monitor — while pairing long with long is right on every panel whose pixels are square, which is
// all of them.
[[nodiscard]] constexpr std::optional<Scale>
DensityFromGeometry(PanelSize size, PixelSize<DeviceSpace> grid, std::int32_t distanceMm, AngularPreference preference)
{
	if (!size.IsKnown() || grid.Width <= 0 || grid.Height <= 0 || distanceMm <= 0 || preference.MilliArcminutes <= 0 ||
	    preference.MilliArcminutes > AngularPreference::MaximumMilliArcminutes)
	{
		return std::nullopt;
	}

	const std::int64_t millimetres = size.WidthMm > size.HeightMm ? size.WidthMm : size.HeightMm;
	const std::int64_t pixels = grid.Width > grid.Height ? grid.Width : grid.Height;

	// scale x 120 = distance x 254 x pixels x preference / (4800 x millimetres x reference), where
	// 254 / 960 is the reference pitch in millimetres and the 960 has been divided into the 600 mm and
	// the two factors of 120. The preference and the reference enter as a *ratio of two angles*, which
	// is what cancels both tangents and keeps this integer — so what either is expressed in never
	// reaches the arithmetic, and moving them off 120ths moved nothing here.
	//
	// **The preference multiplies rather than divides**, which is the whole of what the unit change
	// was for: a logical pixel asked to subtend a wider angle is a logical pixel worth more device
	// ones. Under the reciprocal unit this term was a divisor and the header had to warn that a larger
	// number meant smaller text.
	//
	// Every term is a panel's or a person's, so the largest product a real machine reaches — 2.5 m, an
	// 8K grid, and a preference at the far end of `MaximumMilliArcminutes` — is under 5 x 10^15, and
	// `Nearest` doubles it.
	const std::int64_t numerator = static_cast<std::int64_t>(distanceMm) * 254 * pixels * preference.MilliArcminutes;
	const std::int64_t denominator = 4800 * millimetres * AngularPreference::Reference;

	const std::int64_t derived = Detail::Divide(numerator, denominator, Rounding::Nearest);

	// The nearest integer scale, and whether the derived value is close enough to it to be worth the
	// exactness. Rounded rather than floored, so 1.9 asks about 2 and 2.1 asks about 2 as well.
	const std::int64_t integer = Detail::Divide(derived + (Scale::Denominator / 2), Scale::Denominator, Rounding::Down);
	const std::int64_t snapped = (integer < 1 ? 1 : integer) * Scale::Denominator;
	const std::int64_t error = snapped > derived ? snapped - derived : derived - snapped;

	if (error * DensitySnapDenominator <= derived * DensitySnapNumerator)
	{
		return Scale::FromNumerator(Detail::Saturate(snapped));
	}

	return Scale::FromNumerator(Detail::Saturate(derived));
}

// The scale an output takes when it will not say how big it is.
//
// **A prior on pixel count alone, and a weaker one than the derivation above.** People sit further
// from bigger screens, so distance roughly tracks diagonal and what is left is a function of how
// many pixels there are — which is why a projector at 1080p and a projector at 4K want different
// answers even though neither reports a millimetre. It is deliberately coarse: an integer, so it
// resamples nothing, on the reasoning that a panel gyro knows nothing about is the worst place to
// spend a fraction.
[[nodiscard]] constexpr Scale DensityFromResolution(PixelSize<DeviceSpace> grid) noexcept
{
	const std::int32_t pixels = grid.Width > grid.Height ? grid.Width : grid.Height;

	if (pixels >= 7680)
	{
		return Scale::FromInteger(3);
	}

	if (pixels >= 3840)
	{
		return Scale::FromInteger(2);
	}

	return Scale::FromInteger(1);
}
