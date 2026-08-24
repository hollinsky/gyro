// ppoll and eventfd are POSIX and Linux respectively rather than ISO C, and glibc hides the first
// behind a feature test macro. Named here for the reason Compositor/Compositor.cpp names its own:
// what is wanted from the platform is stated rather than inherited from a build flag.
#define _POSIX_C_SOURCE 200809L

#include "Compositor/Wait.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>

namespace
{
constexpr std::int64_t NanosecondsPerSecond = 1'000'000'000;
}

Result<void> DispatchWait::Open() noexcept
{
	// Non-blocking is not load-bearing here — nothing reads the counter — but an eventfd that could
	// block is one a later drain would have to think about, and the flag costs nothing now.
	const int descriptor = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

	if (descriptor < 0)
	{
		return Failure(errno, "opening the dispatch thread's wait");
	}

	m_Fd = Fd{ descriptor };

	return {};
}

Result<void> DispatchWait::WaitUntil(std::optional<Instant> deadline, Instant now) noexcept
{
	if (!m_Fd.IsValid())
	{
		return Failure(EBADF, "waiting on a dispatch wait that was never opened");
	}

	struct timespec relative = {};
	const struct timespec* timeout = nullptr;

	if (deadline)
	{
		// Clamped at zero rather than trusted: a deadline already past is the ordinary case on a step
		// that took longer than the interval it armed, and a negative `timespec` is `EINVAL` rather than
		// the poll-and-return-immediately that is meant.
		const Duration remaining = Elapsed(now, *deadline);
		const std::int64_t nanoseconds = remaining > Duration::zero() ? remaining.count() : 0;

		relative.tv_sec = nanoseconds / NanosecondsPerSecond;
		relative.tv_nsec = nanoseconds % NanosecondsPerSecond;
		timeout = &relative;
	}

	struct pollfd watched = {};
	watched.fd = m_Fd.Get();
	watched.events = POLLIN;

	// `EINTR` is a return with nothing to show for it, which the contract above permits: the caller
	// re-checks the flag and steps. Anything else is a descriptor that has stopped working, and on this
	// thread that means the stop can no longer arrive — so it is carried out rather than absorbed.
	if (::ppoll(&watched, 1, timeout, nullptr) < 0 && errno != EINTR)
	{
		return Failure(errno, "waiting for the dispatch thread's next iteration");
	}

	return {};
}

void DispatchWait::Stop() noexcept
{
	m_Stopping.store(true, std::memory_order_release);

	if (!m_Fd.IsValid())
	{
		return;
	}

	const std::uint64_t one = 1;

	// Unchecked for Interrupt::Raise's reason: this is called once, the counter saturates far above one,
	// and the only reachable failure is `EAGAIN` on a counter nobody drains — which means the wakeup this
	// call wanted is already pending.
	// A `(void)` cast does not discard a `warn_unused_result` on GCC; see Uring.cpp.
	[[maybe_unused]] const ssize_t raised = ::write(m_Fd.Get(), &one, sizeof(one));
}
