#include "Protocol/Trace.h"

#include <string>
#include <string_view>
#include <vector>

#include "Testing/Test.h"

// What is worth testing here is the name a person reads off a client's row, because it is the one
// thing in this file a reader depends on being exactly right: the pid is what joins gyro's capture to
// a system trace, and a pid that fell off the end of a truncated name joins to nothing.

namespace
{
// The name a row is carrying, or empty for a row nothing has named.
[[nodiscard]] std::string NameOf(std::uint16_t row)
{
	TraceName held{};

	if (!ReadTraceScopeName(row, held))
	{
		return {};
	}

	return std::string{ std::string_view{ held.data() } };
}

// A row of the client pool, given back however the test leaves.
struct ClaimedRow
{
	ClaimedRow() : Row{ ClaimTraceClient() } {}

	~ClaimedRow() { ReleaseTraceClient(Row); }

	ClaimedRow(const ClaimedRow&) = delete;
	ClaimedRow& operator=(const ClaimedRow&) = delete;
	ClaimedRow(ClaimedRow&&) = delete;
	ClaimedRow& operator=(ClaimedRow&&) = delete;

	std::uint16_t Row = TraceThread;
};
} // namespace

GYRO_TEST(ProtocolTrace, AConnectionIsNamedForItsPidBeforeItSaysAnythingElse)
{
	ClaimedRow claimed;

	GYRO_REQUIRE(claimed.Row != TraceThread);

	// A connection is accepted before a single request arrives, so this is what the row is called for
	// the whole of the handshake — and it is already the half that matters, because a merged system
	// trace names the client's own threads by this number.
	NameClientTrace(claimed.Row, {}, 4123);

	GYRO_CHECK(NameOf(claimed.Row) == "pid 4123");
}

GYRO_TEST(ProtocolTrace, AProgramRenamesTheRowAndKeepsThePid)
{
	ClaimedRow claimed;

	GYRO_REQUIRE(claimed.Row != TraceThread);

	NameClientTrace(claimed.Row, {}, 4123);
	NameClientTrace(claimed.Row, "firefox", 4123);

	// Renaming is the ordinary path rather than a correction: `xdg_toplevel.set_app_id` arrives several
	// round trips after the connection did.
	GYRO_CHECK(NameOf(claimed.Row) == "firefox (pid 4123)");
}

GYRO_TEST(ProtocolTrace, ALongProgramIsCutAndThePidSurvives)
{
	ClaimedRow claimed;

	GYRO_REQUIRE(claimed.Row != TraceThread);

	NameClientTrace(claimed.Row, std::string(200, 'x'), 4294967295U);

	const std::string name = NameOf(claimed.Row);

	// The name fits the table, and what it ends with is the number a person types into a system trace's
	// filter. A name truncated the other way — the program kept and the pid cut — would be a row that
	// looks fine and joins to nothing.
	GYRO_CHECK(name.size() < TraceNameLimit);
	GYRO_CHECK(name.ends_with("(pid 4294967295)"));
	GYRO_CHECK(name.starts_with("xxx"));
}

GYRO_TEST(ProtocolTrace, TheDispatchThreadsOwnRowIsNeverRenamed)
{
	// What an exhausted pool answers with, and the sixty-fifth client's events land there deliberately.
	// Naming it would relabel the dispatch thread's own row with one client's name, which is a person
	// reading the whole compositor's loop as though it belonged to Firefox.
	NameClientTrace(TraceThread, "firefox", 4123);

	GYRO_CHECK(NameOf(TraceThread).empty());
}

GYRO_TEST(ProtocolTrace, TheDrainCountIsTakenAndStartsAgain)
{
	// Whatever an earlier test left, because the counter is the process's.
	(void)TakeClientCommits();

	NoteClientCommit();
	NoteClientCommit();

	GYRO_CHECK(TakeClientCommits() == 2);

	// And a drain that took nothing answers zero rather than the last burst, which is the whole reason
	// `Server::Poll` samples it every time: a counter that only ever went up would draw a plateau a
	// reader takes for a backlog that never cleared.
	GYRO_CHECK(TakeClientCommits() == 0);
}

GYRO_TEST(ProtocolTrace, TheRowPoolRunsOutRatherThanFailing)
{
	std::vector<std::uint16_t> held;

	// Every row there is, however many an earlier test is still holding. The pool is round-robin, so
	// this ends rather than cycling.
	for (std::size_t attempt = 0; attempt < TracedClients; ++attempt)
	{
		const std::uint16_t row = ClaimTraceClient();

		if (row == TraceThread)
		{
			break;
		}

		held.push_back(row);
	}

	GYRO_CHECK(ClaimTraceClient() == TraceThread);

	for (const std::uint16_t row : held)
	{
		ReleaseTraceClient(row);
	}

	// And the pool is whole again afterwards, which is what makes exhaustion a state a long-running
	// compositor recovers from rather than one it degrades into.
	const ClaimedRow again;

	GYRO_CHECK(again.Row != TraceThread);
}
