// A socket, a memfd for the format table, and `SCM_RIGHTS` in both directions. POSIX is named for
// Core/Fd.cpp's reason; `memfd_create` is Linux's, which is the ordinary state of this module.
#define _POSIX_C_SOURCE 200809L

#include "Nested/Peer.h"

#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include "Wayland/LinuxDmabufV1.h"
#include "Wayland/LinuxDrmSyncobjV1.h"
#include "Wayland/PresentationTime.h"
#include "Wayland/Wayland.h"
#include "Wayland/XdgDecorationUnstableV1.h"
#include "Wayland/XdgShell.h"
#include "Wire/Writer.h"

namespace Nested
{
namespace
{
using namespace Wayland;

// One entry of `zwp_linux_dmabuf_feedback_v1`'s format table, as the protocol lays it out.
struct TableEntry
{
	std::uint32_t Format = 0;
	std::uint32_t Padding = 0;
	std::uint64_t Modifier = 0;
};

// Enough for one `sendmsg`'s worth of descriptors, matching Wire's own.
struct ControlBuffer
{
	alignas(::cmsghdr) std::array<char, CMSG_SPACE(Wire::MaxFdsPerMessage * sizeof(int))> Bytes{};
};

[[nodiscard]] constexpr std::uint32_t High(std::uint64_t value) noexcept
{
	return static_cast<std::uint32_t>(value >> 32);
}

[[nodiscard]] constexpr std::uint32_t Low(std::uint64_t value) noexcept
{
	return static_cast<std::uint32_t>(value & 0xffffffffU);
}

// Which interface a name binds to. The peer learns most of its object kinds from the requests that
// create them; `wl_registry.bind` is the one that names the interface on the wire, which is exactly
// what the untyped `new_id` is for.
// The serial a peer stamps its pointer events with. A real host's is a counter over everything it has
// ever sent; nothing on the client side compares two of these, so one number is enough for the
// argument to be present and for a wrong-order read to show up.
constexpr std::uint32_t PointerSerial = 900;

[[nodiscard]] PeerObject KindFor(std::string_view interface) noexcept
{
	if (interface == WlCompositor::WireName)
	{
		return PeerObject::Compositor;
	}
	if (interface == XdgWmBase::WireName)
	{
		return PeerObject::Shell;
	}
	if (interface == ZwpLinuxDmabufV1::WireName)
	{
		return PeerObject::Dmabuf;
	}
	if (interface == WpPresentation::WireName)
	{
		return PeerObject::Presentation;
	}
	if (interface == WpLinuxDrmSyncobjManagerV1::WireName)
	{
		return PeerObject::SyncobjManager;
	}
	if (interface == ZxdgDecorationManagerV1::WireName)
	{
		return PeerObject::DecorationManager;
	}
	if (interface == WlSeat::WireName)
	{
		return PeerObject::Seat;
	}

	return PeerObject::Unknown;
}
} // namespace

std::vector<PixelFormat> DefaultOffer()
{
	// A tiled modifier ahead of the linear one, because a real host puts its best band first and
	// because *the host ranked and the device vetoed* is only exercised where the first candidate can
	// actually be refused. The value is a plausible-looking vendor modifier no software device will
	// take.
	return { PixelFormat{ FormatXrgb8888, 0, 0x0300000000000001ULL },
		     PixelFormat{ FormatXrgb8888, 0, ModifierLinear },
		     PixelFormat{ FormatArgb8888, 0, ModifierLinear } };
}

Result<void> Peer::Open()
{
	if (Globals.empty())
	{
		Globals = { PeerGlobal{ WlCompositor::WireName, 6 },
			        PeerGlobal{ XdgWmBase::WireName, 6 },
			        PeerGlobal{ ZwpLinuxDmabufV1::WireName, 5 },
			        PeerGlobal{ WpPresentation::WireName, 1 },
			        PeerGlobal{ WpLinuxDrmSyncobjManagerV1::WireName, 1 },
			        PeerGlobal{ ZxdgDecorationManagerV1::WireName, 1 },
			        PeerGlobal{ WlSeat::WireName, 5 } };
	}

	if (Offered.empty() && !OfferNothing)
	{
		Offered = DefaultOffer();
	}

	std::array<int, 2> ends{ -1, -1 };

	if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends.data()) != 0)
	{
		return Failure(errno, "opening the socketpair a fake wayland host talks over");
	}

	m_Socket = Fd{ ends[1] };

	// `WAYLAND_SOCKET` wins outright in `Wire::Connection::Open` and is unset once read, which is what
	// makes this the way to hand a client a socket it could not otherwise open. The client end is
	// adopted by whoever opens the connection next, so nothing here closes it.
	const std::string number = std::to_string(ends[0]);
	::setenv("WAYLAND_SOCKET", number.c_str(), 1);

	Bind(Wire::ObjectId::Display, PeerObject::Display);

	return {};
}

