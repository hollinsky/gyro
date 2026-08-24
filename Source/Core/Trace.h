#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "Core/Clock.h"
#include "Core/Time.h"
// See Docs/Architecture.md#tracing and decisions 36, 57, and 139.

// What the process was doing, kept so that the answer outlives the thing that went wrong.
//
// **The ring is always armed and it overwrites**, which is the whole shape of this file and the one
// property a profiler you switch on cannot have. A compositor that stutters once an hour is not a
// thing to reproduce on demand; by the time a person has decided the stutter was real, the frame that
// caused it is thirty seconds gone. So the cost is paid continuously and the *snapshot* is the rare
// event: the last however-many seconds are already in memory, and asking for them is a read.
//
// **Emit is legal inside Core/FrameSection.h's guard, and everything below is in service of that.**
// No allocation, no lock, no syscall, no branch on anything a driver owns: a record is four stores and
// a release, the slot is the write index masked by a power-of-two capacity, and a full ring laps its
// own oldest record rather than growing or refusing. A tracer that could block would be a tracer that
// causes the frame drop it is there to explain.
//
// **The fields are atomics rather than a struct the snapshot memcpys.** The snapshot reads while the
// frame thread writes, which is a data race in the abstract machine however benign it is on the
// hardware, and `GYRO_SANITIZE=thread` is a configuration this project supports. An instrument that
// has to be excluded from the sanitizer is an instrument nobody runs under it. Every access is
// relaxed except the index publish, so on x86-64 and aarch64 alike this compiles to the plain stores
// it would have been anyway.
//
// **Decision 57 says the timebase has one reader and no reachable now, and this reads a clock.** What
// that rule is protecting is not the read; it is that a reachable now becomes a now something
// eventually *decides* against, in a file whose subject is something else. So the containment here is
// that nothing in this header ever hands an `Instant` back. A buffer takes a clock, stamps records
// with it, and has no accessor that returns a time — the only way a stamp leaves is inside a
// `TraceEvent` the snapshot writer consumes. The clock is reachable; the now is not.
//
// **A name is a pointer to a string literal and is never copied, hashed, or formatted here.** The
// snapshot writer interns them, which it can afford to do because it runs on its own thread, off both
// loops. That is also why there is no formatted trace event and never will be: a message with a value
// in it is a counter, and a counter is a number.

// What a record says happened. `Begin` and `End` nest into a slice on the record's track; `Mark` is an
// instant; `Count` and `Elapsed` are the same counter sample differing only in what the writer labels
// the axis, since a duration wants nanoseconds on the unit and a queue depth wants none.
enum class TraceKind : std::uint8_t
{
	Begin = 0,
	End = 1,
	Mark = 2,
	Count = 3,
	Elapsed = 4,
};

// How many outputs a trace can name a track for. Frame/Admission.h's `MaxOutputs` is the number this
// may not be smaller than, and Frame/Loop.h static_asserts that rather than this header reaching up a
// tier for it — the Trace module cannot, since it depends on `Core` alone and may not say `MaxOutputs`.
inline constexpr std::size_t TracedOutputs = 16;

// Which track a record lands on, relative to the thread that emitted it.
//
// Zero is the emitting thread's own track, which is where a slice belongs unless it is about one
// screen in particular. The rest are the compositor's tracks rather than a thread's: an output has a
// row of its own so that a person looking at a stutter on one panel is not reading four panels
// interleaved, and its GPU work has a second row because that work is not on any thread at all.
inline constexpr std::uint16_t TraceThread = 0;

[[nodiscard]] constexpr std::uint16_t TraceOutput(std::size_t output) noexcept
{
	return static_cast<std::uint16_t>(1 + (output < TracedOutputs ? output : 0));
}

[[nodiscard]] constexpr std::uint16_t TraceGpu(std::size_t output) noexcept
{
	return static_cast<std::uint16_t>(1 + TracedOutputs + (output < TracedOutputs ? output : 0));
}

// The third row, and it holds what the output was *aiming at* rather than what it did. A budget is a
// span whose end is a deadline, so a frame that overran is a span on the row above that reaches past
// the end of the one here — which is the whole reason this is a row of its own and not a slice the
// work nests inside. Nesting cannot express a child outliving its parent, and the overrun is the one
// case the picture exists for.
[[nodiscard]] constexpr std::uint16_t TraceDeadline(std::size_t output) noexcept
{
	return static_cast<std::uint16_t>(1 + 2 * TracedOutputs + (output < TracedOutputs ? output : 0));
}

inline constexpr std::uint16_t TraceScopes = static_cast<std::uint16_t>(1 + 3 * TracedOutputs);

