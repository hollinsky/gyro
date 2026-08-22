#include "Compositor/Uring.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <source_location>
#include <string_view>
#include <thread>

#include "Core/Clock.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Testing/Test.h"

// Decision 80's other half, against a kernel.
//
// `FrameLoop::Step` decides when the next frame is owed and answers with a `Wake`; this object is
// everything that happens between that answer and the loop being called again. The schedulability
// sweep in Source/Integration/Schedulability.Test.cpp asserts the first half — that an idle scene folds
// to `Wake::Never()` and stays there — and it runs against a `ManualClock` and a simulated shim, so it
// cannot say anything at all about what a real ring does with that answer. Without this file the
// invariant is asserted up to the point where it becomes a syscall and no further, which is the point
// at which it becomes true or false on somebody's machine.
//
// Four things go wrong here and every one of them is silent. A `Never()` that armed a timeout anyway
// would satisfy every assertion the sweep makes and cost a wakeup per period forever, which is
// Docs/Architecture.md#doing-nothing-must-cost-nothing failing on the power bill rather than on the
// glass. A timeout that fired early would return the loop to `Step` before the frame it woke for could
// be reached, which reads as jitter and never as a bug. An interrupt raised in the gap between the
// loop's last iteration and the ring's next enter would be lost, and decision 83 puts dispatch's
// publication nudge on exactly that path — a lost one is a frame thread asleep with a snapshot waiting
// for it. And an eventfd left readable turns the level-triggered poll into a spin at whatever rate the
// loop can iterate, which is the same thread going from blocked to hot with no event lost and nothing
// to see.
//
// **`Open` is called on the thread that submits, in every test below.** `IORING_SETUP_SINGLE_ISSUER`
// binds the ring to one task and refuses another with `EEXIST`, which is why the composition root
// opens it inside the frame thread rather than beside it. A test that opened here and submitted from
// the waker thread would be asserting against its own mistake.
//
// **A machine without `SINGLE_ISSUER` or `DEFER_TASKRUN` fails these rather than skipping them**, and
// it fails with the sentence `Open` already composes: gyro carries no epoll fallback, so the whole of
// what it owes a machine it cannot run on is a diagnostic naming the half that is missing. Reporting
// that here is the same obligation, one layer down.
//
// **Every timing assertion below excuses an early return that `Spurious` accounts for, and that is the
// contract rather than a concession to flakiness.** `WaitFor` is permitted to come back with nothing
// to show for it, and the reason it is permitted is visible one layer down: `io_uring_enter` returns
// the number of submissions it made, so a wait interrupted *after* the submit reports a success with
// no completion behind it. `Spurious` is this object's own name for that, counted at the one reap
// site. A test that asserted a wait was never early would be asserting something stronger than the
// interface promises, and it would fail on a loaded machine rather than on a broken one — which is
// exactly the class of test this file exists instead of.

namespace
{
using namespace std::chrono_literals;

// Reports the diagnostic rather than the expression. `GYRO_REQUIRE(ring.Open())` would print
// `ring.Open()` and throw away the one thing the failure knows.
[[nodiscard]] bool Ready(
	const Result<void>& result,
	std::string_view what,
	const std::source_location& where = std::source_location::current()
)
{
	if (result)
	{
		return true;
	}

	ReportFailure(what, std::format("{}", result.error()), where);

	return false;
}
} // namespace

// The invariant at the bottom of the stack: `Wake::Never()` arms nothing, so nothing but a watched
// descriptor can end the wait.
//
// It is asserted as a duration because that is the only shape the absence of a timer has. Nothing in
// this process can complete on this ring except the interrupt's poll — no timeout was asked for — so a
// wait that came back before the raise, and came back with a completion, is a timer armed for an
// instant `Never()` never named.
GYRO_TEST(FrameRing, WaitingForNeverArmsNothingAndBlocksUntilSomethingElseWakesIt)
{
	const MonotonicClock clock;

	Interrupt interrupt;

	if (!Ready(interrupt.Open(), "the interrupt could not be opened"))
	{
		return;
	}

	FrameRing ring;

	if (!Ready(ring.Open(), "the frame ring could not be opened"))
	{
		return;
	}

	GYRO_REQUIRE(ring.Watch(interrupt));

	constexpr Duration kDelay = 50ms;

	const Instant entered = clock.Now();

	// The dispatch thread, in miniature. It is a second thread rather than a raise before the wait —
	// which is the test below — because the gap this closes is the one where the frame thread is
	// already inside the kernel with nothing armed.
	std::thread waker{ [&interrupt, kDelay] {
		std::this_thread::sleep_for(kDelay);
		interrupt.Raise();
	} };

	while (!interrupt.IsRaised())
	{
		const std::uint64_t spurious = ring.Spurious();

		if (!Ready(ring.WaitFor(Wake::Never()), "the wait failed"))
		{
			break;
		}

		// Back before the raise. Permitted only as an interruption, and re-entered the way the loop
		// above `Step` re-enters it; anything else is a completion this ring should not have had.
		if (!interrupt.IsRaised())
		{
			GYRO_REQUIRE(ring.Spurious() != spurious);
		}
	}

	waker.join();

	// Why it returned: the flag the raise stores before it writes the descriptor, which is the whole
	// point of that ordering.
	GYRO_CHECK(interrupt.IsRaised());

	// And that it did not get there before there was anything to get there for.
	GYRO_CHECK(Elapsed(entered, clock.Now()) >= kDelay);

	GYRO_CHECK(interrupt.Drain());
}

