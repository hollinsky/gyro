// open, stat, and the DRM ioctls. POSIX is named for Core/Fd.cpp's reason; the DRM half is Linux's
// and is exactly why this module is not in the portable tier.
#define _POSIX_C_SOURCE 200809L

#include "Nested/Sync.h"

#include <dirent.h>
#include <drm/drm.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <string_view>
#include <vector>

namespace Nested
{
namespace
{
// Where the kernel puts them. A literal rather than a udev query, because the only thing being looked
// for is a node to mint a counter on and gyro is not enumerating devices here.
constexpr std::string_view DriDirectory = "/dev/dri";

// Retried on `EINTR` because an ioctl on a DRM node is interruptible and a signal arriving mid-call
// is not a failure to report — gyro installs handlers for SIGINT and SIGTERM, so this is the ordinary
// case at shutdown rather than an exotic one.
[[nodiscard]] int Ioctl(RawFd device, unsigned long request, void* argument) noexcept
{
	int result = 0;

	do
	{
		result = ::ioctl(device.Value, request, argument);
	} while (result != 0 && (errno == EINTR || errno == EAGAIN));

	return result;
}

[[nodiscard]] bool IsRenderNode(std::string_view name) noexcept
{
	return name.starts_with("renderD");
}

// Every node in `/dev/dri`, render nodes first and each with the device number it carries. Sorted so
// that a machine with two GPUs opens the same one twice rather than whatever `readdir` felt like.
[[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> Nodes()
{
	std::vector<std::pair<std::string, std::uint64_t>> found;

	::DIR* const directory = ::opendir(DriDirectory.data());

	if (directory == nullptr)
	{
		return found;
	}

	while (const ::dirent* const entry = ::readdir(directory))
	{
		const std::string_view name{ entry->d_name };

		if (!IsRenderNode(name) && !name.starts_with("card"))
		{
			continue;
		}

		std::string path{ DriDirectory };
		path += '/';
		path += name;

		struct ::stat status = {};

		if (::stat(path.c_str(), &status) != 0)
		{
			continue;
		}

		found.emplace_back(std::move(path), static_cast<std::uint64_t>(status.st_rdev));
	}

	::closedir(directory);

	std::ranges::sort(found, [](const auto& left, const auto& right) {
		const bool leftRender = IsRenderNode(std::string_view{ left.first }.substr(DriDirectory.size() + 1));
		const bool rightRender = IsRenderNode(std::string_view{ right.first }.substr(DriDirectory.size() + 1));

		return leftRender != rightRender ? leftRender : left.first < right.first;
	});

	return found;
}
} // namespace

void DrmTimeline::Reset() noexcept
{
	if (m_Handle != 0 && m_Device.IsValid())
	{
		drm_syncobj_destroy destroy = {};
		destroy.handle = m_Handle;

		// Dropped for Core/Fd.cpp's reason: a destroy that fails names a handle this object is the
		// only holder of, so there is nothing a caller could do and this usually runs in a destructor.
		(void)Ioctl(m_Device, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
	}

	m_Device = RawFd{};
	m_Descriptor = Fd{};
	m_Handle = 0;
}

std::uint64_t DrmTimeline::Signalled() const noexcept
{
	if (m_Handle == 0 || !m_Device.IsValid())
	{
		return 0;
	}

	std::uint64_t point = 0;
	std::uint32_t handle = m_Handle;

	drm_syncobj_timeline_array query = {};
	query.handles = reinterpret_cast<std::uintptr_t>(&handle);
	query.points = reinterpret_cast<std::uintptr_t>(&point);
	query.count_handles = 1;

	// Without `LAST_SUBMITTED` this reports what has actually been *signalled*, which is the question
	// being asked: a point the host has merely promised is not a buffer gyro may draw into.
	query.flags = 0;

	if (Ioctl(m_Device, DRM_IOCTL_SYNCOBJ_QUERY, &query) != 0)
	{
		// Not yet, per the header. The ring stalls and the frame loop skips this output, which is the
		// behaviour a host that stopped releasing already produces.
		return 0;
	}

	return point;
}

Result<DrmSyncobjDevice> DrmSyncobjDevice::Open(std::uint64_t device)
{
	const std::vector<std::pair<std::string, std::uint64_t>> nodes = Nodes();

	if (nodes.empty())
	{
		return Failure(ENODEV, "no DRM node to mint a release timeline on");
	}

	// The host's answer first. `O_CLOEXEC` because gyro spawns nothing today and will: a descriptor
	// that survives an exec is one a child holds open on a device it never asked for.
	for (const bool wanted : { true, false })
	{
		for (const auto& [path, number] : nodes)
		{
			if (wanted != (device != 0 && number == device))
			{
				continue;
			}

			const int opened = ::open(path.c_str(), O_RDWR | O_CLOEXEC);

			if (opened < 0)
			{
				continue;
			}

			DrmSyncobjDevice made;
			made.m_Device = Fd{ opened };
			made.m_Path = path;

			return made;
		}
	}

	return Failure(errno == 0 ? ENODEV : errno, "opening a DRM node to mint release timelines on");
}

Result<DrmTimeline> DrmSyncobjDevice::CreateTimeline() const
{
	if (!IsValid())
	{
		return Failure(ENODEV, "no DRM node to create a syncobj on");
	}

	drm_syncobj_create create = {};

	if (Ioctl(m_Device.Borrow(), DRM_IOCTL_SYNCOBJ_CREATE, &create) != 0)
	{
		return Failure(errno, "creating a DRM syncobj for a nested output's release timeline");
	}

	DrmTimeline timeline;
	timeline.m_Device = m_Device.Borrow();
	timeline.m_Handle = create.handle;

	drm_syncobj_handle handle = {};
	handle.handle = create.handle;

	// **`TIMELINE` is not the flag to set here.** That one asks for a *sync file* at a point, which is
	// a binary fence; what `wp_linux_drm_syncobj_v1.import_timeline` wants is the syncobj itself, so
	// the descriptor goes out bare and the host imports the whole timeline.
	handle.flags = 0;
	handle.fd = InvalidFd;

	if (Ioctl(m_Device.Borrow(), DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle) != 0)
	{
		const int failure = errno;

		// The handle is this object's until the descriptor exists, and the move below is what would
		// otherwise have adopted it. Destroyed here so a failed export does not leak one per target
		// per resize.
		drm_syncobj_destroy destroy = {};
		destroy.handle = create.handle;
		(void)Ioctl(m_Device.Borrow(), DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);

		timeline.m_Handle = 0;
		timeline.m_Device = RawFd{};

		return Failure(failure, "exporting a DRM syncobj as a descriptor the host can import");
	}

	timeline.m_Descriptor = Fd{ handle.fd };

	return timeline;
}
} // namespace Nested
