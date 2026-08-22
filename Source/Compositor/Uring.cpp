#include "Compositor/Uring.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

namespace
{

// `IORING_SETUP_DEFER_TASKRUN` is rejected without `IORING_SETUP_SINGLE_ISSUER`, so the requirement is
// one configuration and the diagnostic has to take it apart to say which half is missing.
constexpr unsigned WantedFlags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

[[nodiscard]] bool RingConfigurationWorks(unsigned entries, unsigned flags) noexcept
{
	io_uring ring{};
	io_uring_params params{};
	params.flags = flags;

	if (io_uring_queue_init_params(entries, &ring, &params) < 0)
	{
		return false;
	}

	io_uring_queue_exit(&ring);

	return true;
}

// 0 enabled, 1 privileged only, 2 off entirely; -1 where the knob does not exist, which is a kernel
// predating it rather than one refusing.
[[nodiscard]] int Disabled() noexcept
{
	int value = -1;

	if (FILE* const sysctl = std::fopen("/proc/sys/kernel/io_uring_disabled", "r"))
	{
		if (std::fscanf(sysctl, "%d", &value) != 1)
		{
			value = -1;
		}

		(void)std::fclose(sysctl);
	}

	return value;
}

// Which sentence to hand back when the wanted configuration could not be had. Every one of them names
// something a person can go and change, which is the whole obligation gyro takes on by carrying no
// epoll fallback.
[[nodiscard]] std::string_view Diagnose(unsigned entries) noexcept
{
	switch (Disabled())
	{
		case 2:
			return "io_uring is off: kernel.io_uring_disabled is 2";
		case 1:
			return "io_uring is restricted to privileged processes: kernel.io_uring_disabled is 1";
		default:
			break;
	}

	if (!RingConfigurationWorks(entries, 0))
	{
		return "io_uring is unavailable: a plain ring could not be created either";
	}

	if (!RingConfigurationWorks(entries, IORING_SETUP_SINGLE_ISSUER))
	{
		return "this kernel has no IORING_SETUP_SINGLE_ISSUER, which the frame ring requires";
	}

	return "this kernel has no IORING_SETUP_DEFER_TASKRUN, which the frame ring requires";
}

} // namespace

Result<void> Interrupt::Open() noexcept
{
	// Non-blocking because `Drain` reads to empty and the last read of an empty eventfd is the one that
	// has to fail rather than park the frame thread.
	const int descriptor = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

	if (descriptor < 0)
	{
		return Failure(errno, "opening the frame thread's interrupt");
	}

	m_Fd = Fd{ descriptor };

	return {};
}

