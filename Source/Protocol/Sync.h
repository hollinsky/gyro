#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "Core/Fd.h"
#include "Core/Result.h"

struct wl_event_loop;

// DRM syncobj timelines a *client* owns, imported so that gyro can read one and signal the other.
//
// **This is the mirror of [Nested/Sync.h](../Nested/Sync.h) and deliberately not shared with it.**
// That one mints timelines gyro owns, on a node the host named, and is read from the frame thread;
// this one imports timelines a client owns, on the node gyro serves its own clients against, and is
// read from the dispatch thread. The ioctl vocabulary is the same four calls and the lifetime rules
// are opposite — a nested output destroys what it created, and nothing here may ever destroy a
// counter another process is still writing into. A common home for the pair would be a module below
// both halves of the dispatch waist that names `<drm/drm.h>`, which is a platform module gyro does not
// otherwise need; the duplication is forty lines of `ioctl` wrapping and is cheaper than that.
//
// **Nothing here ever waits.** `Signalled` is a query and `Watch` arms an eventfd; there is no
// blocking call in the file, because the whole reason gyro imports a client's acquire point is to
// avoid putting a client's fence anywhere gyro's own schedule can feel it. See Docs/Decisions.md
// decision 174.
//
// The ABI is `#include`d rather than transcribed, for [Nested/Sync.h](../Nested/Sync.h)'s reason:
// `<drm/drm.h>` arrives with `kernel-headers` rather than with libdrm, so it costs no package and no
// pkg-config entry, and `Protocol` is a platform module.

// One timeline a client imported, held by gyro for as long as anything still owes a point on it.
//
// **Shared rather than owned by the protocol object, and the release direction is why.** A client may
// destroy its `wp_linux_drm_syncobj_timeline_v1` the instant after it commits, and gyro still owes a
// signal on that timeline when it stops reading the buffer — which is frames later. So the handle
// outlives the object that imported it, held by every pending release that names it.
class SyncTimeline
{
public:
	SyncTimeline() = default;

	~SyncTimeline();

	SyncTimeline(const SyncTimeline&) = delete;
	SyncTimeline& operator=(const SyncTimeline&) = delete;
	SyncTimeline(SyncTimeline&&) = delete;
	SyncTimeline& operator=(SyncTimeline&&) = delete;

	[[nodiscard]] bool IsValid() const noexcept { return m_Handle != 0 && m_Device.IsValid(); }

	// How far the client has actually got.
	//
	// **Signalled rather than submitted.** `DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED` would report a
	// point the client has promised, and a promise is not pixels — sampling on one is the tear this
	// protocol exists to remove. A failed query answers zero, which reads as *not yet* and holds the
	// commit, because there is no honest reading of a device that stopped answering under which the
	// buffer is safe.
	[[nodiscard]] std::uint64_t Signalled() const noexcept;

	[[nodiscard]] bool HasSignalled(std::uint64_t point) const noexcept { return Signalled() >= point; }

	// gyro has finished reading the buffer this point was named for.
	//
	// **Signalling a point signals every point below it**, which the protocol says outright and which is
	// what makes the ordering rule bearable: gyro may answer a client's release points in any order it
	// likes, and a client that numbered them monotonically gets the conservative answer for free. The
	// caller's job is only to not signal one early, which is why the whole of this file's release path
	// hangs off the same watermark `wl_buffer.release` already does.
	//
	// Reports rather than aborts: a client that destroyed its DRM node out from under gyro is a client
	// that will wait forever for its own buffer, and that is its own doing rather than a fault here.
	[[nodiscard]] Result<void> Signal(std::uint64_t point) const noexcept;

	// Increment `eventfd` once `point` has signalled.
	//
	// **It accepts a point that does not exist yet, and that is the whole reason this is the ioctl
	// rather than a poll on an exported `sync_file`.** A timeline point may be named before the work
	// behind it is submitted — that is what a timeline is for — and exporting a `sync_file` from an
	// unmaterialized point fails. The kernel registers the wait against the syncobj instead and fires
	// it whenever the point turns up, so a client that commits before it submits is held rather than
	// refused.
	//
	// `ENOSYS` where the kernel has no `DRM_IOCTL_SYNCOBJ_EVENTFD`, which is what `ExplicitSync::Open`
	// probes for before the global is advertised at all.
	[[nodiscard]] Result<void> Watch(std::uint64_t point, RawFd eventfd) const noexcept;

private:
	friend class ExplicitSync;

