#pragma once

#include <array>
#include <atomic>
#include <cstddef>
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
// **Input is that second kind, and it is still one more `pollfd`.** libinput hands over one descriptor
// for the whole seat, so a run driving a panel with clients on it waits on three files rather than
// one-per-device, and what a ring would multiplex is a set small enough to name. What would actually
// change the answer is a set whose *size* is not known when the thread starts — a control socket per
// connected agent — because that is where an array stops being a wait and starts being a registry.
//
// **The timeout is relative, which is the opposite of what Compositor/Uring.h argues for, and the
// difference between the two threads is the argument.** The frame ring uses `IORING_TIMEOUT_ABS`
// because a preemption between reading the clock and entering the kernel lands on the far side of a
// vblank, and a late frame is the one error that contract does not permit. Dispatch has no deadline —
// Dispatch/Loop.h says so in its first line — and a wake served late there costs the *first frame or
// two* of a motion and never its shape, because
// [decision 89](../../Docs/Decisions.md#89-a-commit-resolves-in-two-phases-a-change-becomes-motion-where-its-inputs-are-complete)
// stamps a retarget with the instant it fell due rather than with now. So the animation renders already
// in progress by exactly the lateness, which is what `ISceneAuthor::Advance` is documented to want.
//
// **Stopping is a sticky flag, and the counter is drained because a second party now writes to it.**
// `Nudge` is the frame thread's doorbell — the return channel carries no descriptor, so a report with a
// presented frame in it reaches a sleeping dispatch thread only if something writes here — and a
// counter left readable after one of those would spin the thread for the rest of the run. Draining
// cannot lose a stop: `Stop` releases the flag *before* the write, and every caller re-checks the flag
// rather than asking which descriptor returned, so a wait that consumed the stop's own write still
// finds the reason it was woken.
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
	// belongs to whatever produced it, which today is the Wayland event loop inside the client host or
	// the libinput context beside it. Called once per descriptor, before the dispatch thread starts;
	// `-1` is the ordinary case of a run with neither, and leaves the wait exactly what it was.
	//
	// **Bounded at two, which is what the run with everything in it needs**, and stated as a capacity
	// rather than grown to a vector because the next descriptor after these is the one that reopens
	// decision 126's question rather than one more entry in an array.
	//
	// **Level-triggered and deliberately not drained here**, unlike the stop. What makes it readable is
	// a client with a request pending, and what makes it unreadable again is the author reading that
	// request inside its own `Advance` — so the descriptor is the author's to quiet, and a wait that
	// tried to consume it would be reading a client's traffic on behalf of nobody.
	void Watch(int descriptor) noexcept
	{
		if (descriptor >= 0 && m_Watching < m_Watched.size())
		{
			m_Watched[m_Watching] = descriptor;
			++m_Watching;
		}
	}

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

	// Frame thread. Wake the dispatch thread because a frame reached the glass and somebody is waiting
	// to hear about it.
	//
	// **This is the doorbell the return channel does not have**, and it is the mirror of decision 83's
	// on the forward one: that one exists because the frame thread had nothing to wake it when a
	// snapshot arrived, and this exists because the dispatch thread has nothing to wake it when a report
	// does. Without it a settled world is a deadlock rather than a delay — dispatch arms no deadline, so
	// the only thing that would wake it is the client acting on a frame callback that is sitting behind
	// a drain nobody is going to run.
	//
	// **The root rings it only when a client is actually owed something**, which is what keeps
	// Docs/Architecture.md#doing-nothing-must-cost-nothing true: an idle desktop presents nothing and a
	// desktop presenting for its own sake — a splash, a console — has nothing in the ledger, so the
	// syscall is not spent at all. Ringing it unconditionally would run the dispatch loop at panel rate
	// for the length of every run.
	void Nudge() noexcept;

	[[nodiscard]] bool IsStopping() const noexcept { return m_Stopping.load(std::memory_order_acquire); }

private:
	Fd m_Fd;

	// Borrowed rather than owned, which is why they are plain `int`s beside an `Fd`.
	std::array<int, 2> m_Watched{ -1, -1 };
	std::size_t m_Watching = 0;

	std::atomic<bool> m_Stopping{ false };
};
