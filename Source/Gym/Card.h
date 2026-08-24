#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Geometry/Space.h"

// The picture a gym imports, and it is a measuring instrument before it is an image.
//
// **Gym/Lanes.h's standard, one layer down.** A lane exists so that "the picture is wrong" becomes
// "*which channel*"; this exists so that "the image is wrong" becomes "*which stage*". Sampling an
// image runs a decode, a filter, an alpha fold and a source-rect clamp over the same texels, and every
// one of them fails as a picture that is merely off — a window slightly too dark, a thumbnail slightly
// too soft. So the card is regions, each of which is wrong in a way that names the stage that did it,
// and none of which is legible on a photograph or a logo.
//
// **The bytes are 32-bit little-endian words with alpha in the top byte** — which is DRM's
// `ARGB8888`, spelled in words rather than by naming the code. `Gym` does not depend on `Seam` and
// should not start: a gym stands where a client will stand, and decision 87's rule is that `Protocol`
// may not name `Seam` either, so a gym reaching for a format enum would be building the shape the
// protocol layer cannot copy. What imports these bytes names the format, because that party is
// compositor-side. The alternative was an edge from `Gym` to `Seam`, which is one line of CMake and
// would have hidden the question until there was a protocol to answer it badly.
//
// **Two alpha modes and two phases, because both are diagnostics rather than options.** The same card
// authored premultiplied and authored straight, each tagged as what it is, must composite to the same
// picture — Docs/Architecture.md#premultiplied-alpha-is-the-sharp-edge is the whole reason to be able
// to put the two side by side. The phase is the other half of what a client does: a surface's next
// buffer is different pixels, and a swap that silently does not land is a still picture, which is
// exactly what a scene of springs already looks like when it settles.

// Which of the two buffers this is. A client double-buffers, so a gym that imports once is a
// wallpaper and a gym that alternates is a window.
enum class CardPhase : std::uint8_t
{
	First,
	Second,
};

class Card
{
public:
	// Below this the regions stop being expressible: the card is laid out in sixteenths, and a
	// sixteenth has to be several texels wide before a checkerboard inside it means anything. Refused
	// rather than clamped, because a caller that asked for thirty-two wanted thirty-two and a card
	// silently twice the size it asked for is a filtering result nobody can read.
	static constexpr std::int32_t MinimumSide = 64;

	// Square, and the side is the only dimension because every region is a fraction of it. A card the
	// shape of the panel would put the aspect ratio into every region's arithmetic to say nothing the
	// scene does not already say by where it places the node.
	[[nodiscard]] static Result<Card> Draw(std::int32_t side, AlphaMode alpha, CardPhase phase);

	[[nodiscard]] PixelSize<BufferSpace> Size() const noexcept { return { m_Side, m_Side }; }

	[[nodiscard]] std::uint32_t Stride() const noexcept { return static_cast<std::uint32_t>(m_Side) * 4U; }

	// The pixels, for as long as this object lives. Seam/Importer.h borrows rather than copies and puts
	// the lifetime on the caller, so whoever adopts one of these is holding it until it forgets it.
	[[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return std::as_bytes(std::span{ m_Words }); }

	// The word at a texel, for a test that wants to assert what a region holds rather than that the
	// buffer is the right length. Out of range is transparent black, which is a value the card never
	// writes.
	[[nodiscard]] std::uint32_t At(std::int32_t x, std::int32_t y) const noexcept;

private:
	Card(std::int32_t side, std::vector<std::uint32_t> words) : m_Words{ std::move(words) }, m_Side{ side } {}

	std::vector<std::uint32_t> m_Words;
	std::int32_t m_Side = 0;
};
