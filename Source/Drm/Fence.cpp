#define _POSIX_C_SOURCE 200809L

#include "Drm/Fence.h"

#include <drm/drm.h>
#include <sys/ioctl.h>
#include <xf86drm.h>

#include <cerrno>
#include <utility>

namespace Drm
{
FenceExporter::FenceExporter(FenceExporter&& other) noexcept
	: m_Device{ std::exchange(other.m_Device, RawFd{}) }, m_Timelines{ std::move(other.m_Timelines) },
	  m_Scratch{ std::exchange(other.m_Scratch, 0) }
{
	other.m_Timelines.clear();
}

FenceExporter& FenceExporter::operator=(FenceExporter&& other) noexcept
{
	if (this != &other)
	{
		Reset();
		m_Device = std::exchange(other.m_Device, RawFd{});
		m_Timelines = std::move(other.m_Timelines);
		m_Scratch = std::exchange(other.m_Scratch, 0);
		other.m_Timelines.clear();
	}

	return *this;
}

void FenceExporter::Reset() noexcept
{
	if (!m_Device.IsValid())
	{
		return;
	}

	for (const Imported& timeline : m_Timelines)
	{
		::drmSyncobjDestroy(m_Device.Value, timeline.Handle);
	}

	m_Timelines.clear();

	if (m_Scratch != 0)
	{
		::drmSyncobjDestroy(m_Device.Value, std::exchange(m_Scratch, 0));
	}
}

Result<std::uint32_t> FenceExporter::HandleFor(RawFd timeline)
{
	for (const Imported& imported : m_Timelines)
	{
		if (imported.Descriptor == timeline.Value)
		{
			return imported.Handle;
		}
	}

	std::uint32_t handle = 0;

	if (::drmSyncobjFDToHandle(m_Device.Value, timeline.Value, &handle) != 0)
	{
		return Failure(errno, "importing the renderer's timeline onto the display device");
	}

	// Allocating, and deliberately outside the frame section: this runs the first time a given
	// timeline is seen, which is at the first frame after a target set is bound. Every frame after it
	// finds the handle already here.
	m_Timelines.push_back(Imported{ .Descriptor = timeline.Value, .Handle = handle });

	return handle;
}

Result<Fd> FenceExporter::Export(SyncPoint point)
{
	if (point.IsImmediate())
	{
		return Fd{};
	}

	if (!m_Device.IsValid())
	{
		return Failure(ENODEV, "exporting a fence with no device");
	}

	Result<std::uint32_t> source = HandleFor(point.Timeline);

	if (!source)
	{
		return std::unexpected{ source.error() };
	}

	if (m_Scratch == 0 && ::drmSyncobjCreate(m_Device.Value, 0, &m_Scratch) != 0)
	{
		return Failure(errno, "creating the binary syncobj a sync file is exported from");
	}

	// Point zero of a binary syncobj is the whole of it, which is what makes the transfer the
	// translation: a timeline value becomes the one fence the object holds.
	if (::drmSyncobjTransfer(m_Device.Value, m_Scratch, 0, *source, point.Value, 0) != 0)
	{
		return Failure(errno, "materialising a timeline point as a fence");
	}

	int descriptor = InvalidFd;

	if (::drmSyncobjExportSyncFile(m_Device.Value, m_Scratch, &descriptor) != 0)
	{
		return Failure(errno, "exporting a sync file from the display device");
	}

	return Fd{ descriptor };
}
} // namespace Drm
