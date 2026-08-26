#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Text/Font.h"

// A string turned into an image, so that anything able to draw a texture is able to draw text.
//
// **The pixels are 32-bit little-endian words with alpha in the top byte**, which is `Gym/Card.h`'s
// convention and for its reason: `Text` does not depend on `Seam` and should not start, so what
// imports these bytes names the format. A label stands exactly where a client's buffer stands.
//
// **One texture per string, and no atlas.** A debug label and a window title change when their text
// changes, which is rarely, and an atlas plus a quad run is a second draw path with a packer behind it
// that only pays for itself when something wants live digits every frame. A scrolling console readout
// is the caller that will force one, and until it exists this is a `std::vector` and a memcpy.
//
// **No `AlphaMode`.** A bitmap face's coverage is one bit, so every texel is one of the two words
// handed in and nothing is ever a blend — there is nothing to premultiply. The caller's two words are
// whatever the importer will be told they are, and a label passes them through unchanged.

class Label
{
public:
	// `foreground` where a glyph covers the texel and `background` where it does not, the background
	// defaulting to transparent so the ordinary label is glyphs over whatever is behind them.
	//
	// Refuses an empty extent rather than returning a zero-sized image: a texture with no texels is
	// something an importer has to have an answer for, and "the string was empty" is a question the
	// caller can ask before it gets here.
	[[nodiscard]] static Result<Label>
	Draw(const Face& face, std::string_view text, std::uint32_t foreground, std::uint32_t background = 0);

	[[nodiscard]] PixelSize<BufferSpace> Size() const noexcept { return m_Size; }

	[[nodiscard]] std::uint32_t Stride() const noexcept { return static_cast<std::uint32_t>(m_Size.Width) * 4U; }

	// The pixels, for as long as this object lives — Seam/Importer.h borrows rather than copies, so
	// whoever adopts one of these holds it until it forgets it. The same contract Card carries.
	[[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return std::as_bytes(std::span{ m_Words }); }

	// The word at a texel, for a test that asserts what the image holds. Out of range is transparent
	// black, which a label writes only where its caller asked for it.
	[[nodiscard]] std::uint32_t At(std::int32_t x, std::int32_t y) const noexcept;

private:
	Label(PixelSize<BufferSpace> size, std::vector<std::uint32_t> words) : m_Words{ std::move(words) }, m_Size{ size }
	{}

	std::vector<std::uint32_t> m_Words;
	PixelSize<BufferSpace> m_Size;
};
