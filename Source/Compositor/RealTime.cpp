#include "Compositor/RealTime.h"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include <algorithm>
#include <cerrno>

namespace
{

// `RLIMIT_RTTIME` is microseconds. Saturating at the resource limit's own type rather than wrapping,
// because a limit that wrapped to something small would kill the frame thread on its first frame.
[[nodiscard]] rlim_t Microseconds(Duration duration) noexcept
{
	if (duration <= Duration::zero())
	{
		return 0;
	}

	const std::int64_t microseconds = duration.count() / 1'000;

	return static_cast<rlim_t>(std::max<std::int64_t>(microseconds, 1));
}

} // namespace

Result<void> ReserveRealTime(const RealTimePolicy& policy)
{
	const rlimit limit{ .rlim_cur = Microseconds(policy.Warn),
		                .rlim_max = Microseconds(std::max(policy.Warn, policy.Kill)) };

	if (::setrlimit(RLIMIT_RTTIME, &limit) != 0)
	{
		return Failure(errno, "setting RLIMIT_RTTIME on the frame thread");
	}

	return {};
}

Result<void> LockMemory()
{
	// `MCL_FUTURE` as well as `MCL_CURRENT`, because most of what the frame path touches is allocated
	// after this call — the arenas, the target mappings, the ring — and locking only what exists now
	// would cover the least interesting half.
	if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
	{
		return Failure(errno, "locking gyro's pages into memory");
	}

	return {};
}

Result<void> PromoteToRealTime(const RealTimePolicy& policy)
{
	const int lowest = ::sched_get_priority_min(SCHED_FIFO);
	const int highest = ::sched_get_priority_max(SCHED_FIFO);

	if (lowest < 0 || highest < 0)
	{
		return Failure(errno, "reading the SCHED_FIFO priority range");
	}

	sched_param parameters{};
	parameters.sched_priority = std::clamp(policy.Priority, lowest, highest);

	// `pthread_setschedparam` rather than `sched_setscheduler`, because this is one thread's property
	// and the process has another thread that must *not* acquire it: Docs/Decisions.md decision 45 has
	// dispatch outranked by the frame thread, and promoting the whole process would erase exactly the
	// ordering that keeps a priority inversion from forming across the publication boundary.
	if (const int error = ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &parameters); error != 0)
	{
		return Failure(error, "promoting the frame thread to SCHED_FIFO");
	}

	return {};
}
