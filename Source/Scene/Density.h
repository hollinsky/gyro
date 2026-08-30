#pragma once

#include <cstdint>
#include <optional>

#include "Geometry/Scale.h"
#include "Geometry/Space.h"

// Where an output's scale comes from, which is decision 164: gyro stores no per-output scale. It
// stores one angular preference — how big text should be, in logical pixels per degree of visual
// angle — and every output derives its own scale from that, its own pixel pitch, and how far away it
// is. What a person perceives is angular size, and a 55-inch television at three metres and a
// 27-inch monitor at sixty centimetres report identical millimetres per pixel and want scales two
// apart.
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

// The person's one preference, as a multiple of the reference in 120ths.
//
// **The reference is 96 DPI at 600 mm**, which is the history the whole ecosystem already assumes:
// a pitch of 25.4 / 96 = 0.2646 mm, one degree at that distance spanning 600 · tan 1° = 10.47 mm,
// so 39.6 logical pixels per degree, or 1.52 arcminutes to the logical pixel. Stored as a multiple
// of it rather than as the px/degree figure itself because every use is a ratio against the
// reference and the tangent cancels — see `DensityFromGeometry`, which has no transcendental in it.
//
// Not a `Scale`, though it is the same exact rational in the same 120ths. A scale is an output's and
// this is a person's, and the one mistake this whole entry is about is keeping the second in a type
// that reads like the first.
struct AngularPreference
{
	static constexpr std::int32_t Denominator = 120;

	// 120 is the reference. Larger is *smaller* text, and the direction is worth stating rather than
	// inferring: more logical pixels to the degree means each one subtends less, so a glyph of a fixed
	// logical size takes up less of the visual field. It divides into the derivation for that reason.
	std::int32_t Multiple = Denominator;

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
// where `k` is the preference as a multiple of the reference and the pitch is millimetres over a
// pixel count. A ratio of distances times a ratio of pitches, with nothing transcendental left in
// it — so it is integer arithmetic for decision 53's own reason. What falls out is *not* a 120th:
// the denominator is a panel's millimetres times a person's distance and nothing makes that divide
// 120. So the last step is a rounding to the nearest 120th, worth under 0.4% of angular size, and
// decision 53's exact rational is what comes out of that step rather than what goes into it.
//
// **The pitch is taken along the longer axis of each.** A panel's millimetres are the panel's and
// the grid may be turned, so pairing width with width is wrong by the aspect ratio on a rotated
// monitor — while pairing long with long is right on every panel whose pixels are square, which is
// all of them.
[[nodiscard]] constexpr std::optional<Scale>
DensityFromGeometry(PanelSize size, PixelSize<DeviceSpace> grid, std::int32_t distanceMm, AngularPreference preference)
{
	if (!size.IsKnown() || grid.Width <= 0 || grid.Height <= 0 || distanceMm <= 0 || preference.Multiple <= 0)
	{
		return std::nullopt;
	}

	const std::int64_t millimetres = size.WidthMm > size.HeightMm ? size.WidthMm : size.HeightMm;
	const std::int64_t pixels = grid.Width > grid.Height ? grid.Width : grid.Height;

	// scale x 120 = distance x 254 x pixels / (40 x millimetres x preference), where 254 / 960 is the
	// reference pitch in millimetres and the 960 has already been divided into the 600 mm and the two
	// factors of 120. Every term is a panel's or a person's, so the largest product a real machine
	// reaches — 2.5 m, an 8K grid — is about 5 x 10^9 and the rounding doubles it.
	const std::int64_t numerator = static_cast<std::int64_t>(distanceMm) * 254 * pixels;
	const std::int64_t denominator = 40 * millimetres * preference.Multiple;

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
