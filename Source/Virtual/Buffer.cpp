// munmap is POSIX and the dma-buf sync ioctl is Linux's, which is the whole reason this module is not
// in the portable tier. Requested in the translation unit for the reason Core/Fd.cpp requests
// _POSIX_C_SOURCE: what is wanted from the platform is named where it is wanted.
#define _POSIX_C_SOURCE 200809L

#include "Virtual/Buffer.h"

#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

void Mapping::Reset() noexcept
{
	if (m_Pixels != nullptr)
	{
		// Dropped for Core/Fd.cpp's reason, and the reasoning transfers exactly: the only way munmap
		// fails is a base or a length that never described a mapping, which is a defect here rather
		// than something the caller could act on, and the caller is usually a destructor that has no
		// way to report it.
		::munmap(m_Pixels, m_Length);
	}

	m_Pixels = nullptr;
	m_Length = 0;
}

namespace
{
// One direction, both phases. `DMA_BUF_SYNC_READ` is what a consumer inspecting a finished frame
// is doing; a writer would want `WRITE` and there is no writer here, since gyro fills these
// through a device and not through the mapping.
void Sync(RawFd descriptor, std::uint64_t phase) noexcept
{
	if (!descriptor.IsValid())
	{
		return;
	}

	dma_buf_sync sync{};
	sync.flags = phase | DMA_BUF_SYNC_READ;

	// Dropped, per the header. An exporter that refuses to start a sync has nothing the caller can
	// do about it, and the end phase runs in a destructor.
	::ioctl(descriptor.Value, DMA_BUF_IOCTL_SYNC, &sync);
}
} // namespace

DmabufBuffer::CpuRead::CpuRead(const DmabufBuffer& buffer) noexcept
	: m_Descriptor{ buffer.IsMapped() ? buffer.Descriptor() : RawFd{} }, m_Bytes{ buffer.Pixels() }
{
	Sync(m_Descriptor, DMA_BUF_SYNC_START);
}

DmabufBuffer::CpuRead::~CpuRead()
{
	Sync(m_Descriptor, DMA_BUF_SYNC_END);
}
