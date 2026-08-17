#pragma once

#include <cstdint>

// The frame path, marked so that a machine can see it.
//
// Decision 36 states the target as no operation on the frame path may take unbounded time, and then
// makes it enforceable rather than aspirational: the frame loop wraps the span from acquiring the
// published snapshot through `Present` in one of these, and a heap allocation inside that span
// aborts. Core/DebugAllocator.cpp is the half that does the aborting; this is the half that says
// where. Neither is worth anything without the other, and both are worth writing before there is
// anything to catch — written afterwards they are archaeology.
//
// **Thread-local is the operative word, not an implementation detail.** The dispatch thread
// allocates constantly and must — per-message allocation, shm mapping, dmabuf import are exactly
// what decision 45 moved onto it — so this can never be a process-wide ban. A design that reaches
// for one has misunderstood which side of the boundary is being defended.
//
// **Why mechanical.** The allocation is essentially never a literal `new` that a reviewer would
// spot. It is a `std::vector` growing past its reserve, a `std::function` whose capture crossed the
// small-object threshold, a `std::string` built for a log line, a `std::unordered_map` rehashing.
// Every one of those diffs looks reasonable in isolation, which is the same failure mode the motion
// catalog exists to prevent, one layer down.
//
// **What it does not catch**, because two of these are load-bearing elsewhere in the design:
//
//   `free` running wherever the last reference dies. `operator delete` is hooked too — it is free
//   to hook and a `free()` on the frame path is the same arena lock as a `malloc()` — but a
//   `shared_ptr` crossing the publication boundary releases its control block on whichever thread
//   drops the last reference, which may be dispatch on the run that was tested and the frame thread
//   on the run that shipped. This check is deterministic; shared ownership is probabilistic. That
//   is precisely why the design forbids it structurally, with deferred reclamation against a
//   consumed-sequence watermark, rather than relying on this to find it.
//
//   Anything that is not the C++ heap. First-touch page faults (which is why the frame thread's
//   working set is the `mlockall` target rather than the process), `vkCreateImage` and
//   `vkAllocateMemory` (why the snapshot atlas is reserved at output configuration), blocking
//   syscalls, and file I/O (why spdlog must be on an async sink).
//
//   `malloc` called from C. Replacing `operator new` does not reach the Mesa or driver allocations
//   inside `vkQueueSubmit`. Interposing `malloc` itself would, and aborting on a driver's
//   allocation is not actionable — if that is ever wanted, it should be wanted as measurement
//   rather than as a check.
//
// There is deliberately no escape hatch. If one ever becomes necessary it should look like
// `Motion::Custom` does: a single named thing, greppable in one command and obvious in review.

namespace Detail
{
// Unconditional, and not gated on the check being compiled in. It is one thread-local increment per
// frame section — once per frame per output, not once per operation — so there is no budget
// argument to have, and gating it would make `OnFramePath` answer false in Release for code that
// reasonably asks.
inline thread_local std::uint32_t FrameSectionDepth = 0;
} // namespace Detail

// The predicate the debug allocator asks, exposed because anything else wanting to assert "not from
// here" should ask the same question rather than grow a second notion of where the frame path is.
[[nodiscard]] inline bool OnFramePath() noexcept
{
	return Detail::FrameSectionDepth != 0;
}

// A depth counter rather than a flag, so that the guard nests — the frame loop's section will
// eventually contain narrower ones — and so that it is exception-safe by construction.
class FrameSection
{
public:
	FrameSection() noexcept { ++Detail::FrameSectionDepth; }
	~FrameSection() noexcept { --Detail::FrameSectionDepth; }

	FrameSection(const FrameSection&) = delete;
	FrameSection& operator=(const FrameSection&) = delete;
};
