#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "Core/Clock.h"
#include "Core/Time.h"
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

// One thing that was true of the run, as a name and a printed value.
//
// **Plain strings, because Trace may not learn where any of them came from.** Version, kernel, GPU,
// backend, clocksource and whether `SCHED_FIFO` was actually granted are `uname`, `/sys`, a generated
// header and a Vulkan device — none of which a `PORTABLE` module may reach, and all of which the
// composition root already holds. So the root gathers and this encodes, which is the same split
// `TraceSource` already draws around a ring.
struct TraceFact
{
	std::string_view Name;
	std::string_view Value;
};

// `uname`, as the four fields Perfetto's own `Utsname` message has. Four members rather than one
// kernel-release string because a reader that already knows this message can compare it against the
// one a system trace concatenated alongside carries.
struct TraceMachine
{
	std::string_view Sysname;
	std::string_view Release;
	std::string_view Version;
	std::string_view Machine;
};

// One line the process logged, on the timeline beside what the process was doing when it logged it.
//
// **A runtime string, which is why it does not come through a `TraceSource`.** A ring record carries a
// pointer to a string literal and a `uint64`, deliberately, so a message assembled at runtime has no
// way through it — see `Trace/Recorder.h`, which holds the bounded store these come out of. The level
// *is* a literal, so it stays a pointer.
struct TraceLine
{
	Instant Stamp{};
	const char* Level = nullptr;
	std::string_view Message;
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

	// The invocation, one element per argument. Empty is *nothing was said*, which is right for a test
	// and wrong for a file somebody opens three weeks later.
	std::span<const std::string_view> CommandLine{};

	TraceMachine Machine{};

	// **Written twice into the file, as process labels and as string annotations on one instant, and
	// the duplication is the point.** A label is what Perfetto shows against the process without a
	// person clicking anything, and it is a flat list — so `sched=fifo:20` reads there but nothing can
	// ask it for the value of `sched`. An annotation is a name and a value a tool reads back one key at
	// a time, and it is only visible once somebody finds the instant it hangs on. Neither alone is both
	// legible at a glance and machine-readable, and both together cost a few hundred bytes once per
	// snapshot against a file that is megabytes of slices.
	std::span<const TraceFact> Facts{};
};

// **Unbalanced slices at both ends of the window are the ordinary case, not a defect.** A ring that
// holds the last thirty seconds starts in the middle of whatever was running thirty seconds ago and
// ends in the middle of the iteration the snapshot interrupted. So an end with no beginning is
// dropped, and a beginning with no end is closed at the newest record in the source — which draws the
// interrupted iteration as a slice reaching the right edge, which is what it was.
//
// **The log lines are a second span rather than a fourth kind of `TraceSource`.** They carry no scope,
// no nesting and no interning — every message is unique, so a table of them would be a table with one
// entry per row — and they were written under a mutex by whichever thread logged rather than into a
// ring. What they share with a source is only the timeline.
[[nodiscard]] std::vector<std::byte>
EncodeTrace(const TraceIdentity& identity, std::span<const TraceSource> sources, std::span<const TraceLine> logs = {});
