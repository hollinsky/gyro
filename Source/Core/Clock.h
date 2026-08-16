#pragma once

#include <atomic>

#include "Core/Time.h"

// The process's only source of "now".
//
// It is an interface for two reasons that pull the same way. The headless backend's fake clock has
// to be the real thing rather than a test fiction — the schedulability sweep places time at
// arbitrary phase relationships and the idle assertion needs it not to advance at all, neither of
// which a real clock can be asked for. And a clock that is globally reachable is a clock the frame
// path eventually reaches, so nothing owns one ambiently: it is handed to the subsystems entitled
// to read it, and the frame loop is expected to read it once per iteration rather than per output.
//
// The virtual call is roughly a tenth of the vDSO read it wraps, on a call that happens tens of
// times per frame. It is not on the hot path in any sense that survives measurement.
class IClock
{
public:
	virtual ~IClock() = default;

	[[nodiscard]] virtual Instant Now() const noexcept = 0;

protected:
	IClock() = default;
	IClock(const IClock&) = default;
	IClock& operator=(const IClock&) = default;
};

// The one implementation that reads the kernel. clock_gettime appears in exactly one translation
// unit in the process; CMake/CheckClockDiscipline.cmake makes that a build failure rather than a
// review comment.
//
// Stateless and cheap to construct, deliberately. A singleton would put a reachable now back
// exactly where this design just took one out.
class MonotonicClock final : public IClock
{
public:
	[[nodiscard]] Instant Now() const noexcept override;
};

// The clock the headless backend and the tests drive.
//
// Atomic because headless runs the same two real threads the native backend does, so both sides
// genuinely read this concurrently. Relaxed, because it is a value and not a synchronization edge:
// the publication boundary orders what crosses between those threads, and a clock that also
// ordered things would be a second channel between them.
class ManualClock final : public IClock
{
public:
	ManualClock() = default;

	explicit ManualClock(Instant now) noexcept : m_Now{ now } {}

	[[nodiscard]] Instant Now() const noexcept override { return m_Now.load(std::memory_order_relaxed); }

	void Set(Instant now) noexcept { m_Now.store(now, std::memory_order_relaxed); }

	// Not a read-modify-write: concurrent readers are the point, concurrent drivers are not.
	void Advance(Duration by) noexcept { Set(Now() + by); }

private:
	std::atomic<Instant> m_Now{};
};

static_assert(std::atomic<Instant>::is_always_lock_free);