void Peer::HangUp() noexcept
{
	m_Socket = Fd{};
}

void Peer::Bind(Wire::ObjectId id, PeerObject kind)
{
	// **An id is one object at a time, and rebinding replaces rather than shadows.** A client reuses an
	// id the moment the host has sent `delete_id` for it — a `wl_callback` from a roundtrip, a
	// `wp_presentation_feedback` the host destroyed after answering — so the ids a long session hands
	// out are recycled constantly. A table that appended would keep answering with the *first* thing an
	// id ever was, and every request to whatever it is now would fall through to the unhandled count.
	// Which is exactly how this was found.
	const auto found = std::ranges::find_if(m_Objects, [id](const auto& bound) { return bound.first == id; });

	if (found != m_Objects.end())
	{
		found->second = kind;

		return;
	}

	m_Objects.emplace_back(id, kind);
}

PeerObject Peer::KindOf(Wire::ObjectId id) const noexcept
{
	const auto found = std::ranges::find_if(m_Objects, [id](const auto& bound) { return bound.first == id; });

	return found == m_Objects.end() ? PeerObject::Unknown : found->second;
}

Result<void> Peer::Flush()
{
	if (!m_Socket.IsValid())
	{
		return Failure(ENOTCONN, "flushing a fake wayland host with no socket");
	}

	for (;;)
	{
		const Wire::OutputBuffer::Batch batch = m_Out.NextBatch();

		if (batch.Bytes.empty())
		{
			break;
		}

		const std::size_t attach = batch.Fds.size();

		::iovec vector{ .iov_base = const_cast<std::byte*>(batch.Bytes.data()), .iov_len = batch.Bytes.size() };
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

			for (std::size_t index = 0; index < attach; ++index)
			{
				const int descriptor = batch.Fds[index].Descriptor.Get();
				std::memcpy(CMSG_DATA(header) + index * sizeof(int), &descriptor, sizeof descriptor);
			}
		}

		const ::ssize_t sent = ::sendmsg(m_Socket.Get(), &message, MSG_NOSIGNAL);

		if (sent <= 0)
		{
			return Failure(errno, "a fake wayland host sending to its client");
		}

		m_Out.Sent(static_cast<std::size_t>(sent), attach);
	}

	return {};
}

Result<void> Peer::Pump()
{
	if (!m_Socket.IsValid())
	{
		return Failure(ENOTCONN, "pumping a fake wayland host with no socket");
	}

	for (;;)
	{
		std::array<std::byte, 4096> staging{};
		ControlBuffer control;

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

			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				break;
			}

			return Failure(errno, "a fake wayland host reading from its client");
		}

		if (read == 0)
		{
			break;
		}

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

		m_In.Append({ staging.data(), static_cast<std::size_t>(read) });
	}

	// Framed off the header's own size word, exactly as the client's runtime does — which is what
	// makes an argument this peer reads in the wrong order show up as a wrong *value* rather than as a
	// desynchronised stream nobody can diagnose.
	for (;;)
	{
		const std::span<const std::byte> available = m_In.Available();

		if (available.size() < Wire::HeaderBytes)
		{
			break;
		}

		const Wire::MessageHeader header = Wire::ReadHeader(available);

		if (!header.IsWellFormed() || available.size() < header.Size)
		{
			break;
		}

		Wire::MessageReader reader{ header,
			                        available.subspan(Wire::HeaderBytes, header.Size - Wire::HeaderBytes),
			                        m_In };

		Dispatch(header, reader);

		m_In.Consume(header.Size);
	}

	m_In.Compact();

	return Flush();
}

