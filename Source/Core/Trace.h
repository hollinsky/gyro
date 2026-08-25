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
// snapshot writer interns them, and where a record carries a `TraceLabel` it is the writer that spells
// the number into the name — which it can afford because it runs on its own thread, off both loops.
// The rule this leaves intact is the one that mattered: a call site hands over a literal and a word,
// never a string it built. There is still no *formatted message*, because a message with a value in it
// is a counter and a counter is a number; what a label adds is an identity, which is the one thing a
// counter cannot carry and the one thing every row of a frame has to agree on.

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

	// A number belonging to the slice already open on this row rather than to a row of its own. See
	// `TraceAttribute`.
	Attribute = 5,
};

// How many outputs a trace can name rows for. Frame/Admission.h's `MaxOutputs` is the number this
// may not be smaller than, and Frame/Loop.h static_asserts that rather than this header reaching up a
// tier for it — the Trace module cannot, since it depends on `Core` alone and may not say `MaxOutputs`.
inline constexpr std::size_t TracedOutputs = 16;

// How many commits a trace can draw as lanes. Seam/RenderTarget.h's `MaxTargets` bounds what any
// presenter will take, and the same rule as above keeps the number here rather than reached for.
inline constexpr std::size_t TracedFlights = 4;

// Which row a record lands on, relative to the thread that emitted it.
//
// **Zero is the emitting thread's own row and every other row belongs to one output.** A thread's row
// holds what the thread did; an output's rows hold what happened to one screen, which outlives
// whichever thread said it and is the unit a person actually reads a compositor in.
inline constexpr std::uint16_t TraceThread = 0;

// **The lanes of one output are consecutive, so a reader scrolls to a screen rather than to a kind.**
// The rows were previously grouped the other way — every output's GPU row together, every output's
// deadline row together — which reads fine with one monitor and puts four screens' worth of rows
// between a frame and its own pixels with two.
inline constexpr std::uint16_t TraceLanesPerOutput = static_cast<std::uint16_t>(4 + TracedFlights);

[[nodiscard]] constexpr std::uint16_t TraceLane(std::size_t output, std::uint16_t lane) noexcept
{
	const std::size_t clamped = output < TracedOutputs ? output : 0;

	return static_cast<std::uint16_t>(1 + clamped * TraceLanesPerOutput + lane);
}

// **The rows are declared in the order a frame happens, because that is the order a person reads.**
// Down one output's lanes is the life of a frame: the refresh it was aimed at, the work that built it,
// the work the device did, the wait for the panel, and the pixels a person saw. A stutter is a step
// that is wide, or a step that is missing, and either way the eye finds it by going down a column.

// The ruler. One slice per refresh interval, ending at the deadline the frame was admitted against, so
// the row tiles the timeline and every other row can be read against it. It is a row of its own rather
// than a slice the work nests inside because the case worth seeing is the child outliving the parent,
// which nesting cannot draw. **It is named for the refresh and not for the frame aimed at it**: every
// other `frame N` in the picture is an extent that happened, and this one ends where a commit is due
// rather than where its pixels appear.
[[nodiscard]] constexpr std::uint16_t TraceGrid(std::size_t output) noexcept
{
	return TraceLane(output, 0);
}

// The frame thread's work for this output: one slice per frame, and one mark per wake that declined.
[[nodiscard]] constexpr std::uint16_t TraceOutput(std::size_t output) noexcept
{
	return TraceLane(output, 1);
}

// The device's work, which is on no thread at all — it is the queue, and a person reading a dropped
// frame needs to see it beside the record that submitted it rather than inside it.
[[nodiscard]] constexpr std::uint16_t TraceGpu(std::size_t output) noexcept
{
	return TraceLane(output, 2);
}

