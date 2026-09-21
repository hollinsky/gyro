#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Core/Wake.h"

// The shape of what crosses the publication boundary, as bytes at offsets.
//
// Docs/Structure.md names Publication the *data waist* — the snapshot, the SPSC ring, the return
// channel, the watermark, the per-buffer hold. This file is the first of those: the on-the-wire
// layout of one published snapshot, read by the frame side and written by the dispatch side, and
// nothing else in the process is entitled to know its bytes.
//
// **Offset-addressed POD, not pointers, and Docs/Decisions.md decision 45 is why.** A published
// snapshot is a single contiguous region in which every internal reference is a byte offset from the
// region's base rather than an address. Decision 49 holds open the option of the dispatch side
// becoming a *process* rather than a thread — the one move that meaningfully shrinks a restart — and
// that is a change of medium (a shared mapping instead of a heap) only if the snapshot can live at a
// base address that differs between the two sides. Offsets survive that; pointers do not. The
// affordance is free at line zero and is the reason this representation is chosen before there is any
// process to want it.
//
// **Publication does not depend on Animation, and this file shows why it need not.** The snapshot
// carries spring coefficients, but it carries them as opaque, homogeneous, fixed-stride runs of
// trivially-copyable bytes: a run is a byte offset, an element count, and the size and alignment the
// writer used, and never a type. That `Positions` is a run of `Spring<double>` is knowledge held by
// Scene, which writes it, and by Frame, which reconstitutes it through Animation/Solve — the two
// modules Docs/Structure.md gives edges to both Publication and Animation. The waist itself stays on
// Core and Geometry, and `CheckLayering.cmake` holds it there. What this file *does* name is the
// schema both sides agree on: which runs exist and in what order. A name is not a type, and fixing
// the schema at the waist both halves bind to is where it belongs.
//
// The node run is the same arrangement one level up. Docs/Decisions.md decision 86 fixes what crosses
// — the scene in preorder, each record carrying the length of its own subtree — and this file
// reserves the slot without naming the record, exactly as it does for the driven ramp.
//
// **The record is `World/Node.h`'s, and it is not Scene's.** *(Revised: decision 91.)* Scene writes
// it and Frame walks it — decision 90 puts the tree's validation on the frame thread's own traversal
// — and Frame may not depend on Scene, so a record Scene declared would be one Frame could not name.
// That is decision 87's rule one level up: a type both halves of the world name lives below both
// waists. What is left open is what a node *carries* rather than where the record lives; the fields
// that wait on Docs/Open.md's scene vocabulary are absent from it and arrive with that vocabulary.
//
// **The one payload it does name by type is the wake schedule, because Core owns that type.**
// Docs/Animation.md#storage puts the reduced wake fold in the snapshot header, one entry per output,
// so the frame thread reads a schedule rather than deriving one. `Wake` is a Core primitive
// (Docs/Structure.md#the-wake-is-in-core-and-the-table-above-is-why), on a declared edge, so the
// header holds it as itself.

// A gyro snapshot, recognisable as one. A magic and a version guard a stale or foreign mapping —
// which decision 45's process option makes a real possibility rather than a theoretical one — so that
// a reader refuses bytes it does not understand instead of misreading them as a scene. The value
// spells "gyro"; its numeric form is arbitrary, since the same architecture writes and reads it.
inline constexpr std::uint32_t SnapshotMagic = 0x6779726Fu;

// Bumped when the layout below changes in a way a reader compiled against the old one would
// misinterpret. A reader that does not recognise the version resolves to nothing rather than to
// garbage — the same conservative direction as every settling and ingest decision in the codebase.
inline constexpr std::uint32_t SnapshotVersion = 5;

// The coefficient runs, and the order they are indexed in. This is the pinned shape of
// Docs/Decisions.md decisions 50, 72, and 90: **one run per channel**, plus the driven ramp beside
// them.
//
// **One per channel rather than as few as will fit, and decision 90 is why.** A run's whole job is
// to be a homogeneous fixed-stride array, and it costs one sixteen-byte directory entry in the
// header below — so there is no pressure to merge, and merging is not free. The channels differ in
// arity as well as precision: translation and scale and the rotation log map are three-component,
// opacity is one, and a `Spring<Vector3<float>>` is fifty-six bytes against a `Spring<float>`'s
// thirty-two. One run for "everything at single precision" is therefore either impossible or
// padded, and padding taxes opacity hardest — the most animated channel in the system, and under
// Docs/Decisions.md decision 71 the *only* channel that moves for a reader who has asked for
// reduced motion.
//
// **Separate runs also make the node's index unambiguous.** A node names its active channels by
// index (decision 86), and with one run per channel that index is a position within its own
// channel's array — so pointing a scale index at a rotation spring stops being expressible rather
// than being caught. Merged, the two are the same element size and nothing downstream could tell
// them apart.
//
// **`DrivenProgress` is decision 72's** — the driven regime is a distinct closed form, not reachable
// from any spring, so it travels as its own fixed-stride run rather than making a spring run a
// tagged union. The waist reserves the slot; the record that fills it is Animation's to define when
// that regime is built.
//
// **The precision split is Docs/Architecture.md#the-spaces**: positions cross at double because
// global space runs out of single precision at exactly wl_fixed's resolution, and every other
// channel is bounded by the node it belongs to.
//
// Blur and corner radius are absent because the material vocabulary they belong to is open
// (Docs/Open.md). They arrive as runs, which is the point of the rule: a channel is added by adding
// a run, never by re-striding one that already works.
enum class SnapshotRun : std::uint32_t
{
	Translation = 0,    // translation springs, at double
	Scale = 1,          // scale springs, three-component at single
	Rotation = 2,       // rotation springs over the log-map deviation, three-component at single
	Opacity = 3,        // opacity springs, scalar at single
	DrivenProgress = 4, // the driven ramp (p0, v0, t0, horizon), usually empty
};

