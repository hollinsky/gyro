#pragma once

#include <cstdint>

// A session's identity, which is one number and is here because two modules that may not see each
// other both have to name it.
//
// `Session/Control.h` mints one on the first accepted offer for a uid — the agent never invents one
// and only quotes it back in a log line — and `Scene/Output.h` carries
// one to say which session an output is showing. `Scene` is portable and `Session` is not, so the id
// cannot live where it is minted. Docs/Decisions.md decision 87 settles where it goes instead: a type
// two parties across a waist both name lives below both of them rather than in `Seam`, and `Core` is
// where the rest of them already are — `Core/Buffer.h` is here for exactly this reason, and
// `TextureId` moved here rather than growing a translation layer.
//
// **Not a `Core/Handle.h` generational id, and what makes the difference is that nothing is reused.**
// An `OutputId` is generational because a monitor is unplugged and the next one takes its slot, so a
// stale reference names a display that is physically not there. `Session/Control.cpp` counts sessions
// up and never hands the same number out twice, so there is no stale reference for a generation to
// catch. It is also a `u32` on the handover wire, where a packed index and generation would be an ABI
// carrying a representation neither side reads.
enum class SessionId : std::uint32_t
{
	// No session. On an output this is Docs/Architecture.md's *gyro's own scene* — the boot splash, the
	// background between one session and the next, and the recovery console — rather than a missing
	// value somebody has not filled in yet. Every output starts here and returns here when the session
	// it was showing ends.
	None = 0,
};
