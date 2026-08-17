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
// **The one payload it does name by type is the wake schedule, because Core owns that type.**
// Docs/Animation.md#storage puts the reduced wake fold in the snapshot header, one entry per output,
// so the frame thread reads a schedule rather than deriving one. `Wake` is a Core primitive
// (Docs/Structure.md#the-wake-is-in-core), on a declared edge, so the header holds it as itself.

// A gyro snapshot, recognisable as one. A magic and a version guard a stale or foreign mapping —
// which decision 45's process option makes a real possibility rather than a theoretical one — so that
// a reader refuses bytes it does not understand instead of misreading them as a scene. The value
// spells "gyro"; its numeric form is arbitrary, since the same architecture writes and reads it.
inline constexpr std::uint32_t SnapshotMagic = 0x6779726Fu;

// Bumped when the layout below changes in a way a reader compiled against the old one would
// misinterpret. A reader that does not recognise the version resolves to nothing rather than to
// garbage — the same conservative direction as every settling and ingest decision in the codebase.
inline constexpr std::uint32_t SnapshotVersion = 1;

// The runs the snapshot carries, and the order they are indexed in. This is the pinned shape of
// Docs/Decisions.md decision 50 and decision 72: two homogeneous spring-coefficient runs split by
// precision, and the driven-progress ramp as its own homogeneous run beside them.
//
// **The precision split is Docs/Architecture.md#the-spaces**: positions cross at double and
// everything else at single, so translation springs are one run and the scale, rotation-log-map,
// opacity, blur, and corner-radius springs are another. **The third run is decision 72's** — the
// driven regime is a distinct closed form, not reachable from any spring, so it travels as its own
// fixed-stride run and the spring runs stay one shape each rather than a tagged union. The waist
// reserves the slot; the record that fills it is Animation's to define when that regime is built.
enum class SnapshotRun : std::uint32_t
{
	Positions = 0,      // translation springs, at double
	Channels = 1,       // scale, rotation, opacity, blur, corner-radius springs, at single
	DrivenProgress = 2, // the driven ramp (p0, v0, t0, horizon), usually empty
};

inline constexpr std::size_t SnapshotRunCount = 3;

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
	RunEntry Wakes = {}; // the per-output wake schedule, one Wake per output
};

static_assert(std::is_trivially_copyable_v<SnapshotHeader> && std::is_standard_layout_v<SnapshotHeader>);
static_assert(sizeof(SnapshotHeader) == 88, "One uint64, four uint32, three run entries, and the wake entry, exactly");
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