// Which chain a flow id belongs to.
//
// **A flow id is a name in one global space, and two unrelated chains that pick the same number are
// spliced into one.** That is not a theoretical hazard: the snapshot sequence counts publications and
// the submission value counts queue submits, both start near one, and a trace of ten seconds had
// forty-two chains in which a Vulkan composite was linked to a scene it had nothing to do with. So
// every id carries the domain that minted it in its top bits, and the domains are enumerated here
// rather than agreed on by convention at four call sites.
enum class TraceFlow : std::uint64_t
{
	Snapshot = 1,
	Submission = 2,
};

// **Zero is not tagged, because zero is how a call site says *no flow at all*.** Trace/Perfetto.cpp
// suppresses the field on a zero payload, and a vblank that showed a frame this compositor did not
// draw relies on that: tagging it would turn *nothing to link to* into a chain of its own.
[[nodiscard]] constexpr std::uint64_t TraceFlowId(TraceFlow domain, std::uint64_t value) noexcept
{
	constexpr std::uint64_t Mask = (std::uint64_t{ 1 } << 60) - 1;

	return value == 0 ? 0 : (static_cast<std::uint64_t>(domain) << 60) | (value & Mask);
}

// A record as the ring holds it: every field separately atomic, because the reader is another thread
// and the writer must not synchronize with it.
//
// Thirty-two bytes, which is two to a cache line and never one record across two. The snapshot's
// validation discards a record the producer lapped mid-copy, so a straddling record would only ever be
// discarded rather than believed — the alignment is for the common case, where the copy is a stride
// through memory the frame thread is still writing the far end of.
struct TraceRecord
{
	std::atomic<Instant> Stamp{};
	std::atomic<const char*> Name{ nullptr };

	// A flow id on `Begin` and `Mark`, the counter's value on `Count` and `Elapsed`, and unread on
	// `End`. A flow id is what draws the arrow from the publish that produced a snapshot to the acquire
	// that took it, which is the one question a two-thread compositor is asked most often and the one a
	// pair of unrelated slices cannot answer.
	std::atomic<std::uint64_t> Payload{ 0 };

	// Kind in the low byte, track in the next two. One word so that the record's tail is not three
	// separate atomics the reader has to agree with each other about.
	std::atomic<std::uint32_t> Detail{ 0 };
};

static_assert(sizeof(TraceRecord) == 32);
static_assert(std::atomic<Instant>::is_always_lock_free);
static_assert(std::atomic<const char*>::is_always_lock_free);

// A record as the snapshot reads it, which is the same thing without the atomics because by then it is
// this thread's copy and nobody else's.
struct TraceEvent
{
	Instant Stamp{};
	const char* Name = nullptr;
	std::uint64_t Payload = 0;
	std::uint16_t Scope = TraceThread;
	TraceKind Kind = TraceKind::Mark;
};

namespace Detail
{

[[nodiscard]] constexpr std::uint32_t PackTrace(TraceKind kind, std::uint16_t scope) noexcept
{
	return static_cast<std::uint32_t>(kind) | (static_cast<std::uint32_t>(scope) << 8);
}

} // namespace Detail

// One thread's ring. Single producer by construction — a buffer is reached through a thread-local
// pointer and never shared — and one reader, which is the snapshot.
class TraceBuffer
{
public:
	TraceBuffer() = default;

	TraceBuffer(const TraceBuffer&) = delete;
	TraceBuffer& operator=(const TraceBuffer&) = delete;

	// The storage is the caller's, because deciding how many seconds to keep is policy and this file
	// holds none of it. A capacity that is not a power of two is refused rather than rounded: the mask
	// is the whole of the slot arithmetic, and a ring that silently held less than it was given is a
	// coverage figure that lies.
	void Arm(std::span<TraceRecord> records, const IClock& clock) noexcept
	{
		if (records.empty() || (records.size() & (records.size() - 1)) != 0)
		{
			return;
		}

		m_Records = records;
		m_Clock = &clock;
	}

	[[nodiscard]] bool IsArmed() const noexcept { return !m_Records.empty(); }

	[[nodiscard]] std::size_t Capacity() const noexcept { return m_Records.size(); }

	// Every record this buffer has ever taken, which is what makes the ring's coverage answerable: the
	// difference between this and the capacity is how much of the run has already been overwritten.
	[[nodiscard]] std::uint64_t Written() const noexcept { return m_Written.load(std::memory_order_acquire); }

	void Emit(TraceKind kind, const char* name, std::uint64_t payload, std::uint16_t scope) noexcept
	{
		if (m_Records.empty())
		{
			return;
		}

		EmitAt(m_Clock->Now(), kind, name, payload, scope);
	}

