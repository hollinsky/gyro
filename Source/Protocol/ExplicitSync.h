#pragma once

#include <cstdint>
#include <optional>

#include "Core/Fd.h"
#include "Protocol/Context.h"
#include "Protocol/Sync.h"
#include "Wayland/Server/LinuxDrmSyncobjV1.h"
#include "Wayland/Server/Wayland.h"

struct wl_event_source;

class ClientSurface;

// `wp_linux_drm_syncobj_v1`: a client saying when its pixels are finished, and gyro saying when it has
// stopped reading them.
//
// **The reason to serve it is not parity with other compositors — it is that the alternative already
// stalls gyro's composite.** Every dmabuf client today is on implicit synchronization, which means the
// kernel attaches the client's fence to the buffer and inserts a wait for it into gyro's *own* queue
// submission at composite time. gyro cannot see that wait, cannot schedule around it, and cannot
// preempt it: preemption preempts work that is running and does nothing to a dependency that has not
// resolved. So a client that is late finishing its frame is today a client that holds up the composite
// for every window on the machine, on a `SCHED_FIFO` thread whose whole promise is hitting the next
// refresh. This protocol is what makes that visible and then removes it — the spec lets a compositor
// ignore implicit synchronization for a surface carrying one of these objects, and gyro does.
//
// **A client's fence never reaches gyro's queue or a plane's `IN_FENCE_FD`.** The commit is held on the
// dispatch thread instead: the acquire point is watched with `DRM_IOCTL_SYNCOBJ_EVENTFD` on the
// display's own event loop, and the surface state becomes current when that fires. What a person sees
// while a client is late is the frame it last finished, which is what they would see anyway — and what
// they no longer see is every *other* window waiting behind it.
//
// **That is the opposite of the trade decision 125 made, and the asymmetry is the point.** There gyro
// was the client and had to name a point it had not reached so the host could plan its frame around
// gyro's; refusing would have cost gyro its own latency. Here gyro is the host, and waiting inside its
// own frame would spend gyro's deadline on a client's. A late client should drop a frame. The
// compositor should not. See Docs/Decisions.md decision 174.
//
// **Version 1, which is the whole protocol.** There is no second version to be behind, and
// [Compositor.h](Compositor.h)'s rule — the number is a promise about what gyro sends — has nothing to
// choose between.
inline constexpr std::uint32_t SyncobjVersion = 1;

// One `wp_linux_drm_syncobj_timeline_v1`: a client's counter, imported.
//
// The object is a handle onto something gyro holds rather than something gyro owns, so destroying it
// does not end the timeline — every release point still owed on it keeps its own reference, which is
// what makes a client free to destroy the object the instant after it commits.
class ClientSyncTimeline final : public Wayland::Server::WpLinuxDrmSyncobjTimelineV1Handler
{
public:
	explicit ClientSyncTimeline(std::shared_ptr<SyncTimeline> timeline) noexcept : m_Timeline{ std::move(timeline) } {}

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	[[nodiscard]] const std::shared_ptr<SyncTimeline>& Timeline() const noexcept { return m_Timeline; }

	// The implementation behind an id a client named, or null where the id was not one of these.
	// `ClientBuffer::Of`'s argument exactly.
	[[nodiscard]] static ClientSyncTimeline* Of(Wayland::Server::WpLinuxDrmSyncobjTimelineV1 timeline) noexcept
	{
		return static_cast<ClientSyncTimeline*>(timeline.Implementation());
	}

private:
	std::shared_ptr<SyncTimeline> m_Timeline;
};

// One `wp_linux_drm_syncobj_surface_v1`: the two points a surface names per commit, and the wait that
// holds the commit until the first of them signals.
//
// **The object holds the staged points and the surface holds the applied ones**, which is the same
// division `wp_viewport` already takes: the protocol double-buffers this state on the `wl_surface`, so
// what lives here is only what has been said since the last commit, and the commit moves it across.
class ClientSyncSurface final : public Wayland::Server::WpLinuxDrmSyncobjSurfaceV1Handler
{
public:
	// Null only where the client named something that was not a `wl_surface`, or where the surface
	// already had one of these — in both cases the caller has ended the client and the object exists
	// only so libwayland has something behind an id the client is holding. `Viewporter.cpp`'s inert
	// object exactly.
	ClientSyncSurface(HostContext& context, ClientSurface* surface) noexcept
		: m_Context{ &context }, m_Surface{ surface }
	{}

