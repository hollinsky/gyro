#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Core/Result.h"
#include "Core/Texture.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"

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
// and coverage taken against the wrong grid is a shape that is neither exact nor a staircase. Who
// answers it is `SceneCursor` below: the density of the output the pointer is on, re-baked when the
// pointer crosses onto a panel of another one. That is one image rather than an image per output held
// forever, and what it costs is a bake at the moment somebody drags the cursor between monitors —
// against a glyph that would otherwise arrive on the second screen at half the size and soft, because
// the sampling stops being a copy. The construction that authored nodes had exactly the same defect
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

// SPEC: the square sizes a display engine will take a cursor at, smallest first.
//
// **This is the one place in gyro where hardware dictates the shape of something gyro draws, and it is
// here because the pointer is the one texture gyro authors.** A client's buffer is the client's and is
// never padded to suit a plane — there is no number to pad it to, since the DRM cursor caps are a
// *maximum* and the constraint an amdgpu cursor plane actually applies is this enumerated set. But the
// glyph is gyro's own pixels, so the width is gyro's to choose, and choosing one the hardware takes
// costs nothing but transparent texels.
//
// **What it buys is every other layer on the screen.** Decision 152's promoted set is a suffix of the
// draw list and the pointer is the frontmost node, so the pointer is in every partition gyro proposes
// — and a display engine that refuses it refuses the whole proposal, including the window underneath.
// A glyph baked at 21 device pixels wide was below the 24-pixel minimum of this machine's overlays and
// below every width its cursor plane accepts, so *nothing on this machine ever promoted*, for the life
// of the project, at about 200 µs of `SCHED_FIFO` frame-thread time per frame spent being told so.
// Three pixels of a drawing were the whole of it.
//
// **Square, and these three numbers, because that is the intersection rather than one card's answer.**
// The set was measured on amdgpu, where the width is enumerated and the height is free; i915 wants a
// square on most generations and 64 square alone on the oldest; 64 square is what the legacy cursor
// ioctl meant and what every driver has taken since. Padding to a square from this set satisfies all of
// them, and it clears the overlays' minimum extent as well — so the glyph is promotable whichever plane
// the backend offers it, which is what keeps `IPresenter::TestLayers` the only arbiter of *fits*
// without it being the only thing that ever says no.
inline constexpr std::array<std::int32_t, 3> CursorPlaneSizes{ 64, 128, 256 };

// The image size a glyph of this extent is baked into: the smallest square in `CursorPlaneSizes` that
// holds it, or the extent itself where none does.
//
// **A glyph too large for the set is left at its own size rather than grown to it.** Padding a 300-pixel
// arrow up to 512 would be a quarter of a megabyte of transparent texels bought against a plane size no
// hardware has ever advertised — and at that extent the overlays' minimum is met by a mile, so the
// layer is promotable on an overlay regardless. The set is a floor to clear, not a grid to land on.
[[nodiscard]] constexpr PixelSize<BufferSpace> CursorImageSize(PixelSize<BufferSpace> glyph) noexcept
{
	for (const std::int32_t side : CursorPlaneSizes)
	{
		if (glyph.Width <= side && glyph.Height <= side)
		{
			return { side, side };
		}
	}

	return glyph;
}

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

	// The whole image, padding included, which is what was adopted and what a plane is handed.
	[[nodiscard]] PixelSize<BufferSpace> Size() const noexcept { return m_Size; }

	// The device pixels the silhouette actually reaches, in the image's top-left corner.
	//
	// **The two differ by the padding and nothing else, and the distinction is worth a verb because
	// every question about how the pointer *looks* is about this one.** A test that asks whether the
	// arrow came out the height it was asked for is asking about the drawing; a test that asks what the
	// node's extent is, or what the display engine was shown, is asking about the image. Reading the
	// padded size for the first is how a change to `CursorPlaneSizes` would silently start passing for
	// the wrong reason.
	[[nodiscard]] PixelSize<BufferSpace> GlyphSize() const noexcept { return m_Glyph; }

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
		PixelSize<BufferSpace> glyph,
		std::vector<std::uint32_t> words,
		std::int32_t hotspotX,
		std::int32_t hotspotY,
		Scale density
	)
		: m_Words{ std::move(words) }, m_Size{ size }, m_Glyph{ glyph }, m_HotspotX{ hotspotX }, m_HotspotY{ hotspotY },
		  m_Density{ density }
	{}

	std::vector<std::uint32_t> m_Words;
	PixelSize<BufferSpace> m_Size;
	PixelSize<BufferSpace> m_Glyph;
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

