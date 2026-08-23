#include "Wire/Connection.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "Wire/Reader.h"

// POSIX rather than Linux, which is what keeps this module in the portable tier — `<sys/socket.h>`
// and `<sys/un.h>` are not on CMake/CheckPortability.cmake's list and should not be, for decision
// 119's reason: a codec talking to a socket the test created is exactly what "builds and runs on a
// machine with no GPU, no seat, and no compositor" means.
//
// Two spellings here are not POSIX and both are flags rather than headers. `SOCK_CLOEXEC` and
// `SOCK_NONBLOCK` on `socket()` save a pair of `fcntl` calls and, more to the point, close the window
// in which a thread could fork between the two. `MSG_CMSG_CLOEXEC` on `recvmsg` closes the same window
// for a descriptor arriving out of band, and there is no `fcntl` that could replace it — the race is
// over a descriptor that does not exist yet. If a platform ever wants them spelled differently, they
// are three tokens in this file.

namespace Wire
{
namespace
{
// `wl_display`'s two events, and the only opcodes this module knows.
constexpr std::uint16_t DisplayError = 0;
constexpr std::uint16_t DisplayDeleteId = 1;

// One `recvmsg` at a time. libwayland's figure, and it is a buffer size rather than a protocol
// limit — a message larger than this is read across two calls like any other stream.
constexpr std::size_t ReadChunkBytes = 4096;

// What the control message can hold. `SCM_RIGHTS` puts the descriptors in one array, so this is the
// send limit and the receive limit at once.
constexpr std::size_t ControlBytes = CMSG_SPACE(MaxFdsPerMessage * sizeof(int));

// A control buffer, aligned for the header the kernel writes into it. A `std::byte` array is
// otherwise aligned for a byte, and `CMSG_FIRSTHDR` would hand back a misaligned `cmsghdr*`.
struct alignas(::cmsghdr) ControlBuffer
{
	std::array<std::byte, ControlBytes> Bytes{};
};

// The socket, connected to a path. Split out because `Open` has two ways in and only one of them
// builds a path.
[[nodiscard]] Result<Fd> ConnectTo(std::string_view path)
{
	::sockaddr_un address{};
	address.sun_family = AF_UNIX;

	// The struct is fixed at 108 bytes and a path that does not fit is not a path that can be
	// connected to. Checked rather than truncated, because a truncated path names a *different*
	// socket, which either does not exist or belongs to something else entirely.
	if (path.empty() || path.size() >= sizeof address.sun_path)
	{
		return Failure(ENAMETOOLONG, "the wayland socket path does not fit a sockaddr_un");
	}

	std::memcpy(address.sun_path, path.data(), path.size());

	Fd socket{ ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0) };

	if (!socket.IsValid())
	{
		return FailFromErrno("creating a socket for the wayland host");
	}

	// Non-blocking, so this can return `EINPROGRESS` in principle. It does not for `AF_UNIX`: a Unix
	// connect either finds a listening socket and completes, or it does not and fails. There is no
	// handshake to be in the middle of.
	if (::connect(socket.Get(), reinterpret_cast<const ::sockaddr*>(&address), sizeof address) != 0)
	{
		return FailFromErrno("connecting to the wayland host");
	}

	return socket;
}
} // namespace

std::unexpected<Error> Connection::Fail(int code, std::string_view context)
{
	// The first failure is the one kept. Everything after it is a consequence — a half-read message,
	// a write to a socket the host already closed — and reporting the last one would name the
	// symptom.
	if (!m_Error)
	{
		m_Error = Error{ code, context };
	}

	return std::unexpected{ *m_Error };
}

Result<void> Connection::Adopt(Fd socket)
{
	if (!socket.IsValid())
	{
		return Failure(EBADF, "adopting a wayland socket that is not a descriptor");
	}

	// The inherited descriptor arrives with whatever flags its opener left on it. Both matter: a
	// blocking socket would stall the frame thread inside `Drain` for as long as the host is busy,
	// and a descriptor without `FD_CLOEXEC` reaches every process gyro ever spawns.
	const int flags = ::fcntl(socket.Get(), F_GETFL);

	if (flags == -1 || ::fcntl(socket.Get(), F_SETFL, flags | O_NONBLOCK) != 0)
	{
		return FailFromErrno("making the inherited wayland socket non-blocking");
	}

	const int descriptorFlags = ::fcntl(socket.Get(), F_GETFD);

	if (descriptorFlags == -1 || ::fcntl(socket.Get(), F_SETFD, descriptorFlags | FD_CLOEXEC) != 0)
	{
		return FailFromErrno("closing the inherited wayland socket over exec");
	}

	m_Socket = std::move(socket);

	return {};
}

