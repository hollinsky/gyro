#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Text/Font.h"

// A string and a face in, coverage out.
//
// **This is the layout, and it is deliberately not one.** There is no shaping, no kerning, no
// bidirectional reordering and no line breaking: a monospaced cell per code point, a newline starting
// the next row of cells, and a tab advancing to the next multiple of eight columns. Decision 38 says
// the recovery console is a fixed grid of pre-rendered glyphs *precisely because* it must work when
// nothing else does, and every one of the absent features is a table, a state machine, or a
// dependency between the person at the keyboard and the message they need to read.
//
// **Coverage is a byte per texel, valued 0 or 255.** A byte rather than a bit because every consumer
// immediately multiplies it — a masked fill in `Blit`'s band, a select between two words in
// `Text/Label.h` — and unpacking at the consumer would put the same shift loop in each of them. The
// two values it takes are the honest range of a bitmap font; nothing here antialiases, and a caller
// that wants a soft edge is asking for a different font rather than a filter over this one.

struct TextExtent
{
	PixelSize<BufferSpace> Size;

	// Rows of cells. A caller placing several strings under each other wants it, and it is free here
	// where recovering it from the height would be a division.
	std::int32_t Lines;
};

// What `Rasterize` would fill. Never taller than the number of lines times the cell, and never wider
// than the longest line — the extent is the text's own box, not a box it was given.
[[nodiscard]] TextExtent Measure(const Face& face, std::string_view text) noexcept;

// Fills the extent's rectangle at the origin of `coverage`, every texel of it, so a caller reusing a
// buffer does not have to clear one first. `stride` is in bytes and may exceed the extent's width;
// texels outside the rectangle are left alone, which is what lets a caller compose several strings
// into one image.
//
// Refuses a buffer the extent does not fit in rather than clipping: a label that silently lost its
// last three characters is a debug view that lies, and the size is knowable before the call.
[[nodiscard]] Result<void>
Rasterize(const Face& face, std::string_view text, std::span<std::uint8_t> coverage, std::int32_t stride) noexcept;

// Columns a tab advances to the next multiple of. Eight, because that is what every log line that
// will ever reach the recovery console was written against.
inline constexpr std::int32_t TabColumns = 8;
