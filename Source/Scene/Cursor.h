#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Core/Result.h"
#include "Core/Texture.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Scene/Store.h"

// The pointer glyph, drawn once into an image.
//
// **Decision 152 settles that gyro draws its own glyph rather than adopting an XCursor theme**, and
// this is the drawing. A theme is a dependency and a set of files on disk, and this process is also
// the boot splash and the recovery console — so a pointer that depends on a theme having been
// installed is a pointer the console is without on the exact machine where being without one is worst.
//
// **It is in `Scene` because everything that authors a world needs one.** A gym, the client host, the
// splash and the console all draw the same cursor, and a glyph that lived in `Gym` would be one three
// of them could not name. [Gym/Pointer.h](../Gym/Pointer.h) is the instrument the shape was chosen
// with, and it still holds the constructions that lost.
//
// **A producer of pixels rather than a subtree of nodes, which is [Text/Label.h](../Text/Label.h)'s
// shape and was arrived at the same way.** The construction this replaced authored the arrow as
// several hundred upright rectangles, one per run of equal coverage per device pixel row, and its
// stated reason was that `Blit` draws an axis-aligned solid quad and nothing else. That was true when
// the instrument was written and is not true now: `Blit/Blit.cpp` samples textures, so the CPU
// renderer draws the baked glyph as readily as the Vulkan one and no part of this waits on a device.
// What the rectangles cost is paid by the one node that is on screen in every frame gyro will ever
// draw — dispatch serialises the scene once per pointer motion, so several hundred nodes in the
// cursor is work done at input rate, at exactly the moment latency is what a person feels.
//
// **The coverage arithmetic is unchanged and is the whole of why this is exact.** The arrow is a
// polygon; the polygon is clipped to every device pixel it touches; the area of each piece is the
// pixel's coverage. That is the box-filtered analytic coverage of the shape — the exact prefiltered
// answer rather than a blur, a blur being a *wider* filter and this the narrowest one that integrates
// instead of sampling. Nothing about it wanted to be a node; it wanted to be a texel.
//
// **What baking removes is the second pass and the conditional coverage it needed.** Drawn as nodes
// the outline and the body were two rectangles compositing `over` with a person's desktop between
// them, so the outline's alpha had to be `(d − l) / (1 − l)` — what it needs *given* that the body did
// not already take the pixel — or the dark would blend through the light and grey the glyph's inside
// edge. Composited into one texel there is nothing between them: the premultiplied colour is the light
// body's coverage and the alpha is the whole silhouette's, and drawing that `over` anything leaves
// `l·W + (1 − d)·B`, which is what was wanted all along.
//
// **What baking adds is an encoding, and it is the same arithmetic wearing different clothes.** A
// texel is eight bits in the output's own encoding, and `Blit` unpremultiplies, linearises and
// remultiplies each one — decision 47 composites in linear light — so the value stored is not `l`. It
// is `a · LinearToSrgb(l / a)`, which decodes back to exactly the linear coverage the sweep computed.
// Writing `l` there directly is the mistake worth naming, because it looks right and is wrong in the
// direction nobody checks: the glyph's antialiased edge comes out about a fifth as bright as it should
// be, which reads as a dark rim around a cursor rather than as an error.
//
// **The image is authored against one density and the node is snapped**, which is decision 156 and is
// unchanged by the bake. Coverage is computed for one alignment of the shape to the device grid, so
// the glyph is right at that alignment and nowhere else — `Node::Snap` lands the node's origin on a
// device pixel whenever it is drawn, which makes the sampling a one-to-one copy rather than a filter
// and the glyph byte-identical from frame to frame. Without it the outline is a stroke a pixel and a
// bit wide, one dark pixel at one sub-pixel phase and two grey ones at the next, and the cursor visibly
// boils as it crosses the screen. The consequence is worth stating plainly: this makes the glyph's
// *shape* exact, not its motion. A pointer still advances a whole device pixel at a time, and baking a
// texture per sub-pixel phase is a different change than this one.
//
// **A density is a parameter and not a lookup**, because the glyph does not know which output it is on
// and coverage taken against the wrong grid is a shape that is neither exact nor a staircase. An image
// per density is what a second panel costs, and picking between them per output is Docs/Open.md's
// business rather than this file's — the construction that authored nodes had exactly the same defect
// and no way to fix it either.

// The arrow's design box, one unit wide by `ArrowHeight` tall, with the hotspot at its top-left
// corner. Exported because [Gym/Pointer.h](../Gym/Pointer.h) approximates the same shape with upright
// columns to show what that costs, and two definitions of one arrow would make the comparison a
// comparison of two glyphs.
inline constexpr double ArrowWidth = 0.70;
inline constexpr double ArrowHeight = 1.10;

