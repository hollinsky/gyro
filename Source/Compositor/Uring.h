#pragma once

#include <liburing.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Wake.h"
#include "Seam/EventSource.h"

// The wait, and nothing else.
//
// Decision 80 gives this its whole contract: *wake at or after this instant, or earlier when a
// registered descriptor is readable.* It decides nothing — it is handed a `Wake` and it is permitted
// to return early — which is what lets the loop above it be portable and be the loop the
// schedulability sweep runs. Every ordering that matters is inside `FrameLoop::Step`, on purpose.
//
// **`DEFER_TASKRUN` is the entire reason this is io_uring rather than a timerfd and `ppoll`.**
// Docs/Architecture.md#why-io_uring is explicit that the argument is jitter and not throughput:
// completion work runs where gyro asks for it instead of preempting mid-record. Two things follow and
// both are load-bearing here rather than elsewhere.
//
// The first is that `DEFER_TASKRUN` defers only while the ring is reaped through `io_uring_enter` with
// `IORING_ENTER_GETEVENTS`. Reading the completion tail directly does not fail — it silently stops
// deferring, which withdraws the property the flag was bought for, permanently and invisibly. So this
// file has exactly one reap site, it is `Reap` below, and it is reached only from `WaitFor`, which has
// always entered the kernel first. liburing's `io_uring_peek_cqe` is safe to drain *with* by the same
// argument — it consults `IORING_SQ_TASKRUN` and enters when it has to — but it is safe only after
// something has entered, which is why the two are not separable calls.
//
// The second is that the flags are a *ring configuration* rather than a kernel version.
// Distributions backport them into kernels that predate them and `io_uring_disabled` subtracts them
// from kernels that have them, so this attempts the configuration and reports which half failed. gyro
// carries no epoll fallback, deliberately — Docs/Architecture.md#dependencies has the argument, which
// is the floor tier's — so what it owes instead is a sentence somebody can act on, on a machine with
// no VT to read a backtrace from.

// What breaks the wait from another thread.
//
// It is an `IEventSource` rather than a mechanism the shim knows about, which is decision 83's shape
// arriving one user early: dispatch's publication has to be able to reach a frame thread that folded
// to idle, and making that a source like any other is what keeps `Step`'s signature from growing a
// case for it. Shutdown is the same problem and gets the same answer, so the machinery is written
// once and the publication nudge is a second instance rather than a second mechanism.
class Interrupt final : public IEventSource
{
public:
	Interrupt() = default;

	~Interrupt() override = default;

	[[nodiscard]] Result<void> Open() noexcept;

	[[nodiscard]] RawFd Descriptor() const noexcept override { return m_Fd.Borrow(); }

	// Frame thread. Reads to empty, per `IEventSource`'s contract, because the ring polls
	// level-triggered and an eventfd left readable is a spin rather than a lost event.
	Result<void> Drain() override;

	// Any thread. The store is released before the write so that a frame thread woken by the descriptor
	// cannot fail to see the reason it was woken.
	void Raise() noexcept;

	[[nodiscard]] bool IsRaised() const noexcept { return m_Raised.load(std::memory_order_acquire); }

private:
	Fd m_Fd;
	std::atomic<bool> m_Raised{ false };
};

class FrameRing
{
public:
	FrameRing() = default;

	~FrameRing();

	// Neither copied nor moved. The ring holds pointers into its own mmapped regions and the poll
	// submissions hold indices into this object, so an address that changes is a submission that
	// completes into a corpse. The composition root constructs it in place, like everything else it
	// owns whose identity matters.
	FrameRing(const FrameRing&) = delete;
	FrameRing& operator=(const FrameRing&) = delete;
	FrameRing(FrameRing&&) = delete;
	FrameRing& operator=(FrameRing&&) = delete;

	// Attempts the one configuration gyro asks for, and names the half that failed if it cannot be had.
	[[nodiscard]] Result<void> Open(unsigned entries = DefaultEntries);