Result<void> Connection::Open()
{
	if (m_Socket.IsValid())
	{
		return Failure(EISCONN, "opening a wayland connection that is already open");
	}

	if (const char* inherited = std::getenv("WAYLAND_SOCKET"); inherited != nullptr)
	{
		// Copied before the unset, because `unsetenv` is entitled to free what `getenv` returned.
		// Unset unconditionally rather than only on success: the variable names a descriptor number,
		// and a number that was wrong for us is wrong for every child too — worse, by the time one
		// runs the number may have been reused for something that is not a socket at all.
		const std::string text{ inherited };

		::unsetenv("WAYLAND_SOCKET");

		int descriptor = -1;
		const char* end = text.data() + text.size();
		const std::from_chars_result parsed = std::from_chars(text.data(), end, descriptor);

		if (parsed.ec != std::errc{} || parsed.ptr != end || descriptor < 0)
		{
			return Failure(EINVAL, "reading the descriptor WAYLAND_SOCKET names");
		}

		return Adopt(Fd{ descriptor });
	}

	const char* display = std::getenv("WAYLAND_DISPLAY");

	if (display == nullptr)
	{
		display = "wayland-0";
	}

	// An absolute `WAYLAND_DISPLAY` is the whole path, which is what every other client does and is
	// how a socket outside the runtime directory is reached at all.
	if (display[0] == '/')
	{
		Result<Fd> socket = ConnectTo(display);

		if (!socket)
		{
			return std::unexpected{ socket.error() };
		}

		return Adopt(std::move(*socket));
	}

	const char* runtime = std::getenv("XDG_RUNTIME_DIR");

	if (runtime == nullptr)
	{
		return Failure(ENOENT, "XDG_RUNTIME_DIR is unset, so there is nowhere to look for a wayland host");
	}

	std::string path{ runtime };
	path += '/';
	path += display;

	Result<Fd> socket = ConnectTo(path);

	if (!socket)
	{
		return std::unexpected{ socket.error() };
	}

	return Adopt(std::move(*socket));
}

Result<void> Connection::Flush()
{
	if (m_Error)
	{
		return std::unexpected{ *m_Error };
	}

	// A message that could not be marshalled fails here rather than at the call site that wrote it,
	// which is what keeps generated marshalling code free of error handling. See Wire/Writer.h.
	if (const std::optional<Error>& fault = m_Out.Fault(); fault)
	{
		return Fail(fault->Code(), fault->Context());
	}

	if (!m_Socket.IsValid())
	{
		return Failure(ENOTCONN, "flushing a wayland connection that is not open");
	}

	// **Whole messages only, and one batch is one `sendmsg`.** Both bounds are Wire/Buffer.h's:
	// `NextBatch` stops short of a message still being marshalled, and stops at the last message whose
	// descriptors this batch can still carry.
	for (;;)
	{
		const OutputBuffer::Batch batch = m_Out.NextBatch();

		if (batch.Bytes.empty())
		{
			break;
		}

		const std::span<const std::byte> bytes = batch.Bytes;
		const std::span<PendingFd> fds = batch.Fds;
		const std::size_t attach = fds.size();

		::iovec vector{ .iov_base = const_cast<std::byte*>(bytes.data()), .iov_len = bytes.size() };
		ControlBuffer control;

		::msghdr message{};
		message.msg_iov = &vector;
		message.msg_iovlen = 1;

		if (attach > 0)
		{
			message.msg_control = control.Bytes.data();
			message.msg_controllen = CMSG_SPACE(attach * sizeof(int));

			::cmsghdr* header = CMSG_FIRSTHDR(&message);
			header->cmsg_level = SOL_SOCKET;
			header->cmsg_type = SCM_RIGHTS;
			header->cmsg_len = CMSG_LEN(attach * sizeof(int));

			// Copied one at a time rather than as a block, because `Fd` owns and the array the kernel
			// reads is plain `int`s.
			for (std::size_t index = 0; index < attach; ++index)
			{
				const int descriptor = fds[index].Descriptor.Get();
				std::memcpy(CMSG_DATA(header) + index * sizeof(int), &descriptor, sizeof descriptor);
			}
		}

		// `MSG_NOSIGNAL` because a host that exited between the poll and this call would otherwise
		// take the whole compositor down with `SIGPIPE`, and the host exiting is an ordinary event
		// gyro reports and recovers from.
		const ::ssize_t sent = ::sendmsg(m_Socket.Get(), &message, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (sent < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}

			// The host has not read yet. Ordinary rather than a failure: what did not go stays
			// buffered, the socket becomes writable, and the next flush carries it.
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				return {};
			}

			return Fail(errno, "sending to the wayland host");
		}

		if (sent == 0)
		{
			// A stream socket does not accept zero bytes of a non-empty write. Breaking rather than
			// looping is what keeps a kernel that surprises us from spinning a SCHED_FIFO thread.
			return {};
		}

		// The descriptors went with the first byte, so a short write still transferred all of them and
		// this end closes its copies. The bytes they belonged to that did not fit go out next time
		// with no control message, which is exactly right: the far end already has them queued.
		m_Out.Sent(static_cast<std::size_t>(sent), attach);
	}

	return {};
}

