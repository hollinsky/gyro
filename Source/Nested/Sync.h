#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "Core/Fd.h"
#include "Core/Result.h"

// DRM syncobj timelines, minted and read by gyro itself, so that a nested surface can name a release
// point.
//
// **This exists because `wp_linux_drm_syncobj_v1` is all-or-nothing per surface.** A surface that has
// one of these objects must name *both* an acquire point and a release point on every commit, or the
// host answers `no_release_point` and the connection ends. The acquire point is the renderer's own
// timeline and arrives inside a `SyncPoint`; the release point is the other direction — the host
// signals it when it has finished reading — and nothing in the tree hands one out. `Nested` may not
// name `Render`, and a verb on Seam/Allocator.h that one of three providers could answer is one
// implementation wearing a seam. So the backend makes its own.
//
// **The device is named by the host rather than guessed at.** `zwp_linux_dmabuf_v1`'s feedback
// carries a main device and a per-tranche target device as `dev_t`s, which is already the fact the
// modifier choice rests on; this reads the same fact for a second purpose. Where the named device
// cannot be opened, any DRM device on the machine will do, and that is a property of the object
// rather than a shortcut: a `drm_syncobj` is DRM core rather than a driver's, its descriptor is a
// file that any DRM node can import a handle for, and nothing here ever submits work or allocates
// memory through this descriptor. It is used to mint a shared counter and to read it.
//
// **The ABI is included rather than transcribed**, unlike Seam/RenderTarget.h's fourccs — the
// difference being that `<drm/drm.h>` arrives with `kernel-headers` rather than with libdrm, so it
// costs no package and no pkg-config entry, and Seam's rule about platform headers is a rule about
// the *portable tier* rather than about this module.
//
// **Absence is an ordinary answer.** A machine whose render node is not reachable, or a host with no
// syncobj protocol, falls back to the path Nested/Output.h describes: the commit is held until the
// composite has actually landed. Everything here reports rather than aborts.

namespace Nested
{
// One timeline, and the two halves of it a nested output needs: a descriptor to hand the host, and a
// handle to read the counter back through.
class DrmTimeline
{
public:
	DrmTimeline() = default;

	~DrmTimeline() { Reset(); }

	DrmTimeline(const DrmTimeline&) = delete;
	DrmTimeline& operator=(const DrmTimeline&) = delete;

	DrmTimeline(DrmTimeline&& other) noexcept
		: m_Device{ other.m_Device }, m_Descriptor{ std::move(other.m_Descriptor) },
		  m_Handle{ std::exchange(other.m_Handle, 0) }
	{
		other.m_Device = RawFd{};
	}

	DrmTimeline& operator=(DrmTimeline&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			m_Device = std::exchange(other.m_Device, RawFd{});
			m_Descriptor = std::move(other.m_Descriptor);
			m_Handle = std::exchange(other.m_Handle, 0);
		}

		return *this;
	}

	[[nodiscard]] bool IsValid() const noexcept { return m_Descriptor.IsValid(); }

	// Borrowed, per Core/Fd.h: the timeline outlives every point named on it, exactly as
	// Seam/SyncPoint.h says of the renderer's.
	[[nodiscard]] RawFd Descriptor() const noexcept { return m_Descriptor.Borrow(); }

	// How far the host has got. **A query rather than a wait**, for `IRenderer::IsComplete`'s reason
	// one seam over: this is read from the frame thread, and a blocking answer would put the host
	// compositor's schedule on gyro's.
	//
	// A failure comes back as *not yet* rather than as an error, because the caller is
	// `AcquireTarget` and the only two things it can say are yes and not now. A device that has
	// stopped answering leaves the ring stalled, which is the same outcome as a host that stopped
	// releasing and is the one the frame loop already tolerates.
	[[nodiscard]] std::uint64_t Signalled() const noexcept;

private:
	friend class DrmSyncobjDevice;

	void Reset() noexcept;

	// Not owned. The device outlives every timeline it made, which the composition root sequences by
	// destroying the outputs before the host.
	RawFd m_Device;

	Fd m_Descriptor;
	std::uint32_t m_Handle = 0;
};

// A DRM node open for one purpose: minting the timelines above.
class DrmSyncobjDevice
{
public:
	DrmSyncobjDevice() = default;

	// Opens the node whose `st_rdev` is `device`, or the first render node that opens where that
	// fails.
	//
	// `device` is what `zwp_linux_dmabuf_feedback_v1.tranche_target_device` carried; zero means the
	// host said nothing and any node will do. `ENODEV` where nothing opened, which is the case a
	// container without `/dev/dri` produces and is a fallback rather than a failure to come up.
	[[nodiscard]] static Result<DrmSyncobjDevice> Open(std::uint64_t device);

	[[nodiscard]] bool IsValid() const noexcept { return m_Device.IsValid(); }

	// Which node was opened, for the line that says whether explicit sync is live and on what.
	[[nodiscard]] const std::string& Path() const noexcept { return m_Path; }

	// A fresh timeline at zero. Unbounded and allocating by contract, like everything else on the
	// target-set path: it runs when an output is configured, never inside a frame.
	[[nodiscard]] Result<DrmTimeline> CreateTimeline() const;

private:
	Fd m_Device;
	std::string m_Path;
};
} // namespace Nested
