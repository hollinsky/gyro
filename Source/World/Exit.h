#pragma once

#include <cstdint>
#include <type_traits>

#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "World/Node.h"

// Where a closing window's pixels are kept while it leaves, as the element type of the exit run.
//
// Docs/Decisions.md decision 20 gives a window its own copy of its last frame the moment it closes,
// rather than holding the client's buffer for the length of the exit, and decision 46 keeps those
// copies in an atlas reserved per output when that output is configured. `Scene/Atlas.h` does the
// reserving; this is that reservation crossing to the side that draws it.
//
// **One entry per output the window is on, and decision 190 is why there is more than one.** A window
// straddling the seam between two screens is captured on both, at each screen's own scale, because
// the two atlases are the two screens' textures and a rectangle in one addresses nothing in the
// other. So this is not a per-kind content run indexed by a node's `Content` — it is a run a node
// names the *start* of, with its entries for that node contiguous and the run scanned forward while
// `Node` still matches. A window on one screen has one entry and the scan reads one.
//
// **The node index is carried rather than implied, and decision 90 is the reason.** The frame thread
// checks what it is given instead of trusting the writer's contiguity: an entry naming another node
// ends the scan, so a serialiser bug is a window that leaves without its snapshot rather than a walk
// reading somebody else's rectangle. The same direction as every other bound at this waist — the
// failure is a missing picture, not a wrong one.
//
// **The texture is per entry even though it is per output**, which duplicates one id across the
// handful of entries an output has. The alternative is a per-output run of atlas textures and a join
// against it on the frame path, to save four bytes on a run that is empty whenever nothing is
// closing. A null id draws nothing by Core/Texture.h's rule, which is exactly a reservation whose
// pixels have not been captured yet.
struct ExitSnapshot
{
	// The node whose snapshot this is: a position in the node run, and the scan's terminator.
	std::uint32_t Node = NoContent;

	// Which output's atlas, as a position in the output set — decision 84's order, the same one
	// `Views`, `Wakes`, and `Sessions` are read in.
	std::uint32_t Output = 0;

	// The atlas the rectangle is in. Null until the capture happens, which is a window that is leaving
	// and has nowhere to have been captured from yet.
	TextureId Texture;

	// The rectangle within that atlas, in its texels. `BufferSpace` for the reason
	// `ImageContent::Source` is in it — an atlas slot is what that field already says it holds — and
	// so that the two are the same coordinate when the exit becomes what the node draws.
	PixelRect<BufferSpace> Slot;

	friend constexpr bool operator==(ExitSnapshot, ExitSnapshot) noexcept = default;
};

static_assert(std::is_trivially_copyable_v<ExitSnapshot> && std::is_standard_layout_v<ExitSnapshot>);
static_assert(std::is_aggregate_v<ExitSnapshot>);
static_assert(sizeof(ExitSnapshot) == 32, "A node index, an output index, a texture handle, and four bounds");
static_assert(alignof(ExitSnapshot) == 4, "Nothing here is wider than an index");

// A record nobody finished belongs to no node, so a scan that reached it stops — the same direction
// as every other default at this waist, where the unwritten value is the one that draws nothing.
static_assert(ExitSnapshot{}.Node == NoContent, "An unfinished entry ends the scan it appears in");
static_assert(ExitSnapshot{}.Texture.IsNull() && ExitSnapshot{}.Slot.IsEmpty());
