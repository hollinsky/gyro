#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "Geometry/Space.h"

// A monospaced bitmap face, and the only thing in gyro that knows what a letter looks like.
//
// **Text is a producer of pixels rather than a verb on a renderer.** Every verb on `Seam/Renderer.h`
// has to be answered by `Blit`, by `Render`, and by `Headless` — which draws nothing and charges a
// cost — so a `DrawText` there would put a font and a glyph cache behind each of them, one per
// output, and oblige the fake backend to fake typography. A face produces coverage instead: the
// recovery console masks a fill with it into `Blit`'s band, and a debug label turns it into texels and
// mints a texture id the way `Gym/Card.h` already does. Everything that can already draw an image can
// then draw text, and the Vulkan sampler picks it up on the day it lands with nothing to add.
//
// **The face is chosen from a ladder, because a bitmap does not scale.** A glyph is texels and
// enlarging one is either a blur or a staircase, so the only way the recovery console is legible on a
// 4K panel and on a 1366x768 laptop is to *have* a glyph at that size. Spleen ships six —
// 5x8 through 32x64 — and `Nearest` is the whole of the sizing policy. See
// Docs/Decisions.md for why that ladder is what selected the font.
//
// **Coverage is one bit.** A texel is inside the glyph or outside it, and nothing between, which is
// why nothing here takes an `AlphaMode`: no texel a face produces is ever a blend of two colours, so
// there is nothing to premultiply. It is also why a label is exact — a test asserts the word at a
// texel rather than a tolerance.

// One glyph's bits: `Face::CellHeight()` rows, each `Face::RowBytes()` bytes, most significant bit
// leftmost. That is the layout BDF already stores, so the bake transcribes rather than repacks.
struct Glyph
{
	std::span<const std::uint8_t> Rows;
};

// A contiguous stretch of code points the face has, and where its first glyph sits in the face's
// bitmap array. Coverage is runs rather than a table because it is genuinely sparse: 5x8 carries
// thirteen box-drawing characters out of a block of a hundred and twenty-eight, and a dense table
// spanning U+2500 would be mostly holes at every size.
struct CodeRun
{
	char32_t First;
	char32_t Last;
	std::uint32_t Index;
};

class Face
{
public:
	// Everything is a span into the baked tables, which have static storage — a Face is a view and
	// never an owner, so copying one is free and none of them outlive the program.
	constexpr Face(
		std::string_view name,
		std::int32_t cellWidth,
		std::int32_t cellHeight,
		std::int32_t ascent,
		std::span<const CodeRun> runs,
		std::span<const std::uint8_t> bitmaps
	) noexcept
		: m_Name{ name }, m_Runs{ runs }, m_Bitmaps{ bitmaps }, m_CellWidth{ cellWidth }, m_CellHeight{ cellHeight },
		  m_Ascent{ ascent }
	{}

	// `spleen-8x16`, for a log line and for a test that wants to say which face it got rather than
	// assert on two numbers that another face could share.
	[[nodiscard]] constexpr std::string_view Name() const noexcept { return m_Name; }

	[[nodiscard]] constexpr PixelSize<BufferSpace> CellSize() const noexcept { return { m_CellWidth, m_CellHeight }; }

	// Rows from the top of the cell down to the baseline. The console needs it to sit a cursor and an
	// underline where the glyphs already are, rather than at a fraction of the cell that is right for
	// one size and wrong for the rest.
	[[nodiscard]] constexpr std::int32_t Ascent() const noexcept { return m_Ascent; }

	[[nodiscard]] constexpr std::int32_t RowBytes() const noexcept { return (m_CellWidth + 7) / 8; }

	// The glyph for a code point, or the notdef box for one the face does not carry — index 0 is
	// always that box, synthesised by the bake rather than taken from the font, so a missing character
	// is a mark somebody can see and report instead of a hole in a sentence.
	[[nodiscard]] Glyph Find(char32_t code) const noexcept;

	// Whether the face carries it, for a caller that wants to fall back rather than draw a box.
	[[nodiscard]] bool Has(char32_t code) const noexcept;

private:
	[[nodiscard]] Glyph At(std::uint32_t index) const noexcept;

	std::string_view m_Name;
	std::span<const CodeRun> m_Runs;
	std::span<const std::uint8_t> m_Bitmaps;
	std::int32_t m_CellWidth;
	std::int32_t m_CellHeight;
	std::int32_t m_Ascent;
};

// Every baked face, ascending by cell height. Defined by the generated table rather than here, which
// is what keeps the font data out of the source tree — see CMake/Fonts.cmake.
[[nodiscard]] std::span<const Face> Faces() noexcept;

// The face whose cell height is closest to what was asked for, rounding down on a tie so that a
// request that falls between two sizes gets the one that fits rather than the one that crops. Never
// empty: there is always a ladder, and the ends of it clamp.
[[nodiscard]] const Face& Nearest(std::int32_t cellHeight) noexcept;