Result<void> Connection::Drain()
{
	if (m_Error)
	{
		return std::unexpected{ *m_Error };
	}

	if (!m_Socket.IsValid())
	{
		return Failure(ENOTCONN, "draining a wayland connection that is not open");
	}

	// Hoisted out of the loop: a drain that finds a busy host reads several times, and re-zeroing four
	// kilobytes per read on a SCHED_FIFO thread is work with nothing to show for it.
	std::array<std::byte, ReadChunkBytes> staging{};
	ControlBuffer control;

	for (;;)
	{
		::iovec vector{ .iov_base = staging.data(), .iov_len = staging.size() };

		::msghdr message{};
		message.msg_iov = &vector;
		message.msg_iovlen = 1;
		message.msg_control = control.Bytes.data();
		message.msg_controllen = control.Bytes.size();

		const ::ssize_t read = ::recvmsg(m_Socket.Get(), &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);

		if (read < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}

			// Nothing left. Seam/EventSource.h's contract: the ordinary result of a wakeup some other
			// source caused, and success.
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				break;
			}

			return Fail(errno, "reading from the wayland host");
		}

		if (read == 0)
		{
			return Fail(EPIPE, "the wayland host closed the connection");
		}

		// Descriptors first, and unconditionally: they are attached to the first byte of the data in
		// this call, so they belong ahead of the bytes that arrive with them rather than after.
		for (::cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header))
		{
			if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
			{
				continue;
			}

			const std::size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);

			for (std::size_t index = 0; index < count; ++index)
			{
				int descriptor = -1;
				std::memcpy(&descriptor, CMSG_DATA(header) + index * sizeof(int), sizeof descriptor);
				m_In.PutFd(Fd{ descriptor });
			}
		}

		// The kernel dropped descriptors because the control buffer was too small, which cannot happen
		// against a peer that respects the 28 limit and is a torn message if it does — every
		// subsequent `GetFd` would take the wrong descriptor. There is no recovering the alignment, so
		// the connection ends here rather than handing a proxy someone else's buffer.
		if ((message.msg_flags & MSG_CTRUNC) != 0)
		{
			return Fail(EPROTO, "the wayland host sent more descriptors than one message may carry");
		}

		m_In.Append(std::span<const std::byte>{ staging }.first(static_cast<std::size_t>(read)));

		if (Result<void> dispatched = DispatchAvailable(); !dispatched)
		{
			return dispatched;
		}

		// Between reads and not between messages: a dispatcher is handed views into this buffer, so
		// the dead prefix can only be dropped once none of them can still be alive.
		m_In.Compact();
	}

	return {};
}

