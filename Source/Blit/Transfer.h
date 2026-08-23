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
// **The decode direction is `DecodeTable` below rather than a second array in here**, and the
// difference between the two is the reason they are two types: this one's input is a composited
// pixel and has 65536 values, and that one's input is a texel and has 256 or 1024. One has to
// interpolate and the other is exact.
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

// The per-texel form: the decode direction, tabulated over the source's own codes.
//
// **Exact rather than sampled, which the encode above could not be.** A texel of an eight-bit format
// has 256 possible values and a ten-bit one has 1024, so every one of them fits in 512 bytes or
// 2 KiB — smaller than the interpolated table above and with no error in it at all. What reads this
// is a `DrawTexture`, once per channel per sampled texel, which for the console's full-screen grid
// is thirty million lookups in a frame; the band scratch has to survive that, and a table this size
// sits beside it rather than evicting it.
//
// **Indexed by the top `bits` of a widened channel.** Seam/Pixel.h widens a source code by bit
// replication, so shifting the widened value back down recovers the code exactly and the two files
// agree by construction rather than by a divide that rounds.
//
// **Why the source is decoded per texel and not once, at adoption.** Decoding an image into the
// band's own linear premultiplied form would pay this once instead of once per frame — and it would
// cost eight bytes a texel, which for a console grid at 4K is 66 MB and a full re-decode every time
// a line is printed. Blit/Band.h refuses that footprint for the scratch and it is refused here for
// the same reason.
class DecodeTable
{
public:
	// `bits` is Seam/Pixel.h's `BitsPerChannel` for the source format, which is 8 or 10. `EINVAL` for
	// any other depth, and for an absolute transfer function for `TransferTable`'s reason.
	[[nodiscard]] Result<void> Build(TransferFunction transfer, std::uint32_t bits) noexcept;

	// The entries themselves, so that a sampler hoists the pointer out of its inner loop rather than
	// calling through this object once per channel per texel.
	[[nodiscard]] const std::uint16_t* Entries() const noexcept { return m_Decode.data(); }

	// What to shift a widened channel down by to index them.
	[[nodiscard]] std::uint32_t Shift() const noexcept { return m_Shift; }

	// Whether the source's codes are already proportional to light. It is what decides whether a
	// premultiplied texel can be converted where it stands — see Blit/Blit.cpp's `Fetch`, which is
	// the one place in the composite that has to divide by an alpha.
	[[nodiscard]] bool IsProportional() const noexcept { return m_Proportional; }

private:
	// Ten bits' worth, which is the deepest source Seam/Pixel.h codes. An eight-bit table uses the
	// first 256 and the rest is 1.5 KiB that never gets touched — cheaper than the branch a second
	// storage size would put in the sampler.
	static constexpr std::size_t MaxEntries = 1024;

	std::array<std::uint16_t, MaxEntries> m_Decode{};
	std::uint32_t m_Shift = 8;
	bool m_Proportional = true;
};