// The top boundary: one straight edge from the tip to the wing's point. This line is the whole reason
// the glyph was ever hard — it is the one thing an upright rectangle cannot express.
[[nodiscard]] constexpr double ArrowTop(double x) noexcept
{
	return x * (0.68 / ArrowWidth);
}

// The bottom boundary, in four pieces: the left edge falling back to the notch, the notch's far side
// dropping to the tail, the tail's own foot, and the wing's underside beyond it.
[[nodiscard]] constexpr double ArrowBottom(double x) noexcept
{
	if (x <= 0.24)
	{
		return 1.00 + (0.76 - 1.00) * (x / 0.24);
	}

	if (x <= 0.40)
	{
		return 0.76 + (1.10 - 0.76) * ((x - 0.24) / 0.16);
	}

	if (x <= 0.56)
	{
		return 1.10 + (1.02 - 1.10) * ((x - 0.40) / 0.16);
	}

	return 0.68;
}

// The dark outline's width, as a fraction of the glyph's height. A cursor is read against whatever is
// behind it, so the outline is not decoration: without it a light glyph over a light window is a hole.
inline constexpr double ArrowOutline = 0.055;

// SPEC: the most texels one baked glyph may cover. A cursor is tens of pixels on a side and this is
// hundreds, so it bounds the arithmetic rather than estimating a working set — Core/SlotAllocator.h's
// reason. What it buys is that a glyph authored at a panel's height arrives as a sentence naming the
// glyph rather than as a megabyte allocated on the dispatch thread.
inline constexpr std::size_t MaxCursorTexels = 512U * 512U;

// The arrow as an image, in the layout Scene/Textures.h adopts: eight bits a channel in a 32-bit
// little-endian word, blue lowest, alpha in the top byte and already multiplied into the other three.
//
// The pixels are the caller's to adopt and the caller's to retire, which is `Label`'s contract and
// `Card`'s — Seam/Importer.h borrows rather than copies one layer down, and holding an image here that
// something else's watermark governed would be this file owning a lifetime it cannot see.
class CursorImage
{
public:
	// `height` is the glyph's design height in the output's own units — about twenty-four for a real
	// cursor — and `density` is that output's scale.
	//
	// `EINVAL` for a height that is not positive or a polygon that clipped to more vertices than the
	// sweep carries; `E2BIG` where the two together ask for more texels than `MaxCursorTexels`.
	[[nodiscard]] static Result<CursorImage> Draw(double height, Scale density);

	[[nodiscard]] PixelSize<BufferSpace> Size() const noexcept { return m_Size; }

	[[nodiscard]] std::uint32_t Stride() const noexcept { return static_cast<std::uint32_t>(m_Size.Width) * 4U; }

	// The pixels, for as long as this object lives.
	[[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return std::as_bytes(std::span{ m_Words }); }

	// The word at a texel, for a test that asserts what the image holds. Out of range is transparent
	// black, which is what surrounds the glyph anyway.
	[[nodiscard]] std::uint32_t At(std::int32_t x, std::int32_t y) const noexcept;

	// Where the hotspot sits inside the image, in device pixels from its top-left corner. Both are
	// positive: the outline reaches above and to the left of the point the pointer is *at*, and that is
	// correct — a hotspot is a place on the screen rather than a corner of a drawing.
	[[nodiscard]] std::int32_t HotspotX() const noexcept { return m_HotspotX; }
	[[nodiscard]] std::int32_t HotspotY() const noexcept { return m_HotspotY; }

	// The density the coverage was taken against, which is what `AuthorCursor` needs to size a node in
	// the output's units and is carried here so a caller cannot pair an image with the wrong one.
	[[nodiscard]] Scale Density() const noexcept { return m_Density; }

private:
	CursorImage(
		PixelSize<BufferSpace> size,
		std::vector<std::uint32_t> words,
		std::int32_t hotspotX,
		std::int32_t hotspotY,
		Scale density
	)
		: m_Words{ std::move(words) }, m_Size{ size }, m_HotspotX{ hotspotX }, m_HotspotY{ hotspotY },
		  m_Density{ density }
	{}

	std::vector<std::uint32_t> m_Words;
	PixelSize<BufferSpace> m_Size;
	std::int32_t m_HotspotX = 0;
	std::int32_t m_HotspotY = 0;
	Scale m_Density;
};

// One image node under `parent`, with the glyph's hotspot at the parent's own origin, so a pointer
// node is placed at the position it holds with nothing subtracted from it.
//
// `texture` is what the caller adopted the image's bytes as. The node carries `Node::Snap` and nothing
// else; there is nothing under it.
[[nodiscard]] Result<EntityId>
AuthorCursor(SceneStore& scene, EntityId parent, TextureId texture, const CursorImage& image);
