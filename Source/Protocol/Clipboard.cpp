#include "Protocol/Clipboard.h"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <format>
#include <cstring>
#include <utility>

#include "Protocol/Context.h"
#include "Protocol/Data.h"
#include "Protocol/Shell.h"
#include "Protocol/Surface.h"

namespace
{
// The text names gyro will answer to out of its own cache, best first. **Both spellings of the UTF-8
// parameter are here on purpose**: the mime grammar makes the parameter case-insensitive and toolkits
// are split down the middle over which one they send, so a compositor that offered only one would be
// invisible to half of them for a string it is holding.
constexpr std::array TextMimes{
	std::string_view{ "text/plain;charset=utf-8" },
	std::string_view{ "text/plain;charset=UTF-8" },
	std::string_view{ "UTF8_STRING" },
	std::string_view{ "text/plain" },
	std::string_view{ "STRING" },
	std::string_view{ "TEXT" },
};

// The hint a password manager sets to say *do not keep this*.
constexpr std::string_view SensitiveMime{ "x-kde-passwordManagerHint" };

// Whether a blob held as `held` may honestly be handed to a client asking for `asked`.
//
// Any text type gyro kept is UTF-8 or a subset of it — the ranking below is what makes that true, by
// preferring the names that say so — and every other text name is that same byte sequence under a
// different label. A non-text request is refused, which is the offer list telling the truth.
[[nodiscard]] bool Serves(std::string_view held, std::string_view asked) noexcept
{
	return held == asked || (ClipboardTextRank(held) > 0 && ClipboardTextRank(asked) > 0);
}

// What to call a client in a log line a person might read.
//
// The application's own `app_id` where it has a mapped window, because that is the name a person would
// use; the pid otherwise, which covers a client that has not opened a window yet and is exactly the
// case worth being able to see.
[[nodiscard]] std::string Describe(const HostContext& context, wl_client* client)
{
	if (client == nullptr)
	{
		return "a client that has gone";
	}

	for (const ClientXdgSurface* const window : context.Windows())
	{
		const ClientXdgToplevel* const toplevel = window->Toplevel();
		const ClientSurface* const surface = window->Content();

		if (toplevel == nullptr || surface == nullptr || toplevel->AppId().empty())
		{
			continue;
		}

		if (surface->Object().WireClient() == client)
		{
			return std::string{ toplevel->AppId() };
		}
	}

	pid_t pid = 0;
	uid_t uid = 0;
	gid_t gid = 0;

	wl_client_get_credentials(client, &pid, &uid, &gid);

	return std::format("pid {}", pid);
}
} // namespace

int ClipboardTextRank(std::string_view mime) noexcept
{
	if (mime == "text/plain;charset=utf-8" || mime == "text/plain;charset=UTF-8")
	{
		return 4;
	}

	if (mime == "UTF8_STRING")
	{
		return 3;
	}

	if (mime == "text/plain")
	{
		return 2;
	}

	if (mime == "STRING" || mime == "TEXT")
	{
		return 1;
	}

	return 0;
}

bool ClipboardIsSensitive(std::span<const std::string> mimes) noexcept
{
	return std::any_of(mimes.begin(), mimes.end(), [](const std::string& mime) noexcept {
		return mime == SensitiveMime;
	});
}

ClipboardFetch::~ClipboardFetch()
{
	if (m_Deadline != nullptr)
	{
		wl_event_source_remove(m_Deadline);
	}

	if (m_Source != nullptr)
	{
		wl_event_source_remove(m_Source);
	}
}

