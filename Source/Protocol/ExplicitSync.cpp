// `eventfd` is Linux's, which is what the header's availability rule already rests on.
#define _GNU_SOURCE 1

#include "Protocol/ExplicitSync.h"

#include <spdlog/spdlog.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "Protocol/Buffer.h"
#include "Protocol/Surface.h"

namespace
{
// The protocol splits a 64-bit point across two 32-bit arguments, high word first, and every call site
// in this file would otherwise write the shift by hand.
[[nodiscard]] constexpr std::uint64_t Joined(std::uint32_t high, std::uint32_t low) noexcept
{
	return (static_cast<std::uint64_t>(high) << 32U) | static_cast<std::uint64_t>(low);
}
} // namespace

ClientSyncSurface::~ClientSyncSurface()
{
	Disarm();

	if (m_Surface != nullptr)
	{
		m_Surface->ForgetSync(*this);
	}
}

void ClientSyncSurface::OnSetAcquirePoint(
	Wayland::Server::WpLinuxDrmSyncobjTimelineV1 timeline,
	std::uint32_t pointHi,
	std::uint32_t pointLo
)
{
	if (m_Surface == nullptr)
	{
		Object().PostError(
			Wayland::Server::WpLinuxDrmSyncobjSurfaceV1Error::NoSurface,
			"set_acquire_point on a wp_linux_drm_syncobj_surface_v1 whose wl_surface was destroyed"
		);

		return;
	}

	ClientSyncTimeline* const held = ClientSyncTimeline::Of(timeline);

	if (held == nullptr)
	{
		// Not reachable through a well-formed connection — the argument is typed, so libwayland has
		// already refused anything that is not one of these. A null here is gyro's own dispatch table
		// disagreeing with itself, which is a fault rather than a client error.
		Wayland::Server::RecordFault("set_acquire_point named a timeline gyro did not make");

		return;
	}

	// **The newest replaces the oldest rather than accumulating**, which the protocol states for both
	// requests: a client is describing one buffer, and two acquire points on one commit would be two
	// answers to a question with one.
	m_Acquire = SyncTimelinePoint{ held->Timeline(), Joined(pointHi, pointLo) };
}

void ClientSyncSurface::OnSetReleasePoint(
	Wayland::Server::WpLinuxDrmSyncobjTimelineV1 timeline,
	std::uint32_t pointHi,
	std::uint32_t pointLo
)
{
	if (m_Surface == nullptr)
	{
		Object().PostError(
			Wayland::Server::WpLinuxDrmSyncobjSurfaceV1Error::NoSurface,
			"set_release_point on a wp_linux_drm_syncobj_surface_v1 whose wl_surface was destroyed"
		);

		return;
	}

	ClientSyncTimeline* const held = ClientSyncTimeline::Of(timeline);

	if (held == nullptr)
	{
		Wayland::Server::RecordFault("set_release_point named a timeline gyro did not make");

		return;
	}

	m_Release = SyncTimelinePoint{ held->Timeline(), Joined(pointHi, pointLo) };
}

bool ClientSyncSurface::TakeCommit(
	const std::optional<Wayland::Server::WlBuffer>& attached,
	SyncTimelinePoint& acquire,
	SyncTimelinePoint& release
)
{
	// **A buffer, rather than an attach.** A commit that did not attach keeps the content it had and a
	// commit that attached nothing is a window taking itself off the screen; neither is a content update
	// anybody could synchronize, so neither obliges a point.
	const bool hasBuffer = attached.has_value() && attached->IsValid();

	if (!hasBuffer)
	{
		if (m_Acquire.IsSet() || m_Release.IsSet())
		{
			Object().PostError(
				Wayland::Server::WpLinuxDrmSyncobjSurfaceV1Error::NoBuffer,
				"a timeline point was set on a commit that attached no buffer"
			);

			return false;
		}

		return true;
	}

	// **Asked before the points are, because it is a question about what the client attached rather than
	// about what it said.** A software client that took one of these objects out of habit is told which
	// of the two it has to change, and told it at the commit rather than at the request — the protocol
	// puts the check here for the reason it puts the other three here, that a buffer may legally be
	// described before it is attached.
	const ClientBuffer* const buffer = ClientBuffer::Of(*attached);

	if (buffer == nullptr || !buffer->SupportsExplicitSync())
	{
		Object().PostError(
			Wayland::Server::WpLinuxDrmSyncobjSurfaceV1Error::UnsupportedBuffer,
			"explicit synchronization is supported for zwp_linux_dmabuf_v1 buffers only"
		);

		return false;
	}

	if (!m_Acquire.IsSet())
	{
		Object().PostError(
			Wayland::Server::WpLinuxDrmSyncobjSurfaceV1Error::NoAcquirePoint,
			"a buffer was committed to a synchronized surface with no acquire point"
		);

		return false;
	}

	if (!m_Release.IsSet())
	{
		Object().PostError(
			Wayland::Server::WpLinuxDrmSyncobjSurfaceV1Error::NoReleasePoint,
			"a buffer was committed to a synchronized surface with no release point"
		);

		return false;
	}

	// **Only on the same timeline, which is the protocol's own scoping of the question.** Two points on
	// two counters cannot be ordered against each other at all, and a compositor that guessed would be
	// refusing legal traffic. On one counter the release must come strictly after the acquire, because
	// signalling a point signals every point below it — so a release at or below the acquire is a client
	// telling gyro the buffer is free at the moment it becomes ready.
	if (m_Acquire.Timeline == m_Release.Timeline && m_Release.Point <= m_Acquire.Point)
	{
		Object().PostError(
			Wayland::Server::WpLinuxDrmSyncobjSurfaceV1Error::ConflictingPoints,
			"the release point is not after the acquire point on the same timeline"
		);

		return false;
	}

	acquire = std::move(m_Acquire);
	release = std::move(m_Release);

	m_Acquire = {};
	m_Release = {};

	return true;
}

