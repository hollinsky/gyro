#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "Core/Clock.h"
#include "Core/Trace.h"

// Core/Trace.h's records as a file Perfetto opens.
//
// **The format is Perfetto's protobuf trace rather than Chrome's JSON**, and the reason is not
// fidelity — the JSON format has slices, counters, and flows too, and it would have been a tenth of
// this module. It is that a `.pftrace` is a concatenation of self-delimiting packets, so gyro's trace
// and a system trace taken over the same run are merged by `cat`. What that buys is the half of the
// picture gyro cannot see about itself: which thread the kernel ran instead of the frame thread, when
// the io_uring timeout actually fired, and when the GPU scheduler started the batch that was submitted
// three slices ago. Decision 139.
//
// **Timestamps are `CLOCK_MONOTONIC` nanoseconds and the trace carries the one snapshot that makes
// them readable.** ftrace stamps in the kernel's boot-time domain, which counts through suspend where
// the monotonic one does not, so a reader given monotonic timestamps and no relation drops every
// packet — which is what it did, and is the reason `Core/Clock.h` grew `ReadClockAnchor`. The
// relation is one pair of readings and it is emitted once, at the front of the file.

// One recorded thread. The events are the caller's copy of a ring, oldest first.
struct TraceSource
{
	std::string_view Name;

	// The kernel's thread id, so that a slice lands on the *same row* as that thread's scheduling in a
	// merged system trace rather than on a row beside it. Zero where the caller has none, which costs
	// the merge and nothing else.
	std::int32_t Tid = 0;

	std::span<const TraceEvent> Events;
};

struct TraceIdentity
{
	std::int32_t Pid = 0;
	std::string_view Name = "gyro";

	// The two domains read together, which is what lets this trace be concatenated with one `traced`
	// took over the same run. A default-constructed anchor is *no relation offered*, and the trace then
	// says nothing about which clock it stamped in — which is correct for a test and wrong for a file
	// anybody opens.
	ClockAnchor Anchor{};
};

// **Unbalanced slices at both ends of the window are the ordinary case, not a defect.** A ring that
// holds the last thirty seconds starts in the middle of whatever was running thirty seconds ago and
// ends in the middle of the iteration the snapshot interrupted. So an end with no beginning is
// dropped, and a beginning with no end is closed at the newest record in the source — which draws the
// interrupted iteration as a slice reaching the right edge, which is what it was.
[[nodiscard]] std::vector<std::byte> EncodeTrace(const TraceIdentity& identity, std::span<const TraceSource> sources);