bool ClipboardFetch::Begin(ClientDataSource& source)
{
	std::array<int, 2> ends{ InvalidFd, InvalidFd };

	// `O_NONBLOCK` on both ends because neither may ever park this thread: the read below runs on the
	// dispatch loop, and the *write* end is the descriptor the source client is about to be handed —
	// which shares its open file description with the copy the client receives, so a client writing
	// more than the pipe holds would get `EAGAIN` rather than blocking. That is the client's business
	// to handle and every toolkit does; what matters here is that gyro is not the one waiting.
	if (::pipe2(ends.data(), O_CLOEXEC | O_NONBLOCK) < 0)
	{
		spdlog::warn("no pipe to keep the clipboard with: {}", std::strerror(errno));

		return false;
	}

	m_Read = Fd{ ends[0] };

	// libwayland duplicates the descriptor into the connection's own out-of-band queue, so this copy is
	// gyro's to close and closing it is what lets the source see end-of-file when it is done writing.
	const Fd write{ ends[1] };

	source.Object().Send(m_Mime.c_str(), write.Borrow());

	m_Source = wl_event_loop_add_fd(m_Loop, m_Read.Get(), WL_EVENT_READABLE, &ClipboardFetch::OnReadable, this);

	if (m_Source == nullptr)
	{
		return false;
	}

	m_Deadline = wl_event_loop_add_timer(m_Loop, &ClipboardFetch::OnDeadline, this);

	if (m_Deadline != nullptr)
	{
		wl_event_source_timer_update(m_Deadline, static_cast<int>(ClipboardFetchDeadlineMs));
	}

	return true;
}

int ClipboardFetch::OnReadable(int descriptor, std::uint32_t mask, void* data) noexcept
{
	static_cast<void>(mask);

	auto* const self = static_cast<ClipboardFetch*>(data);

	// A page at a time, and around again on the next wakeup rather than in a loop here: a source that
	// keeps writing must not be able to hold the dispatch thread for as long as it feels like, which is
	// the same argument the non-blocking read rests on.
	std::array<char, 4096> chunk{};
	const ssize_t read = ::read(descriptor, chunk.data(), chunk.size());

	if (read < 0)
	{
		if (errno == EAGAIN || errno == EINTR)
		{
			return 0;
		}

		self->Finish(false);

		return 0;
	}

	if (read == 0)
	{
		self->Finish(true);

		return 0;
	}

	self->m_Bytes.append(chunk.data(), static_cast<std::size_t>(read));

	// **Abandoned rather than truncated**, per `ClipboardEntryLimit`: a paste that silently loses its
	// tail is a document with a sentence missing and nothing anywhere saying so.
	if (self->m_Bytes.size() > ClipboardEntryLimit)
	{
		self->Finish(false);
	}

	return 0;
}

int ClipboardFetch::OnDeadline(void* data) noexcept
{
	static_cast<ClipboardFetch*>(data)->Finish(false);

	return 0;
}

void ClipboardFetch::Finish(bool kept)
{
	// The clipboard destroys this inside the call, so nothing may touch a member afterwards.
	m_Clipboard->Fetched(std::move(m_Mime), kept ? std::move(m_Bytes) : std::string{}, kept);
}

ClipboardWrite::~ClipboardWrite()
{
	if (m_Deadline != nullptr)
	{
		wl_event_source_remove(m_Deadline);
	}

	if (m_Source != nullptr)
	{
		wl_event_source_remove(m_Source);
	}
}

bool ClipboardWrite::Begin(Fd fd)
{
	m_Write = std::move(fd);

	// The receiver's descriptor arrives however the client made it, which is usually blocking. Setting
	// it here is safe and necessary in equal measure: the flag lives on the open file description, and
	// the client holds the *read* end, which is a different description entirely.
	const int flags = ::fcntl(m_Write.Get(), F_GETFL, 0);

	if (flags < 0 || ::fcntl(m_Write.Get(), F_SETFL, flags | O_NONBLOCK) < 0)
	{
		return false;
	}

	// Most pastes are a line of text and fit in the pipe whole, so the common case never arms anything.
	if (Push())
	{
		return false;
	}

	m_Source = wl_event_loop_add_fd(m_Loop, m_Write.Get(), WL_EVENT_WRITABLE, &ClipboardWrite::OnWritable, this);

	if (m_Source == nullptr)
	{
		return false;
	}

	m_Deadline = wl_event_loop_add_timer(m_Loop, &ClipboardWrite::OnDeadline, this);

	if (m_Deadline != nullptr)
	{
		wl_event_source_timer_update(m_Deadline, static_cast<int>(ClipboardWriteDeadlineMs));
	}

	return true;
}