// A wake is permitted to be late and is not permitted to be early, which is the asymmetry the absolute
// timeout exists for: a relative one is computed from a `now` read before the enter, and every
// preemption between the two lands on the far side of the deadline.
//
// Early is the direction that cannot be recovered from. The loop woken before its record point
// predicts a frame it cannot yet start, does nothing, and arms again; woken late it draws what it can
// still reach, which is decision 35's whole argument. So this asserts the direction rather than a
// tolerance, and it asserts it repeatedly — a timer that is early one time in thirty is a timer that
// misses a vblank one time in thirty, and a single wait would find it once a fortnight.
GYRO_TEST(FrameRing, WaitingForAnInstantNeverReturnsBeforeIt)
{
	const MonotonicClock clock;

	FrameRing ring;

	if (!Ready(ring.Open(), "the frame ring could not be opened"))
	{
		return;
	}

	// Nothing is watched, so a timeout and the cancellation of a timeout are the only completions this
	// ring can produce — and a cancellation exists only where the wait before left one standing. That
	// is what makes the excuse below exact rather than a tolerance: an early return is permitted where
	// the previous wait was interrupted, and nowhere else. The first one is unexcused, which is where a
	// timer firing early lands.
	bool interrupted = false;

	for (std::size_t attempt = 0; attempt < 32; ++attempt)
	{
		const std::uint64_t spurious = ring.Spurious();
		const Instant deadline = Advanced(clock.Now(), 2ms);

		GYRO_REQUIRE(ring.WaitFor(Wake::At(deadline)));

		if (clock.Now() < deadline)
		{
			GYRO_REQUIRE(interrupted || ring.Spurious() != spurious);
		}

		interrupted = ring.Spurious() != spurious;
	}
}

// The race decision 83 is about, from the losing side: dispatch publishes and raises while the frame
// thread is between iterations, so the descriptor is already readable when the wait is entered.
//
// A design that armed the poll and then checked would drop this one, and it would drop it exactly when
// the frame thread was idle — which is the only time the nudge is needed at all, since a loop already
// running picks the snapshot up on its next `Acquire` regardless.
GYRO_TEST(FrameRing, AnInterruptRaisedBeforeTheWaitIsNotLost)
{
	const MonotonicClock clock;

	Interrupt interrupt;

	if (!Ready(interrupt.Open(), "the interrupt could not be opened"))
	{
		return;
	}

	FrameRing ring;

	if (!Ready(ring.Open(), "the frame ring could not be opened"))
	{
		return;
	}

	GYRO_REQUIRE(ring.Watch(interrupt));

	interrupt.Raise();

	// A far deadline rather than `Never()`, so that a lost wakeup fails this run rather than hanging
	// it — and the assertion is therefore the elapsed time and not the return, because waiting the
	// whole two seconds *is* the loss.
	constexpr Duration kPatience = 2s;

	const Instant entered = clock.Now();
	const std::uint64_t spurious = ring.Spurious();

	GYRO_REQUIRE(ring.WaitFor(Wake::At(Advanced(entered, kPatience))));

	// It came back at once, and it came back carrying something — which on this ring, this early, can
	// only be the poll. Both halves are needed: an interruption would also come back at once and would
	// prove nothing about the descriptor.
	GYRO_CHECK(Elapsed(entered, clock.Now()) < kPatience / 4);
	GYRO_CHECK_EQ(ring.Spurious(), spurious);
	GYRO_CHECK(interrupt.IsRaised());
	GYRO_CHECK(interrupt.Drain());
}

