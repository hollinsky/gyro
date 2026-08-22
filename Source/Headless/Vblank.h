#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

#include "Core/Time.h"

// The display timeline a headless flip lands on: where the vblanks are, and which one a commit makes.
//
// **This is the whole of what headless simulates, and everything else in the module is bookkeeping
// around it.** Docs/Architecture.md#headless says the backend is "the only place the presentation
// timing work can be tested precisely", and what makes that true is that a fake clock takes arbitrary
// rates *and phases* — so the schedulability claim of decision 29 can be swept across every phase
// relationship rather than asserted at whichever one the developer's monitors happened to be at. A
// phase is an epoch here, and that is the whole mechanism.
//
// **The period is what the panel does, not what it was configured to be.** Docs/Architecture.md#phase-
// drift-is-not-something-to-track opens by observing that real panels do not run at nominal rates, and
// Seam/PresentationInfo.h makes `Period` "what the panel did, never what gyro asked for" precisely so
// that a backend echoing the commanded value cannot close the VRR servo's loop on its own output. A
// simulated panel that ran at exactly its `OutputConfiguration::Period` would make that distinction
// untestable in the one place built to test it, so the timeline is configured independently of the
// configuration and a test that wants them equal says so.
//
// **Jitter is a table rather than a generator**, because the sweep's whole value is that a failure
// reproduces. A pseudo-random panel would produce a schedule nobody can re-run, which is the property
// that turns a caught deadline miss into an anecdote.

// Enough for a repeating pattern, and small enough to sit inside a timeline by value. The table is
// owned rather than borrowed for the reason a `std::span` member always earns: a timeline outlives the
// call that configured it, and a sweep that built its samples in a loop body would be reading a dead
// array on the frame that mattered.
inline constexpr std::size_t MaxJitterSamples = 16;

class VblankTimeline
{
public:
	VblankTimeline() = default;

	// `epoch` is vblank zero, which is the phase; `period` is what the panel actually runs at.
	//
	// A non-positive period is clamped to a nanosecond for FrameClock's reason — it is reachable from a
	// configuration nobody validated, and a panel that owes infinitely many frames is the direction
	// that does not recover.
	constexpr void Configure(Instant epoch, Duration period) noexcept
	{
		m_Epoch = epoch;
		m_Period = std::max(period, Duration{ 1 });
		Clamp();
	}

	// Per-vblank offsets, applied by sequence modulo the table's length.
	//
	// **Clamped so that the timeline stays monotonic**, which is not a nicety: a vblank that arrived
	// before its predecessor would hand `FrameClock::Observe` a negative measured interval and the
	// defect would surface as a scheduling bug three layers up rather than as bad input here. Just
	// under half a period either way is the bound that keeps `At(n) < At(n + 1)` for every pair; see
	// `Clamp` for why "just under".
	constexpr void SetJitter(std::span<const Duration> samples) noexcept
	{
		m_JitterCount = std::min(samples.size(), MaxJitterSamples);

		for (std::size_t index = 0; index < m_JitterCount; ++index)
		{
			m_Jitter[index] = samples[index];
		}

		Clamp();
	}

	[[nodiscard]] constexpr Duration Period() const noexcept { return m_Period; }

	[[nodiscard]] constexpr Instant Epoch() const noexcept { return m_Epoch; }

	// When vblank `sequence` happens.
	[[nodiscard]] constexpr Instant At(std::uint64_t sequence) const noexcept
	{
		return Advanced(Advanced(m_Epoch, Scaled(sequence)), JitterFor(sequence));
	}

	// The interval between this vblank and the one before it, which is what a flip reports as
	// `PresentationInfo::Period` — the *observed* figure, so under jitter it is not `Period()` and that
	// difference is the point. Vblank zero has no predecessor and reports the nominal period, which is
	// the honest answer for a first observation rather than a zero the clock would read as "does not
	// know".
	[[nodiscard]] constexpr Duration IntervalBefore(std::uint64_t sequence) const noexcept
	{
		return sequence == 0 ? m_Period : Elapsed(At(sequence - 1), At(sequence));
	}

	// The first vblank strictly after `instant`.
	//
	// Strictly, so that a commit issued at the exact instant of a vblank lands on the next one. The
	// alternative reads as a zero-latency flip, which no hardware does and which would let a loop
	// present and observe within one iteration — a state nothing downstream is written against.
	[[nodiscard]] constexpr std::uint64_t After(Instant instant) const noexcept
	{
		if (instant < At(0))
		{
			return 0;
		}

		const Duration ahead = Elapsed(m_Epoch, instant);
		const std::int64_t estimate = ahead.count() / m_Period.count();
		std::uint64_t sequence = estimate > 0 ? static_cast<std::uint64_t>(estimate) : 0;

		// Jitter moves a vblank by less than half a period, so the estimate is out by at most one in
		// either direction. Bounded rather than trusted: the loops are what make this correct for a
		// table somebody widens later, and they cost two comparisons in the case that matters.
		while (sequence > 0 && At(sequence - 1) > instant)
		{
			--sequence;
		}

		while (At(sequence) <= instant && sequence < std::numeric_limits<std::uint64_t>::max())
		{
			++sequence;
		}

		return sequence;
	}

