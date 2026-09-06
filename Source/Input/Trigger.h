#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Signal.h"
#include "Input/Chord.h"
#include "Seam/EventSource.h"

// The chord's verbs, asked for from outside the process. A development mechanism and nothing else.
//
// **What it is for is that a person cannot take a screenshot of the thing they are debugging.**
// `Ctrl+Alt+Esc S` writes what the panel is showing, and the frame it writes is the frame the key
// was pressed on — so anybody who needs to look at a capture has to be sitting at the machine with
// their hands on it, and cannot be a script, a test that just arranged a window, or somebody working
// on gyro from another terminal. This is the same three verbs reachable by writing a letter into a
// pipe: `echo s > /run/gyro/chord` takes the picture, `t` writes a trace, `q` stops the compositor.
// The kernel's magic SysRq is the shape being copied, down to the vocabulary being one letter — and
// [Chord.h](Chord.h)'s `Verbs` is where both spellings of a verb are declared, so a letter typed on a
// keyboard and a letter written here cannot come to mean different things.
//
// **It is off unless the command line asks for it, and that is a security boundary rather than
// tidiness.** One of the verbs stops the compositor, and gyro is one process serving every session on
// the machine — so a pipe anybody can write to is a pipe anybody can end everybody's session with.
// There is no authentication here and there should not be one: the answer to *who may ask gyro to do
// this* is `Session/Control.h`'s, which has `SO_PEERCRED` and a uid behind it, and a debugging pipe
// that grew a credential check would be a second answer to that question, drifting. So the whole of
// the protection is that a run without `--chord-pipe` has no pipe at all, and the flag's own
// documentation says what taking it costs.
//
// **Rejected: the shell's system socket.** gyro binds a `Trust::System` listener for the shell that
// will eventually run on it, and folding these verbs into that listener was the obvious saving. It is
// wrong on the verb rather than on the mechanism: `q` ends every session on the machine, and a shell
// — which is a program a person's desktop restarts, updates and crashes — must never be able to do
// that. The tiers are about what a party is *trusted with*, and nothing about being the shell earns
// the power to switch the computer off.
//
// **Rejected: a signal.** `SIGUSR1` already writes a trace and is the reason the trace verb is cheap,
// but there are three verbs and two user signals, and the next one would have no signal left. A pipe
// carries a vocabulary; a signal carries the fact that it arrived.
//
// **A FIFO rather than a socket**, because the whole of the client side has to be `echo`. A socket
// would want a connect, and the point of this is that somebody debugging gyro at two in the morning
// over ssh does not have to write a program to press a key.

namespace Input
{
// Where the pipe goes when the command line names no path. Under the runtime directory gyro already
// owns, rather than in `/tmp` where anything on the machine can have made the file first.
inline constexpr std::string_view DefaultTriggerPath = "/run/gyro/chord";

// A named pipe, and the verbs written into it.
class ChordTrigger final : public IEventSource
{
public:
	~ChordTrigger() override;

	ChordTrigger(const ChordTrigger&) = delete;
	ChordTrigger& operator=(const ChordTrigger&) = delete;
	ChordTrigger(ChordTrigger&&) = delete;
	ChordTrigger& operator=(ChordTrigger&&) = delete;

	// Make the pipe if it is not there, and open it.
	//
	// **Opened read-write, which is the whole trick and is worth the sentence.** A FIFO with no writer
	// reads end-of-file, and a reader polling one that has gone to end-of-file is a descriptor that is
	// permanently readable — a spin on the dispatch thread for the rest of the run, every time
	// somebody's `echo` finishes. Holding a write end of gyro's own means the pipe never has zero
	// writers, so a drain that finds nothing is `EAGAIN` and the descriptor goes quiet again.
	//
	// **An existing path that is not a FIFO is refused rather than unlinked.** The path is named on a
	// command line and a typo that pointed at something else would be gyro deleting it.
	[[nodiscard]] static Result<std::unique_ptr<ChordTrigger>> Open(std::string path);

	[[nodiscard]] RawFd Descriptor() const noexcept override { return m_Pipe.Borrow(); }

	// Read what was written and emit a verb per letter. Anything that is not one — a newline, a space,
	// a letter naming nothing — is skipped, because the ordinary way to write to this is `echo` and
	// what `echo` appends is a newline.
	[[nodiscard]] Result<void> Drain() override;

	// What was asked for, emitted on the dispatch thread that drained it — which is the same thread
	// the keyboard's verbs arrive on, so the composition root does not have to know which one it was.
	Signal<ChordAction> Action;

private:
	explicit ChordTrigger(Fd pipe, std::string path) noexcept : m_Pipe{ std::move(pipe) }, m_Path{ std::move(path) } {}

	Fd m_Pipe;

	// Kept so that the pipe can be removed when the run ends: a stale FIFO nobody is reading is a
	// person's `echo` blocking forever with no compositor behind it.
	std::string m_Path;
};
} // namespace Input
