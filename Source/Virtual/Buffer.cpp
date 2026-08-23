// The dma-buf sync ioctl is Linux's, which is the whole reason this file is not in the portable tier
// and the reason Seam/Buffer.h could go there without it.
#include "Virtual/Buffer.h"

#include <linux/dma-buf.h>
#include <sys/ioctl.h>

#include <cstdint>

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

DmabufRead::DmabufRead(const DmabufBuffer& buffer) noexcept
	: m_Descriptor{ buffer.IsMapped() ? buffer.Descriptor() : RawFd{} }, m_Bytes{ buffer.Pixels() }
{
	Sync(m_Descriptor, DMA_BUF_SYNC_START);
}

DmabufRead::~DmabufRead()
{
	Sync(m_Descriptor, DMA_BUF_SYNC_END);
}
