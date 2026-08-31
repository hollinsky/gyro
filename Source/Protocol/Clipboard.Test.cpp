#include "Protocol/Clipboard.h"

#include <unistd.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <vector>

#include "Protocol/Context.h"
#include "Testing/Test.h"

// What a person loses and what they keep, checked without a client on the other end.
//
// The conversation — a `set_selection` arriving, an offer going out, a toolkit reading it back — is a
// sequence of events over a socket, and Integration/ProtocolRoundTrip.Test.cpp is where gyro's server
// is checked against a demarshaller that is not its own. What that cannot see is the part this
// compositor does differently: which type is worth keeping, what an offer says once the application
// that made it has gone, and that a paste out of gyro's own copy produces the bytes a person copied.

namespace
{
// A loop with no display behind it, which is all the clipboard's background reads and writes need.
class Loop
{
public:
	Loop() : m_Loop{ wl_event_loop_create() } {}

	~Loop()
	{
		if (m_Loop != nullptr)
		{
			wl_event_loop_destroy(m_Loop);
		}
	}

	Loop(const Loop&) = delete;
	Loop& operator=(const Loop&) = delete;

	[[nodiscard]] wl_event_loop* Get() const noexcept { return m_Loop; }

private:
	wl_event_loop* m_Loop = nullptr;
};

// What a client would read out of the descriptor it handed over.
[[nodiscard]] std::string Paste(SessionClipboard& clipboard, std::string_view mime)
{
	std::array<int, 2> ends{ -1, -1 };

	if (::pipe(ends.data()) < 0)
	{
		return {};
	}

	const Fd read{ ends[0] };

	clipboard.Serve(mime, Fd{ ends[1] }, nullptr);

	std::string pasted;
	std::array<char, 256> chunk{};

	for (;;)
	{
		const ssize_t got = ::read(read.Get(), chunk.data(), chunk.size());

		if (got <= 0)
		{
			break;
		}

		pasted.append(chunk.data(), static_cast<std::size_t>(got));
	}

	return pasted;
}
} // namespace

GYRO_TEST(Clipboard, TheTypeWorthKeepingIsTheOneThatSaysItsEncoding)
{
	// **A UTF-8 type outranks bare `text/plain`, and the ranking is what makes the aliasing honest**:
	// what gyro keeps is re-offered under every text name, and it can only do that if it knows the
	// bytes are UTF-8. `text/plain` alone says nothing about the encoding.
	GYRO_CHECK(ClipboardTextRank("text/plain;charset=utf-8") > ClipboardTextRank("text/plain"));
	GYRO_CHECK(ClipboardTextRank("text/plain;charset=UTF-8") > ClipboardTextRank("text/plain"));
	GYRO_CHECK(ClipboardTextRank("UTF8_STRING") > ClipboardTextRank("text/plain"));
	GYRO_CHECK(ClipboardTextRank("text/plain") > ClipboardTextRank("STRING"));

	// Anything that is not text is not held at all, which is the bound the whole design rests on: an
	// image lives in the application that copied it and dies with it, exactly as it does everywhere.
	GYRO_CHECK(ClipboardTextRank("image/png") == 0);
	GYRO_CHECK(ClipboardTextRank("text/uri-list") == 0);
}

GYRO_TEST(Clipboard, ASourceMayAskNotToBeRemembered)
{
	// The one convention there is, and the program that must honour it is exactly the one that holds
	// the clipboard after the window has gone.
	const std::vector<std::string> vault{ "text/plain;charset=utf-8", "x-kde-passwordManagerHint" };
	const std::vector<std::string> ordinary{ "text/plain;charset=utf-8", "text/html" };

	GYRO_CHECK(ClipboardIsSensitive(vault));
	GYRO_CHECK(!ClipboardIsSensitive(ordinary));
}

GYRO_TEST(Clipboard, WhatGyroKeptIsOfferedUnderEveryTextName)
{
	const Loop loop;
	HostContext context;
	SessionClipboard clipboard{ context, loop.Get() };

	GYRO_CHECK(clipboard.IsEmpty());

	clipboard.Fetched("text/plain;charset=utf-8", "the line a person copied", true);

	GYRO_REQUIRE(!clipboard.IsEmpty());

	const std::vector<std::string> offered = clipboard.Offered();

	// A toolkit asks by whichever name it prefers, and the split over the spelling of the UTF-8
	// parameter is real — so the offer carries both, and the older X11 names beside them.
	const auto lists = [&offered](std::string_view mime) {
		return std::find(offered.begin(), offered.end(), mime) != offered.end();
	};

	GYRO_CHECK(lists("text/plain;charset=utf-8"));
	GYRO_CHECK(lists("text/plain;charset=UTF-8"));
	GYRO_CHECK(lists("UTF8_STRING"));
	GYRO_CHECK(lists("text/plain"));

	// The type it was kept under appears once rather than twice.
	GYRO_CHECK(std::count(offered.begin(), offered.end(), "text/plain;charset=utf-8") == 1);

	// And nothing gyro cannot actually produce is claimed.
	GYRO_CHECK(!lists("image/png"));
}

