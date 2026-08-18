#pragma once

#include "Core/Fd.h"
#include "Core/Result.h"

// What a loop polls and pumps so that a backend's completions arrive.
//
// Seam/Presenter.h promises that `Reconfigured` is emitted "from the loop's own drain", and this is
// that drain. Everything a presenter signals — a flip reaching the glass, a transition completing,
// a target set going away — originates as a readable file somewhere below it, and something has to
// turn one into the other on the thread the signal is claimed by.
//
// **It is not a member of `IPresenter`, and the granularity is why.** A presenter is one output's:
// PresentationInfo carries no output identity precisely because the presenter is already the answer
// to which output is speaking. A backend's event descriptor is not one output's. KMS has one DRM
// file per *device*, carrying page-flip events for every CRTC on it; the nested backend has one
// connection to the host, carrying feedback for every window it opened. One descriptor and N
// presenters, both times. Had the descriptor hung off IPresenter, a loop would register the same
// file N times and each presenter's drain would read events belonging to its siblings.
//
// So a source belongs to the backend at whatever granularity its descriptor actually has, and the
// presenters are what it emits *into*. See Docs/Decisions.md decision 80.
//
// **Exactly one thread pumps a source, for the whole of its life.** That is what lets Drain() hold
// no lock at all, and it is what obliges the nested backend to open a second connection to the host
// rather than partitioning one by event queue — presentation feedback is frame-side and input is
// dispatch-side, and a shared connection is a mutex spanning the publication boundary. Decision 81
// has the reading that settles it, and the rule generalizes past nested: a source that two threads
// pump is a design error rather than a configuration.
//
// **The loop drains every source every iteration and never asks which one is ready.** Being handed a
// readiness set is the obvious alternative and it is rejected, because "drain before evaluating" is a
// *correctness* ordering — a Presented arriving after FrameClock was read yields a deadline one
// iteration stale — and it would live in the platform code that does the waiting, which is the one
// part of the loop the headless sweep never runs. What that costs instead is one read returning
// EAGAIN per source per wakeup, on a machine that ordinarily has one source.

class IEventSource
{
public:
	IEventSource() = default;

	virtual ~IEventSource() = default;

	// Neither copied nor moved: a loop holds sources by address for as long as the backend lives, and
	// a backend is replaced by being destroyed and rebuilt — which decision 41's migration already
	// does on every boot, sequenced by the composition root.
	IEventSource(const IEventSource&) = delete;
	IEventSource& operator=(const IEventSource&) = delete;
	IEventSource(IEventSource&&) = delete;
	IEventSource& operator=(IEventSource&&) = delete;

	// What a loop waits on, or an invalid descriptor where there is nothing to wait on.
	//
	// **Invalid is an ordinary answer rather than a failure.** A headless backend's flips are a
	// function of the ManualClock the test drives and no file becomes readable when one falls due; it
	// is drained like every other source and reports what the clock says has happened. That case is
	// the second argument against a readiness set — a source with no descriptor could never appear in
	// one, so headless would never present.
	//
	// Borrowed, per Core/Fd.h: the descriptor belongs to the backend and is closed with it. A loop
	// registers it and never closes it.
	[[nodiscard]] virtual RawFd Descriptor() const noexcept = 0;

	// Read what has arrived and emit it, until nothing is left.
	//
	// **To empty, not once.** A loop polling level-triggered — io_uring's multishot poll is what this
	// is written against — wakes again immediately on whatever was left behind. That is a spin at
	// whatever rate the source produces rather than a lost event, which makes it the kind of defect
	// that shows up as battery life on a machine nobody is watching rather than as a missed frame.
	//
	// Named for what it does rather than `Dispatch`, which in this codebase names a thread. A source
	// drained on the frame thread and a member called Dispatch would read as the wrong one every time.
	//
	// The failure vocabulary is Present()'s, because it is the same file failing the same ways:
	// `ENODEV` or `EACCES` where the device is gone or paused, which is the composition root's problem
	// and not the loop's. There is no transient refusal in it — a read that finds nothing is success,
	// and it is the ordinary result of a wakeup that some other source caused.
	virtual Result<void> Drain() = 0;
};
