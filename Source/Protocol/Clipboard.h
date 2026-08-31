#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Fd.h"
#include "Core/Session.h"

struct wl_client;
struct wl_event_loop;
struct wl_event_source;

class ClientDataSource;
class HostContext;
class SessionClipboard;

// The clipboard: what a person copied, held by the compositor rather than by the application they
// copied it from.
//
// **This is where gyro departs from every other Wayland compositor, and the departure is the whole
// point of the file.** `wl_data_device` makes the selection an object *the source client owns*: the
// bytes never exist anywhere but in that client, and a paste is a pipe the compositor hands over so
// the two applications can talk. So when the application closes, the clipboard empties. A person
// copies a line out of a terminal, closes it, pastes into a browser and gets nothing — and there is
// nothing wrong with any of the three programs. Every compositor behaves this way, and the usual
// answer is a clipboard manager: a daemon that takes the selection *away* from the application in
// order to hold it, which makes a third party the owner of everything anybody copies and breaks the
// protocol's own account of who is offering what.
//
// gyro holds the text itself instead. At the moment a selection is set, the compositor asks the source
// for one text type and keeps the answer; when the source goes away the offer stays and the paste
// still works. That is not a manager and there is nothing to install: it is the system layer doing
// what a person already believes their system does.
//
// **What is cached is one text type and nothing else, and the bound is what makes this safe.** The
// tempting version reads *every* type a source offers, and it is the wrong one for a reason a person
// feels rather than measures: generating a format is work the application does on demand, so a
// compositor that asks for all of them turns every Ctrl+C in a spreadsheet into the app rendering
// ODF, HTML, RTF and a bitmap that nobody will ever paste. Text is what is cheap in every toolkit,
// and it is also what a person notices losing. An image, a file list or a rich document behaves
// exactly as it does everywhere else — it lives in the application and dies with it — and the offer
// gyro publishes says so, because the mime list shrinks to what is actually still available.
//
// **The fetch never blocks the dispatch thread, and on this compositor that is not a nicety.** gyro
// runs `SCHED_FIFO`; a blocking read on a pipe whose other end belongs to an application that has
// stopped is the whole machine, every session, gone. So the read is armed on the display's event loop
// exactly as a held commit's acquire point is ([ExplicitSync.h](ExplicitSync.h)) and completes
// whenever the client gets round to it, with a deadline past which the copy is simply not cached.
//
// **A paste from a live source still goes client to client.** The cache is a fallback rather than a
// path: while the application that copied is running, `wl_data_source.send` hands it the receiving
// client's own descriptor and the bytes never enter this process. That keeps a hundred-megabyte image
// paste at zero cost here, and it means what a person pastes is always what the application would say
// now rather than what it said at copy time.
//
// **Reading the clipboard is recorded, because on every other system it is now something a person is
// told about.** A phone says *Notes pasted from Safari*; a desktop compositor says nothing at all,
// and an application that reads the selection every time it gains focus is indistinguishable from one
// that does it once when a person pressed Ctrl+V. gyro logs the first read of each selection by each
// application, which is the fact a notification would render — the surfacing needs a shell and a
// protocol to carry it, and that is Docs/Open.md's.

// The most gyro will hold for one selection. A megabyte is far past any plausible copied *text* — it
// is a quarter of a million words — and it is the number that decides what a person loses: a copy
// larger than this is not cached at all rather than cached truncated, because half a paste that
// arrives silently is worse than an empty one.
inline constexpr std::size_t ClipboardEntryLimit = 1024 * 1024;

// How many past selections a session keeps. Nothing surfaces them yet.
inline constexpr std::size_t ClipboardHistoryDepth = 16;

// The most a session's history may hold across every entry, evicting oldest first. A person who
// copies megabyte pastes all afternoon costs four of them rather than sixteen.
inline constexpr std::size_t ClipboardHistoryLimit = 4 * 1024 * 1024;

// How long a source has to answer gyro's request for the text before the copy stops being cached. Two
// seconds is far past a toolkit writing a string into a pipe and short enough that a wedged
// application is not holding a descriptor here for the life of the session.
inline constexpr std::uint32_t ClipboardFetchDeadlineMs = 2000;

// How long a paste out of the cache waits on a client that is not reading it. A pipe holds 64 KiB
// without anybody reading, so a receiver that has consumed nothing in half a minute has stopped.
inline constexpr std::uint32_t ClipboardWriteDeadlineMs = 30000;

// One selection as gyro holds it: the type it was asked for and the bytes that came back.
//
// The bytes are shared because a paste in flight outlives the selection — a person may copy something
// else while a slow reader is still draining the last one, and a write that lost its buffer halfway
// would hand that reader a truncated document.
struct ClipboardEntry
{
	std::string Mime;

	std::shared_ptr<const std::string> Bytes;
};

// Whether gyro will hold a copy of this type, and how much it would rather have it than another.
//
// **Preference order rather than a set**, because a source offers several and the one gyro keeps has
// to be the one that survives a round trip: a UTF-8 type can be honestly re-offered as every other
// text name, and `text/plain` alone cannot, since nothing in it says what the encoding was. Zero is
// *not text, do not hold it*.
[[nodiscard]] int ClipboardTextRank(std::string_view mime) noexcept;

