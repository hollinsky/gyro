#pragma once

#include <array>
#include <cstdint>

#include "Core/ColorState.h"
#include "Core/Result.h"

// The transfer function, in the two forms a CPU composite needs it in.
//
// **This exists because a blend in the target's own encoding is visibly wrong, and `Blit` draws the
// one picture where it shows.** Decision 47 composites in linear light; Render/Renderer.h says in as
// many words that it does not yet convert and that what this leaves standing is a blend in the
// target's encoding. On a GPU that is a deferred conversion. Here it is the boot logo's antialiased
// edge — a half-covered pixel between white and black should carry half the light, and blending in
// the target's encoding puts code 128 there, which is 22% of it. Every antialiased edge in the boot
// picture comes out with a dark rim, and it is one table lookup away from correct.
//
// **Only the transfer function, and never the primaries.** Blit refuses an item whose colour state
// differs from the output's — Render/Renderer.h's refusal, for its reason — so every value in a
// composite is already in the output's primaries and no matrix is ever needed. That is not a
// simplification that will have to be undone: the conversion belongs beside the one that handles
// primaries, and Docs/Open.md's wire-colorimetry entry is what says when there is a second set to
// convert between.
//
// **Two forms because there are two rates.** A solid's colour converts once per item, where an
// exact `std::pow` costs nothing measurable; a composited pixel converts once per pixel of the
// damage region, where it cannot. The table below is the second, and `Transfer.Test.cpp` is what
// holds it to the first.

// The exact forms, in the algebra the sRGB specification is written in.
[[nodiscard]] float SrgbToLinear(float encoded) noexcept;
[[nodiscard]] float LinearToSrgb(float linear) noexcept;

// The per-pixel form: the encode direction, sampled and interpolated.
//
// **Sampled at 4096 points and interpolated rather than tabulated at all 65536**, which is 8 KiB
// against 128 KiB. The band scratch is sized to stay in cache and a table that evicted it would give
// back more than it saved. The error the interpolation costs is bounded where the curve bends
// hardest, just above the linear segment's knee, and it is under a hundredth of an eight-bit code —
// `Transfer.Test.cpp` sweeps the whole range against the exact form rather than taking that on
// trust.
//
// **The inverse direction is not here.** Nothing in a composite decodes per pixel yet: a solid's
// colour is converted once per item and the clear is black in every encoding. A `DrawTexture` is
// what adds one, and it wants a table over the source's own depth rather than over sixteen bits.
class TransferTable
{
public:
	// `EINVAL` for a transfer function this cannot tabulate, which is `Pq` and `Hlg`. They are absent
	// rather than approximated because they are absolute rather than relative — a PQ code is a
	// number of nits, so encoding into one means deciding what a composite's 1.0 was worth, and
	// Docs/Open.md's wire-colorimetry entry is where that is decided rather than here.
	[[nodiscard]] Result<void> Build(TransferFunction transfer) noexcept;

	// Linear light in, the output's encoding out, both full-range sixteen-bit. Total: an unbuilt
	// table is the identity, which is what `Linear` builds anyway.
	[[nodiscard]] std::uint16_t Encode(std::uint16_t linear) const noexcept
	{
		if (m_Identity)
		{
			return linear;
		}

		// The top twelve bits pick the segment and the low four interpolate along it. Exact at every
		// sample and at both ends of the range, which is what keeps black black and white white.
		const std::uint32_t index = static_cast<std::uint32_t>(linear) >> 4U;
		const std::uint32_t fraction = static_cast<std::uint32_t>(linear) & 0xFU;

		const std::uint32_t low = m_Encode[index];
		const std::uint32_t high = m_Encode[index + 1];

		return static_cast<std::uint16_t>(low + (((high - low) * fraction + 8U) >> 4U));
	}

private:
	// One past the last segment, so that the interpolation above never indexes off the end.
	static constexpr std::size_t Samples = 4097;

	std::array<std::uint16_t, Samples> m_Encode{};
	bool m_Identity = true;
};