bool ClientSyncSurface::Watch(const SyncTimelinePoint& acquire)
{
	Disarm();

	if (!acquire.IsSet() || m_Context == nullptr)
	{
		return false;
	}

	ExplicitSync* const sync = m_Context->Sync();

	if (sync == nullptr || sync->Loop() == nullptr)
	{
		return false;
	}

	// `EFD_CLOEXEC` for `Sync.cpp`'s reason and `EFD_NONBLOCK` because the read below happens on the
	// dispatch thread, which owes no deadline and must still never block on a client.
	const int descriptor = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

	if (descriptor < 0)
	{
		spdlog::warn("no eventfd for a held commit: {}", std::strerror(errno));

		return false;
	}

	m_Eventfd = Fd{ descriptor };

	if (const Result<void> armed = acquire.Timeline->Watch(acquire.Point, m_Eventfd.Borrow()); !armed)
	{
		spdlog::warn("could not hold a commit on its acquire point: {}", armed.error());

		m_Eventfd = Fd{};

		return false;
	}

	m_Source =
		wl_event_loop_add_fd(sync->Loop(), m_Eventfd.Get(), WL_EVENT_READABLE, &ClientSyncSurface::OnSignalled, this);

	if (m_Source == nullptr)
	{
		// The kernel still holds a reference to the eventfd until the point signals, which is harmless:
		// nothing reads it, and closing the descriptor is what drops the wait.
		m_Eventfd = Fd{};

		return false;
	}

	return true;
}

int ClientSyncSurface::OnSignalled(int descriptor, std::uint32_t mask, void* data) noexcept
{
	static_cast<void>(mask);

	auto* const self = static_cast<ClientSyncSurface*>(data);

	// Read so the level-triggered source does not fire again. The value is the kernel's count of how
	// many points signalled and is not a fact anybody needs — one wait was armed, and it has fired.
	std::uint64_t count = 0;
	const ssize_t read = ::read(descriptor, &count, sizeof(count));
	static_cast<void>(read);

	self->Disarm();

	if (self->m_Surface != nullptr)
	{
		self->m_Surface->OnAcquireSignalled();
	}

	return 0;
}

void ClientSyncSurface::Disarm() noexcept
{
	if (m_Source != nullptr)
	{
		wl_event_source_remove(m_Source);
		m_Source = nullptr;
	}

	m_Eventfd = Fd{};
}

Wayland::Server::WpLinuxDrmSyncobjSurfaceV1Handler* ClientSyncManager::OnGetSurface(Wayland::Server::WlSurface surface)
{
	ClientSurface* const onto = ClientSurface::Of(surface);

	// An inert object rather than a null, for `Viewporter.cpp`'s reason: the client is holding an id and
	// libwayland needs something behind it. There is no error in this enumeration for *that was not a
	// surface*, so the connection ends the only way left.
	if (onto == nullptr)
	{
		Object().PostNoMemory();

		return new ClientSyncSurface{ *m_Context, nullptr };
	}

	auto* const made = new ClientSyncSurface{ *m_Context, onto };

	// **Claimed last, exactly as a role and a viewport are.** A surface that already has one keeps it,
	// and the object the client just asked for stays inert rather than becoming a second writer of one
	// surface's timeline points.
	if (!onto->AdoptSync(*made))
	{
		Object().PostError(
			Wayland::Server::WpLinuxDrmSyncobjManagerV1Error::SurfaceExists,
			"wp_linux_drm_syncobj_manager_v1.get_surface on a surface that already has one"
		);

		made->ForgetSurface();
	}

	return made;
}

Wayland::Server::WpLinuxDrmSyncobjTimelineV1Handler* ClientSyncManager::OnImportTimeline(Fd fd)
{
	ExplicitSync* const sync = m_Context == nullptr ? nullptr : m_Context->Sync();

	if (sync == nullptr)
	{
		// Unreachable: the global is not advertised at all where there is no node to import against, so
		// a client cannot have got this far. Recorded rather than asserted, for `Surface.cpp`'s reason —
		// reaching it would be gyro advertising a contract it does not implement.
		Wayland::Server::RecordFault("import_timeline reached a host with no DRM node open");

		Object().PostError(
			Wayland::Server::WpLinuxDrmSyncobjManagerV1Error::InvalidTimeline,
			"this compositor has no device to import a timeline against"
		);

		return nullptr;
	}

	Result<std::shared_ptr<SyncTimeline>> imported = sync->Import(std::move(fd));

	if (!imported)
	{
		// **Fatal to the client, which the protocol chooses rather than gyro.** A descriptor that is not
		// a syncobj is one every later point named on it would have to be refused for, one at a time,
		// with no way for the client to find out why.
		Object().PostError(
			Wayland::Server::WpLinuxDrmSyncobjManagerV1Error::InvalidTimeline,
			"the descriptor is not a DRM syncobj this compositor can import"
		);

		return nullptr;
	}

	return new ClientSyncTimeline{ std::move(*imported) };
}

Wayland::Server::WpLinuxDrmSyncobjManagerV1Handler* SyncobjGlobal::OnBind(wl_client& client, std::uint32_t version)
{
	static_cast<void>(client);
	static_cast<void>(version);

	return new ClientSyncManager{ *m_Context };
}
