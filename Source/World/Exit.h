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
// closing. A null id draws nothing by Core/Texture.h's rule, which is exactly an output whose atlas
// the device had no room for.
//
// **Whether the pixels are there yet is the frame thread's own knowledge and is deliberately not a
// field here.** *(2026-09-06, with the capture.)* The party that copies a window's last frame into
// its rectangle is the party that composites, and a flag on this record would have to travel out,
// come back on the return leg, and be published again before the picture it describes could be
// drawn — three publications of latency on the frame a window closes. What crosses instead is
// `Reservation` below, which is what the frame thread needs in order to tell one occupant of a
// rectangle from the next.
struct ExitSnapshot
{
	// The node whose snapshot this is: a position in the node run, and the scan's terminator.
	std::uint32_t Node = NoContent;

	// Which output's atlas, as a position in the output set — decision 84's order, the same one
	// `Views`, `Wakes`, and `Sessions` are read in.
	std::uint32_t Output = 0;

	// Which reservation this is, counted once for the whole process. Zero is no reservation.
	//
	// **It exists so that the copy into the rectangle happens once**, which is the difference between
	// decision 46's *thirty-three megabytes read and written* being a cost an exit pays and one every
	// frame of an exit pays. The frame thread remembers what it has already filled, and what it has to
	// remember it by is not the rectangle: a shelf hands the same texels to the next occupant as soon
	// as the one before it is done, so two windows closing a frame apart onto the same rectangle would
	// be one window wearing the other's picture — on screen, for the length of a fade, and only ever
	// on a busy machine. A count that never repeats is what makes that unrepresentable rather than
	// unlikely.
	//
	// Not a `Handle`: nothing is ever looked up by it, and the generational half of one exists to make
	// a *stale* id miss, where the whole use of this is to compare two ids for being the same.
	std::uint32_t Reservation = 0;

	// The atlas the rectangle is in, from the moment the rectangle is taken. Null where that output
	// has no atlas image — no texture space, or a device with nothing left to give — which is a
	// closing window that cuts instead of fading, decision 46's exhaustion answer.
	TextureId Texture;

	// The rectangle within that atlas, in its texels. `BufferSpace` for the reason
	// `ImageContent::Source` is in it — an atlas slot is what that field already says it holds — and
	// so that the two are the same coordinate when the exit becomes what the node draws.
	PixelRect<BufferSpace> Slot;

	friend constexpr bool operator==(ExitSnapshot, ExitSnapshot) noexcept = default;
};

static_assert(std::is_trivially_copyable_v<ExitSnapshot> && std::is_standard_layout_v<ExitSnapshot>);
static_assert(std::is_aggregate_v<ExitSnapshot>);
static_assert(
	sizeof(ExitSnapshot) == 36,
	"A node index, an output index, a reservation, a texture handle, and four bounds"
);
static_assert(alignof(ExitSnapshot) == 4, "Nothing here is wider than an index");

// A record nobody finished belongs to no node, so a scan that reached it stops — the same direction
// as every other default at this waist, where the unwritten value is the one that draws nothing.
static_assert(ExitSnapshot{}.Node == NoContent, "An unfinished entry ends the scan it appears in");
static_assert(ExitSnapshot{}.Texture.IsNull() && ExitSnapshot{}.Slot.IsEmpty());
static_assert(ExitSnapshot{}.Reservation == 0, "Nobody reserved anything, so nothing was ever captured into it");
