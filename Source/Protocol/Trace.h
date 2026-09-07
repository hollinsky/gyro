#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string_view>

#include "Core/Trace.h"

// What a client did, said in the words a person reading a capture needs.
//
// **A client gets a row of its own and the row is named for the program and its pid**, which is the
// whole of this file. `Core/Trace.h` mints the row and holds the name table; what belongs here is the
// two things the protocol side knows and it does not — how a client's row is spelled, and what may
// never be spelled onto one.
//
// **The pid is in the name because the interesting half of a client's frame loop is not gyro's to
// record.** A `.pftrace` concatenates with a system trace (139), so a capture merged with `traced`
// carries the client's own `sched_switch` rows beside gyro's — and those rows are named by pid.
// Without it a person reading *the window stuttered* can see gyro's side of the conversation and
// cannot tell a client whose render thread was descheduled from one gyro kept waiting, which is the
// first question the row exists to answer. It is the same argument that puts the real kernel thread id
// on gyro's own thread rows.
//
// **A window title never goes in a trace, and `app_id` does.** A title is the document a person has
// open — a file name, a message, a patient record — and a capture is a file that gets mailed to
// somebody to look at. `app_id` is the name of a program and says nothing about what is in the window.
// A row named `firefox (pid 4123)` is as much as a reader needs to find the client they are chasing;
// `ClientXdgToplevel::Title()` exists a few lines from the call that names the row, and it must stay
// unread from here.
//
// **Nothing in this file is reachable from the frame thread.** Naming takes the name table's mutex and
// claiming allocates nothing but does take it too, both of which are fine on the dispatch thread and
// neither of which may happen inside `Core/FrameSection.h`'s guard. Everything in `Protocol` runs on
// the dispatch thread, which is what makes that a statement rather than a rule to remember.

// How long a program's name may be before the pid it is followed by would not fit. The pid is what
// survives truncation because it is the half that joins to a system trace: a `chromium-brow (pid
// 4123)` still finds its scheduling, where a full name with the number cut off finds nothing.
inline constexpr std::size_t ClientProgramLimit = TraceNameLimit - sizeof(" (pid 4294967295)");

// Name a client's row for its program and its pid, or for the pid alone before the client has said
// what it is.
//
// **Called again to rename, which is the ordinary path rather than a correction.** A connection is
// accepted before a single request arrives, so at that instant the pid is the only thing known about
// it; a program says what it is at `xdg_toplevel.set_app_id`, several round trips later. A row that
// waited for the name would have the connect mark and often the first commits on a row labelled
// nothing at all.
//
// `TraceThread` is what an exhausted pool answers with (`ClaimTraceClient`), and naming it here would
// relabel the dispatch thread's own row with a client's name — so it is refused rather than passed
// through to a table that would happily take it.
inline void NameClientTrace(std::uint16_t row, std::string_view program, std::uint32_t pid)
{
	if (row == TraceThread)
	{
		return;
	}

	TraceName spelled{};
	char* const first = spelled.data();
	char* at = first;

	if (!program.empty())
	{
		const std::size_t kept = std::min(program.size(), ClientProgramLimit);

		at = std::copy_n(program.data(), kept, at);
		*at++ = ' ';
		*at++ = '(';
	}

	at = std::copy_n("pid ", 4, at);

	// The end is the last byte of the buffer rather than one past it, because a name is handed on as a
	// `string_view` over what was written and the closing parenthesis still has to fit.
	const std::to_chars_result written = std::to_chars(at, first + TraceNameLimit - 1, pid);

	at = written.ec == std::errc{} ? written.ptr : at;

	if (!program.empty())
	{
		*at++ = ')';
	}

	NameTraceScope(row, std::string_view{ first, static_cast<std::size_t>(at - first) });
}

namespace Detail
{

// What the Wayland server has drained since the dispatch thread last looked, which is a plain
// `std::uint64_t` because the only thread that touches it is the one running every handler that
// increments it and the `Server::Poll` that reads it.
inline std::uint64_t ClientCommits = 0;

} // namespace Detail

// One more content update read off a client's socket. Counted here rather than sampled per surface
// because what the figure answers is about the *loop* — how much work one wake of the dispatch thread
// had to do — and a counter per client would need a reader to add sixty-four rows up by eye.
inline void NoteClientCommit() noexcept
{
	++Detail::ClientCommits;
}

// What this drain took, and the count starts again from zero.
//
// **Sampled on every drain including the empty ones**, which is `Server::Poll`'s to do. A counter
// emitted only when it is nonzero draws a horizontal line from the last burst to the next one, and a
// person reading that sees a backlog that never cleared rather than a queue that emptied immediately.
[[nodiscard]] inline std::uint64_t TakeClientCommits() noexcept
{
	const std::uint64_t drained = Detail::ClientCommits;

	Detail::ClientCommits = 0;

	return drained;
}