inline constexpr std::size_t SnapshotRunCount = 5;

[[nodiscard]] constexpr std::size_t RunIndex(SnapshotRun run) noexcept
{
	return static_cast<std::size_t>(run);
}

// One run's location within the snapshot. Everything is a byte offset from the snapshot base, so the
// entry is meaningful in a mapping placed at any address.
//
// The writer records the size and alignment it used, and the reader checks them against the type it
// is asked to resolve. That is the boundary's type-safety: a Frame that asks for the wrong element
// type at a slot, or is compiled against a record whose size has drifted from the writer's, gets an
// empty run rather than a reinterpretation of bytes into the wrong shape.
struct RunEntry
{
	std::uint32_t Offset = 0;       // bytes from the snapshot base to the first element
	std::uint32_t Count = 0;        // number of elements; zero is an absent run
	std::uint32_t ElementSize = 0;  // sizeof(element), as the publisher wrote it
	std::uint32_t ElementAlign = 0; // alignof(element), as the publisher wrote it
};

static_assert(std::is_trivially_copyable_v<RunEntry> && std::is_standard_layout_v<RunEntry>);
static_assert(sizeof(RunEntry) == 16, "Four uint32s, and no padding to leave uninitialised");

// The fixed head of every snapshot, at offset zero.
//
// The runs are addressed through their directory here rather than laid out at fixed offsets, because
// their sizes are the scene's and not the format's — a run of active springs is as long as the scene
// has active springs. The wake schedule is beside them for the reason Docs/Animation.md#storage
// gives: the frame thread reads the reduced fold rather than folding per output itself.
//
// **`Nodes` is a member rather than a sixth entry in `Runs`, and the distinction is load bearing.**
// `Runs` is indexed by channel, and Docs/Decisions.md decision 90 makes that index the thing a node
// record names — so every entry in that array has to *be* a channel, or the index stops meaning one
// position in one channel's array. The scene's topology is not a channel. It is what the channels
// belong to, so it sits beside the wake schedule as its own named entry, addressed by name and never
// by `RunIndex`. See decision 86 for the record it holds and the preorder-plus-subtree-length shape
// the frame thread walks it in.
//
// **`Views`, `Images`, and `Solids` join it on the same terms, and for the same reason none of them
// is a channel.** A view is one output's placement — where the world sits on that output's grid —
// and it crosses positionally under decision 84's rule, exactly as `Wakes` does: one entry per
// output, in output order, meaningless unless the run's length is the output set's. The other two
// are what a leaf draws, which decision 95 keeps in per-kind runs beside the node run rather than
// in a union inside the record, so that the common node — a container with nothing to draw — does
// not drag a payload through cache in order not to use it. A node names a position in whichever of
// them its kind selects.
//
// **`Roots` and `Sessions` are decision 21's partition, and they are two runs because they are
// counted by two different things.** A published scene holds every connected session's roots at once
// and an output shows the one it is assigned to, so the frame thread needs both halves of that
// comparison: `Roots` names each top-level node and the session it belongs to, and is as long as the
// scene has roots; `Sessions` names what each output is showing, and is one entry per output
// in output order under decision 84's rule exactly as `Wakes` and `Views` are. Neither is a channel,
// so neither is in `Runs`.
//
// **A `Sessions` entry is a pair and a coefficient index rather than one session**, which is decision
// 188: an output moving from one session to another composites both, live, for the length of the
// transition, so the run carries the session being left and where its opacity is as well as the one
// being shown. The coefficient indexes the *opacity* run — the one coefficient in a snapshot that no
// node points at — which is what keeps a transition from being a run of its own, with a second length
// for decision 84 to check and a second thing to leave unstaged. A reassignment carrying no
// coefficient is the cut, and that is what every entry says today.
//
// **An absent `Sessions` or `Roots` run means an unpartitioned scene rather than no information**,
// which is the one place this file departs from decision 84's *a short run is nothing* reading, and
// it departs for a reason rather than for convenience: `SessionId::None` is the default on both
// sides, so a run of all-`None` and no run at all describe the same world — a machine with no
// sessions on it, which is what gyro is between boot and the first agent's offer. A `Sessions` run
// that is present and the *wrong length* is neither: decision 84 makes it no information, and the
// frame thread resolves the output to `None` and draws gyro's own roots only — a black panel somebody
// reports rather than a screen that might be showing the wrong person's windows.
//
// All of them are addressed by name and resolved through templates, so this module still names
// neither the world's records nor a coordinate: `World/Content.h` holds the two content records,
// `World/Root.h` the root record, and `Geometry/AxisTransform.h` the adapter, and a field added to
// any of them touches the waist not at all.
//
// **`Sequence` is the snapshot's identity, and it is here for shape rather than for use this cut.**
// Decision 45's deferred reclamation has the frame thread publish the sequence it last consumed and
// the dispatch thread free below that. The watermark and the ring that carry it are a later part of
// this module; the number they will name is part of the snapshot from the start, so it is pinned
// here and left unread rather than retrofitted into the header once the ring exists.
//
// **`Reserved` is padding that is spelled**, so that the trailing four bytes before the directory are
// the publisher's to value-initialise rather than whatever the arena last held — the same obligation
// Core/Wake.h and Animation/Solve/Spring.h record for their own tail padding, and the reason the
// publisher builds this from a value-initialised object rather than field by field into raw bytes.
struct SnapshotHeader
{
	std::uint64_t Sequence = 0;
	std::uint32_t Magic = SnapshotMagic;
	std::uint32_t Version = SnapshotVersion;
	std::uint32_t ByteSize = 0; // total bytes of the whole snapshot, header included
	std::uint32_t Reserved = 0;
	RunEntry Runs[SnapshotRunCount] = {};
	RunEntry Wakes = {};  // the per-output wake schedule, one Wake per output
	RunEntry Nodes = {};  // the scene, in preorder, one record per node
	RunEntry Views = {};  // the per-output placement, one adapter per output
	RunEntry Images = {}; // what an image node draws
	RunEntry Solids = {}; // what a solid node draws
	RunEntry Roots = {};  // the top-level nodes, and the session each belongs to

