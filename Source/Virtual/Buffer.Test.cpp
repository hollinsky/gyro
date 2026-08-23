#include "Virtual/Buffer.h"

#include "Seam/Buffer.h"
#include "Testing/Test.h"

// The one half of a buffer that stayed platform when decision 120 moved the rest to the waist: the
// dma-buf sync bracket. Seam/Buffer.Test.cpp has the ownership rules and the description.

GYRO_TEST(Buffer, CpuReadOnAnUnmappedBufferIsEmptyRatherThanUnsafe)
{
	const DmabufBuffer buffer;
	const DmabufRead read{ buffer };

	// An allocator that could not map hands back a buffer with no mapping, and a consumer that asks
	// to look must get nothing rather than a null span with a length on it.
	GYRO_CHECK(read.Bytes().empty());
}
