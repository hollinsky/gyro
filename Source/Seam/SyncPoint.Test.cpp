#include "Seam/SyncPoint.h"

#include <format>
#include <string>

#include "Core/Fd.h"
#include "Testing/Test.h"

// Most of this type's contract is in static_asserts beside it, where a violation is a compile error
// rather than a test run. What is left for here is the part a person reads in a log and the part that
// would be a silent hazard: a point that names no timeline is *immediate*, not *unset*, and the two
// spellings of it must be one value.

GYRO_TEST(SyncPoint, TheDefaultIsImmediateRatherThanUnset)
{
	GYRO_CHECK(SyncPoint{}.IsImmediate());
	GYRO_CHECK_EQ(SyncPoint{}, SyncPoint::Immediate());
	GYRO_CHECK_EQ(SyncPoint::Immediate().Value, std::uint64_t{ 0 });
}

// A borrowed descriptor of zero is a perfectly ordinary descriptor, which is why Core/Fd.h refuses to
// give RawFd an operator bool. A sync point on it is a real wait and must not read as immediate.
GYRO_TEST(SyncPoint, DescriptorZeroIsATimeline)
{
	const SyncPoint point{ RawFd{ 0 }, 1 };

	GYRO_CHECK(!point.IsImmediate());
}

GYRO_TEST(SyncPoint, FormatsForALog)
{
	GYRO_CHECK_EQ(std::format("{}", SyncPoint::Immediate()), std::string{ "sync immediate" });
	GYRO_CHECK_EQ(std::format("{}", SyncPoint{ RawFd{ 7 }, 42 }), std::string{ "sync fd 7 @ 42" });
}