// **The wait between a frame being submitted and the pixels landing, and the lane is the commit slot.**
// This is the extent nothing in the trace used to draw: a frame handed to the presenter is gone from
// every row until a vblank mentions it, and on a sixty hertz panel that is a whole refresh a person is
// trying to account for. It could not be one row, because commits complete in the order they were made
// and a track's slices have to nest — so frame N would open before N+1 and close before it, which is
// the one shape Perfetto refuses. A lane per slot is the honest drawing anyway: how many lanes are
// occupied at an instant *is* the queue depth, read without a counter.
[[nodiscard]] constexpr std::uint16_t TraceFlight(std::size_t output, std::size_t slot) noexcept
{
	return TraceLane(output, static_cast<std::uint16_t>(3 + (slot < TracedFlights ? slot : 0)));
}

// What is on the glass. One slice per vblank, tiling, named for the *frame* it showed and carrying the
// publication behind it as an attribute — so a frame scanned out twice is one wide slice rather than
// two marks a reader has to notice are the same number, and that is the only thing this row merges.
// Keyed on the scene, which is what it was first built as, it merged a dozen refreshes that were a
// dozen different pictures and drew an animation as a freeze.
[[nodiscard]] constexpr std::uint16_t TraceGlass(std::size_t output) noexcept
{
	return TraceLane(output, static_cast<std::uint16_t>(3 + TracedFlights));
}

inline constexpr std::uint16_t TraceScopes = static_cast<std::uint16_t>(1 + TracedOutputs * TraceLanesPerOutput);

// Which chain an arrow belongs to.
//
// **A flow id is a name in one global space, and two unrelated chains that pick the same number are
// spliced into one.** That is not a theoretical hazard: the snapshot sequence counts publications and
// an output's frame sequence counts vblanks, both start near one, and a trace of ten seconds had
// forty-two chains in which a Vulkan composite was linked to a scene it had nothing to do with. So
// every id carries the domain that minted it in its top bits, and the domains are enumerated here
// rather than agreed on by convention at four call sites.
enum class TraceDomain : std::uint64_t
{
	Scene = 1,
	Frame = 2,
};

// What a record carries beside its name.
//
// **The number is printed into the name, and that is the change the whole picture turned on.** A trace
// used to join one frame's rows with arrows: the budget, the composite, the extract, the blur and the
// resumed composite all carried one id, so clicking any of them fanned eight lines across four rows and
// a seven-second capture held three thousand of them. What a person wanted from all that machinery was
// to know the slices were the same frame — and a slice that simply *says* `frame 142` answers it
// without drawing anything, keeps answering it when the two ends are scrolled apart, and lets a search
// for `frame 142` light up every row at once. So a label is printed by default and an arrow is the
// exception.
//
// **An arrow is for a join where the two sides count differently**, which after the change is one join
// and not eight: a publication is numbered by the dispatch thread and a frame by the panel, so nothing
// in the name of one can find the other and only a line can say which scene a frame drew. `TraceTag`
// is everything else.
//
// **Zero is no label at all**, which is how a call site says it has nothing to add — Trace/Perfetto.cpp
// suppresses both the suffix and the arrow on a zero value. A vblank that showed a frame this
// compositor did not draw relies on it: labelling it would turn *nothing to say* into a name.
class TraceLabel
{
public:
	constexpr TraceLabel() = default;

	[[nodiscard]] constexpr std::uint64_t Value() const noexcept { return m_Value; }

	[[nodiscard]] constexpr bool IsArrow() const noexcept { return m_Arrow; }

	[[nodiscard]] constexpr bool IsEmpty() const noexcept { return m_Value == 0; }

private:
	friend constexpr TraceLabel TraceTag(std::uint64_t) noexcept;
	friend constexpr TraceLabel TraceFlow(TraceDomain, std::uint64_t) noexcept;

	constexpr TraceLabel(std::uint64_t value, bool arrow) noexcept : m_Value{ value }, m_Arrow{ arrow } {}

	std::uint64_t m_Value = 0;
	bool m_Arrow = false;
};