int ClipboardWrite::OnWritable(int descriptor, std::uint32_t mask, void* data) noexcept
{
	static_cast<void>(descriptor);
	static_cast<void>(mask);

	auto* const self = static_cast<ClipboardWrite*>(data);

	if (self->Push())
	{
		// Destroys this, so nothing below may touch a member.
		self->m_Clipboard->Finished(*self);
	}

	return 0;
}

int ClipboardWrite::OnDeadline(void* data) noexcept
{
	auto* const self = static_cast<ClipboardWrite*>(data);

	self->m_Clipboard->Finished(*self);

	return 0;
}

bool ClipboardWrite::Push() noexcept
{
	while (m_Sent < m_Bytes->size())
	{
		const ssize_t written = ::write(m_Write.Get(), m_Bytes->data() + m_Sent, m_Bytes->size() - m_Sent);

		if (written < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}

			// **`EPIPE` is the ordinary end of a paste rather than a failure**: a toolkit that only wanted
			// the first line closes its end, and there is no `SIGPIPE` to worry about because libwayland's
			// display already blocks it for the process.
			return errno != EAGAIN;
		}

		m_Sent += static_cast<std::size_t>(written);
	}

	return true;
}

SessionClipboard::~SessionClipboard()
{
	Close();
}

void SessionClipboard::Close() noexcept
{
	m_Fetch.reset();
	m_Writes.clear();
	m_Source = nullptr;
	m_Held = {};
	m_Readers.clear();
	m_History.clear();
}

bool SessionClipboard::IsEmpty() const noexcept
{
	return m_Source == nullptr && m_Held.Bytes == nullptr;
}

void SessionClipboard::Set(ClientDataSource* source)
{
	// **What was held goes into the history before it is replaced**, which is the only moment the bytes
	// are in hand: once the offer is superseded the source may destroy itself and there is nothing left
	// to ask.
	Remember();

	// **The application that was offering is told it has been replaced**, which is the protocol's own
	// `cancelled` and is what a command-line `wl-copy` waits for before it exits. A compositor that
	// skipped it would leave one process per copy sitting on the machine for the rest of the session.
	if (m_Source != nullptr && m_Source != source)
	{
		m_Source->Cancel();
	}

	// A fetch for the selection being replaced is answering a question nobody is asking any more.
	m_Fetch.reset();

	m_Held = {};
	m_Readers.clear();
	m_Source = source;

	++m_Generation;

	if (source != nullptr)
	{
		Fetch(*source);
	}
}

void SessionClipboard::Forget(const ClientDataSource& source)
{
	if (m_Source != &source)
	{
		return;
	}

	m_Source = nullptr;

	// A fetch outliving its source is one whose pipe will read end-of-file at whatever it had, which is
	// the right answer: an application that wrote its text and then exited has been read.
	if (m_Held.Bytes == nullptr && m_Fetch == nullptr)
	{
		// Nothing was kept, so the selection is genuinely gone — a non-text copy, or a source that asked
		// not to be remembered. The generation moves so that offers already handed out go inert.
		++m_Generation;
	}
}

std::vector<std::string> SessionClipboard::Offered() const
{
	std::vector<std::string> offered;

	if (m_Source != nullptr)
	{
		const std::span<const std::string> mimes = m_Source->Mimes();

		offered.assign(mimes.begin(), mimes.end());

		return offered;
	}

	if (m_Held.Bytes == nullptr)
	{
		return offered;
	}

	offered.push_back(m_Held.Mime);

	for (const std::string_view alias : TextMimes)
	{
		if (alias != m_Held.Mime)
		{
			offered.emplace_back(alias);
		}
	}

	return offered;
}