void Peer::Dispatch(Wire::MessageHeader header, Wire::MessageReader& reader)
{
	switch (KindOf(header.Target))
	{
		case PeerObject::Display:
			DispatchDisplay(reader);
			return;

		case PeerObject::Registry:
			DispatchRegistry(reader);
			return;

		case PeerObject::Seat:
			// `get_pointer`. The keyboard and the touch device are not modelled, so asking for one is an
			// unhandled request rather than a silent agreement — which is the whole of this peer's rule.
			if (reader.Opcode() == 0)
			{
				m_Pointer = reader.GetNewId();
				Bind(m_Pointer, PeerObject::Pointer);

				return;
			}

			// `release`.
			if (reader.Opcode() == 3)
			{
				m_Pointer = Wire::ObjectId::None;

				return;
			}
			break;

		case PeerObject::Pointer:
			// `set_cursor`, and what is recorded is the one thing a nested compositor has to get right:
			// a null surface, which is how a client says *draw no cursor over my window*.
			if (reader.Opcode() == 0)
			{
				(void)reader.GetUint();

				if (reader.GetObject() == Wire::ObjectId::None)
				{
					++CursorsHidden;
				}

				(void)reader.GetInt();
				(void)reader.GetInt();

				return;
			}

			// `release`.
			if (reader.Opcode() == 1)
			{
				m_Pointer = Wire::ObjectId::None;

				return;
			}
			break;

		case PeerObject::Compositor:
			if (reader.Opcode() == 0)
			{
				m_Surface = reader.GetNewId();
				Bind(m_Surface, PeerObject::Surface);

				return;
			}
			break;

		case PeerObject::Shell:
			// `get_xdg_surface` is the new id then the surface, which is the order the XML declares and
			// the opposite of `wp_presentation.feedback` below. Reading it the other way round is
			// exactly the defect this peer exists to catch, so it is spelled rather than assumed.
			if (reader.Opcode() == 2)
			{
				m_XdgSurface = reader.GetNewId();
				Bind(m_XdgSurface, PeerObject::XdgSurface);
				(void)reader.GetObject();

				return;
			}

			if (reader.Opcode() == 3)
			{
				(void)reader.GetUint();
				++Pongs;

				return;
			}
			break;

		case PeerObject::XdgSurface:
			if (reader.Opcode() == 1)
			{
				m_Toplevel = reader.GetNewId();
				Bind(m_Toplevel, PeerObject::Toplevel);

				return;
			}

			// `set_window_geometry` and `destroy` say nothing this peer acts on.
			if (reader.Opcode() == 3 || reader.Opcode() == 0)
			{
				return;
			}

			if (reader.Opcode() == 4)
			{
				Acked = reader.GetUint();

				return;
			}
			break;

		case PeerObject::Toplevel:
			// Title, app id and destroy. Nothing here reads them back; that a title arrived at all is
			// what the unhandled count would otherwise complain about.
			if (reader.Opcode() <= 3)
			{
				return;
			}
			break;

		case PeerObject::Dmabuf:
			DispatchDmabuf(header.Target, reader);
			return;

		case PeerObject::Params:
			DispatchParams(header.Target, reader);
			return;

		case PeerObject::Surface:
			DispatchSurface(header.Target, reader);
			return;

		case PeerObject::Presentation:
			if (reader.Opcode() == 1)
			{
				// The surface *then* the id, which is the other order. See `Shell` above.
				(void)reader.GetObject();

				const Wire::ObjectId feedback = reader.GetNewId();
				Bind(feedback, PeerObject::PresentationFeedback);
				m_Building.Feedback = feedback;

				return;
			}
			break;

		case PeerObject::SyncobjManager:
			DispatchSyncobj(header.Target, reader);
			return;

		case PeerObject::SyncobjSurface:
			if (reader.Opcode() == 1 || reader.Opcode() == 2)
			{
				(void)reader.GetObject();

				const std::uint32_t high = reader.GetUint();
				const std::uint32_t low = reader.GetUint();
				const std::uint64_t point = (static_cast<std::uint64_t>(high) << 32) | low;

				if (reader.Opcode() == 1)
				{
					m_Building.HasAcquire = true;
					m_Building.Acquire = point;
				}
				else
				{
					m_Building.HasRelease = true;
					m_Building.Release = point;
				}

				return;
			}

			if (reader.Opcode() == 0)
			{
				return;
			}
			break;

		case PeerObject::DecorationManager:
			if (reader.Opcode() == 1)
			{
				Bind(reader.GetNewId(), PeerObject::Decoration);
				(void)reader.GetObject();

				return;
			}
			break;

		case PeerObject::Decoration:
		case PeerObject::Buffer:
		case PeerObject::SyncobjTimeline:
		case PeerObject::FeedbackObject:
		case PeerObject::PresentationFeedback:
		case PeerObject::Callback:
			// Destroys and mode-setting, none of which this peer models. Answered as handled so that
			// the unhandled count stays a signal.
			return;

		case PeerObject::Unknown:
			break;
	}

	++Unhandled;
}

