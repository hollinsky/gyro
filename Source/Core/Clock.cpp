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

namespace
{

[[nodiscard]] std::int64_t Nanoseconds(const timespec& reading) noexcept
{
	return static_cast<std::int64_t>(reading.tv_sec) * 1'000'000'000 + reading.tv_nsec;
}

} // namespace

ClockAnchor ReadClockAnchor() noexcept
{
	// CLOCK_BOOTTIME is Linux's rather than POSIX's, and the portable tier is where this has to live
	// anyway — the reader is one file and this is that file. Where the platform has no such clock the
	// anchor relates the monotonic domain to itself, which is true, and leaves a trace that merges with
	// nothing rather than one that merges wrongly.
#ifdef CLOCK_BOOTTIME
	constexpr clockid_t Boottime = CLOCK_BOOTTIME;
#else
	constexpr clockid_t Boottime = CLOCK_MONOTONIC;
#endif

	timespec monotonic = {};
	timespec boottime = {};
	timespec realtime = {};

	::clock_gettime(CLOCK_MONOTONIC, &monotonic);
	::clock_gettime(Boottime, &boottime);
	::clock_gettime(CLOCK_REALTIME, &realtime);

	return ClockAnchor{ .Monotonic = Nanoseconds(monotonic),
		                .Boottime = Nanoseconds(boottime),
		                .Realtime = Nanoseconds(realtime) };
}