	// Arm a multishot poll for one source's descriptor. A source with no descriptor is accepted and
	// registers nothing — Seam/EventSource.h makes that an ordinary answer rather than a failure, and
	// the headless backend is the case: its flips are a function of the clock, so the timeout below is
	// the only thing that ever wakes this ring.
	[[nodiscard]] Result<void> Watch(const IEventSource& source);

	// Wait for the wake, or for any watched descriptor, whichever comes first. Returns once something
	// has happened; it is permitted to return before either.
	[[nodiscard]] Result<void> WaitFor(Wake wake);

	// How many times the wait has returned with nothing to show for it. A diagnostic rather than a
	// control input: the contract permits it, and a count that climbs is how a wasteful arming pattern
	// is noticed at all.
	[[nodiscard]] std::uint64_t Spurious() const noexcept { return m_Spurious; }

	// Descriptors whose poll reported an error and were given up on. Nothing is lost by it that the
	// loop does not already recover — decision 80 has every source drained on every iteration whatever
	// woke it, so a broken poll costs the *wakeup* and never the event, and the timer still runs. It is
	// counted rather than retried because a poll that errored once errors again, and the retry is a
	// spin.
	[[nodiscard]] std::uint64_t Broken() const noexcept { return m_Broken; }

	static constexpr unsigned DefaultEntries = 64;

private:
	// The kinds of completion this ring can see, in the top byte of `user_data` so that a stale one is
	// identifiable rather than merely unexpected.
	enum class Tag : std::uint8_t
	{
		Timeout = 1,
		Poll = 2,
		Cancel = 3,
	};

	[[nodiscard]] static constexpr std::uint64_t Encode(Tag tag, std::uint64_t value) noexcept
	{
		return (static_cast<std::uint64_t>(tag) << 56) | (value & 0x00ff'ffff'ffff'ffffULL);
	}

	[[nodiscard]] static constexpr Tag KindOf(std::uint64_t data) noexcept { return static_cast<Tag>(data >> 56); }

	[[nodiscard]] static constexpr std::uint64_t ValueOf(std::uint64_t data) noexcept
	{
		return data & 0x00ff'ffff'ffff'ffffULL;
	}

	// A watched descriptor and the poll that is standing on it. The generation is what a re-arm bumps,
	// so that a completion for a poll this object has already replaced is discarded rather than counted.
	struct Watched
	{
		RawFd Descriptor{};
		std::uint64_t Generation = 0;
		bool Armed = false;
	};

	[[nodiscard]] Result<io_uring_sqe*> Sqe();

	[[nodiscard]] Result<void> Arm(std::size_t index);

	// The one reap site. Returns whether the armed timeout was among the completions, which is what
	// decides whether it still has to be cancelled.
	// What one enter produced, in the terms the caller asked in. The distinction is load-bearing rather
	// than descriptive: see `WaitFor`, where a wait satisfied by neither is a wait that has not happened.
	struct Reaped
	{
		// This iteration's timeout, by tag. A tag from an earlier one is a timeout cancelled too late to
		// stop and is not this.
		bool Fired = false;

		// A watched descriptor had something to say.
		bool Woke = false;
	};

	Reaped Reap(std::uint64_t timeout);

	void Cancel(std::uint64_t timeout);

	// SPEC: sized rather than measured, like the loop's own capacities. One source per rendering device
	// plus dispatch's is the shape decision 80 describes; eight is well past any machine gyro has been
	// pointed at, and the frame section forbids growing it.
	static constexpr std::size_t MaxWatched = 8;

	io_uring m_Ring{};
	bool m_Open = false;

	std::array<Watched, MaxWatched> m_Watched{};
	std::size_t m_WatchedCount = 0;

	std::uint64_t m_Timeouts = 0;
	std::uint64_t m_Spurious = 0;
	std::uint64_t m_Broken = 0;
};
