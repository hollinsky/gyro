#pragma once

#include <pthread.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>

#include "Core/Fd.h"
#include "Core/Time.h"
#include "Seam/RenderTarget.h"

// The thread that issues the atomic commit, and the one-slot mailbox the frame thread hands it
// through.
//
// **Why the ioctl leaves the frame thread at all.** A non-blocking atomic commit returns having
// *queued* the work rather than done it: `intel_atomic_commit` hands the state to a worker on
// `display->wq.flip`, and that worker is what waits on the in-fence, evades the vblank and writes the
// registers that arm the flip. `wq.flip` is `WQ_HIGHPRI`, which is nice -20 and still `SCHED_OTHER`,
// so the last few hundred microseconds between a composited frame and the glass run at a priority gyro
// cannot raise, pin or measure — and a `SCHED_FIFO` thread preempts that worker, which means gyro's own
// real-time priority can delay the work arming gyro's own flip. Measured on an i915 Tiger Lake: 288 to
// 387 microseconds of commit path on an idle machine, against 2.67% of commits missing their vblank
// under a parallel kernel build, in bursts, with the composite's fence signalled four to seven times
// the idle cost ahead of the deadline. On the system's only compositor a build is Tuesday, and what a
// person sees is the pointer stuttering while they compile.
//
// **The blocking commit is the fix, and it is the kernel's own escape hatch rather than a workaround.**
// The same function, four lines down: `} else { ... intel_atomic_commit_tail(state); }`. Drop
// `DRM_MODE_ATOMIC_NONBLOCK` and the fence wait, the vblank evasion and the register writes all run on
// the thread that made the ioctl. That is the generic helper's shape rather than an i915 courtesy —
// `drm_mode_atomic_ioctl` calls `drm_atomic_commit` where it would have called
// `drm_atomic_nonblocking_commit` — so it holds across drivers. There is no kworker left to invert
// against, because there is no handoff.
//
// **What it costs is that the ioctl does not return until the flip is done**, which is why this is a
// thread rather than a flag. The frame thread may not sit in a syscall for a refresh; this thread has
// nothing else to do for that refresh anyway, because KMS refuses a second commit on a CRTC that has
// not flipped. One thread per output, for that reason exactly: the rule is per CRTC, and a thread
// shared between two panels would put one panel's flip-done wait in front of the other panel's latch.
//
// **The locks the blocking ioctl holds are the ones it acquired, and they are per object.**
// `drm_mode_atomic_ioctl` drops its `drm_modeset_acquire_ctx` after `drm_atomic_commit` returns rather
// than after the swap, so a blocking commit does hold locks for the whole flip — but they are taken
// lazily per object touched, and a plane-only flip on one CRTC touches that CRTC and its planes. Two
// panels take disjoint sets and do not serialize. What can still collide is a driver's *global* state —
// i915's bandwidth and cdclk — and that already collided on the frame thread, because `atomic_check`
// runs synchronously on the caller whether or not `NONBLOCK` is set. This moves it; it does not add it.
//
// **The completion path is untouched.** `DRM_MODE_PAGE_FLIP_EVENT` still rides the commit, the event
// still arrives on the card's descriptor, and the device's drain still turns it into `Presented`. The
// ioctl's *return* is used for exactly two things: freeing the slot, and reporting a commit that
// failed — which cannot arrive as a `Present` failure any more, because `Present` has already returned.
// It arrives as `Missed` instead, which Seam/Presenter.h built for exactly this: a frame the host
// accepted and will never show.

namespace Drm
{
// Everything one atomic commit is, copied out of the output so the frame thread may write its own
// arrays again the moment it has armed.
//
// **A copy rather than a pointer to the output's arrays**, which is the race that is easy to miss:
// `TestLayers` fills the same arrays from the frame thread, and it is called on the iteration *after* a
// commit whose ioctl has not returned yet — the flip event arrives before the tail finishes. A few
// hundred bytes of `memcpy` inside the frame section costs nothing and allocates nothing; sharing the
// arrays would put a `DRM_MODE_ATOMIC_TEST_ONLY` under the kernel's feet mid-flip.
struct CommitRequest
{
	static constexpr std::size_t PropertiesPerPlane = 11;
	static constexpr std::size_t MaxProperties = MaxLayers * PropertiesPerPlane;

	std::array<std::uint32_t, MaxLayers> Objects{};
	std::array<std::uint32_t, MaxLayers> Counts{};
	std::array<std::uint32_t, MaxProperties> Properties{};
	std::array<std::uint64_t, MaxProperties> Values{};

	std::uint32_t ObjectCount = 0;
	std::uint32_t Flags = 0;