// Whether this source is asking not to be remembered.
//
// **`x-kde-passwordManagerHint` is the one convention there is**, and password managers, `wl-clipboard`
// and Klipper all already speak it. A compositor that holds the clipboard forever is exactly the
// program that must honour it: the thing a person copies out of a vault is the thing that must not
// outlive the window it came from. A source that sets it is offered live and never cached, never
// entered in the history, and its bytes never touch this process.
[[nodiscard]] bool ClipboardIsSensitive(std::span<const std::string> mimes) noexcept;

// gyro asking a source for the text it just offered, once, in the background.
//
// One of these is alive for at most `ClipboardFetchDeadlineMs` after a `set_selection`, and it is
// destroyed by the answer arriving, by the deadline, or by the selection being replaced under it.
class ClipboardFetch
{
public:
	ClipboardFetch(SessionClipboard& clipboard, wl_event_loop& loop, std::string mime) noexcept
		: m_Clipboard{ &clipboard }, m_Loop{ &loop }, m_Mime{ std::move(mime) }
	{}

	~ClipboardFetch();

	ClipboardFetch(const ClipboardFetch&) = delete;
	ClipboardFetch& operator=(const ClipboardFetch&) = delete;
	ClipboardFetch(ClipboardFetch&&) = delete;
	ClipboardFetch& operator=(ClipboardFetch&&) = delete;

	// Make the pipe, hand the writing end to the source, and arm the reading end. False where the
	// kernel refused a descriptor, which is a copy that is simply not held.
	[[nodiscard]] bool Begin(ClientDataSource& source);

private:
	static int OnReadable(int descriptor, std::uint32_t mask, void* data) noexcept;

	static int OnDeadline(void* data) noexcept;

	// Hand what was read to the clipboard and destroy this. `kept` false abandons the copy, which is
	// what a deadline, an over-long selection and a read error all are.
	void Finish(bool kept);

	SessionClipboard* m_Clipboard = nullptr;
	wl_event_loop* m_Loop = nullptr;

	std::string m_Mime;
	std::string m_Bytes;

	Fd m_Read;
	wl_event_source* m_Source = nullptr;
	wl_event_source* m_Deadline = nullptr;
};

// gyro answering a paste out of its own cache, without ever blocking on the client reading it.
//
// Only ever built where the source is gone; a live source is handed the descriptor directly and this
// process does no I/O at all.
class ClipboardWrite
{
public:
	ClipboardWrite(SessionClipboard& clipboard, wl_event_loop& loop, std::shared_ptr<const std::string> bytes) noexcept
		: m_Clipboard{ &clipboard }, m_Loop{ &loop }, m_Bytes{ std::move(bytes) }
	{}

	~ClipboardWrite();

	ClipboardWrite(const ClipboardWrite&) = delete;
	ClipboardWrite& operator=(const ClipboardWrite&) = delete;
	ClipboardWrite(ClipboardWrite&&) = delete;
	ClipboardWrite& operator=(ClipboardWrite&&) = delete;

	// Take the client's descriptor and start. Answers whether this write still has to be held: false is
	// the common case of a line of text that fitted in the pipe whole, and it is also a descriptor that
	// could not be armed — where the receiver reads end-of-file and pastes nothing, which is the same
	// thing it gets from a source that has no such type.
	[[nodiscard]] bool Begin(Fd fd);

private:
	static int OnWritable(int descriptor, std::uint32_t mask, void* data) noexcept;

	static int OnDeadline(void* data) noexcept;

	// Push what fits. Answers whether this write is finished, however it finished.
	[[nodiscard]] bool Push() noexcept;

	SessionClipboard* m_Clipboard = nullptr;
	wl_event_loop* m_Loop = nullptr;

	std::shared_ptr<const std::string> m_Bytes;
	std::size_t m_Sent = 0;

	Fd m_Write;
	wl_event_source* m_Source = nullptr;
	wl_event_source* m_Deadline = nullptr;
};

// One session's clipboard: the current selection, what gyro kept of it, and what came before.
//
// **Per session rather than per machine**, which [Tier.h](Tier.h) puts in the session-scoped rung and
// which is a distinction only a compositor shaped like this one has to make: gyro serves every user on
// the machine from one process (21), and one person's clipboard reaching another person's windows is
// the disclosure the per-uid split exists to prevent. A per-session compositor gets this right by
// having nowhere else to put it.
class SessionClipboard
{
public:
	explicit SessionClipboard(HostContext& context, wl_event_loop* loop) noexcept
		: m_Context{ &context }, m_Loop{ loop }
	{}

	~SessionClipboard();

	SessionClipboard(const SessionClipboard&) = delete;
	SessionClipboard& operator=(const SessionClipboard&) = delete;
	SessionClipboard(SessionClipboard&&) = delete;
	SessionClipboard& operator=(SessionClipboard&&) = delete;

