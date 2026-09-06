#pragma once

#include "Core/Time.h"

// The wait for a page flip event the kernel has already promised, and what to do when it never comes.
//
// **gyro's atomic commit is blocking**, which is what makes this a short, tight rule rather than a
// guess. Drm/Commit.h runs the ioctl without `DRM_MODE_ATOMIC_NONBLOCK` on a thread of its own, so the
// kernel returns from it only after `drm_atomic_helper_wait_for_flip_done` — the flip has *happened*
// by the time the commit thread reports success, and the `DRM_MODE_PAGE_FLIP_EVENT` the request asked
// for is queued on the device's descriptor waiting to be read.
//
// So the interval this bounds is not a hardware wait at all. It is the gap between the kernel saying
// *done* and gyro reading the event off a file it polls every iteration, which is ordinarily the very
// next drain. What makes that interval worth a watchdog is what happens when it never ends: a driver
// whose internal wait timed out returns **zero** and sends nothing, `DrmOutput` stays flip-pending
// forever, `NextEvent` goes quiet because the commit thread is idle and no file will ever become
// readable, and the frame loop sleeps on an output that will never speak again. That is a panel gone
// for the life of the process, and it is what decision 181's readback bug produced on this machine —
// the readback is fixed and this is the class of failure it exposed.
//
// **It is a rule rather than a member of `DrmOutput` because it is the only half of that file a
// machine with no panel can run.** `DrmOutput` wants a card, a pipeline and an allocator to exist at
// all, which is why it has no test beside it; the arithmetic that decides an output has been abandoned
// is the part worth being sure of, so it is here, where a test can drive it with a clock it owns.
// Drm/Catalog.h is the same split for the same reason.

namespace Drm
{
class FlipWatchdog
{
public:
	// Where the deadline comes from when the output has no mode period to scale it to — before the
	// first flip, or on a catalog that could not read the timings. Generous on purpose: the number
	// exists so that an unknown period cannot arm a deadline of zero, not to be tuned.
	static constexpr Duration Fallback = std::chrono::milliseconds{ 50 };

	// The kernel has reported a commit it took. `at` is when that report was *read*, which is after the
	// flip rather than before it — see the header.
	//
	// **One refresh, and it is already enormous for what it bounds.** The event is queued and the
	// deadline is only ever tested straight after a read of the descriptor it is queued on, so the
	// honest expectation is the next drain and this allows a whole frame of them. Scaling to the period
	// rather than fixing a number keeps it in the units every other figure in the loop is written in,
	// and means a 24 Hz panel is not held to a 240 Hz panel's patience.
	void Arm(Instant at, Duration period) noexcept
	{
		m_Deadline = Advanced(at, period > Duration::zero() ? period : Fallback);
		m_Armed = true;
	}

	// The event arrived, or the frame it belonged to stopped existing. Idempotent: every path that
	// clears `m_Flipping` calls this, and several of them can run with nothing armed.
	void Disarm() noexcept
	{
		m_Armed = false;
		m_Deadline = Instant{};
	}

	[[nodiscard]] bool IsArmed() const noexcept { return m_Armed; }

	// What `IPresenter`'s owner has to wake at for this to be tested at all. Unarmed answers
	// `Duration::max()`, which is the quiet answer every other branch of `DrmOutput::NextEvent` gives.
	[[nodiscard]] Instant Deadline() const noexcept { return m_Armed ? m_Deadline : Instant{ Duration::max() }; }

	// **At the deadline rather than past it**, because the loop is woken *at* the instant `Deadline`
	// named and a strict comparison there would sleep the whole period again on every wake that landed
	// exactly on it.
	[[nodiscard]] bool HasExpired(Instant now) const noexcept { return m_Armed && now >= m_Deadline; }

private:
	Instant m_Deadline{};
	bool m_Armed = false;
};
} // namespace Drm
