#pragma once

#include <atomic>
#include <cstdint>

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

// Three clocks read at one instant, which is the only thing in the process that ever needs more than
// one.
//
// **It exists for one consumer and is useless to any other, which is what keeps decision 57 intact.**
// A trace gyro writes stamps in this timebase; a system trace stamps in the kernel's boot-time domain,
// which counts through suspend where this one does not. Merging the two is a matter of relating them
// once, and relating them requires having read both at the same moment. Nothing else in gyro cares
// what the other domain says.
//
// **The wall clock is here for the reader rather than for any merge.** A trace stamped only in the
// monotonic domain says a stutter happened 41,203 seconds into the run, which cannot be lined up with
// a journal line, a screen recording, or a person saying it went wrong at about quarter past. One
// reading of `CLOCK_REALTIME` in the same window turns every slice in the file into a time of day.
// Nothing computes with it — it is not a clock anything schedules against, precisely because it steps
// when NTP says so.
//
// **Raw counts rather than `Instant`s, deliberately.** An `Instant` is a value the schedule is
// entitled to compare against a deadline, and handing one back from a second clock reader is exactly
// the reachable now decision 57 exists to prevent. These are three integers destined for a file.
struct ClockAnchor
{
	std::int64_t Monotonic = 0;
	std::int64_t Boottime = 0;
	std::int64_t Realtime = 0;
};

// Read together and as close together as two syscalls can be. The pair is a relation rather than a
// measurement, so what matters is that nothing long happens between them, and nothing does.
[[nodiscard]] ClockAnchor ReadClockAnchor() noexcept;

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
