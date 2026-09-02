#define _POSIX_C_SOURCE 200809L

#include "Drm/Fence.h"

#include <drm/drm.h>
#include <sys/ioctl.h>
#include <xf86drm.h>

#include <cerrno>
#include <cstddef>
#include <utility>

namespace Drm
{
FenceExporter::FenceExporter(FenceExporter&& other) noexcept
	: m_Device{ std::exchange(other.m_Device, RawFd{}) }, m_Timelines{ other.m_Timelines },
	  m_Held{ std::exchange(other.m_Held, 0) }, m_Next{ std::exchange(other.m_Next, 0) },
	  m_Scratch{ std::exchange(other.m_Scratch, 0) }
{
}

FenceExporter& FenceExporter::operator=(FenceExporter&& other) noexcept
{
	if (this != &other)
	{
		Reset();
		m_Device = std::exchange(other.m_Device, RawFd{});
		m_Timelines = other.m_Timelines;
		m_Held = std::exchange(other.m_Held, 0);
		m_Next = std::exchange(other.m_Next, 0);
		m_Scratch = std::exchange(other.m_Scratch, 0);
	}

	return *this;
}

void FenceExporter::Reset() noexcept
{
	if (!m_Device.IsValid())
	{
		return;
	}

	for (std::size_t index = 0; index < m_Held; ++index)
	{
		::drmSyncobjDestroy(m_Device.Value, m_Timelines[index].Handle);
	}

	m_Held = 0;
	m_Next = 0;

	if (m_Scratch != 0)
	{
		::drmSyncobjDestroy(m_Device.Value, std::exchange(m_Scratch, 0));
	}
}

Result<std::uint32_t> FenceExporter::HandleFor(RawFd timeline)
{
	for (std::size_t index = 0; index < m_Held; ++index)
	{
		if (m_Timelines[index].Descriptor == timeline.Value)
		{
			return m_Timelines[index].Handle;
		}
	}

	std::uint32_t handle = 0;

	if (::drmSyncobjFDToHandle(m_Device.Value, timeline.Value, &handle) != 0)
	{
		return Failure(errno, "importing the renderer's timeline onto the display device");
	}

	// Allocation-free, because this runs inside the frame section: the first frame after a target set
	// is bound is a frame like any other and the debug allocator does not grant it an exception. Past
	// the bound the oldest entry goes, and its handle with it — the descriptor it named belongs to a
	// device that has been replaced, so nothing on the machine can still ask for it.
	if (m_Held == m_Timelines.size())
	{
		::drmSyncobjDestroy(m_Device.Value, m_Timelines[m_Next].Handle);
	}
	else
	{
		++m_Held;
	}

	m_Timelines[m_Next] = Imported{ .Descriptor = timeline.Value, .Handle = handle };
	m_Next = (m_Next + 1) % m_Timelines.size();

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