Result<void> Interrupt::Drain()
{
	if (!m_Fd.IsValid())
	{
		return {};
	}

	// An eventfd counter is a single 8-byte read that zeroes it, so this reads once and then confirms
	// it is empty. The loop is there because a signal can interrupt the read, not because a second
	// value can be waiting.
	while (true)
	{
		std::uint64_t count = 0;
		const ssize_t read = ::read(m_Fd.Get(), &count, sizeof(count));

		if (read == sizeof(count))
		{
			continue;
		}

		if (read < 0 && errno == EINTR)
		{
			continue;
		}

		if (read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		{
			return {};
		}

		return Failure(read < 0 ? errno : EIO, "draining the frame thread's interrupt");
	}
}

void Interrupt::Raise() noexcept
{
	// Before the write, so that a frame thread woken by the descriptor and asking why cannot see the
	// wakeup without seeing the reason. The write is what makes the wait return; the store is what
	// makes the answer available.
	m_Raised.store(true, std::memory_order_release);

	if (!m_Fd.IsValid())
	{
		return;
	}

	const std::uint64_t one = 1;

	// Unchecked deliberately, and it is the one place in this file that is. The counter saturates at
	// 2^64 - 2 and this is called once per shutdown; the only reachable failure is EAGAIN on a counter
	// nobody has drained, which means a wakeup is already pending and this call had nothing to add.
	(void)::write(m_Fd.Get(), &one, sizeof(one));
}

FrameRing::~FrameRing()
{
	if (m_Open)
	{
		io_uring_queue_exit(&m_Ring);
	}
}

Result<void> FrameRing::Open(unsigned entries)
{
	if (m_Open)
	{
		return Failure(EALREADY, "the frame ring is already open");
	}

	io_uring_params params{};
	params.flags = WantedFlags;

	const int result = io_uring_queue_init_params(entries, &m_Ring, &params);

	if (result < 0)
	{
		return Failure(-result, Diagnose(entries));
	}

	m_Open = true;

	return {};
}

Result<void> FrameRing::Watch(const IEventSource& source)
{
	const RawFd descriptor = source.Descriptor();

	if (!descriptor.IsValid())
	{
		// Seam/EventSource.h: an ordinary answer. The source is still drained every iteration; it simply
		// never causes a wakeup, which is exactly right for one whose events are a function of the clock.
		return {};
	}

	if (m_WatchedCount >= m_Watched.size())
	{
		return Failure(ENOSPC, "more event sources than the frame ring watches");
	}

	m_Watched[m_WatchedCount] = Watched{ .Descriptor = descriptor, .Generation = 0, .Armed = false };
	++m_WatchedCount;

	return {};
}

Result<io_uring_sqe*> FrameRing::Sqe()
{
	if (io_uring_sqe* const sqe = io_uring_get_sqe(&m_Ring))
	{
		return sqe;
	}

	// The queue is full, which on this ring means an iteration armed more than it reaped. Flush and try
	// once more; a second refusal is a defect rather than backpressure.
	if (const int submitted = io_uring_submit(&m_Ring); submitted < 0)
	{
		return Failure(-submitted, "flushing the frame ring's submission queue");
	}

	if (io_uring_sqe* const sqe = io_uring_get_sqe(&m_Ring))
	{
		return sqe;
	}

	return Failure(ENOSPC, "the frame ring's submission queue is full after a flush");
}

Result<void> FrameRing::Arm(std::size_t index)
{
	Watched& watched = m_Watched[index];

	const Result<io_uring_sqe*> sqe = Sqe();

	if (!sqe)
	{
		return std::unexpected{ sqe.error() };
	}

	++watched.Generation;

	io_uring_prep_poll_multishot(*sqe, watched.Descriptor.Value, POLLIN);
	io_uring_sqe_set_data64(*sqe, Encode(Tag::Poll, (watched.Generation << 8) | index));

	watched.Armed = true;

	return {};
}

Result<void> FrameRing::WaitFor(Wake wake)
{
	if (!m_Open)
	{
		return Failure(EBADF, "waiting on a frame ring that was never opened");
	}

	for (std::size_t index = 0; index < m_WatchedCount; ++index)
	{
		if (!m_Watched[index].Armed && m_Watched[index].Descriptor.IsValid())
		{
			if (const Result<void> armed = Arm(index); !armed)
			{
				return armed;
			}
		}
	}

	const std::uint64_t timeout = ++m_Timeouts;
	const bool timed = wake.Which != Wake::Kind::Settled;

	// Held across the submit below, which is what the kernel reads it during. `Continuous` arms exactly
	// as `Timed` does: the interval is the loop's business and the loop returns a fresh wake every
	// iteration, so what this needs from either is one instant.
	__kernel_timespec deadline{};

	if (timed)
	{
		const Result<io_uring_sqe*> sqe = Sqe();

		if (!sqe)
		{
			return std::unexpected{ sqe.error() };
		}

		// Absolute rather than relative, and it is the reason this file holds no clock. A relative
		// timeout has to be computed from a `now` read before the enter, so every preemption between the
		// two lands on the far side of the deadline — which is a late wake, and a late wake is the one
		// error the contract does not permit this to make.
		const std::int64_t nanoseconds = Monotonic::ToNanoseconds(wake.When);

		deadline.tv_sec = nanoseconds / 1'000'000'000;
		deadline.tv_nsec = nanoseconds % 1'000'000'000;

		io_uring_prep_timeout(*sqe, &deadline, 0, IORING_TIMEOUT_ABS);
		io_uring_sqe_set_data64(*sqe, Encode(Tag::Timeout, timeout));
	}

	// The enter, and the only one. `Settled` waits here indefinitely with nothing armed but the polls,
	// which is Docs/Architecture.md#doing-nothing-must-cost-nothing reaching the bottom of the stack:
	// no timer, no periodic wakeup, and the thread off the run queue until something happens.
	const int result = io_uring_submit_and_wait(&m_Ring, 1);

	if (result < 0 && result != -EINTR && result != -ETIME)
	{
		return Failure(-result, "waiting on the frame ring");
	}

	const bool fired = Reap(timeout);

	if (timed && !fired)
	{
		// Something else woke us, so the timeout is still standing. Removing it costs one submission and
		// keeps the ring's outstanding set equal to what this iteration asked for; leaving it would be
		// correct — a stale tag is discarded and the contract permits a spurious wake — but it would make
		// every early wakeup pay for a wakeup later that nothing wanted.
		Cancel(timeout);
	}

	return {};
}

bool FrameRing::Reap(std::uint64_t timeout)
{
	bool fired = false;
	std::size_t seen = 0;

	io_uring_cqe* cqe = nullptr;

	// The one reap site, per this file's header. `io_uring_peek_cqe` is reached only after the enter
	// above, which is what keeps `DEFER_TASKRUN` deferring rather than silently not.
	while (io_uring_peek_cqe(&m_Ring, &cqe) == 0)
	{
		const std::uint64_t data = io_uring_cqe_get_data64(cqe);
		const bool more = (cqe->flags & IORING_CQE_F_MORE) != 0;
		const std::int32_t status = cqe->res;

		io_uring_cqe_seen(&m_Ring, cqe);
		++seen;

		switch (KindOf(data))
		{
			case Tag::Timeout:
				// A tag that is not this iteration's is a timeout cancelled too late to stop, and is
				// exactly what the generation exists to discard.
				fired = fired || ValueOf(data) == timeout;
				break;

			case Tag::Poll:
			{
				const std::size_t index = ValueOf(data) & 0xff;
				const std::uint64_t generation = ValueOf(data) >> 8;

				if (index >= m_WatchedCount || m_Watched[index].Generation != generation)
				{
					break;
				}

				// Nothing is read here and nothing is emitted. The loop drains every source on every
				// iteration whatever woke it, so what a poll completion means to this object is only
				// whether the poll is still standing.
				if (!more)
				{
					m_Watched[index].Armed = false;

					if (status < 0)
					{
						m_Watched[index].Descriptor = RawFd{};
						++m_Broken;
					}
				}

				break;
			}

			case Tag::Cancel:
				break;
		}
	}

	if (seen == 0)
	{
		++m_Spurious;
	}

	return fired;
}

void FrameRing::Cancel(std::uint64_t timeout)
{
	io_uring_sqe* const sqe = io_uring_get_sqe(&m_Ring);

	if (sqe == nullptr)
	{
		// Nothing to do about it and nothing broken by it: the timeout fires once, its tag is stale by
		// then, and `Reap` discards it. This is the one place a full queue is not a defect.
		return;
	}

	io_uring_prep_timeout_remove(sqe, Encode(Tag::Timeout, timeout), 0);
	io_uring_sqe_set_data64(sqe, Encode(Tag::Cancel, timeout));

	// Submitted and not waited on. Both completions — the removal's and the cancelled timeout's
	// `-ECANCELED` — are reaped by whichever iteration next enters the kernel, and both are discarded
	// by tag when they arrive.
	(void)io_uring_submit(&m_Ring);
}