// The number this record is about, printed beside its name and joined to nothing.
[[nodiscard]] constexpr TraceLabel TraceTag(std::uint64_t value) noexcept
{
	return TraceLabel{ value, false };
}

// The same, and an arrow through every record that names the same id in the same domain.
[[nodiscard]] constexpr TraceLabel TraceFlow(TraceDomain domain, std::uint64_t value) noexcept
{
	constexpr std::uint64_t Mask = (std::uint64_t{ 1 } << 60) - 1;

	return value == 0 ? TraceLabel{} : TraceLabel{ (static_cast<std::uint64_t>(domain) << 60) | (value & Mask), true };
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

	// A label's value on `Begin` and `Mark`, the counter's value on `Count` and `Elapsed`, the
	// attribute's value on `Attribute`, and unread on `End`. A label is the number the slice is about —
	// the frame, the scene — which the writer prints into the name, and which on the one join that needs
	// it also draws an arrow. See `TraceLabel`.
	std::atomic<std::uint64_t> Payload{ 0 };

	// Kind in the low byte, row in the next two, and whether the payload draws an arrow in the byte
	// above them. One word so that the record's tail is not four separate atomics the reader has to
	// agree with each other about.
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

	// Whether `Payload` is a flow id the writer should draw a line through, as against a number it
	// should only print. See `TraceLabel`.
	bool Arrow = false;
};

namespace Detail
{

inline constexpr std::uint32_t TraceArrowBit = std::uint32_t{ 1 } << 24;

[[nodiscard]] constexpr std::uint32_t PackTrace(TraceKind kind, std::uint16_t scope, bool arrow = false) noexcept
{
	return static_cast<std::uint32_t>(kind) | (static_cast<std::uint32_t>(scope) << 8) | (arrow ? TraceArrowBit : 0U);
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

	void Emit(TraceKind kind, const char* name, std::uint64_t payload, std::uint16_t scope, bool arrow = false) noexcept
	{
		if (m_Records.empty())
		{
			return;
		}

		EmitAt(m_Clock->Now(), kind, name, payload, scope, arrow);
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
	void EmitAt(
		Instant stamp,
		TraceKind kind,
		const char* name,
		std::uint64_t payload,
		std::uint16_t scope,
		bool arrow = false
	) noexcept
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
		record.Detail.store(Detail::PackTrace(kind, scope, arrow), std::memory_order_relaxed);

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
				                      .Kind = static_cast<TraceKind>(detail & 0xFFU),
				                      .Arrow = (detail & Detail::TraceArrowBit) != 0 };

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
inline void TraceMark(const char* name, std::uint16_t scope = TraceThread, TraceLabel label = {}) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->Emit(TraceKind::Mark, name, label.Value(), scope, label.IsArrow());
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
TraceSpanAt(const char* name, Instant began, Instant ended, std::uint16_t scope, TraceLabel label = {}) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->EmitAt(began, TraceKind::Begin, name, label.Value(), scope, label.IsArrow());
		buffer->EmitAt(ended, TraceKind::End, nullptr, 0, scope);
	}
}

// One end of a span opened where control is, whose other end is somewhere else entirely. The flight
// lanes are the caller: a frame enters the queue when the present returns, and the instant that
// happened is the instant this runs — asking the iteration for the `now` it read before it decided to
// draw would open the wait a frame is *waiting* at a point before the frame existed, which is what the
// row did and is a slice that begins before the work it is waiting on.
inline void TraceOpen(const char* name, std::uint16_t scope = TraceThread, TraceLabel label = {}) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->Emit(TraceKind::Begin, name, label.Value(), scope, label.IsArrow());
	}
}

// The same at a remembered instant, for a span whose opening is learned about after the fact: the
// glass row, where a slice begins at a vblank the loop is told about milliseconds later and is closed
// by the vblank that opens the next one. A `TraceSpan` cannot express either shape — there is no scope
// the two ends live in, and they are separated by a sleep — so balancing is the writer's, exactly as it
// is for the ring's own truncated ends.
inline void TraceOpenAt(const char* name, Instant began, std::uint16_t scope, TraceLabel label = {}) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->EmitAt(began, TraceKind::Begin, name, label.Value(), scope, label.IsArrow());
	}
}

