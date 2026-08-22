#pragma once

#include "Core/Result.h"
#include "Core/Time.h"

// What makes the frame thread a real-time thread, and what stops it taking the machine with it.
//
// Three things, applied in one place because they are one decision: `SCHED_FIFO` so the frame thread
// outranks dispatch and is not preempted by it, `mlockall` so a page fault is not a missed frame, and
// `RLIMIT_RTTIME` so a spinning real-time thread is killed rather than left holding a machine with
// [no VT](Docs/Architecture.md#boot-and-the-display-lifetime) to escape to.
//
// **The limit is the half that is not optional.** Docs/Architecture.md#dependencies has it on the list
// of things to build from the first thread, and the reason is asymmetric: the priority is a
// performance property and the limit is the only thing between a defect and a machine somebody has to
// power-cycle. It is also only half the cover — a frame thread *blocked* on a driver lock accrues no
// real-time budget and trips nothing, which is what `sysrq` is for and is not gyro's to fix.
//
// **Every one of these is refusable, and none of them is fatal.** They need `LimitRTPRIO=` and
// `LimitMEMLOCK=` from the service manager, or capabilities; a developer running the binary from a
// shell has neither, and a compositor that declined to start there would be one nobody could work on.
// So the failure is reported and the caller decides — which under headless and nested is *do not ask
// in the first place*, per Docs/Architecture.md#backends.

struct RealTimePolicy
{
	// `SCHED_FIFO` priority. Below the kernel's threaded IRQ handlers, which is where a userspace
	// real-time thread belongs: gyro outranking the interrupt that delivers its own vblank would be a
	// priority inversion with the hardware.
	int Priority = 50;

	// `RLIMIT_RTTIME`'s soft limit — CPU time consumed without a blocking call, after which `SIGXCPU`
	// arrives. The frame thread blocks on the ring every iteration, so the counter resets every frame
	// and anything approaching this figure is a spin rather than an expensive frame.
	Duration Warn = std::chrono::milliseconds{ 200 };

	// The hard limit, where the kernel sends `SIGKILL`. Reachable only by a thread that ignored the
	// signal above, which is to say by one that is not running gyro's code any more.
	Duration Kill = std::chrono::seconds{ 1 };
};

// Process-wide, and called once before any thread is started. `RLIMIT_RTTIME` is set on the process
// and enforced per thread, so the ordering that matters is *before the thread exists* rather than
// on it.
[[nodiscard]] Result<void> ReserveRealTime(const RealTimePolicy& policy);

// Page residency, process-wide. Separate from the call above because it is the one of the three that
// is worth having even when the priority was refused: a normal-priority frame thread that does not
// fault is still better than one that does.
[[nodiscard]] Result<void> LockMemory();

// The calling thread. This is the frame thread's own call and it is made from inside the thread,
// because that is the only place its identity is unambiguous.
[[nodiscard]] Result<void> PromoteToRealTime(const RealTimePolicy& policy);