void Peer::DispatchDisplay(Wire::MessageReader& reader)
{
	if (reader.Opcode() == 0)
	{
		// `sync`: the barrier every roundtrip is. Answered immediately, so everything queued ahead of
		// it — a registry's globals, a feedback conversation, a configure — is already on the wire.
		const Wire::ObjectId callback = reader.GetNewId();

		Wire::MessageWriter done{ m_Out, callback, 0 };
		done.PutUint(0);
		done.Send();

		// And the id is freed, which is what a real host does the instant it has answered — the window
		// Wire/Connection.h's `Deleted` state is written for.
		Wire::MessageWriter deleted{ m_Out, Wire::ObjectId::Display, 1 };
		deleted.PutUint(static_cast<std::uint32_t>(callback));
		deleted.Send();

		return;
	}

	if (reader.Opcode() == 1)
	{
		m_Registry = reader.GetNewId();
		Bind(m_Registry, PeerObject::Registry);

		for (const PeerGlobal& global : Globals)
		{
			Wire::MessageWriter announce{ m_Out, m_Registry, 0 };
			announce.PutUint(m_Name++);
			announce.PutString(global.Interface);
			announce.PutUint(global.Version);
			announce.Send();
		}

		return;
	}

	++Unhandled;
}

void Peer::DispatchRegistry(Wire::MessageReader& reader)
{
	if (reader.Opcode() != 0)
	{
		++Unhandled;

		return;
	}

	(void)reader.GetUint();

	const std::string_view interface = reader.GetString();
	const PeerObject kind = KindFor(interface);

	(void)reader.GetUint();

	const Wire::ObjectId id = reader.GetNewId();

	Bind(id, kind);

	if (kind == PeerObject::Seat)
	{
		m_Seat = id;

		// Capabilities the moment the global is bound, which is what a real host does and what the
		// client reads before it asks for anything. A keyboard is claimed as well as a pointer, because
		// a seat that has one is the ordinary case and a client that ignores it is what is being tested.
		Wire::MessageWriter capabilities{ m_Out, id, 0 };
		capabilities.PutUint(0x1U | 0x2U);
		capabilities.Send();

		Wire::MessageWriter name{ m_Out, id, 1 };
		name.PutString("seat0");
		name.Send();
	}

	if (kind == PeerObject::Presentation)
	{
		// `clock_id` arrives unprompted the moment the global is bound, which is what lets the client
		// decide whether a timestamp means anything before it has committed a frame.
		Wire::MessageWriter clock{ m_Out, id, 0 };
		clock.PutUint(static_cast<std::uint32_t>(CLOCK_MONOTONIC));
		clock.Send();
	}
}

