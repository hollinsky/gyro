// clock_gettime is POSIX rather than ISO C, and glibc hides it under -std=c++NN. Requesting it in
// the translation unit keeps the file correct whether or not the build asks for GNU extensions.
#define _POSIX_C_SOURCE 200809L

#include "Core/Clock.h"

#include <time.h>

Instant MonotonicClock::Now() const noexcept
{
	// The only clock read in the process.
	//
	// Unchecked deliberately. clock_gettime fails on an invalid clock id or a bad timespec pointer;
	// CLOCK_MONOTONIC is required by POSIX and the timespec is on our own stack, so neither is
	// reachable. There is no error to handle and no value worth returning if there were.
	timespec now = {};
	::clock_gettime(CLOCK_MONOTONIC, &now);

	return Monotonic::FromNanoseconds(static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec);
}