// SPEC: the arrow's design height, in an output's own units.
//
// Twenty-four is what every desktop ships and is a size rather than a policy — a person who wants a
// larger pointer is asking for a setting nothing in the tree holds yet, and the day that arrives this
// is what it writes. It is stated here rather than at the call site because two of the three things
// that draw a cursor are the splash and the recovery console, and a console whose pointer is a
// different size from the session's would read as a different machine.
inline constexpr double CursorHeight = 24.0;

// The pointer as something on screen, kept on `ScenePointer` by the dispatch loop.
//
// **It is the loop's rather than an author's, and that is the whole reason it exists.** A gym, the
// boot splash, the recovery console and the client host all want the same cursor, and an author that
// had to remember to draw one is an author that forgets — the console being the machine where a
// missing pointer is worst, because it is the machine somebody is already unhappy to be looking at.
// So the loop steps this after the author has run and before the scene is serialised, and no author
// names it.
//
// **The glyph is authored on first use, and raised to the front of the roots every iteration.** A
// client's window is parented into `Protocol/Floor.h`'s container and never becomes a root itself, so
// authoring on first motion looked like enough to make the pointer the last root and therefore the
// frontmost node (55). It was not: a floor is created when a session's agent hands its listener over,
// which is when somebody logs in rather than at startup — so a person who moved the mouse before the
// first window arrived had the cursor drawn *behind* every window for the rest of the session.
// `SceneStore::Raise` is a comparison where the node is already last, which is every iteration but the
// one after a floor opened, so the answer is to re-raise rather than to author later.
//
// **A hidden pointer has no node at all rather than a transparent one.** Nothing in the frame walk
// culls a fully faded node, so a cursor faded out is a quad composited over the whole screen's worth
// of a person's work for as long as they are using a touchscreen. Taking it away costs one entity on
// the transition between touching the screen and reaching for the mouse, which is a rate a person
// sets with their hands.
class SceneCursor
{
public:
	// One dispatch iteration: author the glyph where there is a pointer to draw, keep it under the
	// position, and take it away where there is not.
	//
	// Silent about failure by design — an image that would not bake or would not adopt is a compositor
	// that keeps running without a pointer drawn on it, and there is nobody on this call to tell. It is
	// not retried per iteration: the refusal is remembered against the density it was made at, so a
	// texture space with no renderer that can sample does not re-bake an arrow at input rate.
	void Step(SceneStore& scene, ITextures& textures);

	// The container the glyph hangs under, or null while there is none. For a test, and for the day
	// hit-testing has to know which node is not a window.
	[[nodiscard]] EntityId Container() const noexcept { return m_Container; }

	[[nodiscard]] TextureId Texture() const noexcept { return m_Texture; }

private:
	[[nodiscard]] bool Author(SceneStore& scene, ITextures& textures, Scale density);

	void Withdraw(SceneStore& scene, ITextures& textures) noexcept;

	EntityId m_Container{};
	TextureId m_Texture{};

	// The density the live image was baked against, and what a pointer crossing onto a screen of
	// another one is compared with. Coverage is taken against one device grid, so a glyph carried onto
	// a panel of a different scale is a shape drawn at the wrong size through a filter — which is worth
	// one re-bake at the moment a person drags the pointer between monitors and is not worth an image
	// per output held forever.
	Scale m_Density{};

	// Whether the bake or the adopt was refused at `m_Density`. Cleared by a density that is not that
	// one, because the refusal that matters — a texture space that takes no bytes — is not the only one
	// there is: `E2BIG` is a function of the height and the scale together.
	bool m_Refused = false;
};
