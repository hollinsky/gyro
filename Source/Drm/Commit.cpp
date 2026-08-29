#include "Drm/Commit.h"

#include <sched.h>

#include <algorithm>
#include <cerrno>
#include <utility>

#include "Core/Clock.h"
#include "Core/Trace.h"

namespace Drm
{
CommitThread::~CommitThread()
{
	Stop();
}

void CommitThread::Start(CommitIssuer issuer, void* context)
{
	if (m_Thread.joinable())
	{
		return;
	}

	m_Issuer = issuer;
	m_Context = context;
	m_State.store(State::Idle, std::memory_order_release);

	m_Thread = std::thread{ [this] { Run(); } };
}

bool CommitThread::Arm(CommitRequest&& request) noexcept
{
	if (m_State.load(std::memory_order_acquire) != State::Idle || !m_Thread.joinable())
	{
		return false;
	}

	m_Request = std::move(request);

	// Whose priority this thread should sit above. A register store rather than a syscall, because this
	// runs inside Core/FrameSection.h's guard.
	m_Armer.store(::pthread_self(), std::memory_order_relaxed);

	const MonotonicClock clock;
	m_ArmedAt = clock.Now();

	// The release that publishes the request, paired with the acquire in `Run`.
	m_State.store(State::Armed, std::memory_order_release);
	m_State.notify_one();

	return true;
}

std::optional<CommitOutcome> CommitThread::Reap() noexcept
{
	if (m_State.load(std::memory_order_acquire) != State::Done)
	{
		return std::nullopt;
	}

	const CommitOutcome outcome = m_Outcome;

	// The descriptors were closed by the commit thread before it published `Done`; this drops whatever
	// the moved-from request still holds and leaves the slot clean for the next arm.
	m_Request = CommitRequest{};

	m_State.store(State::Idle, std::memory_order_release);

	return outcome;
}

void CommitThread::Stop() noexcept
{
	if (!m_Thread.joinable())
	{
		return;
	}

	m_Stopping.store(true, std::memory_order_release);

	// A store that actually *changes* the word, because a futex waiter only re-examines the world when
	// the value it slept on has moved. A commit in flight overwrites this with `Done` on its way out and
	// then reads the flag, which is why the flag rather than this is what the loop tests.
	m_State.store(State::Stopping, std::memory_order_release);
	m_State.notify_one();

	m_Thread.join();
}

void CommitThread::Inherit() noexcept
{
	if (m_Inherited)
	{
		return;
	}

	m_Inherited = true;

	const pthread_t armer = m_Armer.load(std::memory_order_relaxed);

	if (armer == pthread_t{})
	{
		return;
	}

	int policy = 0;
	sched_param parameters{};

	if (::pthread_getschedparam(armer, &policy, &parameters) != 0)
	{
		return;
	}

	// Only where the thread driving this one is real-time. Everywhere else — a test, a nested session,
	// a developer's shell without the privilege — there is no priority to sit above and nothing to do.
	if (policy != SCHED_FIFO)
	{
		return;
	}

	const int highest = ::sched_get_priority_max(SCHED_FIFO);

	parameters.sched_priority = std::min(parameters.sched_priority + 1, highest);

	(void)::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &parameters);
}

void CommitThread::Run() noexcept
{
	while (true)
	{
		State state = m_State.load(std::memory_order_acquire);

		// `Armed` is the only state that is this thread's business. Everything else — including `Done`,
		// which is a commit the frame thread has not read yet — means the slot is not ours to touch.
		//
		// `Reap` moving the slot from `Done` to `Idle` deliberately does not notify: there is nothing for
		// this thread to do at `Idle` either, and the wake that matters is the `Arm` that follows it.
		while (state != State::Armed && !m_Stopping.load(std::memory_order_acquire))
		{
			m_State.wait(state, std::memory_order_acquire);
			state = m_State.load(std::memory_order_acquire);
		}

		// **Tested on the flag rather than on the state**, because a commit that was in the ioctl when
		// `Stop` ran publishes its outcome on the way out and overwrites the word it would otherwise have
		// read the shutdown from.
		if (m_Stopping.load(std::memory_order_acquire))
		{
			return;
		}

		m_State.store(State::Running, std::memory_order_release);

		// After the first arm rather than at construction, because the parameters being read are the frame
		// thread's and it is not real-time until the composition root has promoted it — which happens after
		// every output is open.
		Inherit();

		const MonotonicClock clock;
		const Instant began = clock.Now();

		const int error = m_Issuer != nullptr ? m_Issuer(m_Context, m_Request) : ENODEV;

		const Instant ended = clock.Now();

		// The descriptors are this thread's to close: the kernel has read them, and the frame thread must
		// not touch the request again until it reaps.
		for (Fd& fence : m_Request.Fences)
		{
			fence = Fd{};
		}

		m_Outcome = CommitOutcome{ .Error = error, .Elapsed = ::Elapsed(began, ended) };

		// **The one row this thread writes**, and it is the figure KernelWishlist.md asks the kernel for.
		// Off the frame thread, so it costs the composite nothing.
		TraceElapsed("commit path", m_Outcome.Elapsed);

		if (error != 0)
		{
			TraceMark("commit failed", TraceThread, TraceTag(static_cast<std::uint64_t>(error)));
		}

		m_State.store(State::Done, std::memory_order_release);
		m_State.notify_one();
	}
}
} // namespace Drm