	~ClientSyncSurface() override;

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately. The staged
	// points go with it, which is what the protocol says a destroy may do to points set since the last
	// commit — and it says nothing about ones already committed, which is why the wait below is not
	// cancelled here.
	void OnDestroy() override {}

	void OnSetAcquirePoint(
		Wayland::Server::WpLinuxDrmSyncobjTimelineV1 timeline,
		std::uint32_t pointHi,
		std::uint32_t pointLo
	) override;

	void OnSetReleasePoint(
		Wayland::Server::WpLinuxDrmSyncobjTimelineV1 timeline,
		std::uint32_t pointHi,
		std::uint32_t pointLo
	) override;

	// The `wl_surface` went out from under this object. Everything after that is `no_surface`.
	void ForgetSurface() noexcept { m_Surface = nullptr; }

	// Check what this commit staged and hand it over.
	//
	// **All four commit-time errors are asked here and nowhere else**, because they are questions about
	// the *pair* — whether a buffer arrived with both points, or neither, or two points that cannot both
	// be true — and a check split across the two request handlers would be asking each half about a
	// state the other half has not written yet.
	//
	// `attached` is what `wl_surface.attach` staged: an absent optional is a commit that did not attach,
	// and an optional holding an invalid resource is a client taking its window off the screen. Neither
	// is *a buffer attached*, so neither obliges a point.
	//
	// False having ended the client, which is what every error in this protocol is.
	[[nodiscard]] bool TakeCommit(
		const std::optional<Wayland::Server::WlBuffer>& attached,
		SyncTimelinePoint& acquire,
		SyncTimelinePoint& release
	);

	// Wake the surface when `acquire` signals. False where the wait could not be armed at all, which
	// the caller answers by publishing rather than by holding the window forever — a kernel that
	// refused the ioctl is gyro's problem and a frozen window is the client's punishment for it.
	[[nodiscard]] bool Watch(const SyncTimelinePoint& acquire);

	// The implementation behind an id a client named, or null where the id was not one of these.
	[[nodiscard]] static ClientSyncSurface* Of(Wayland::Server::WpLinuxDrmSyncobjSurfaceV1 surface) noexcept
	{
		return static_cast<ClientSyncSurface*>(surface.Implementation());
	}

private:
	// libwayland's `wl_event_loop_fd_func_t`: the acquire point this surface was held on has signalled.
	static int OnSignalled(int descriptor, std::uint32_t mask, void* data) noexcept;

	// Take down whatever wait is armed. Called by the destructor and by each new arming, because a
	// surface has at most one held commit and a second one supersedes the first.
	void Disarm() noexcept;

	HostContext* m_Context = nullptr;

	// The surface this object synchronizes, or null once that surface has gone.
	ClientSurface* m_Surface = nullptr;

	// What has been said since the last commit. Cleared by `TakeCommit`, which is the double buffering
	// the protocol specifies.
	SyncTimelinePoint m_Acquire;
	SyncTimelinePoint m_Release;

	// The armed wait: an eventfd the kernel increments and the loop source reading it. Both null while
	// nothing is held, which is every surface whose client is keeping up.
	Fd m_Eventfd;
	wl_event_source* m_Source = nullptr;
};

// One client's `wp_linux_drm_syncobj_manager_v1`. No per-client state: the node is the host's and the
// timelines belong to the objects this mints.
class ClientSyncManager final : public Wayland::Server::WpLinuxDrmSyncobjManagerV1Handler
{
public:
	explicit ClientSyncManager(HostContext& context) noexcept : m_Context{ &context } {}

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately. Destroying
	// the factory does not touch what it made, which the protocol states outright.
	void OnDestroy() override {}

	Wayland::Server::WpLinuxDrmSyncobjSurfaceV1Handler* OnGetSurface(Wayland::Server::WlSurface surface) override;

	Wayland::Server::WpLinuxDrmSyncobjTimelineV1Handler* OnImportTimeline(Fd fd) override;

private:
	HostContext* m_Context = nullptr;
};

// The global itself, owned by whoever advertises it and outliving every client that binds it.
class SyncobjGlobal final : public Wayland::Server::WpLinuxDrmSyncobjManagerV1Binding
{
public:
	explicit SyncobjGlobal(HostContext& context) noexcept : m_Context{ &context } {}

	Wayland::Server::WpLinuxDrmSyncobjManagerV1Handler* OnBind(wl_client& client, std::uint32_t version) override;

private:
	HostContext* m_Context = nullptr;
};