void Peer::DispatchDmabuf(Wire::ObjectId, Wire::MessageReader& reader)
{
	if (reader.Opcode() == 1)
	{
		Bind(reader.GetNewId(), PeerObject::Params);

		return;
	}

	if (reader.Opcode() == 2)
	{
		const Wire::ObjectId feedback = reader.GetNewId();
		Bind(feedback, PeerObject::FeedbackObject);
		SendFeedback(feedback);

		return;
	}

	if (reader.Opcode() == 0)
	{
		return;
	}

	++Unhandled;
}

void Peer::SendFeedback(Wire::ObjectId feedback)
{
	if (Offered.empty())
	{
		// A host with nothing to offer still says `done`, which is what makes *the set is empty* a fact
		// the client reads rather than a timeout it infers.
		Wire::MessageWriter done{ m_Out, feedback, 0 };
		done.Send();

		return;
	}

	// The table is a real file, mapped by the client. A `memfd` rather than a temporary, so nothing
	// touches a filesystem and the descriptor is the only way in.
	std::vector<TableEntry> entries;
	entries.reserve(Offered.size());

	for (const PixelFormat& format : Offered)
	{
		entries.push_back(TableEntry{ .Format = format.Code, .Padding = 0, .Modifier = format.Modifier });
	}

	const std::size_t bytes = entries.size() * sizeof(TableEntry);
	const int backing = ::memfd_create("gyro-format-table", MFD_CLOEXEC);

	if (backing < 0)
	{
		++Unhandled;

		return;
	}

	m_Table = Fd{ backing };

	if (::ftruncate(m_Table.Get(), static_cast<::off_t>(bytes)) != 0)
	{
		++Unhandled;

		return;
	}

	void* const mapped = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, m_Table.Get(), 0);

	if (mapped == MAP_FAILED)
	{
		++Unhandled;

		return;
	}

	std::memcpy(mapped, entries.data(), bytes);
	::munmap(mapped, bytes);

	{
		Wire::MessageWriter table{ m_Out, feedback, 1 };
		table.PutFd(Duplicate(m_Table.Borrow()));
		table.PutUint(static_cast<std::uint32_t>(bytes));
		table.Send();
	}

	const auto device = [&](Wire::ObjectId target, std::uint16_t opcode) {
		Wire::MessageWriter writer{ m_Out, target, opcode };
		writer.PutArray({ reinterpret_cast<const std::byte*>(&MainDevice), sizeof(::dev_t) });
		writer.Send();
	};

	device(feedback, 2);
	device(feedback, 4);

	{
		// One tranche holding everything, indices in the order the table was written — which is the
		// host's preference order and the thing the client walks.
		std::vector<std::uint16_t> indices;
		indices.reserve(entries.size());

		for (std::uint16_t index = 0; index < static_cast<std::uint16_t>(entries.size()); ++index)
		{
			indices.push_back(index);
		}

		Wire::MessageWriter formats{ m_Out, feedback, 5 };
		formats.PutArray(
			{ reinterpret_cast<const std::byte*>(indices.data()), indices.size() * sizeof(std::uint16_t) }
		);
		formats.Send();
	}

	{
		Wire::MessageWriter flags{ m_Out, feedback, 6 };
		flags.PutUint(0);
		flags.Send();
	}

	{
		Wire::MessageWriter trancheDone{ m_Out, feedback, 3 };
		trancheDone.Send();
	}

	{
		Wire::MessageWriter done{ m_Out, feedback, 0 };
		done.Send();
	}
}