Result<void> Connection::DispatchAvailable()
{
	for (;;)
	{
		const std::span<const std::byte> available = m_In.Available();

		if (available.size() < HeaderBytes)
		{
			return {};
		}

		const MessageHeader header = ReadHeader(available);

		// A size that is not a whole number of words, or is smaller than the header claiming it, is a
		// message no amount of further reading fixes. Distinguished from a short read for exactly that
		// reason: waiting for the rest of a message whose length is a lie is a hang rather than a
		// failure.
		if (!header.IsWellFormed())
		{
			return Fail(EPROTO, "the wayland host sent a message with an impossible size");
		}

		if (available.size() < header.Size)
		{
			return {};
		}

		const std::span<const std::byte> body = available.subspan(HeaderBytes, header.Size - HeaderBytes);
		MessageReader reader{ header, body, m_In };

		if (header.Target == ObjectId::Display)
		{
			// Consumed before dispatching, because `delete_id` recycles an id that a proxy may
			// immediately allocate again and the reader is done with the bytes by then.
			if (Result<void> handled = DispatchDisplay(reader); !handled)
			{
				return handled;
			}

			m_In.Consume(header.Size);

			continue;
		}

		Entry* entry = Find(header.Target);

		// **An event for an id that has never had a dispatcher is a connection error**, which is the
		// runtime half of decision 2's argument that a dispatch table cannot have a hole. The
		// alternative — discard it and carry on — is one this module cannot take: it has no signatures
		// by construction, so a message whose arguments go unread leaves the descriptor queue one
		// entry out of step and the *next* message takes a buffer belonging to something else.
		//
		// An id whose *proxy* is gone is a different thing and the ordinary one — the host had the
		// event on the wire before it read the destroy — so `Unbind` leaves the dispatcher behind and
		// the message is read with a null `self`. See Wire/Connection.h's `Dispatcher`.
		if (entry == nullptr || entry->Dispatch == nullptr)
		{
			return Fail(EPROTO, "the wayland host sent an event for an object nothing is bound to");
		}

		// Copied out before the call, because a dispatcher routinely allocates an id — every request
		// that creates an object does — and that grows the table this entry points into.
		const Dispatcher dispatch = entry->Dispatch;
		//
		// `Retired` is the only state that loses its object: `Deleted` means the host has freed the id
		// while the proxy is still there to be told, which is the order a `wl_callback` arrives in.
		void* const self = entry->State == Slot::Retired ? nullptr : entry->Self;

		dispatch(self, header.Opcode, reader);

		// Checked once here rather than at every accessor, per Wire/Reader.h. A dispatcher that ran
		// off the end read nothing it did not own, and the connection ends before the next message is
		// framed against a queue that is now misaligned.
		if (reader.Failed())
		{
			return Fail(EPROTO, "a wayland event carried fewer arguments than its interface declares");
		}

		m_In.Consume(header.Size);
	}
}

Result<void> Connection::DispatchDisplay(MessageReader& reader)
{
	switch (reader.Opcode())
	{
		case DisplayError:
		{
			// `object_id`, `code`, `message`. Read before the failure is built, because the whole
			// point of surfacing it is that a host's protocol error names the request that caused it
			// and that string is the only diagnostic there will ever be.
			const ObjectId object = reader.GetObject();
			const std::uint32_t code = reader.GetUint();
			const std::string_view text = reader.GetString();

			if (reader.Failed())
			{
				return Fail(EPROTO, "the wayland host sent a malformed protocol error");
			}

			m_Fault = ProtocolFault{ .Object = object, .Code = code, .Message = std::string{ text } };

			// `Error` holds a `string_view` over static storage and cannot carry the text, so the
			// context is fixed and `Fault()` is where the detail is. See Wire/Connection.h.
			return Fail(EPROTO, "the wayland host reported a protocol error");
		}

		case DisplayDeleteId:
		{
			const ObjectId id = static_cast<ObjectId>(reader.GetUint());

			if (reader.Failed())
			{
				return Fail(EPROTO, "the wayland host sent a malformed delete_id");
			}

			return Recycle(id);
		}

		default:
			// wl_display has two events and has had two for the life of the protocol. A third means
			// this is not a Wayland host, or the id is being answered by something that is not
			// wl_display — either way there is nothing to do with it.
			return Fail(EPROTO, "the wayland host sent an event wl_display does not have");
	}
}

Connection::Entry* Connection::Find(ObjectId id) noexcept
{
	if (id == ObjectId::None)
	{
		return nullptr;
	}

	if (IsServerId(id))
	{
		const auto found = std::ranges::find(m_Server, id, &std::pair<ObjectId, Entry>::first);

		return found == m_Server.end() ? nullptr : &found->second;
	}

	const std::size_t index = static_cast<std::uint32_t>(id) - 1;

	return index < m_Client.size() ? &m_Client[index] : nullptr;
}

