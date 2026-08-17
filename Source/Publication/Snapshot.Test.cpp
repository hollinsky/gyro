#include "Publication/Snapshot.h"

#include <cstdint>

#include "Testing/Test.h"

// The format's own arithmetic, checked where a wrong answer is silent: an alignment that does not
// align, or a bounds test that admits a run reaching past the end. The layout invariants are
// static_asserts in Snapshot.h itself; these are the parts a runtime value could get wrong.

// AlignUp rounds up to a power-of-two boundary and leaves an already-aligned offset alone.
static_assert(Detail::AlignUp(0, 8) == 0);
static_assert(Detail::AlignUp(1, 8) == 8);
static_assert(Detail::AlignUp(8, 8) == 8);
static_assert(Detail::AlignUp(9, 8) == 16);
static_assert(Detail::AlignUp(88, 8) == 88, "The header ends on an eight-boundary, so the first run needs no padding");
static_assert(Detail::AlignUp(5, 4) == 8);

// A zero-count run is vacuously within bounds — the reader reads the count first and never consults
// the offset — so its offset being nonsense does not matter.
static_assert(Detail::RunWithinBounds(RunEntry{ .Offset = 0, .Count = 0, .ElementSize = 0, .ElementAlign = 0 }, 0));
static_assert(Detail::RunWithinBounds(RunEntry{ .Offset = 999, .Count = 0, .ElementSize = 24, .ElementAlign = 8 }, 88));

// A run that ends exactly at the snapshot's end fits; one byte more does not.
static_assert(Detail::RunWithinBounds(RunEntry{ .Offset = 88, .Count = 2, .ElementSize = 24, .ElementAlign = 8 }, 136));
static_assert(
	!Detail::RunWithinBounds(RunEntry{ .Offset = 88, .Count = 2, .ElementSize = 24, .ElementAlign = 8 }, 135)
);

// An offset past the end is rejected before the span is even considered.
static_assert(
	!Detail::RunWithinBounds(RunEntry{ .Offset = 200, .Count = 1, .ElementSize = 24, .ElementAlign = 8 }, 88)
);

// The pinned schema: three runs, indexed in order, with the driven regime last.
static_assert(RunIndex(SnapshotRun::Positions) == 0);
static_assert(RunIndex(SnapshotRun::Channels) == 1);
static_assert(RunIndex(SnapshotRun::DrivenProgress) == 2);
static_assert(SnapshotRunCount == 3);

// A fresh header carries the magic and version rather than zeroes, so a value-initialised header is
// already a recognisable one before its fields are filled.
static_assert(SnapshotHeader{}.Magic == SnapshotMagic);
static_assert(SnapshotHeader{}.Version == SnapshotVersion);
static_assert(SnapshotHeader{}.ByteSize == 0);

// The bounds test computes its span in a width the 32-bit product cannot overflow. A count and size
// whose product exceeds 32 bits must not wrap to a small number and be admitted — the failure the
// widening exists to prevent, and one only a runtime value exercises.
GYRO_TEST(Snapshot, BoundsDoesNotOverflow)
{
	const RunEntry huge{ .Offset = 0, .Count = 0x1000'0000u, .ElementSize = 0x40u, .ElementAlign = 8 };

	// Count * ElementSize is 0x1000'0000 * 0x40 = 0x4'0000'0000, which truncates to zero in 32 bits and
	// would then be admitted. Taken in 64 bits it is sixteen gigabytes and fits in nothing here.
	GYRO_CHECK(!Detail::RunWithinBounds(huge, 4096));
}
