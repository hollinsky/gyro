#pragma once

#include <cstdint>
#include <vector>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Seam/SyncPoint.h"

// A point on the renderer's timeline, turned into the thing an atomic commit takes.
//
// **The two ends of gyro's sync story do not speak the same dialect, and this is the translation.**
// Seam/SyncPoint.h is uniform on purpose — DRM syncobj timelines throughout, exported from Vulkan
// timeline semaphores — but `IN_FENCE_FD` is not a syncobj: it is a `sync_file`, a binary fence with
// no timeline behind it. The kernel's own answer to that mismatch is `DRM_IOCTL_SYNCOBJ_TRANSFER`,
// which materialises one point of a timeline into a binary syncobj, and `HANDLE_TO_FD` with
// `EXPORT_SYNC_FILE`, which hands that out as a descriptor.
//
// **Two ioctls per frame, and the rest is cached because the frame section forbids the rest.** The
// import of the renderer's timeline is per timeline rather than per frame, and the binary syncobj the
// point lands in is created once and reused — so what remains inside `Present` is a transfer, an
// export, and a close, none of which allocate. Core/FrameSection.h would abort on anything that did.
//
// **Failure is a fallback rather than an error.** A device that will not export a sync file leaves
// the caller holding a commit until the composite has actually landed, which is Nested/Output.h's
// held-commit path and costs the overlap between drawing a frame and handing it over. Saying so once
// in the log is the whole of the report; there is nothing a frame loop can do about it per frame.

namespace Drm
{
class FenceExporter
{
public:
	FenceExporter() = default;

	// `device` is the card node the commit will go out on, borrowed: a syncobj handle is per file
	// description, so the handles cached here mean nothing on any other descriptor.
	explicit FenceExporter(RawFd device) noexcept : m_Device{ device } {}

	~FenceExporter() { Reset(); }

	FenceExporter(const FenceExporter&) = delete;
	FenceExporter& operator=(const FenceExporter&) = delete;

	FenceExporter(FenceExporter&& other) noexcept;
	FenceExporter& operator=(FenceExporter&& other) noexcept;

	// A `sync_file` descriptor that signals when `point` does, or nothing where this device cannot
	// produce one. An immediate point answers an invalid descriptor and success — a commit with no
	// fence on it is the correct commit for content that is already finished.
	[[nodiscard]] Result<Fd> Export(SyncPoint point);

private:
	// One imported timeline: the descriptor it came in on, and the handle it has on this device.
	// Matched by descriptor number, which is what a renderer hands out per frame and what decision 41's
	// device rebuild changes — a replaced renderer arrives as a descriptor this has never seen rather
	// than as a silent reuse of the old handle.
	struct Imported
	{
		int Descriptor = InvalidFd;
		std::uint32_t Handle = 0;
	};

	void Reset() noexcept;

	[[nodiscard]] Result<std::uint32_t> HandleFor(RawFd timeline);

	RawFd m_Device;
	std::vector<Imported> m_Timelines;

	// Where a transferred point lands before it is exported. One per exporter and therefore one per
	// output, which is what makes the reuse safe: an output has at most `CommitDepth` commits in the
	// air and overwrites this between them, and the descriptor already exported keeps the fence it
	// named alive on its own.
	std::uint32_t m_Scratch = 0;
};
} // namespace Drm
