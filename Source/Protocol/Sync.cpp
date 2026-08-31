// open, stat, and the DRM ioctls. POSIX is named for Core/Fd.cpp's reason; the DRM half is Linux's and
// is why this file sits in a module that is not in the portable tier.
#define _POSIX_C_SOURCE 200809L

#include "Protocol/Sync.h"

#include <dirent.h>
#include <drm/drm.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cerrno>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
// Where the kernel puts them. A literal rather than a udev query, for Nested/Sync.cpp's reason: the
// only thing being looked for is a node to import a counter against.
constexpr std::string_view DriDirectory = "/dev/dri";

// Retried on `EINTR` because an ioctl on a DRM node is interruptible and a signal arriving mid-call is
// not a failure to report — gyro installs handlers for SIGINT and SIGTERM, so this is the ordinary
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

// Whether this kernel can arm a wait at all, asked once at open.
//
// **The probe is the ioctl itself with a handle that cannot exist**, because there is nothing else to
// ask: a kernel without it answers `ENOTTY` before it looks at the arguments, and one with it gets as
// far as failing to find handle zero. Anything other than `ENOTTY` therefore means the call is
// implemented, which is the whole of the question.
[[nodiscard]] bool HasEventfd(RawFd device) noexcept
{
	drm_syncobj_eventfd request = {};
	request.handle = 0;
	request.fd = InvalidFd;

	return Ioctl(device, DRM_IOCTL_SYNCOBJ_EVENTFD, &request) == 0 || errno != ENOTTY;
}
} // namespace

SyncTimeline::~SyncTimeline()
{
	if (m_Handle == 0 || !m_Device.IsValid())
	{
		return;
	}

	// **This destroys gyro's *handle*, never the client's counter.** A syncobj is refcounted by the
	// kernel and the client holds its own reference through the descriptor it imported from, so what
	// goes away here is one process's name for a shared object.
	drm_syncobj_destroy destroy = {};
	destroy.handle = m_Handle;

	// Dropped for Core/Fd.cpp's reason: a destroy that fails names a handle this object is the only
	// holder of, so there is nothing a caller could do and this runs in a destructor.
	(void)Ioctl(m_Device, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
}

std::uint64_t SyncTimeline::Signalled() const noexcept
{
	if (!IsValid())
	{
		return 0;
	}

	std::uint64_t point = 0;
	std::uint32_t handle = m_Handle;

	drm_syncobj_timeline_array query = {};
	query.handles = reinterpret_cast<std::uintptr_t>(&handle);
	query.points = reinterpret_cast<std::uintptr_t>(&point);
	query.count_handles = 1;
	query.flags = 0;

	if (Ioctl(m_Device, DRM_IOCTL_SYNCOBJ_QUERY, &query) != 0)
	{
		return 0;
	}

	return point;
}

Result<void> SyncTimeline::Signal(std::uint64_t point) const noexcept
{
	if (!IsValid())
	{
		return Failure(ENODEV, "signalling a release point on a timeline that did not import");
	}

	std::uint64_t value = point;
	std::uint32_t handle = m_Handle;

	drm_syncobj_timeline_array signal = {};
	signal.handles = reinterpret_cast<std::uintptr_t>(&handle);
	signal.points = reinterpret_cast<std::uintptr_t>(&value);
	signal.count_handles = 1;

	if (Ioctl(m_Device, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &signal) != 0)
	{
		return Failure(errno, "signalling a client's buffer release point");
	}

	return {};
}

Result<void> SyncTimeline::Watch(std::uint64_t point, RawFd eventfd) const noexcept
{
	if (!IsValid())
	{
		return Failure(ENODEV, "watching an acquire point on a timeline that did not import");
	}

	drm_syncobj_eventfd watch = {};
	watch.handle = m_Handle;
	watch.point = point;
	watch.fd = eventfd.Value;

	// Zero rather than `WAIT_AVAILABLE`: the question is whether the client's pixels are *finished*,
	// not whether it has submitted the work that will finish them.
	watch.flags = 0;

	if (Ioctl(m_Device, DRM_IOCTL_SYNCOBJ_EVENTFD, &watch) != 0)
	{
		return Failure(errno == ENOTTY ? ENOSYS : errno, "arming a wait on a client's acquire point");
	}

	return {};
}

Result<void> ExplicitSync::Open(std::uint64_t device, wl_event_loop& loop)
{
	// **No device means no clients to synchronize, and that is a rule rather than a shortcut.** The
	// number is `ITextures::MainDevice()`, which is what the dmabuf feedback tells a client to allocate
	// against — zero there is a machine whose format list is empty, where every client draws into shared
	// memory and `wl_shm` is `unsupported_buffer` anyway. Opening whatever node happened to be in
	// `/dev/dri` would make the global's presence depend on a directory rather than on whether gyro can
	// take a descriptor at all.
	if (device == 0)
	{
		return Failure(ENODEV, "no main device, so there are no dmabuf clients to synchronize");
	}

	const std::vector<std::pair<std::string, std::uint64_t>> nodes = Nodes();

	if (nodes.empty())
	{
		return Failure(ENODEV, "no DRM node to import client timelines against");
	}

	// The device the clients are told to allocate against first, any render node second.
	for (const bool wanted : { true, false })
	{
		for (const auto& [path, number] : nodes)
		{
			if (wanted != (number == device))
			{
				continue;
			}

			// `O_CLOEXEC` because gyro spawns nothing today and will: a descriptor that survives an exec
			// is one a child holds open on a device it never asked for.
			const int opened = ::open(path.c_str(), O_RDWR | O_CLOEXEC);

			if (opened < 0)
			{
				continue;
			}

			Fd held{ opened };

			if (!HasEventfd(held.Borrow()))
			{
				return Failure(
					ENOSYS,
					"this kernel has no DRM_IOCTL_SYNCOBJ_EVENTFD, so a client's acquire point could only "
					"be waited on by blocking the dispatch thread"
				);
			}

			m_Device = std::move(held);
			m_Path = path;
			m_Loop = &loop;

			return {};
		}
	}

	return Failure(errno == 0 ? ENODEV : errno, "opening a DRM node to import client timelines against");
}

Result<std::shared_ptr<SyncTimeline>> ExplicitSync::Import(Fd descriptor) const
{
	if (!IsAvailable())
	{
		return Failure(ENODEV, "importing a client timeline with no DRM node open");
	}

	if (!descriptor.IsValid())
	{
		return Failure(EINVAL, "importing a client timeline from no descriptor");
	}

	drm_syncobj_handle handle = {};
	handle.fd = descriptor.Get();

	// **Bare rather than `IMPORT_SYNC_FILE`**, which is the same distinction Nested/Sync.cpp draws in
	// the other direction: what the protocol hands over is the syncobj itself, so what comes back is a
	// handle onto the client's whole timeline rather than a fence at one point of it.
	handle.flags = 0;

	if (Ioctl(m_Device.Borrow(), DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle) != 0)
	{
		return Failure(errno, "importing a client's DRM syncobj timeline");
	}

	auto timeline = std::make_shared<SyncTimeline>();
	timeline->m_Device = m_Device.Borrow();
	timeline->m_Handle = handle.handle;

	return timeline;
}
