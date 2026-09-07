#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "Core/Time.h"
#include "Core/Trace.h"

// Where the time between a hand moving and the pixels moving actually went.
//
// **Input latency is the number a person perceives most and the trace did not contain it.** Every
// other row starts at the instant gyro decided to do something; this one starts at the instant the
// kernel says the device produced the event, which is the earliest moment anything can know about.
// Two marks come out of here — the event's own timestamp and the moment gyro drained it — and
// `Protocol/Seat.cpp` adds the third where a client is handed it. The gap between the first two is
// the kernel, libinput and how long the dispatch thread took to reach the descriptor; the gap to the
// third is gyro's own routing. Without the split, a pointer that lags looks the same whether the
// stack ahead of gyro stalled or gyro did, and those are fixed in different places.
//
// **The name is the kind of event and never its content, and this is not negotiable.** A keycode in
// a trace is a keylogger, and a `.pftrace` is a file people mail to each other when they report a
// stutter. So no keycode, no keysym, no modifier state, no text, and no pointer coordinate — the
// question this row answers is *when*, and every *what* that could be added to it is either useless
// for that question or a thing gyro would be handing out. `Core/Trace.h` makes the rule easy to keep:
// a record carries a string literal, so there is nothing to format a value into.
//
// **A press and a release are separate literals, because the pair is what shows a stuck key.** A
// release that never arrived, or arrived a second late, is a real bug and one a person reports as
// *the key repeated on its own* — the two marks beside each other are the whole diagnosis, and one
// literal for both would hide it.

namespace Input
{
// One kind of event, in the two places it is marked.
//
// **Two literals rather than one and a suffix**, since the ring cannot build a string: a call site
// hands over a pointer to a literal and nothing else. They are declared in pairs so that the drained
// mark can never drift out of step with the arrival it belongs to.
struct InputMark
{
	// Where the kernel said it happened.
	const char* Arrived = nullptr;

	// Where gyro took it out of libinput's queue.
	const char* Drained = nullptr;

	// **Whether this kind is folded into one mark per drain**, which is the difference between an
	// instrument and a ring that lapped because somebody moved the mouse. A thousand-hertz mouse
	// produces a thousand motions a second and a touchscreen ten contacts' worth of the same, and
	// nothing downstream treats them individually anyway — `Scene/Pointer.h` sums the displacements
	// and the seat sends one `wl_pointer.motion` for the whole iteration. So a continuous kind is
	// marked once per drain, at the *oldest* stamp in it, carrying how many were folded: the width of
	// that one mark to the drain edge is the worst latency in the batch, which is the figure worth
	// keeping, and the count says how far behind gyro had fallen. Marking each of them would put the
	// highest-rate thing in the system into a ring that has to hold the frame it caused.
	bool Continuous = false;
};

inline constexpr InputMark InputKey{ .Arrived = "key", .Drained = "key drained" };
inline constexpr InputMark InputKeyUp{ .Arrived = "key up", .Drained = "key up drained" };

inline constexpr InputMark InputButton{ .Arrived = "button", .Drained = "button drained" };
inline constexpr InputMark InputButtonUp{ .Arrived = "button up", .Drained = "button up drained" };

inline constexpr InputMark InputMotion{ .Arrived = "motion", .Drained = "motion drained", .Continuous = true };

inline constexpr InputMark InputScroll{ .Arrived = "scroll", .Drained = "scroll drained" };

inline constexpr InputMark InputTouch{ .Arrived = "touch", .Drained = "touch drained" };
inline constexpr InputMark InputTouchMotion{ .Arrived = "touch motion",
	                                         .Drained = "touch motion drained",
	                                         .Continuous = true };
inline constexpr InputMark InputTouchUp{ .Arrived = "touch up", .Drained = "touch up drained" };
inline constexpr InputMark InputTouchCancel{ .Arrived = "touch cancel", .Drained = "touch cancel drained" };

inline constexpr InputMark InputTool{ .Arrived = "tool", .Drained = "tool drained" };
inline constexpr InputMark InputToolMotion{ .Arrived = "tool motion",
	                                        .Drained = "tool motion drained",
	                                        .Continuous = true };

// The two marks a drain leaves behind, and the fold that keeps a burst from filling the ring.
//
// **It holds the fold and nothing else, so a drain that produced no continuous event costs nothing.**
// One of these belongs to the device set and lives as long as it does; `Drained` is called once per
// pass over libinput's queue and is what closes the fold.
class InputLatency
{
public:
	// One event, said as it comes out of libinput's queue and before it is routed anywhere.
	//
	// **The arrival is stamped with the device's instant rather than with the clock**, which is the
	// whole point of the row: `TraceMarkAt` puts the mark where the kernel said the event happened, so
	// the distance to the drain mark beside it is the stack in front of gyro drawn to scale. A mark at
	// the clock's now for both would draw that distance as zero and lose the only thing here that
	// cannot be measured any other way.
	void Saw(const InputMark& mark, Instant when) noexcept
	{
		if (!mark.Continuous)
		{
			TraceMarkAt(mark.Arrived, when, TraceInput());
			TraceMark(mark.Drained, TraceInput());

			return;
		}

		for (Fold& fold : m_Folds)
		{
			if (fold.Mark == nullptr)
			{
				fold = Fold{ .Mark = &mark, .Oldest = when, .Count = 1 };

				return;
			}

			if (fold.Mark != &mark)
			{
				continue;
			}

			// The oldest rather than the newest, because the batch's latency is the latency of the event
			// that waited longest — and a drain that folded eight motions is one where the first of them
			// has been waiting for all eight.
			fold.Oldest = when < fold.Oldest ? when : fold.Oldest;
			++fold.Count;

			return;
		}

		// More continuous kinds at once than there are slots, which cannot happen with the three
		// declared above. Marked on its own rather than dropped: a burst that goes unfolded is loud in
		// the ring, and an event missing from the row is a latency nobody can see.
		TraceMarkAt(mark.Arrived, when, TraceInput());
		TraceMark(mark.Drained, TraceInput());
	}

	// The queue is empty, which is what closes every fold this pass opened.
	//
	// **A folded kind's drain edge is the end of the drain rather than the moment it left the queue**,
	// and that is the honest edge rather than a convenience. Everything downstream sees one motion for
	// the whole pass, so the instant that decides when the pointer can move is the instant gyro
	// finished reading — a per-event edge would be drawing a handover that does not happen.
	void Drained() noexcept
	{
		for (Fold& fold : m_Folds)
		{
			if (fold.Mark == nullptr)
			{
				continue;
			}

			// The count is the one number on this row, and it is a count of events rather than anything
			// about them. It is printed into the name by the writer thread, so `motion 8` reads as eight
			// device events behind one mark.
			TraceMarkAt(fold.Mark->Arrived, fold.Oldest, TraceInput(), TraceTag(fold.Count));
			TraceMark(fold.Mark->Drained, TraceInput(), TraceTag(fold.Count));

			fold = Fold{};
		}
	}

private:
	struct Fold
	{
		// Identity is the pointer to the constant above, which is what makes the lookup a comparison
		// rather than a string.
		const InputMark* Mark = nullptr;

		Instant Oldest{};
		std::uint64_t Count = 0;
	};

	// One slot per continuous kind declared above, and the fallback in `Saw` is what happens if a
	// fourth is added and this is not.
	std::array<Fold, 3> m_Folds{};
};
} // namespace Input