// `IEventSource`'s read-to-empty contract, spent. The poll is level-triggered and multishot, so an
// eventfd whose counter is still non-zero is a descriptor that is still readable, and every wait after
// it returns immediately.
//
// **The failure is a spin and not a lost event**, which is why it is worth a test of its own. Nothing
// is dropped, nothing is drawn wrong, and no assertion anywhere else in the tree changes: the frame
// thread simply stops blocking, and a boot service that never sleeps is discovered by a fan rather
// than by a report. Measuring the *next* wait is the only way to see it from outside.
GYRO_TEST(FrameRing, DrainingTheInterruptStopsTheLevelTriggeredPollFiringAgain)
{
	const MonotonicClock clock;

	Interrupt interrupt;

	if (!Ready(interrupt.Open(), "the interrupt could not be opened"))
	{
		return;
	}

	FrameRing ring;

	if (!Ready(ring.Open(), "the frame ring could not be opened"))
	{
		return;
	}

	GYRO_REQUIRE(ring.Watch(interrupt));

	interrupt.Raise();

	// `Never()` rather than a deadline, and it is not incidental: a wait woken by a descriptor cancels
	// the timeout it armed, and the removal's completion plus the cancelled timeout's `-ECANCELED`
	// arrive some time afterwards for the next enter to reap. Both are discarded by tag, and both would
	// end the measured wait below before its own deadline — so the wait that consumes the raise arms
	// nothing, and what the measurement then sees is the descriptor alone. There is no hang to risk
	// here because the raise has already happened.
	GYRO_REQUIRE(ring.WaitFor(Wake::Never()));
	GYRO_REQUIRE(interrupt.Drain());

	constexpr Duration kSettle = 20ms;

	// Retried while the ring comes back interrupted, since an interrupted wait has measured nothing.
	// Bounded, so that a run in which every one of them was interrupted fails here rather than quietly
	// asserting nothing at all.
	for (std::size_t attempt = 0; attempt < 4; ++attempt)
	{
		const std::uint64_t spurious = ring.Spurious();
		const Instant entered = clock.Now();

		GYRO_REQUIRE(ring.WaitFor(Wake::At(Advanced(entered, kSettle))));

		if (ring.Spurious() != spurious)
		{
			continue;
		}

		GYRO_CHECK(Elapsed(entered, clock.Now()) >= kSettle);

		return;
	}

	GYRO_FAIL("every wait after the drain came back interrupted, so none of them measured anything");
}

// The counter is a diagnostic, and a diagnostic nobody pins is a number that quietly stops meaning
// anything. `Spurious` exists so that a wasteful arming pattern is noticeable at all — the contract
// permits this object to return with nothing to show for it, so no other assertion in the tree can
// ever fail because of one — and it is only worth reading if the ordinary case is known to be near
// nothing.
//
// **Near nothing rather than nothing**, and the difference is this file's header: an `io_uring_enter`
// that submitted an SQE reports the submission count, so a wait interrupted after that point is
// indistinguishable from a served one except by the completion it does not carry. That is what the
// counter counts, it is not gyro's to prevent, and it happens on a machine with something else running
// on it. What a wasteful arming pattern would produce is one per wait, so what this pins is that the
// count does not scale with the waits at all.
//
// The interrupt is watched and never raised, which is the shape of an idle frame thread rather than a
// simplification: a multishot poll completing without its descriptor being readable would land here
// too, and `Broken` is the other way that goes wrong.
GYRO_TEST(FrameRing, SpuriousDoesNotClimbWithOrdinaryTimedWaits)
{
	const MonotonicClock clock;

	Interrupt interrupt;

	if (!Ready(interrupt.Open(), "the interrupt could not be opened"))
	{
		return;
	}

	FrameRing ring;

	if (!Ready(ring.Open(), "the frame ring could not be opened"))
	{
		return;
	}

	GYRO_REQUIRE(ring.Watch(interrupt));

	constexpr std::uint64_t kWaits = 128;

	for (std::uint64_t wait = 0; wait < kWaits; ++wait)
	{
		GYRO_REQUIRE(ring.WaitFor(Wake::At(Advanced(clock.Now(), 1ms))));
	}

	GYRO_CHECK(ring.Spurious() * 16 < kWaits);
	GYRO_CHECK_EQ(ring.Broken(), std::uint64_t{ 0 });
}