inline void TraceCloseAt(Instant ended, std::uint16_t scope) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->EmitAt(ended, TraceKind::End, nullptr, 0, scope);
	}
}

// The same, closed where control is rather than at a remembered instant, for the end that arrives as a
// cancellation rather than as a completion: a commit dropped by a mode set is a frame whose flight
// lane nothing will ever close, and a lane left open runs to the end of the trace as a slice claiming
// the panel spent thirty seconds on one frame.
inline void TraceClose(std::uint16_t scope) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->Emit(TraceKind::End, nullptr, 0, scope);
	}
}

// A number that belongs to the slice already open on this row, printed in Perfetto's argument panel
// when a person clicks it.
//
// **It is for the number a slice has to carry and must not be joined by.** The glass row is what this
// exists for: a slice there is named for the frame a person is looking at, because that is the identity
// every other row of that frame agrees on — and it also has a scene behind it, which is a different
// count on a different thread. Printed into the name, the two numbers would read as one identity and a
// search for either would light up the wrong rows. Given a row of its own, the scene would be a counter
// stepping at flips, which is the *two spellings of one fact* habit these rows were rewritten to lose.
// An attribute is neither: it is on the slice, it is only seen when asked for, and it joins nothing.
//
// **It binds to the slice open on this row and nowhere else**, so it is emitted immediately after the
// `Begin` it belongs to and with that `Begin`'s instant. Trace/Recorder.h sorts stably, so two records
// sharing a stamp keep the order they were written in; one that arrives with no slice open on its row —
// the ring having lapped the `Begin` — is dropped rather than attached to whatever comes next.
//
// A name and a number rather than a formatted string, for the reason the rest of this header gives: a
// call site hands over a literal and a word, never something it built.
inline void TraceAttribute(const char* name, std::uint64_t value, std::uint16_t scope = TraceThread) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->Emit(TraceKind::Attribute, name, value, scope);
	}
}

// The same for a slice opened at a remembered instant, which is every slice that has an attribute
// today: the stamp must be the one the `Begin` carried, or the sort puts the attribute somewhere else
// on the row and it binds to a slice it is not about.
inline void TraceAttributeAt(const char* name, Instant stamp, std::uint64_t value, std::uint16_t scope) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->EmitAt(stamp, TraceKind::Attribute, name, value, scope);
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

// The same for a figure that is a duration, so that what a frame *cost* can be sampled where it was
// measured and land on the same axis as what it was *predicted* to cost. Without it a trace carried
// only the prediction, and an estimate with nothing beside it is a number a reader has to trust.
inline void TraceElapsedAt(const char* name, Instant stamp, Duration value, std::uint16_t scope) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->EmitAt(stamp, TraceKind::Elapsed, name, static_cast<std::uint64_t>(value.count()), scope);
	}
}

// A mark belonging to a moment that has passed, for the same reason as the two above: a flip
// happens at the panel's vblank and is only learned about when the feedback is drained, and what
// is worth reading is the moment it happened rather than the moment it was reported.
inline void TraceMarkAt(const char* name, Instant stamp, std::uint16_t scope, TraceLabel label = {}) noexcept
{
	if (TraceBuffer* const buffer = Detail::Tracing)
	{
		buffer->EmitAt(stamp, TraceKind::Mark, name, label.Value(), scope, label.IsArrow());
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
	explicit TraceSpan(const char* name, std::uint16_t scope = TraceThread, TraceLabel label = {}) noexcept
		: m_Scope{ scope }
	{
		if (TraceBuffer* const buffer = Detail::Tracing)
		{
			buffer->Emit(TraceKind::Begin, name, label.Value(), scope, label.IsArrow());
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