	// The exported in-fences the values above name by number. They ride here because a raw fd in
	// `Values` is only valid while something holds the descriptor open, and the frame thread's stack
	// stopped being that something the moment the ioctl moved to another thread.
	std::array<Fd, MaxLayers> Fences{};
};

// What the commit thread did with the request: the errno the ioctl returned, and how long it spent in
// it.
//
// **The duration is the measurement the kernel does not offer.** KernelWishlist.md's weakest of three
// asks is *a way to read how long the commit path actually took*, so that the 500 microseconds
// Drm/Output.cpp compiles in could be measured rather than inferred from dropped frames. A blocking
// ioctl is that instrument for free: the bracket around it is the path. It is the whole flip rather
// than the arming alone — the tail waits for the vblank before it returns — so what it bounds is the
// path plus the wait, and it is read as *the commit was still running at this point* rather than as the
// latch lead itself.
struct CommitOutcome
{
	int Error = 0;
	Duration Elapsed{};
};

// What a request is handed to. A function pointer and a context rather than a `std::function`, because
// the frame thread must not allocate to arm one and because the test needs an issuer that is not an
// ioctl.
using CommitIssuer = int (*)(void* context, const CommitRequest& request) noexcept;

// One output's commit thread and the single slot it is fed through.
//
// **One slot, and it is the hardware's rule restated rather than a simplification.** KMS refuses a
// second commit on a CRTC that has not flipped, which is what `IPresenter::CommitDepth`'s default of one
// already holds the loop to. A queue here would be a queue of commits the kernel would refuse.
class CommitThread
{
public:
	CommitThread() = default;

	~CommitThread();

	CommitThread(const CommitThread&) = delete;
	CommitThread& operator=(const CommitThread&) = delete;
	CommitThread(CommitThread&&) = delete;
	CommitThread& operator=(CommitThread&&) = delete;

	// Start the thread. Off the frame path — an output opens before the frame thread exists — which is
	// what makes the one allocation in this file legal.
	void Start(CommitIssuer issuer, void* context);

	[[nodiscard]] bool IsRunning() const noexcept { return m_Thread.joinable(); }

	// Whether the slot is free. The frame thread's own question before it fills one.
	[[nodiscard]] bool IsIdle() const noexcept { return m_State.load(std::memory_order_acquire) == State::Idle; }

	// Whether the commit has finished and nobody has read the outcome.
	[[nodiscard]] bool IsComplete() const noexcept { return m_State.load(std::memory_order_acquire) == State::Done; }

	// Fill the slot and wake the thread. The frame thread's whole cost, and it is one atomic store plus
	// a futex wake — no allocation, no lock, and nothing that can block on the commit thread.
	//
	// The request is moved because it carries descriptors. Returns false where the slot was not free,
	// which the caller has already refused on and which is checked here so that the rule cannot be broken
	// silently.
	bool Arm(CommitRequest&& request) noexcept;

	// Take the outcome and free the slot, or nothing where the commit is still running. The frame
	// thread's, and the only reader of it.
	[[nodiscard]] std::optional<CommitOutcome> Reap() noexcept;

	// When a commit was armed. What Drm/Output.h's backstop is measured from.
	[[nodiscard]] Instant ArmedAt() const noexcept { return m_ArmedAt; }

	// Stop the thread and join it. Idempotent, and called off the frame path.
	void Stop() noexcept;

private:
	enum class State : std::uint32_t
	{
		Idle,

		// Filled by the frame thread and not yet picked up.
		Armed,

		// In the ioctl.
		Running,

		// Out of the ioctl, with an outcome nobody has read.
		Done,

		Stopping,
	};

	void Run() noexcept;

	// Take the priority of the thread that armed this one, plus one, and only where that thread is
	// real-time.
	//
	// **One above whoever drives it, rather than a number plumbed through from the composition root.**
	// The tail this thread runs is the most deadline-critical work in the process and it is what the
	// frame thread is waiting on; below the frame thread it would rebuild the inversion this file exists
	// to remove, with gyro's own composite in the kworker's role. Above it costs the composite the
	// microseconds of preemption where the two collide, which is bounded and which the budget already
	// covers. Expressed as a *relationship* so that `Compositor/RealTime.h` stays the one place a
	// priority is chosen, and so that a run without the privilege to set one — a test, a nested session,
	// a developer's shell — simply does nothing here.
	void Inherit() noexcept;

	std::atomic<State> m_State{ State::Idle };

	// Shutdown, held apart from the state because a commit already in the ioctl publishes its outcome
	// into the state on the way out and would overwrite a stop written there.
	std::atomic<bool> m_Stopping{ false };

	CommitRequest m_Request;
	CommitOutcome m_Outcome;

	CommitIssuer m_Issuer = nullptr;
	void* m_Context = nullptr;

	// The thread that armed the current commit, captured so this one can read its scheduling parameters.
	// Stored by the frame thread inside the frame section, which is why it is a plain register store.
	std::atomic<pthread_t> m_Armer{};
	bool m_Inherited = false;

	Instant m_ArmedAt{};

	std::thread m_Thread;
};
} // namespace Drm
