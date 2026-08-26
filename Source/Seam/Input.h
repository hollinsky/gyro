#pragma once

#include "Core/Input.h"
#include "Core/Signal.h"
#include "Seam/EventSource.h"

// Where gyro's input comes from: one device set, drained on the dispatch thread, reporting what the
// devices on it produced.
//
// **It is an `IEventSource` rather than a second thing shaped like one**, and the reason is that the
// drain contract Seam/EventSource.h writes down is the contract wanted here word for word — one
// descriptor for a whole device set, read to empty rather than once, an invalid descriptor being an
// ordinary answer for an implementation whose events come from somewhere that is not a file. What
// differs is not the source, it is who pumps it: presentation is frame-side and input is dispatch's,
// which is the partition Docs/Structure.md#threads-are-a-second-partition already draws. That is a
// property of the object rather than of the type — decision 81 states the rule per source, not per
// interface — so the composition root registering this one with the dispatch thread's wait instead of
// the frame ring is the whole of the difference.
//
// **One source, several signals, and no union.** A device set produces keys, pointer displacements,
// scroll increments, contacts and tablet tools, and they are five signals rather than one carrying a
// tagged event. The reason is who listens: `Input/Chord.h` wants keys and nothing else,
// `Scene/Pointer.h` wants displacements and nothing else, and a single signal would have every
// observer opening every event to discard most of them on the thread a keystroke is travelling on.
// `Core/Signal.h` costs nothing per signal that has no observer, so the split is free in the
// direction that matters.
//
// **What is not here is a frame signal.** A physical event that produces a motion and two scroll
// increments is one group, and `wl_pointer.frame` says so — but the grouping is made by whoever
// regroups these for a client, and libinput's own frame boundary is not the protocol's. A drain that
// went to empty is the boundary gyro actually has, and it is the caller's, not this interface's.
//
// **A key is reported as the kernel numbers it, with no keymap in front of it.** Translation is
// xkbcommon's and belongs beside the seat that sends a keymap to clients; what a device produces is a
// keycode, and putting a layout in the path here would mean the compositor's own escape chord moved
// when somebody selected Dvorak. Nothing below cares which letter is painted on the key it is being
// told about.

// A device set and the events it produces.
class IInput : public IEventSource
{
public:
	// Emitted from `Drain`, on the thread that drains — which is dispatch's, so an observer may author
	// into the store from it and a chain from a keystroke to a commit stays one causal sequence on one
	// thread. Docs/Architecture.md#threads has why that chain may not be split.
	Signal<const KeyEvent&> Key;

	// A pointer moved by a displacement, or was placed at one. Two signals for the two events'
	// reason — `Core/Input.h` has it — and both are emitted per device event rather than coalesced,
	// because the acceleration curve is nonlinear and summing before it is applied is a different
	// pointer. Coalescing belongs to the thing that holds the position.
	Signal<const PointerMotion&> Motion;
	Signal<const PointerPosition&> Position;

	// A button on a mouse, a touchpad, or the barrel of a stylus.
	Signal<const PointerButton&> Button;

	// One scroll increment on one axis, including the one that carries no distance and means the
	// fingers left the pad.
	Signal<const PointerScroll&> Scroll;

	// One contact, one phase. Not one frame: several fingers moving together arrive as several of
	// these with the same instant.
	Signal<const TouchEvent&> Touch;

	// A tablet tool, in range or touching. A hover produces these and no pointer event at all.
	Signal<const ToolEvent&> Tool;

	// A device is gone, and every sequence keyed to it is over without an ending.
	//
	// **It is a signal rather than something a stale id is discovered by**, because the discovery
	// never happens: a touch sequence is a fact a listener holds, and a touchscreen unplugged with a
	// finger on it produces no up and no cancel — so a listener with no notice keeps that contact
	// forever and whatever was tracking it never lets go. The id is emitted before it is freed, so an
	// observer can still compare it against what it is holding.
	Signal<InputDeviceId> Removed;

protected:
	IInput() = default;
};