void SessionClipboard::Serve(std::string_view mime, Fd fd, wl_client* reader)
{
	Noticed(reader, mime);

	// **A live source is handed the receiver's own descriptor**, so the bytes go from one application
	// to the other and never enter this process. That is the protocol's design and it is the right one:
	// it costs the compositor nothing whatever the size of what is being pasted, and what arrives is
	// what the application would say now.
	if (m_Source != nullptr)
	{
		if (m_Source->Offers(mime))
		{
			m_Source->Object().Send(std::string{ mime }.c_str(), fd.Borrow());
		}

		return;
	}

	if (m_Held.Bytes == nullptr || m_Loop == nullptr || !Serves(m_Held.Mime, mime))
	{
		return;
	}

	auto write = std::make_unique<ClipboardWrite>(*this, *m_Loop, m_Held.Bytes);

	if (write->Begin(std::move(fd)))
	{
		m_Writes.push_back(std::move(write));
	}
}

void SessionClipboard::Fetched(std::string mime, std::string bytes, bool kept)
{
	if (kept && !bytes.empty())
	{
		m_Held.Mime = std::move(mime);
		m_Held.Bytes = std::make_shared<const std::string>(std::move(bytes));
	}

	m_Fetch.reset();
}

void SessionClipboard::Finished(const ClipboardWrite& write) noexcept
{
	std::erase_if(m_Writes, [&write](const std::unique_ptr<ClipboardWrite>& held) noexcept {
		return held.get() == &write;
	});
}

void SessionClipboard::Fetch(ClientDataSource& source)
{
	if (m_Loop == nullptr)
	{
		return;
	}

	const std::span<const std::string> mimes = source.Mimes();

	if (ClipboardIsSensitive(mimes))
	{
		// Offered live and never kept. Said out loud because a person who later finds their password is
		// not in the clipboard history should be able to find out why.
		spdlog::debug("not keeping a copy of a selection that asked not to be remembered");

		return;
	}

	const std::string* best = nullptr;

	for (const std::string& mime : mimes)
	{
		if (ClipboardTextRank(mime) > (best == nullptr ? 0 : ClipboardTextRank(*best)))
		{
			best = &mime;
		}
	}

	if (best == nullptr)
	{
		return;
	}

	auto fetch = std::make_unique<ClipboardFetch>(*this, *m_Loop, *best);

	if (fetch->Begin(source))
	{
		m_Fetch = std::move(fetch);
	}
}

void SessionClipboard::Remember()
{
	if (m_Held.Bytes == nullptr)
	{
		return;
	}

	if (!m_History.empty() && *m_History.front().Bytes == *m_Held.Bytes)
	{
		return;
	}

	m_History.insert(m_History.begin(), m_Held);

	std::size_t total = 0;

	for (std::size_t at = 0; at < m_History.size(); ++at)
	{
		total += m_History[at].Bytes->size();

		if (at + 1 >= ClipboardHistoryDepth || total > ClipboardHistoryLimit)
		{
			m_History.resize(at + 1);

			break;
		}
	}
}

void SessionClipboard::Noticed(wl_client* reader, std::string_view mime)
{
	if (reader == nullptr || m_Context == nullptr)
	{
		return;
	}

	// An application reading back what it itself offered is not a read of somebody else's clipboard,
	// and toolkits do it constantly while a person is selecting text.
	if (m_Source != nullptr && m_Source->Object().WireClient() == reader)
	{
		return;
	}

	if (std::find(m_Readers.begin(), m_Readers.end(), reader) != m_Readers.end())
	{
		return;
	}

	m_Readers.push_back(reader);

	spdlog::info("{} read the clipboard as {}", Describe(*m_Context, reader), mime);
}

SessionClipboard* SessionClipboards::For(HostContext& context, wl_event_loop* loop, SessionId session)
{
	for (const Held& held : m_Clipboards)
	{
		if (held.Session == session)
		{
			return held.Clipboard.get();
		}
	}

	if (loop == nullptr)
	{
		return nullptr;
	}

	m_Clipboards.push_back(Held{ .Session = session, .Clipboard = std::make_unique<SessionClipboard>(context, loop) });

	return m_Clipboards.back().Clipboard.get();
}

void SessionClipboards::Close(SessionId session) noexcept
{
	std::erase_if(m_Clipboards, [session](const Held& held) noexcept { return held.Session == session; });
}

void SessionClipboards::Close() noexcept
{
	m_Clipboards.clear();
}