	// The same record with its time supplied rather than read, for the one thing that happens and is
	// only learned about later: a span of GPU work, whose two ends were stamped by the device's own
	// clock and are read back some frames after the batch finished.
	//
	// **A ring written through this is no longer in timestamp order, and that is handled downstream
	// rather than here.** The alternative — a ring of its own for records that arrive late — sounds
	// tidier and gives the wrong picture: two rings lap independently, so the window a GPU slice
	// covers would drift out of the window the frame-thread slice that submitted it covers, and the
	// one question a GPU track is opened to answer is which of the two the frame was waiting on. One
	// ring means one window. Trace/Recorder.h sorts what it copied, on a thread that can afford to.
	void EmitAt(Instant stamp, TraceKind kind, const char* name, std::uint64_t payload, std::uint16_t scope) noexcept
	{
		if (m_Records.empty())
		{
			return;
		}

		const std::uint64_t slot = m_Written.load(std::memory_order_relaxed);
		TraceRecord& record = m_Records[static_cast<std::size_t>(slot) & (m_Records.size() - 1)];

		record.Stamp.store(stamp, std::memory_order_relaxed);
		record.Name.store(name, std::memory_order_relaxed);
		record.Payload.store(payload, std::memory_order_relaxed);
		record.Detail.store(Detail::PackTrace(kind, scope), std::memory_order_relaxed);

		// The publish, and the only ordered store in the function. Everything above it is visible to a
		// reader that acquires this, and everything after it is a record the reader will not copy.
		m_Written.store(slot + 1, std::memory_order_release);
	}

	// The snapshot, oldest first, bounded by what the destination will hold.
	//
	// **The producer is not stopped and is not asked anything.** The copy runs against a ring the frame
	// thread is still writing, and what makes that sound is re-reading the index afterwards: a record at
	// index `i` was intact iff the producer had not reached `i + capacity` by the time the copy
	// finished. So the validation is one load and a comparison against the front of the window, and the
	// records it condemns are dropped rather than repaired. On a ring holding tens of seconds against a
	// copy taking milliseconds this discards nothing, which is not the reason it is correct.
	[[nodiscard]] std::size_t Copy(std::span<TraceEvent> into) const noexcept
	{
		if (m_Records.empty() || into.empty())
		{
			return 0;
		}

		const std::uint64_t capacity = m_Records.size();
		const std::uint64_t end = m_Written.load(std::memory_order_acquire);
		const std::uint64_t held = end < capacity ? end : capacity;
		const std::uint64_t wanted = held < into.size() ? held : into.size();
		const std::uint64_t first = end - wanted;

		std::size_t count = 0;

		for (std::uint64_t index = first; index < end; ++index)
		{
			const TraceRecord& record = m_Records[static_cast<std::size_t>(index) & (m_Records.size() - 1)];
			const std::uint32_t detail = record.Detail.load(std::memory_order_relaxed);

			into[count] = TraceEvent{ .Stamp = record.Stamp.load(std::memory_order_relaxed),
				                      .Name = record.Name.load(std::memory_order_relaxed),
				                      .Payload = record.Payload.load(std::memory_order_relaxed),
				                      .Scope = static_cast<std::uint16_t>((detail >> 8) & 0xFFFFU),
				                      .Kind = static_cast<TraceKind>(detail & 0xFFU) };

			++count;
		}

		// What the producer did while the copy was running. Anything the ring has since lapped was
		// possibly read mid-write, so it goes rather than being trusted.
		const std::uint64_t after = m_Written.load(std::memory_order_acquire);
		const std::uint64_t oldest = after < capacity ? 0 : after - capacity;

		if (oldest <= first)
		{
			return count;
		}

		const std::uint64_t lost = oldest - first;

		if (lost >= count)
		{
			return 0;
		}

		const std::size_t survivors = count - static_cast<std::size_t>(lost);
		std::copy(
			into.begin() + static_cast<std::ptrdiff_t>(lost),
			into.begin() + static_cast<std::ptrdiff_t>(count),
			into.begin()
		);

		return survivors;
	}

private:
	std::span<TraceRecord> m_Records{};
	const IClock* m_Clock = nullptr;
	std::atomic<std::uint64_t> m_Written{ 0 };
};

namespace Detail
{

// Thread-local for Core/FrameSection.h's reason, said one layer along: the dispatch thread and the
// frame thread produce at wildly different rates and must not share a ring, and a process-wide one
// would put a `fetch_add` from two threads on the frame path to serve a buffer nobody reads
// interleaved anyway. Null until the composition root enrolls the thread, which is what makes an
// un-enrolled thread — a test, a device worker, the writer itself — cost a load and a branch.
inline thread_local TraceBuffer* Tracing = nullptr;

} // namespace Detail

// Whether this thread is recording, for a caller that would otherwise compute something only to trace
// it. Nothing on the frame path should need it: the verbs below already cost a load and a predictable
// branch when the thread is not enrolled.
[[nodiscard]] inline bool IsTracing() noexcept
{
	return Detail::Tracing != nullptr;
}