Result<void> Connection::Recycle(ObjectId id)
{
	// Only client ids are recycled. A host-allocated id was never handed out by `Allocate`, and the
	// host frees its own; `delete_id` naming one is a host talking about a range it does not own.
	if (IsServerId(id) || id == ObjectId::None || id == ObjectId::Display)
	{
		return Fail(EPROTO, "the wayland host freed an id it did not allocate");
	}

	Entry* entry = Find(id);

	if (entry == nullptr || entry->State == Slot::Free || entry->State == Slot::Deleted)
	{
		return Fail(EPROTO, "the wayland host freed an id that was not in use");
	}

	// Still bound. The host frees an id the moment it destroys the object, which for a `wl_callback`
	// is immediately after it sends `done` — so this is the ordinary order rather than the odd one,
	// and the id stays out of circulation until the proxy is unbound.
	if (entry->State != Slot::Retired)
	{
		entry->State = Slot::Deleted;

		return {};
	}

	*entry = Entry{};
	m_Recycled.push_back(id);

	return {};
}

void Connection::Reserve(std::size_t ids)
{
	m_Client.reserve(ids);

	// The recycle list cannot hold more than the table does, so one figure sizes both — and the push
	// that fills it happens on a `delete_id`, which is inside `Drain` rather than inside a frame.
	m_Recycled.reserve(ids);
}

ObjectId Connection::Allocate()
{
	if (!m_Recycled.empty())
	{
		const ObjectId id = m_Recycled.back();
		m_Recycled.pop_back();

		Entry* entry = Find(id);
		entry->State = Slot::Reserved;

		return id;
	}

	if (m_NextId >= FirstServerId)
	{
		return ObjectId::None;
	}

	const ObjectId id = static_cast<ObjectId>(m_NextId);
	++m_NextId;

	m_Client.resize(static_cast<std::uint32_t>(id));
	m_Client.back().State = Slot::Reserved;

	return id;
}

Result<void> Connection::Bind(ObjectId id, void* self, Dispatcher dispatch)
{
	if (self == nullptr || dispatch == nullptr)
	{
		return Failure(EINVAL, "binding a wayland object to nothing");
	}

	if (id == ObjectId::Display)
	{
		return Failure(EINVAL, "binding wl_display, which the wire runtime answers itself");
	}

	if (IsServerId(id))
	{
		if (Find(id) != nullptr)
		{
			return Failure(EEXIST, "binding a host-allocated wayland id that is already bound");
		}

		m_Server.emplace_back(id, Entry{ .Self = self, .Dispatch = dispatch, .State = Slot::Live });

		return {};
	}

	Entry* entry = Find(id);

	// Reserved and nothing else. `Free` is an id that was never allocated, `Live` is one bound twice,
	// and `Retired` or `Deleted` is one reused before the host said it could be — which is the bug
	// that manifests as an event arriving for the wrong object, months later, in someone else's code.
	if (entry == nullptr || entry->State != Slot::Reserved)
	{
		return Failure(EINVAL, "binding a wayland id that was not reserved by Allocate");
	}

	entry->Self = self;
	entry->Dispatch = dispatch;
	entry->State = Slot::Live;

	return {};
}

void Connection::Unbind(ObjectId id) noexcept
{
	Entry* entry = Find(id);

	if (entry == nullptr)
	{
		return;
	}

	if (IsServerId(id))
	{
		// Nothing to wait for: the host allocated the id and frees it on the destructor request, and
		// there is no `delete_id` for this range. Erased rather than emptied, because the table is a
		// list of what is live.
		std::erase_if(m_Server, [id](const std::pair<ObjectId, Entry>& bound) { return bound.first == id; });

		return;
	}

	switch (entry->State)
	{
		case Slot::Deleted:
			// The host got there first. Free now, since there is nothing left to wait for.
			*entry = Entry{};
			m_Recycled.push_back(id);
			break;

		case Slot::Reserved:
			// Allocated and never bound, so the id has never been marshalled and the host has never
			// heard of it: there is no `delete_id` coming and nothing in flight to answer. Retiring it
			// would strand it for the life of the connection. See `Allocate`.
			*entry = Entry{};
			m_Recycled.push_back(id);
			break;

		case Slot::Live:
			// Out of circulation until `delete_id`. The proxy is gone but the id is not free, because
			// the host may have events in flight naming it and a reuse would answer them with the
			// wrong object.
			//
			// **The dispatcher stays and the object goes.** Those in-flight events still have to be
			// read, or the descriptors they carry are never taken off the queue and every message
			// after them takes the wrong one.
			*entry = Entry{ .Self = nullptr, .Dispatch = entry->Dispatch, .State = Slot::Retired };
			break;

		case Slot::Free:
		case Slot::Retired:
			break;
	}
}
} // namespace Wire