	// A client set the selection. `source` may be null, which is a client clearing the clipboard and is
	// the one case where what gyro holds is dropped rather than kept: an explicit clear is a person
	// saying the clipboard is empty, and answering it out of a cache would be a compositor arguing.
	void Set(ClientDataSource* source);

	// The source went away. **The selection does not**: this is the moment the whole file exists for,
	// and what happens is that the live offer becomes the cached one. A session with nothing cached
	// ends up empty here, which is every non-text copy and is the behaviour of every other compositor.
	void Forget(const ClientDataSource& source);

	// The live source, or null where the selection is gyro's cached copy or there is none.
	[[nodiscard]] ClientDataSource* Source() const noexcept { return m_Source; }

	// Whether there is anything at all to offer.
	[[nodiscard]] bool IsEmpty() const noexcept;

	// Which serial of the selection this is. An offer carries the number it was made under and is inert
	// once it no longer matches, so a client that kept an old `wl_data_offer` and asked it for bytes
	// gets end-of-file rather than somebody else's copy.
	[[nodiscard]] std::uint32_t Generation() const noexcept { return m_Generation; }

	// What a `wl_data_offer` for the current selection advertises.
	//
	// **The list shrinks when the source goes, and that honesty is the point.** A live source's own
	// list goes out verbatim. What is left afterwards is the one type gyro kept plus the text names it
	// can honestly answer to — a UTF-8 string *is* `text/plain` and `UTF8_STRING` — so a toolkit
	// asking by any of the usual names finds it, and a toolkit asking for `image/png` is told it is not
	// there rather than handed a pipe that closes empty.
	[[nodiscard]] std::vector<std::string> Offered() const;

	// A client asked for the selection's bytes. Takes the descriptor whatever happens: a request that
	// cannot be answered is answered with end-of-file, which is what the protocol gives a receiver for
	// a type nobody has.
	void Serve(std::string_view mime, Fd fd, wl_client* reader);

	// What gyro kept of past selections, newest first. **Nothing reads this yet** — surfacing it needs
	// a shell and a protocol, per Docs/Open.md — and it is built now because the only moment the bytes
	// are reachable is the one the cache above already passes through.
	[[nodiscard]] std::span<const ClipboardEntry> History() const noexcept { return m_History; }

	// Drop everything, including any transfer in flight. Called before the display goes, because an
	// event source outliving its loop is a use-after-free at shutdown rather than a leak.
	void Close() noexcept;

	// `ClipboardFetch` answering. `bytes` empty with `kept` true is a source that offered a type and
	// had nothing in it, which is cached as nothing rather than as an empty string.
	void Fetched(std::string mime, std::string bytes, bool kept);

	// `ClipboardWrite` finishing, however it finished.
	void Finished(const ClipboardWrite& write) noexcept;

	[[nodiscard]] wl_event_loop* Loop() const noexcept { return m_Loop; }

private:
	// Ask the source for the one type worth keeping, or do nothing where there is none — a source
	// offering only an image, or one that asked not to be remembered.
	void Fetch(ClientDataSource& source);

	// Put the current cache at the front of the history, unless it is what is already there. A person
	// who copies the same line twice has copied it once as far as this is concerned.
	void Remember();

	// Log the first read of this selection by this client. **Once per application per copy**, because a
	// toolkit reads the clipboard whenever a window gains focus and a line per read would say a person
	// pasted when they did not.
	void Noticed(wl_client* reader, std::string_view mime);

	HostContext* m_Context = nullptr;
	wl_event_loop* m_Loop = nullptr;

	// The client that is offering, or null. Borrowed: a `wl_data_source` is the client's object and
	// tells us through `Forget` on its way out.
	ClientDataSource* m_Source = nullptr;

	// What gyro kept of the current selection, or an empty mime where it kept nothing.
	ClipboardEntry m_Held;

	// Bumped by every `Set`, so an offer from a superseded selection can tell.
	std::uint32_t m_Generation = 0;

	// Who has already been seen reading this selection, cleared with every `Set`.
	std::vector<wl_client*> m_Readers;

	std::unique_ptr<ClipboardFetch> m_Fetch;

	// Pastes in flight out of the cache. A vector because it is normally empty and never more than the
	// applications a person has pasted into in the last few seconds.
	std::vector<std::unique_ptr<ClipboardWrite>> m_Writes;

	std::vector<ClipboardEntry> m_History;
};

// Every session's clipboard, keyed the way `SessionFloors` is and for the same reason.
class SessionClipboards
{
public:
	// The clipboard for a session, made on first use. Null only before the server is open, which is
	// before any client can have asked.
	[[nodiscard]] SessionClipboard* For(HostContext& context, wl_event_loop* loop, SessionId session);

	// The session ended: its clipboard goes with it, and a person logging out does not leave what they
	// copied behind on a machine somebody else is still using.
	void Close(SessionId session) noexcept;

	// Every clipboard, for the host's teardown.
	void Close() noexcept;

private:
	// A vector and a scan for `SessionFloors`' reason: the count is the people logged into this machine.
	struct Held
	{
		SessionId Session = SessionId::None;

		std::unique_ptr<SessionClipboard> Clipboard;
	};

	std::vector<Held> m_Clipboards;
};
