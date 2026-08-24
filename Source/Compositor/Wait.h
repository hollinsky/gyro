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
// **There is now a second thing, and it is still not enough to want a ring.** A run that hosts clients
// waits on the Wayland event loop's one descriptor as well as the stop — libwayland multiplexes every
// client socket behind that fd, so the count goes from one to two and not from one to one-per-client.
// `Watch` is where the composition root hands it over: an author that drives from a descriptor owns
// the descriptor, and the root is the only party that can put it in the set the thread actually sleeps
// on. Decision 126's threshold was a second *kind* of thing to wait on — evdev, a control socket — and
// two descriptors is not it.
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

	// Also wake when this descriptor is readable. The root's, borrowed and never closed here — it
	// belongs to whatever produced it, which today is the Wayland event loop inside the client host.
	// Called once, before the dispatch thread starts; `-1` is the ordinary case of a run with no
	// clients, and leaves the wait exactly what it was.
	//
	// **Level-triggered and deliberately not drained here**, unlike the stop. What makes it readable is
	// a client with a request pending, and what makes it unreadable again is the author reading that
	// request inside its own `Advance` — so the descriptor is the author's to quiet, and a wait that
	// tried to consume it would be reading a client's traffic on behalf of nobody.
	void Watch(int descriptor) noexcept { m_Watched = descriptor; }

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

	// Borrowed rather than owned, which is why it is a plain `int` beside an `Fd`.
	int m_Watched = -1;

	std::atomic<bool> m_Stopping{ false };
};