void Peer::DispatchParams(Wire::ObjectId target, Wire::MessageReader& reader)
{
	if (reader.Opcode() == 1)
	{
		// `add`: a descriptor, then the plane's placement. The descriptor is kept because a test that
		// wants to prove a real dmabuf crossed can read it, and because dropping it here would close a
		// file the client still owns a copy of.
		Fd plane = reader.GetFd();

		(void)reader.GetUint();
		(void)reader.GetUint();
		(void)reader.GetUint();

		const std::uint32_t high = reader.GetUint();
		const std::uint32_t low = reader.GetUint();

		auto found = std::ranges::find_if(Buffers, [target](const PeerBuffer& buffer) { return buffer.Id == target; });

		if (found == Buffers.end())
		{
			// Keyed on the params object until `create_immed` renames it, because a plane arrives
			// before the buffer it belongs to exists.
			Buffers.push_back(
				PeerBuffer{ .Id = target,
			                .Width = 0,
			                .Height = 0,
			                .Format = 0,
			                .Modifier = (static_cast<std::uint64_t>(high) << 32) | low,
			                .Planes = {} }
			);
			found = Buffers.end() - 1;
		}

		found->Planes.push_back(std::move(plane));

		return;
	}

	if (reader.Opcode() == 3)
	{
		const Wire::ObjectId buffer = reader.GetNewId();
		Bind(buffer, PeerObject::Buffer);

		const auto found =
			std::ranges::find_if(Buffers, [target](const PeerBuffer& made) { return made.Id == target; });

		if (found != Buffers.end())
		{
			found->Id = buffer;
			found->Width = reader.GetInt();
			found->Height = reader.GetInt();
			found->Format = reader.GetUint();
			(void)reader.GetUint();
		}

		return;
	}

	if (reader.Opcode() == 0)
	{
		return;
	}

	++Unhandled;
}

void Peer::DispatchSurface(Wire::ObjectId target, Wire::MessageReader& reader)
{
	switch (reader.Opcode())
	{
		case 1:
			m_Building.Surface = target;
			m_Building.Buffer = reader.GetObject();
			(void)reader.GetInt();
			(void)reader.GetInt();

			return;

		case 9:
		{
			const std::int32_t x = reader.GetInt();
			const std::int32_t y = reader.GetInt();
			const std::int32_t width = reader.GetInt();
			const std::int32_t height = reader.GetInt();

			m_Building.Damage.push_back(PixelRect<DeviceSpace>{ { x, y }, { width, height } });

			return;
		}

		case 6:
			// **The first commit carries no buffer and is not a frame.** A surface takes its role, says
			// nothing else, and commits; the host answers with the configure that is the window's first
			// size, and only then may a buffer be attached. So this is where the initial configure comes
			// from, and it is why `Commits` holds frames rather than every commit — a test asserting on
			// `Commits.front()` means the first *picture*.
			if (m_Building.Buffer == Wire::ObjectId::None)
			{
				if (m_XdgSurface != Wire::ObjectId::None && !m_Configured)
				{
					m_Configured = true;
					(void)SendConfigure(ConfigureWidth, ConfigureHeight);
				}

				m_Building = PeerCommit{};

				return;
			}

			// Everything since the last commit becomes one thing, which is what double-buffered state
			// means and why the peer assembles rather than recording per request.
			m_Building.Surface = target;
			Commits.push_back(std::move(m_Building));
			m_Building = PeerCommit{};

			return;

		case 0:
			return;

		default:
			break;
	}

	++Unhandled;
}

void Peer::DispatchSyncobj(Wire::ObjectId, Wire::MessageReader& reader)
{
	if (reader.Opcode() == 1)
	{
		m_SyncSurface = reader.GetNewId();
		Bind(m_SyncSurface, PeerObject::SyncobjSurface);
		(void)reader.GetObject();

		return;
	}

	if (reader.Opcode() == 2)
	{
		Bind(reader.GetNewId(), PeerObject::SyncobjTimeline);

		// The syncobj descriptor. Taken and dropped: this peer never signals a release point, which is
		// itself worth having — a test that wants a stalled ring gets one by doing nothing.
		(void)reader.GetFd();

		return;
	}

	if (reader.Opcode() == 0)
	{
		return;
	}

	++Unhandled;
}