	// What each output is showing, what it is leaving, and where the fade is: one entry per output in
	// output order.
	RunEntry Sessions = {};

	// Where a closing window's pixels are kept while it leaves, one entry per output the window is on.
	// Named rather than a channel for `Nodes`' reason and addressed by a node's own slot rather than
	// positionally, because it is neither per channel nor per output — see `World/Exit.h`.
	RunEntry Exits = {};

	// What each output is asked to be, one entry per output in output order: decision 73's generation
	// and the request it carries. Named for `Sessions`' reason, being per output rather than per
	// channel, and read by the loop rather than by the walk — a request is acted on once when its
	// generation moves, not drawn from every frame. See `World/Configuration.h`.
	RunEntry Configurations = {};
};

static_assert(std::is_trivially_copyable_v<SnapshotHeader> && std::is_standard_layout_v<SnapshotHeader>);
static_assert(
	sizeof(SnapshotHeader) == 248,
	"One uint64, four uint32, five coefficient run entries, and the nine named ones, exactly"
);
static_assert(
	alignof(SnapshotHeader) == 8,
	"The base is eight-aligned, and so the header decides nothing the runs do not"
);

// The alignment the snapshot base must meet. Every element that crosses — a spring, a wake, the
// driven record — carries an Instant, whose alignment is eight, and nothing crosses wider than that.
// Aligning the base to the widest fundamental alignment leaves headroom for a future element that is
// wider without revisiting the publisher, and costs nothing: the allocation is already there.
inline constexpr std::size_t SnapshotBaseAlignment = alignof(std::max_align_t);

namespace Detail
{
// Round an offset up to the next multiple of a power-of-two alignment. The publisher places each run
// at an offset aligned for its element, so that a base meeting SnapshotBaseAlignment yields absolute
// addresses the reader can hand back as typed spans without copying.
[[nodiscard]] constexpr std::size_t AlignUp(std::size_t offset, std::size_t alignment) noexcept
{
	return (offset + alignment - 1) & ~(alignment - 1);
}

// Whether a run lies wholly within a snapshot of the given size, computed without overflow. The
// reader asks this of every entry before it resolves it, so that a corrupt or truncated mapping — the
// failure decision 45's bounds-check exists for — yields an empty run rather than a read past the
// end. A zero-count run is vacuously within bounds and resolves to nothing.
[[nodiscard]] constexpr bool RunWithinBounds(const RunEntry& run, std::size_t byteSize) noexcept
{
	if (run.Count == 0)
	{
		return true;
	}

	// Guard the multiplication before performing it: element count and size are both 32-bit, so their
	// product is taken in a width that cannot wrap, and the sum is checked against the size rather than
	// formed and compared after it might have.
	const std::uint64_t span = static_cast<std::uint64_t>(run.Count) * run.ElementSize;

	return run.Offset <= byteSize && span <= byteSize - run.Offset;
}
} // namespace Detail
