#include "Blit/Transfer.h"

#include <algorithm>
#include <cerrno>
#include <cmath>

// The sRGB specification's own constants and its own break point. Written as the piecewise form
// rather than as a bare 2.2 power: the linear segment near black is not a rounding detail, it is
// what keeps the derivative finite at zero, and a pure power there turns the darkest few codes into
// a step the boot logo's shadow sits directly on.
namespace
{
constexpr float LinearSegmentEnd = 0.0031308F;
constexpr float EncodedSegmentEnd = 0.04045F;
constexpr float SegmentSlope = 12.92F;
constexpr float Gain = 1.055F;
constexpr float Offset = 0.055F;
constexpr float Gamma = 2.4F;
} // namespace

float SrgbToLinear(float encoded) noexcept
{
	const float clamped = std::clamp(encoded, 0.0F, 1.0F);

	if (clamped <= EncodedSegmentEnd)
	{
		return clamped / SegmentSlope;
	}

	// Full range is full range. The algebra says so exactly and the float arithmetic lands one unit in
	// the last place short of it, which the table below would then carry into every white pixel — a
	// logo one code below the background it sits on is a seam at the one moment the design exists to
	// make seamless.
	if (clamped >= 1.0F)
	{
		return 1.0F;
	}

	return std::pow((clamped + Offset) / Gain, Gamma);
}

float LinearToSrgb(float linear) noexcept
{
	const float clamped = std::clamp(linear, 0.0F, 1.0F);

	if (clamped <= LinearSegmentEnd)
	{
		return clamped * SegmentSlope;
	}

	if (clamped >= 1.0F)
	{
		return 1.0F;
	}

	return Gain * std::pow(clamped, 1.0F / Gamma) - Offset;
}

Result<void> TransferTable::Build(TransferFunction transfer) noexcept
{
	switch (transfer)
	{
		case TransferFunction::Linear:
			m_Identity = true;

			return {};

		case TransferFunction::Srgb:
			break;

		case TransferFunction::Pq:
		case TransferFunction::Hlg:
			return Failure(EINVAL, "no CPU composite encodes an absolute transfer function yet");
	}

	// Sample `j` stands for the linear value `j * 16`, so that `Encode` finds its segment with a
	// shift. The last sample is one step past the top of the range and saturates there, which is what
	// makes the interpolation land on exactly full-range white rather than one below it.
	for (std::size_t index = 0; index < Samples; ++index)
	{
		const float linear = static_cast<float>(std::min<std::size_t>(index * 16, 65535)) / 65535.0F;

		m_Encode[index] = static_cast<std::uint16_t>(LinearToSrgb(linear) * 65535.0F + 0.5F);
	}

	m_Identity = false;

	return {};
}

Result<void> DecodeTable::Build(TransferFunction transfer, std::uint32_t bits) noexcept
{
	// Eight and ten are what Seam/Pixel.h decodes, so they are what there are texels of. A depth this
	// does not know is refused rather than tabulated at the wrong width, which would sample every
	// channel through a table whose top entry is not white.
	if (bits != 8 && bits != 10)
	{
		return Failure(EINVAL, "no CPU composite samples a texel of this depth");
	}

	switch (transfer)
	{
		case TransferFunction::Linear:
		case TransferFunction::Srgb:
			break;

		case TransferFunction::Pq:
		case TransferFunction::Hlg:
			return Failure(EINVAL, "no CPU composite decodes an absolute transfer function yet");
	}

	m_Shift = 16 - bits;
	m_Proportional = transfer == TransferFunction::Linear;

	const std::uint32_t codes = 1U << bits;
	const float top = static_cast<float>(codes - 1);

	for (std::uint32_t code = 0; code < codes; ++code)
	{
		const float encoded = static_cast<float>(code) / top;
		const float linear = m_Proportional ? encoded : SrgbToLinear(encoded);

		m_Decode[code] = static_cast<std::uint16_t>(linear * 65535.0F + 0.5F);
	}

	return {};
}
