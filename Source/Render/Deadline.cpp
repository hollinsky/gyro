// open and ioctl. POSIX is named for Core/Fd.cpp's reason; the DRM half is Linux's, which is what
// keeps this module out of the portable tier and this file out of Blit.
#define _POSIX_C_SOURCE 200809L

#include "Render/Deadline.h"

#include <drm/drm.h>
#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <sys/ioctl.h>

#include <array>
#include <cerrno>
#include <format>

namespace
{
// Retried on `EINTR` for Nested/Sync.cpp's reason: gyro installs handlers for SIGINT and SIGTERM, so a
// signal arriving mid-ioctl is the ordinary case at shutdown rather than an exotic one.
[[nodiscard]] int Ioctl(int device, unsigned long request, void* argument) noexcept
{
	int result = 0;

	do
	{
		result = ::ioctl(device, request, argument);
	} while (result != 0 && (errno == EINTR || errno == EAGAIN));

	return result;
}
} // namespace

std::string FenceDeadline::NodePath(std::int64_t renderMinor)
{
	return std::format("/dev/dri/renderD{}", renderMinor);
}

std::uint64_t FenceDeadline::Nanoseconds(Instant deadline) noexcept
{
	const std::int64_t nanoseconds = Monotonic::ToNanoseconds(deadline);

	return nanoseconds <= 0 ? 0 : static_cast<std::uint64_t>(nanoseconds);
}

FenceDeadline FenceDeadline::OpenNode(const char* path, RawFd timeline)
{
	FenceDeadline deadline;

	if (!timeline.IsValid())
	{
		return deadline;
	}

	deadline.m_Device = Fd{ ::open(path, O_RDWR | O_CLOEXEC) };

	if (!deadline.m_Device.IsValid())
	{
		return deadline;
	}

	drm_syncobj_handle handle = {};
	handle.fd = timeline.Value;

	if (Ioctl(deadline.m_Device.Get(), DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle) != 0 || handle.handle == 0)
	{
		// The descriptor is not a syncobj this node will adopt — which is what a device that exported
		// something other than an `anon_inode:syncobj_file` looks like from here, and is a report rather
		// than a defect. The node goes with it, so an invalid object holds nothing open.
		deadline.m_Device = Fd{};

		return deadline;
	}

	deadline.m_Handle = handle.handle;

	return deadline;
}

FenceDeadline FenceDeadline::Open(std::int64_t renderMinor, RawFd timeline)
{
	if (renderMinor < 0)
	{
		spdlog::info("no frame deadline: VK_EXT_physical_device_drm gave no render node");

		return {};
	}

	if (!timeline.IsValid())
	{
		// Decision 108's device, and the line says so as information rather than as a warning: a
		// renderer that cannot export a timeline finishes the frame inside `Record` on the CPU, so there
		// is no queued work for a deadline to hurry along.
		spdlog::info("no frame deadline: this device exports no timeline to name a point on");

		return {};
	}

	const std::string path = NodePath(renderMinor);
	FenceDeadline deadline = OpenNode(path.c_str(), timeline);

	if (!deadline.IsValid())
	{
		spdlog::warn("no frame deadline: {} would not adopt the renderer's timeline", path);
	}

	return deadline;
}

void FenceDeadline::State(std::uint64_t point, Instant deadline) noexcept
{
	// The epoch is *no schedule to state*, which an output reaches before its clock has been observed
	// and after a mode set invalidates it. Sending zero would say the frame was wanted before the
	// machine booted, which a driver that honours the hint would read as maximum urgency forever.
	if (!IsValid() || deadline == Instant{})
	{
		return;
	}

	// The ioctl takes arrays by address rather than by value even for one handle, so both live on the
	// stack here — no allocation, per decision 36, and nothing outlives the call.
	std::array<std::uint32_t, 1> handles{ m_Handle };
	std::array<std::uint64_t, 1> points{ point };

	drm_syncobj_timeline_wait wait = {};
	wait.handles = reinterpret_cast<std::uintptr_t>(handles.data());
	wait.points = reinterpret_cast<std::uintptr_t>(points.data());
	wait.timeout_nsec = 0;
	wait.count_handles = 1;
	wait.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE;
	wait.deadline_nsec = Nanoseconds(deadline);

	// Dropped whatever it says. `ETIME` is the expected answer — the composite was submitted a
	// microsecond ago — and the interesting outcomes are indistinguishable from success anyway, since a
	// driver with no `->set_deadline` takes the number and discards it.
	(void)Ioctl(m_Device.Get(), DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait);
}