GYRO_TEST(Clipboard, APasteOutOfGyrosOwnCopyIsWhatWasCopied)
{
	const Loop loop;
	HostContext context;
	SessionClipboard clipboard{ context, loop.Get() };

	clipboard.Fetched("text/plain;charset=utf-8", "the line a person copied", true);

	// **The application that copied is gone and the paste still works**, which is the whole file.
	GYRO_CHECK(Paste(clipboard, "text/plain;charset=utf-8") == "the line a person copied");

	// Under a name it was not kept under, because a UTF-8 string is all of these.
	GYRO_CHECK(Paste(clipboard, "text/plain") == "the line a person copied");
	GYRO_CHECK(Paste(clipboard, "UTF8_STRING") == "the line a person copied");

	// And nothing at all for a type nobody has, which is the end-of-file a receiver already handles.
	GYRO_CHECK(Paste(clipboard, "image/png").empty());
}

GYRO_TEST(Clipboard, ClearingTheSelectionEmptiesIt)
{
	const Loop loop;
	HostContext context;
	SessionClipboard clipboard{ context, loop.Get() };

	clipboard.Fetched("text/plain;charset=utf-8", "the line a person copied", true);

	const std::uint32_t before = clipboard.Generation();

	// A client clearing the clipboard is a person saying it is empty, and answering that out of a cache
	// would be the compositor arguing with them.
	clipboard.Set(nullptr);

	GYRO_CHECK(clipboard.IsEmpty());
	GYRO_CHECK(clipboard.Generation() != before);
	GYRO_CHECK(clipboard.Offered().empty());
}

GYRO_TEST(Clipboard, WhatWasCopiedBeforeIsStillHeld)
{
	const Loop loop;
	HostContext context;
	SessionClipboard clipboard{ context, loop.Get() };

	clipboard.Fetched("text/plain;charset=utf-8", "the first line", true);
	clipboard.Set(nullptr);
	clipboard.Fetched("text/plain;charset=utf-8", "the second line", true);
	clipboard.Set(nullptr);

	GYRO_REQUIRE(clipboard.History().size() == 2);

	// Newest first, which is the order anything that ever shows this to a person would want it in.
	GYRO_CHECK(*clipboard.History()[0].Bytes == "the second line");
	GYRO_CHECK(*clipboard.History()[1].Bytes == "the first line");
}

GYRO_TEST(Clipboard, CopyingTheSameThingTwiceIsOneEntry)
{
	const Loop loop;
	HostContext context;
	SessionClipboard clipboard{ context, loop.Get() };

	clipboard.Fetched("text/plain;charset=utf-8", "the same line", true);
	clipboard.Set(nullptr);
	clipboard.Fetched("text/plain;charset=utf-8", "the same line", true);
	clipboard.Set(nullptr);

	// A person who selects the same word again has copied it once as far as a history is concerned, and
	// a list whose top two entries are identical is one nobody can use.
	GYRO_CHECK(clipboard.History().size() == 1);
}

GYRO_TEST(Clipboard, TheHistoryIsBounded)
{
	const Loop loop;
	HostContext context;
	SessionClipboard clipboard{ context, loop.Get() };

	for (std::size_t at = 0; at < ClipboardHistoryDepth * 2; ++at)
	{
		clipboard.Fetched("text/plain;charset=utf-8", std::format("line {}", at), true);
		clipboard.Set(nullptr);
	}

	GYRO_CHECK(clipboard.History().size() == ClipboardHistoryDepth);
	GYRO_CHECK(*clipboard.History().front().Bytes == std::format("line {}", ClipboardHistoryDepth * 2 - 1));
}

GYRO_TEST(Clipboard, ACopyTooLargeToHoldIsNotHeldTruncated)
{
	const Loop loop;
	HostContext context;
	SessionClipboard clipboard{ context, loop.Get() };

	// What a failed fetch looks like from the clipboard's side: nothing is kept. A half-cached
	// selection would paste a document with its tail missing and nothing anywhere saying so.
	clipboard.Fetched("text/plain;charset=utf-8", std::string{}, false);

	GYRO_CHECK(clipboard.IsEmpty());
}

GYRO_TEST(Clipboard, AClipboardEndsWithItsSession)
{
	const Loop loop;
	HostContext context;
	SessionClipboards clipboards;

	SessionClipboard* const first = clipboards.For(context, loop.Get(), static_cast<SessionId>(1));

	GYRO_REQUIRE(first != nullptr);

	// The same session finds the same clipboard, which is what makes two windows of one person share
	// what they copied.
	GYRO_CHECK(clipboards.For(context, loop.Get(), static_cast<SessionId>(1)) == first);

	// And another person's is a different one, which is the whole reason this is keyed at all: gyro
	// serves every session on the machine from one process.
	GYRO_CHECK(clipboards.For(context, loop.Get(), static_cast<SessionId>(2)) != first);

	first->Fetched("text/plain;charset=utf-8", "what one person copied", true);

	clipboards.Close(static_cast<SessionId>(1));

	// Logging out does not leave what was copied on a machine somebody else is still using.
	SessionClipboard* const after = clipboards.For(context, loop.Get(), static_cast<SessionId>(1));

	GYRO_REQUIRE(after != nullptr);
	GYRO_CHECK(after->IsEmpty());
}