	// The vblank a commit issued at `committedAt` latches into.
	//
	// `lead` is how early the commit must be programmed for that vblank to take it, which
	// Frame/FrameClock.h's `FrameClockPolicy::LatchLead` documents as zero for headless — nothing sits
	// between gyro and the presentation here. It is a parameter anyway, because a non-zero lead is how
	// a sweep injects a missed latch on purpose: the commit was issued in time by the schedule's
	// arithmetic and the panel took it a frame later regardless, which is the failure real hardware
	// produces and which no amount of correct scheduling prevents.
	[[nodiscard]] constexpr std::uint64_t Latch(Instant committedAt, Duration lead) const noexcept
	{
		std::uint64_t sequence = After(committedAt);

		if (lead <= Duration::zero())
		{
			return sequence;
		}

		while (Elapsed(committedAt, At(sequence)) < lead && sequence < std::numeric_limits<std::uint64_t>::max())
		{
			++sequence;
		}

		return sequence;
	}

private:
	// `m_Period * sequence`, saturating. The caller naming an absurd sequence is speculative early
	// rendering asking about frames that have not happened, which is a question to answer badly rather
	// than a precondition to assume — the same treatment Frame/FrameClock.h gives it.
	[[nodiscard]] constexpr Duration Scaled(std::uint64_t sequence) const noexcept
	{
		constexpr std::uint64_t Ceiling = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
		const std::uint64_t period = static_cast<std::uint64_t>(m_Period.count());

		if (sequence != 0 && period != 0 && sequence > Ceiling / period)
		{
			return Duration::max();
		}

		return Duration{ static_cast<std::int64_t>(sequence * period) };
	}

	[[nodiscard]] constexpr Duration JitterFor(std::uint64_t sequence) const noexcept
	{
		return m_JitterCount == 0 ? Duration::zero() : m_Jitter[sequence % m_JitterCount];
	}

	// See SetJitter. Applied on both entry points, because either can be the one that made the table
	// and the period disagree.
	//
	// `(period - 1) / 2` rather than `period / 2`, and the off-by-one is the whole point: at exactly
	// half a period, adjacent samples at opposite extremes put two vblanks on the *same instant*, which
	// is an interval of zero and reads to `FrameClock::Observe` as a backend that does not know its own
	// period. One nanosecond of headroom is what makes `At(n) < At(n + 1)` true for every pair rather
	// than for every pair but one.
	constexpr void Clamp() noexcept
	{
		const Duration bound = (m_Period - Duration{ 1 }) / 2;

		for (std::size_t index = 0; index < m_JitterCount; ++index)
		{
			m_Jitter[index] = std::clamp(m_Jitter[index], -bound, bound);
		}
	}

	Instant m_Epoch{};
	Duration m_Period{ 1 };
	std::array<Duration, MaxJitterSamples> m_Jitter{};
	std::size_t m_JitterCount = 0;
};

// The contract everything downstream assumes. A timeline sits inside an output by value and is read on
// the frame thread, so it owns its table and has no lifetime of its own.
static_assert(std::is_trivially_copyable_v<VblankTimeline>);

// The default is a timeline nothing has configured, and the assertions below are what make the
// arithmetic's edges visible where a reader will find them.
static_assert([] {
	VblankTimeline timeline;
	timeline.Configure(Monotonic::FromNanoseconds(1'000), Duration{ 100 });

	return timeline.At(0) == Monotonic::FromNanoseconds(1'000) && timeline.At(7) == Monotonic::FromNanoseconds(1'700);
}());

// A commit at the exact instant of a vblank takes the next one, which is the strictness above.
static_assert([] {
	VblankTimeline timeline;
	timeline.Configure(Monotonic::FromNanoseconds(1'000), Duration{ 100 });

	return timeline.After(Monotonic::FromNanoseconds(1'000)) == 1 &&
	       timeline.After(Monotonic::FromNanoseconds(1'050)) == 1 &&
	       timeline.After(Monotonic::FromNanoseconds(1'099)) == 1 &&
	       timeline.After(Monotonic::FromNanoseconds(1'100)) == 2;
}());

// Before the epoch is vblank zero rather than an error: an output configured with a phase in the
// future is exactly what a sweep constructs.
static_assert([] {
	VblankTimeline timeline;
	timeline.Configure(Monotonic::FromNanoseconds(1'000), Duration{ 100 });

	return timeline.After(Monotonic::FromNanoseconds(0)) == 0 && timeline.At(0) > Monotonic::FromNanoseconds(0);
}());

// A lead pushes the commit past the vblank it would otherwise have made, which is the injected miss.
static_assert([] {
	VblankTimeline timeline;
	timeline.Configure(Monotonic::FromNanoseconds(1'000), Duration{ 100 });

	return timeline.Latch(Monotonic::FromNanoseconds(1'090), Duration::zero()) == 1 &&
	       timeline.Latch(Monotonic::FromNanoseconds(1'090), Duration{ 20 }) == 2;
}());
