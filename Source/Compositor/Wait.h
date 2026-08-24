#pragma once

#include <atomic>
#include <optional>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Time.h"

// What the dispatch thread blocks on, and the argument for it not being a ring.
//
// Docs/Architecture.md#the-dispatch-loop gives that thread its own io_uring, and it will have one: the
// loop it describes waits on client sockets, evdev, and a control socket, and round-robins demarshalling
// across them. None of those exist. What waits here today is a gym and a retry timer, so the whole of
// what a ring would multiplex is one descriptor and one deadline — and Architecture.md's own sentence on
// the subject is that on the dispatch ring io_uring is a convenience and plain epoll would be defensible.
// A `ppoll` is that sentence taken at its word until there is a second thing to wait on.
//
// **The timeout is relative, which is the opposite of what Compositor/Uring.h argues for, and the
// difference between the two threads is the argument.** The frame ring uses `IORING_TIMEOUT_ABS`
// because a preemption between reading the clock and entering the kernel lands on the far side of a
// vblank, and a late frame is the one error that contract does not permit. Dispatch has no deadline —
// Dispatch/Loop.h says so in its first line — and a wake served late there costs the *first frame or
// two* of a motion and never its shape, because
// [decision 89](../../Docs/Decisions.md#89-a-model-value-is-set-under-a-shared-origin-and-setting-it-is-a-retarget)
// stamps a retarget with the instant it fell due rather than with now. So the animation renders already
// in progress by exactly the lateness, which is what `ISceneAuthor::Advance` is documented to want.
//
// **Stopping is a sticky flag and a counter nothing drains**, which is deliberate. `Interrupt` in
// Compositor/Uring.h drains because the ring polls level-triggered every iteration and an eventfd left
// readable is a spin; here the only write is the last one this object will ever see, so leaving it
// readable is what makes every subsequent wait return at once instead of parking a thread the root is
// waiting to join.
class DispatchWait
{
public:
	DispatchWait() = default;

	~DispatchWait() = default;

	// Neither copied nor moved: the root constructs it in place and two threads hold its address.
	DispatchWait(const DispatchWait&) = delete;
	DispatchWait& operator=(const DispatchWait&) = delete;
	DispatchWait(DispatchWait&&) = delete;
	DispatchWait& operator=(DispatchWait&&) = delete;

	[[nodiscard]] Result<void> Open() noexcept;

	// Dispatch thread. Blocks until the deadline falls due or the wait is stopped, whichever is first;
	// `std::nullopt` is *the world has stopped changing*, and blocks until stopped. `now` is passed in
	// rather than read because Core/Clock.cpp is the timebase's one reader.
	//
	// Permitted to return early, exactly as the frame ring's wait is: the caller re-checks the stop flag
	// and steps again, and a step with nothing due publishes the same scene under the same sequence.
	[[nodiscard]] Result<void> WaitUntil(std::optional<Instant> deadline, Instant now) noexcept;

	// Any thread. The store is released before the write so that a dispatch thread woken by the
	// descriptor cannot fail to see the reason it was woken — Interrupt::Raise's ordering, for
	// Interrupt::Raise's reason.
	void Stop() noexcept;

	[[nodiscard]] bool IsStopping() const noexcept { return m_Stopping.load(std::memory_order_acquire); }

private:
	Fd m_Fd;
	std::atomic<bool> m_Stopping{ false };
};
