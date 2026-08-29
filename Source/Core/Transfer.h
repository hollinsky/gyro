#pragma once

#include <algorithm>
#include <cmath>

// The sRGB transfer function, exactly, in both directions.
//
// **It is here rather than in `Blit` because two tiers need it and neither may name the other.** It
// began beside the CPU renderer's tables, which was right while a composite was the only thing that
// converted: decision 47 blends in linear light, so `Blit` decodes every texel and encodes every
// pixel. What moved it is Scene/Cursor.h — a scene author baking coverage into an image has to write
// the *encoded* value a linear composite will decode back to the coverage it meant, and a
// world-authoring module reaching into a renderer for the curve would be `Scene` depending on the
// frame side of the waist. `Blit/Transfer.h` keeps the tables, which are the per-pixel form of this
// and the part that is a renderer's business.
//
// **The piecewise form rather than a bare 2.2 power.** The linear segment near black is not a rounding
// detail — it is what keeps the derivative finite at zero, and a pure power there turns the darkest
// few codes into a step the boot logo's shadow sits directly on.
namespace Srgb
{
inline constexpr float LinearSegmentEnd = 0.0031308F;
inline constexpr float EncodedSegmentEnd = 0.04045F;
inline constexpr float SegmentSlope = 12.92F;
inline constexpr float Gain = 1.055F;
inline constexpr float Offset = 0.055F;
inline constexpr float Gamma = 2.4F;
} // namespace Srgb

[[nodiscard]] inline float SrgbToLinear(float encoded) noexcept
{
	const float clamped = std::clamp(encoded, 0.0F, 1.0F);

	if (clamped <= Srgb::EncodedSegmentEnd)
	{
		return clamped / Srgb::SegmentSlope;
	}

	// Full range is full range. The algebra says so exactly and the float arithmetic lands one unit in
	// the last place short of it, which a table would then carry into every white pixel — a logo one
	// code below the background it sits on is a seam at the one moment the design exists to make
	// seamless.
	if (clamped >= 1.0F)
	{
		return 1.0F;
	}

	return std::pow((clamped + Srgb::Offset) / Srgb::Gain, Srgb::Gamma);
}

[[nodiscard]] inline float LinearToSrgb(float linear) noexcept
{
	const float clamped = std::clamp(linear, 0.0F, 1.0F);

	if (clamped <= Srgb::LinearSegmentEnd)
	{
		return clamped * Srgb::SegmentSlope;
	}

	if (clamped >= 1.0F)
	{
		return 1.0F;
	}

	return Srgb::Gain * std::pow(clamped, 1.0F / Srgb::Gamma) - Srgb::Offset;
}
