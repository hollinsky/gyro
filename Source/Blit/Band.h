#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Core/Result.h"

// The scratch a CPU composite happens in: a few rows of the output, in linear premultiplied light.
//
// **A band rather than a second framebuffer, and the reason is what the target's memory is.** A dumb
// buffer from a real KMS driver is write-combined — writes stream and reads are one to two orders
// slower — so the composite cannot happen there and the result has to be copied in. Making that
// scratch full-size is 66 MB at 4K and drags every pixel through the cache twice; making it a few
// dozen rows keeps the composite inside the cache and turns the copy into exactly the sequential
// write-combining likes. The band is the whole reason the size of the panel does not appear in this
// module's memory footprint.
//
// **It does not persist and does not need to.** What a renderer owes outside the damage region is
// what was already there, and outside the damage region nothing here writes. Inside it, the bottom
// of the composite is an opaque clear this file put down — so *what is beneath* a translucent item
// is always something the band already holds, and the target is never read on any driver. That is
// Docs/Decisions.md decision 110's *`Blit` never reads its target*, arrived at one step earlier than
// the entry does: the entry gets there per driver, and it is true per composite.
//
// **Premultiplied, linear, sixteen bits a channel.** Premultiplied because `src + dst * (1 - a)` is
// the composite and anything else is a divide per pixel; linear because decision 47 says so and
// because Blit/Transfer.h has what it costs on screen; sixteen bits because the seam already names
// ten-bit targets and eight would quantize a gradient the encode is about to widen again. Full-range
// white is 65535 and there is no headroom above it — a boot composite has no highlight to lose, and
// the day one arrives it arrives with the wire colorimetry Docs/Open.md is still holding.

// One pixel of the composite.
struct Light
{
	std::uint16_t Red = 0;
	std::uint16_t Green = 0;
	std::uint16_t Blue = 0;
	std::uint16_t Alpha = 0;

	friend constexpr bool operator==(Light, Light) noexcept = default;
};

// `a * b` over the full range, rounded to nearest and exact at both ends — `Multiply(x, 65535) == x`
// and `Multiply(x, 0) == 0`, which is what keeps an opaque composite exactly opaque however many
// items are drawn into it. The doubled shift is the sixteen-bit form of the familiar division by
// 255; a plain `/ 65535` is the same answer and a divide per channel per pixel.
[[nodiscard]] constexpr std::uint16_t Multiply(std::uint16_t a, std::uint16_t b) noexcept
{
	const std::uint32_t product = static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b) + 32768U;

	return static_cast<std::uint16_t>((product + (product >> 16U)) >> 16U);
}

// A premultiplied colour scaled by a coverage or an opacity, which for premultiplied values is the
// same operation on all four channels. That identity is the reason the item's opacity, the pixel's
// coverage, and a group's fade can be one number by the time they reach a span.
[[nodiscard]] constexpr Light Attenuate(Light light, std::uint16_t by) noexcept
{
	return { Multiply(light.Red, by), Multiply(light.Green, by), Multiply(light.Blue, by), Multiply(light.Alpha, by) };
}

// A weighted average of two premultiplied colours: `by` of the second and the rest of the first.
//
// **One rounding rather than two**, which is what makes it exact at both ends — a resample whose
// weight lands on zero returns the texel it landed on, bit for bit, rather than a value one unit
// away from it. That is the property the sharp path and the filtered path are compared against, and
// two roundings would make them disagree by a least-significant bit everywhere.
[[nodiscard]] constexpr std::uint16_t Blend(std::uint16_t from, std::uint16_t to, std::uint16_t by) noexcept
{
	const std::uint32_t keep = 65535U - static_cast<std::uint32_t>(by);
	const std::uint32_t product = static_cast<std::uint32_t>(from) * keep +
	                              static_cast<std::uint32_t>(to) * static_cast<std::uint32_t>(by) + 32768U;

	return static_cast<std::uint16_t>((product + (product >> 16U)) >> 16U);
}

// The same across all four channels, which for premultiplied values is the whole of a filter tap:
// the weights sum to one, so a weighted sum of premultiplied colours is the premultiplied colour of
// the weighted sum and nothing has to be undone in between.
[[nodiscard]] constexpr Light Mix(Light from, Light to, std::uint16_t by) noexcept
{
	return { Blend(from.Red, to.Red, by),
		     Blend(from.Green, to.Green, by),
		     Blend(from.Blue, to.Blue, by),
		     Blend(from.Alpha, to.Alpha, by) };
}

// `source` over `under`, both premultiplied.
[[nodiscard]] constexpr Light Over(Light source, Light under) noexcept
{
	const std::uint16_t keep = static_cast<std::uint16_t>(65535U - source.Alpha);

	return { static_cast<std::uint16_t>(source.Red + Multiply(under.Red, keep)),
		     static_cast<std::uint16_t>(source.Green + Multiply(under.Green, keep)),
		     static_cast<std::uint16_t>(source.Blue + Multiply(under.Blue, keep)),
		     static_cast<std::uint16_t>(source.Alpha + Multiply(under.Alpha, keep)) };
}