// Enrollment, called by the thread being recorded as its first act.
//
// A pointer rather than a reference, and null is how a thread stops recording. The composition root
// never needs that — it owns the buffers and outlives both loops — but a test does: a buffer whose
// storage went out of scope while this thread still pointed at it is a write into freed memory on the
// next trace call, which is a strange way for the instrument to be the thing that crashes.
inline void EnrollTracing(TraceBuffer* buffer) noexcept
{
	Detail::Tracing = buffer;
}

// A point in time worth naming: a frame skipped, a target refused, a flip that landed.
inline void TraceMark(const char* name, std::uint16_t scope = TraceThread, std::uint64_t flow = 0) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->Emit(TraceKind::Mark, name, flow, scope);
	}
}

// A number that has a value at every instant and changes at this one — a queue depth, a mark, a count.
inline void TraceCount(const char* name, std::int64_t value, std::uint16_t scope = TraceThread) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->Emit(TraceKind::Count, name, static_cast<std::uint64_t>(value), scope);
	}
}

// The same, for a figure that is a duration. Separate so that the writer can label the axis in
// nanoseconds rather than every call site converting to milliseconds and losing the ability to compare
// two counters that chose differently.
inline void TraceElapsed(const char* name, Duration value, std::uint16_t scope = TraceThread) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->Emit(TraceKind::Elapsed, name, static_cast<std::uint64_t>(value.count()), scope);
	}
}

// A span of work that is over, both of whose ends are known now.
//
// **The one shape `TraceSpan` cannot express, and it is not a convenience.** A scope guard stamps its
// two records when control passes them, which is exactly right for work this thread is doing and
// exactly wrong for work it merely submitted: the GPU's composite for frame N is read back during
// frame N+2, and drawn where the scope guard ran it would sit two frames to the right of the pixels
// it produced. So the caller supplies both instants, having converted them out of whatever domain
// stamped them, and the record lands where the work was.
//
// A zero-length or inverted span is emitted as given rather than repaired. A GPU pass really can
// measure zero — the counter's tick is 52 ns on the hardware this was written against — and a
// conversion that came out backwards is a calibration defect the picture should show.
inline void
TraceSpanAt(const char* name, Instant began, Instant ended, std::uint16_t scope, std::uint64_t flow = 0) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->EmitAt(began, TraceKind::Begin, name, flow, scope);
		buffer->EmitAt(ended, TraceKind::End, nullptr, 0, scope);
	}
}

// A counter sample belonging to a moment that has passed, for the same reason as the span above: a
// figure a submission produced belongs beside that submission rather than beside its readback.
inline void TraceCountAt(const char* name, Instant stamp, std::int64_t value, std::uint16_t scope) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->EmitAt(stamp, TraceKind::Count, name, static_cast<std::uint64_t>(value), scope);
	}
}

// A mark belonging to a moment that has passed, for the same reason as the two above: a flip
// happens at the panel's vblank and is only learned about when the feedback is drained, and what
// is worth reading is the moment it happened rather than the moment it was reported.
inline void TraceMarkAt(const char* name, Instant stamp, std::uint16_t scope, std::uint64_t flow = 0) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->EmitAt(stamp, TraceKind::Mark, name, flow, scope);
	}
}

// A span of work, opened where it starts and closed by leaving the scope.
//
// **An unmatched half is expected rather than exceptional**, and the writer is what absorbs it. A ring
// that holds the last thirty seconds begins in the middle of whatever was running thirty seconds ago,
// so its oldest records are ends with no beginning; its newest are beginnings the snapshot interrupted.
// Both are the window rather than a defect, which is why this type carries no state beyond the track it
// has to close on and makes no attempt to balance anything itself.
class TraceSpan
{
public:
	explicit TraceSpan(const char* name, std::uint16_t scope = TraceThread, std::uint64_t flow = 0) noexcept
		: m_Scope{ scope }
	{
		if (TraceBuffer* const buffer = Detail::Tracing)
		{
			buffer->Emit(TraceKind::Begin, name, flow, scope);
		}
	}

	~TraceSpan() noexcept { Close(); }

	// Ends the span before the scope does, for the shape a frame loop is full of: a call whose *result*
	// has to outlive the work being measured. Closing twice is closing once, so a caller that closes
	// early and then returns is not depending on having remembered to.
	void Close() noexcept
	{
		if (!m_Open)
		{
			return;
		}

		m_Open = false;

		if (TraceBuffer* const buffer = Detail::Tracing)
		{
			buffer->Emit(TraceKind::End, nullptr, 0, m_Scope);
		}
	}

	TraceSpan(const TraceSpan&) = delete;
	TraceSpan& operator=(const TraceSpan&) = delete;

private:
	std::uint16_t m_Scope;
	bool m_Open = true;
};