	// Not owned. The device outlives every timeline imported through it, which the host sequences by
	// destroying its clients before the node they imported against.
	RawFd m_Device;

	std::uint32_t m_Handle = 0;
};

// One point on one client timeline: the two words a `set_acquire_point` or a `set_release_point`
// carries, with the timeline held rather than named.
//
// **The `shared_ptr` is the release direction paying for both.** An acquire point is read within the
// commit that carried it and could borrow; a release point outlives the client's timeline object by
// however long gyro goes on reading the buffer, and one type for both is what stops the second case
// being written as the first by somebody reading only the first.
struct SyncTimelinePoint
{
	std::shared_ptr<SyncTimeline> Timeline;

	std::uint64_t Point = 0;

	[[nodiscard]] bool IsSet() const noexcept { return Timeline != nullptr; }

	// Whether the client has got here yet. False for a point nobody set, which is what makes an
	// unset acquire read as *nothing to wait for* at the one call site that asks.
	[[nodiscard]] bool HasSignalled() const noexcept { return IsSet() && Timeline->HasSignalled(Point); }
};

// The DRM node gyro imports its clients' timelines against, and the loop a held commit is resumed
// from.
//
// **One per host rather than one per client**, because a syncobj descriptor is a file any DRM node can
// import a handle for — nothing here submits work or allocates memory — so the node is an
// implementation detail of gyro rather than a device a client is being given access to.
//
// **Absence is an ordinary answer and it is the whole of the availability rule.** A machine with no
// GPU has no dmabuf clients to synchronize; a kernel without the eventfd ioctl cannot arm a wait, and
// the only alternatives are blocking the dispatch thread or sampling before the acquire point, so gyro
// does not advertise the global at all rather than advertising one it would have to break the contract
// to serve.
class ExplicitSync
{
public:
	ExplicitSync() = default;

	ExplicitSync(const ExplicitSync&) = delete;
	ExplicitSync& operator=(const ExplicitSync&) = delete;
	ExplicitSync(ExplicitSync&&) = delete;
	ExplicitSync& operator=(ExplicitSync&&) = delete;

	// Open the node whose `st_rdev` is `device` — `ITextures::MainDevice()`, which is the same number
	// the dmabuf feedback tells a client to allocate against — or the first render node that opens
	// where that fails, per [Nested/Sync.h](../Nested/Sync.h)'s argument that any node will do.
	//
	// **A zero device is refused rather than treated as *any node will do*.** That fallback is right
	// once gyro knows there are descriptors in play and wrong before: zero is a machine whose format
	// list is empty, so nothing a client could attach is synchronizable, and a global advertised there
	// would appear or not according to whether `/dev/dri` exists.
	//
	// `ENODEV` where there is no device or nothing opened; `ENOSYS` where the node opened and the kernel
	// has no eventfd ioctl. Both are answers the caller degrades on rather than fails to come up over.
	[[nodiscard]] Result<void> Open(std::uint64_t device, wl_event_loop& loop);

	[[nodiscard]] bool IsAvailable() const noexcept { return m_Device.IsValid(); }

	// Which node was opened, for the line that says explicit sync is live and on what.
	[[nodiscard]] const std::string& Path() const noexcept { return m_Path; }

	[[nodiscard]] wl_event_loop* Loop() const noexcept { return m_Loop; }

	// A client's `wp_linux_drm_syncobj_manager_v1.import_timeline` descriptor as something gyro can
	// read and write. The descriptor is consumed either way — the kernel duplicates what it needs into
	// the handle, and a descriptor the caller kept would be one nobody closes.
	//
	// A failure is `invalid_timeline`, which the protocol makes fatal to the client: a handle that will
	// not import is one every later request on it would have to answer for.
	[[nodiscard]] Result<std::shared_ptr<SyncTimeline>> Import(Fd descriptor) const;

private:
	Fd m_Device;
	std::string m_Path;

	// Not owned; the display outlives the host that opened this.
	wl_event_loop* m_Loop = nullptr;
};