Result<void> Peer::SendConfigure(std::int32_t width, std::int32_t height)
{
	if (m_Toplevel == Wire::ObjectId::None || m_XdgSurface == Wire::ObjectId::None)
	{
		return Failure(EINVAL, "configuring a window the client has not created yet");
	}

	{
		Wire::MessageWriter configure{ m_Out, m_Toplevel, 0 };
		configure.PutInt(width);
		configure.PutInt(height);
		configure.PutArray({});
		configure.Send();
	}

	{
		// The `xdg_surface.configure` is what makes everything above true at once, and it is the one
		// the client acknowledges.
		Wire::MessageWriter configure{ m_Out, m_XdgSurface, 0 };
		configure.PutUint(++Serial);
		configure.Send();
	}

	return Flush();
}

Result<void>
Peer::SendPresented(std::size_t commit, std::uint64_t nanoseconds, std::uint32_t refresh, std::uint64_t sequence)
{
	if (commit >= Commits.size() || Commits[commit].Feedback == Wire::ObjectId::None)
	{
		return Failure(EINVAL, "presenting a commit that carried no feedback request");
	}

	const Wire::ObjectId feedback = Commits[commit].Feedback;

	{
		Wire::MessageWriter presented{ m_Out, feedback, 1 };
		presented.PutUint(High(nanoseconds / 1'000'000'000));
		presented.PutUint(Low(nanoseconds / 1'000'000'000));
		presented.PutUint(static_cast<std::uint32_t>(nanoseconds % 1'000'000'000));
		presented.PutUint(refresh);
		presented.PutUint(High(sequence));
		presented.PutUint(Low(sequence));
		presented.PutUint(
			static_cast<std::uint32_t>(WpPresentationFeedbackKind::Vsync) |
			static_cast<std::uint32_t>(WpPresentationFeedbackKind::HwClock)
		);
		presented.Send();
	}

	Wire::MessageWriter deleted{ m_Out, Wire::ObjectId::Display, 1 };
	deleted.PutUint(static_cast<std::uint32_t>(feedback));
	deleted.Send();

	return Flush();
}

Result<void> Peer::SendDiscarded(std::size_t commit)
{
	if (commit >= Commits.size() || Commits[commit].Feedback == Wire::ObjectId::None)
	{
		return Failure(EINVAL, "discarding a commit that carried no feedback request");
	}

	const Wire::ObjectId feedback = Commits[commit].Feedback;

	{
		Wire::MessageWriter discarded{ m_Out, feedback, 2 };
		discarded.Send();
	}

	Wire::MessageWriter deleted{ m_Out, Wire::ObjectId::Display, 1 };
	deleted.PutUint(static_cast<std::uint32_t>(feedback));
	deleted.Send();

	return Flush();
}

Result<void> Peer::SendRelease(Wire::ObjectId buffer)
{
	Wire::MessageWriter release{ m_Out, buffer, 0 };
	release.Send();

	return Flush();
}

Result<void> Peer::SendPointerEnter(double x, double y)
{
	if (m_Pointer == Wire::ObjectId::None)
	{
		return Failure(EINVAL, "sending pointer enter to a client with no wl_pointer");
	}

	Wire::MessageWriter enter{ m_Out, m_Pointer, 0 };
	enter.PutUint(PointerSerial);
	enter.PutObject(m_Surface);
	enter.PutFixed(Wire::Fixed::FromDouble(x));
	enter.PutFixed(Wire::Fixed::FromDouble(y));
	enter.Send();

	return Flush();
}

Result<void> Peer::SendPointerLeave()
{
	if (m_Pointer == Wire::ObjectId::None)
	{
		return Failure(EINVAL, "sending pointer leave to a client with no wl_pointer");
	}

	Wire::MessageWriter leave{ m_Out, m_Pointer, 1 };
	leave.PutUint(PointerSerial);
	leave.PutObject(m_Surface);
	leave.Send();

	return Flush();
}

Result<void> Peer::SendPointerMotion(double x, double y)
{
	if (m_Pointer == Wire::ObjectId::None)
	{
		return Failure(EINVAL, "sending pointer motion to a client with no wl_pointer");
	}

	Wire::MessageWriter motion{ m_Out, m_Pointer, 2 };
	motion.PutUint(0);
	motion.PutFixed(Wire::Fixed::FromDouble(x));
	motion.PutFixed(Wire::Fixed::FromDouble(y));
	motion.Send();

	return Flush();
}

Result<void> Peer::SendPointerButton(std::uint32_t button, bool pressed)
{
	if (m_Pointer == Wire::ObjectId::None)
	{
		return Failure(EINVAL, "sending a pointer button to a client with no wl_pointer");
	}

	Wire::MessageWriter event{ m_Out, m_Pointer, 3 };
	event.PutUint(PointerSerial);
	event.PutUint(0);
	event.PutUint(button);
	event.PutUint(pressed ? 1U : 0U);
	event.Send();

	return Flush();
}

Result<void> Peer::SendPointerAxis(std::uint32_t axis, double value)
{
	if (m_Pointer == Wire::ObjectId::None)
	{
		return Failure(EINVAL, "sending a pointer axis to a client with no wl_pointer");
	}

	Wire::MessageWriter event{ m_Out, m_Pointer, 4 };
	event.PutUint(0);
	event.PutUint(axis);
	event.PutFixed(Wire::Fixed::FromDouble(value));
	event.Send();

	return Flush();
}

Result<void> Peer::SendPointerFrame()
{
	if (m_Pointer == Wire::ObjectId::None)
	{
		return Failure(EINVAL, "sending a pointer frame to a client with no wl_pointer");
	}

	Wire::MessageWriter event{ m_Out, m_Pointer, 5 };
	event.Send();

	return Flush();
}

Result<void> Peer::SendPointerAxisSource(std::uint32_t source)
{
	if (m_Pointer == Wire::ObjectId::None)
	{
		return Failure(EINVAL, "sending a pointer axis source to a client with no wl_pointer");
	}

	Wire::MessageWriter event{ m_Out, m_Pointer, 6 };
	event.PutUint(source);
	event.Send();

	return Flush();
}

Result<void> Peer::SendPointerAxisStop(std::uint32_t axis)
{
	if (m_Pointer == Wire::ObjectId::None)
	{
		return Failure(EINVAL, "sending a pointer axis stop to a client with no wl_pointer");
	}

	Wire::MessageWriter event{ m_Out, m_Pointer, 7 };
	event.PutUint(0);
	event.PutUint(axis);
	event.Send();

	return Flush();
}

Result<void> Peer::SendPointerAxisValue120(std::uint32_t axis, std::int32_t value120)
{
	if (m_Pointer == Wire::ObjectId::None)
	{
		return Failure(EINVAL, "sending a pointer value120 to a client with no wl_pointer");
	}

	Wire::MessageWriter event{ m_Out, m_Pointer, 9 };
	event.PutUint(axis);
	event.PutInt(value120);
	event.Send();

	return Flush();
}

Result<void> Peer::SendPing(std::uint32_t serial)
{
	const auto shell =
		std::ranges::find_if(m_Objects, [](const auto& bound) { return bound.second == PeerObject::Shell; });

	if (shell == m_Objects.end())
	{
		return Failure(EINVAL, "pinging a client that has not bound xdg_wm_base");
	}

	Wire::MessageWriter ping{ m_Out, shell->first, 0 };
	ping.PutUint(serial);
	ping.Send();

	return Flush();
}

Result<void> Peer::SendClose()
{
	if (m_Toplevel == Wire::ObjectId::None)
	{
		return Failure(EINVAL, "closing a window the client has not created yet");
	}

	Wire::MessageWriter close{ m_Out, m_Toplevel, 1 };
	close.Send();

	return Flush();
}
} // namespace Nested