class Band
{
public:
	// How many rows a band of this width holds. Sized so the whole scratch stays inside a core's
	// private cache, which is what makes the banding worth doing at all: a band that spilled would
	// be a full-size scratch written in an awkward order.
	//
	// **256 KiB is a floor across the machines gyro boots on rather than a measurement**, and it is
	// the number to revisit with a profile in front of it. A wider band costs nothing but memory; a
	// band that does not fit costs the entire point.
	[[nodiscard]] static std::int32_t RowsFor(std::int32_t width) noexcept;

	// Allocating, and therefore `BindTargets`-only: Seam/Renderer.h permits that call to be slow and
	// Core/FrameSection.h aborts on an allocation inside a frame. `EINVAL` for a width that is not a
	// width, `ENOMEM` where the scratch will not fit.
	[[nodiscard]] Result<void> Reserve(std::int32_t width);

	void Release() noexcept;

	[[nodiscard]] std::int32_t Width() const noexcept { return m_Width; }

	[[nodiscard]] std::int32_t Rows() const noexcept { return m_Rows; }

	// One row of the band, for the encode that reads it out. Empty for a row this band does not hold,
	// which is a caller's bug rather than a condition — an empty span writes nothing rather than
	// walking off the end.
	[[nodiscard]] std::span<const Light> Row(std::int32_t row) const noexcept;

	// Opaque black across `[left, right)` of every row in `[0, rows)`, which is the bottom of every
	// composite. Clipped to the band; a rectangle outside it writes nothing.
	void Clear(std::int32_t rows, std::int32_t left, std::int32_t right) noexcept;

	// `source` over `[left, right)` of one row. The source is already attenuated by whatever coverage
	// and opacity apply to the whole run, which is what makes this the tight loop it needs to be: one
	// constant blended across a span, rather than a per-pixel decision repeated.
	void BlendRun(std::int32_t row, std::int32_t left, std::int32_t right, Light source) noexcept;

	// The same run where the source varies per pixel, which is what a texture makes it. `source` is
	// positional over `[left, right)` and is already attenuated, exactly as the constant above is —
	// so a sampled span and a solid reach the band having had their coverage and opacity applied in
	// the same place, and the two paths differ only in where the colour came from.
	//
	// **The constant form above is kept and is not a special case of this one.** Most of a boot
	// screen is a fill: the console's background, a solid panel, the interior of a logo. That path
	// stores where this one has to blend, reads one register where this one reads a second array,
	// and is the reason a full-screen repaint on a CPU is affordable at all.
	void BlendRun(std::int32_t row, std::int32_t left, std::int32_t right, std::span<const Light> source) noexcept;

	// One pixel, for a partially covered edge where the run's constant does not hold.
	void BlendPixel(std::int32_t row, std::int32_t column, Light source) noexcept;

	[[nodiscard]] Light At(std::int32_t row, std::int32_t column) const noexcept;

private:
	// Whether `[left, right)` of `row` lies inside the band, narrowed to what does. False where
	// nothing does.
	[[nodiscard]] bool Clip(std::int32_t row, std::int32_t& left, std::int32_t& right) const noexcept;

	std::vector<Light> m_Pixels;
	std::int32_t m_Width = 0;
	std::int32_t m_Rows = 0;
};

// The algebra the composite rests on, at compile time. `Over` an opaque backdrop stays exactly
// opaque however many times it runs, which is what lets the encode skip unpremultiplying and what
// would otherwise decay a boot screen one least-significant bit per item drawn.
static_assert(Multiply(65535, 65535) == 65535 && Multiply(65535, 0) == 0 && Multiply(1234, 65535) == 1234);
static_assert(Over(Light{ 0, 0, 0, 0 }, Light{ 100, 200, 300, 65535 }) == Light{ 100, 200, 300, 65535 });
static_assert(Over(Light{ 65535, 0, 0, 65535 }, Light{ 0, 0, 65535, 65535 }) == Light{ 65535, 0, 0, 65535 });

// Half coverage over opaque black is half the light, which is the assertion the whole linear-light
// argument reduces to. It is 32768 here. The same edge blended in the target's own encoding puts
// code 128 down, which is 14000 of these units — the dark rim around everything Blit draws.
static_assert(Over(Attenuate(Light{ 65535, 65535, 65535, 65535 }, 32768), Light{ 0, 0, 0, 65535 }).Red == 32768);

// Attenuating by nothing and by everything are both exact, so an opacity of one costs no precision.
static_assert(Attenuate(Light{ 65535, 1, 2, 65535 }, 65535) == Light{ 65535, 1, 2, 65535 });
static_assert(Attenuate(Light{ 65535, 65535, 65535, 65535 }, 0) == Light{});

// A filter tap that landed on a texel is that texel, at both ends and in the middle of the range.
// The first two are what let the sharp path and the filtered path be compared byte for byte; the
// third is the same linear-light claim the edge above makes, reached through the resample instead.
static_assert(Mix(Light{ 65535, 1, 2, 65535 }, Light{ 3, 4, 5, 6 }, 0) == Light{ 65535, 1, 2, 65535 });
static_assert(Mix(Light{ 3, 4, 5, 6 }, Light{ 65535, 1, 2, 65535 }, 65535) == Light{ 65535, 1, 2, 65535 });
static_assert(Mix(Light{ 0, 0, 0, 65535 }, Light{ 65535, 65535, 65535, 65535 }, 32768).Red == 32768);
