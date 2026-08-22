// memfd_create and the seals are glibc's under _GNU_SOURCE rather than POSIX's, which is a step past
// what Core/Fd.cpp and Core/Clock.cpp need and is stated here for the same reason: what is wanted
// from the platform is named where it is wanted. This module is not in the portable tier and
// CMake/CheckPortability.cmake is what holds that distinction rather than this line.
//
// Guarded because g++ defines it for C++ already and redefining it is a warning this build treats as
// an error. Asking anyway is what keeps the requirement stated rather than inherited from a compiler
// that happens to be generous.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "Virtual/Udmabuf.h"

#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>

namespace
{
// What the kernel rounds to. Asked rather than assumed to be 4096, because a page is 64 KiB on
// some configurations and a hard-coded constant would produce an `EINVAL` nobody could read.
[[nodiscard]] std::size_t PageSize() noexcept
{
	const long size = ::sysconf(_SC_PAGESIZE);

	return size > 0 ? static_cast<std::size_t>(size) : 4096;
}

// The memfd, sized and sealed. Sealing against shrink is what `UDMABUF_CREATE` requires: the
// driver pins these pages, and a file that could be truncated underneath them is the
// truncate-and-fault hazard Seam/RenderTarget.h refuses `wl_shm` over, one layer down.
[[nodiscard]] Result<Fd> SealedMemfd(std::size_t bytes)
{
	Fd memfd{ ::memfd_create("gyro-virtual-target", MFD_ALLOW_SEALING | MFD_CLOEXEC) };

	if (!memfd.IsValid())
	{
		return FailFromErrno("creating a memfd for a virtual output target");
	}

	if (::ftruncate(memfd.Get(), static_cast<off_t>(bytes)) != 0)
	{
		return FailFromErrno("sizing a virtual output target");
	}

	if (::fcntl(memfd.Get(), F_ADD_SEALS, F_SEAL_SHRINK) != 0)
	{
		return FailFromErrno("sealing a virtual output target against shrinking");
	}

	return memfd;
}

// The whole allocation, over a borrowed device, so that `OpenAndProbeUdmabuf` can run it without
// owning an allocator and `UdmabufAllocator::Allocate` can be the format check plus this.
[[nodiscard]] Result<DmabufBuffer> AllocateFrom(RawFd device, PixelSize<DeviceSpace> size, PixelFormat format)
{
	if (size.IsEmpty())
	{
		return Failure(EINVAL, "allocating a virtual output target with no extent");
	}

	const std::uint32_t stride = BytesPerPixel(format.Code) * static_cast<std::uint32_t>(size.Width);

	if (stride == 0)
	{
		return Failure(EINVAL, "allocating a virtual output target in a format udmabuf cannot produce");
	}

	const std::size_t bytes = UdmabufImageBytes(size, stride);

	Result<Fd> memfd = SealedMemfd(bytes);

	if (!memfd)
	{
		return std::unexpected{ memfd.error() };
	}

	udmabuf_create create{};
	create.memfd = static_cast<std::uint32_t>(memfd->Get());
	create.flags = UDMABUF_FLAGS_CLOEXEC;
	create.offset = 0;
	create.size = bytes;

	Fd dmabuf{ ::ioctl(device.Value, UDMABUF_CREATE, &create) };

	if (!dmabuf.IsValid())
	{
		// ENOTTY here is a device node with no udmabuf driver behind it, which a container can
		// have and a workstation cannot. It is why `OpenAndProbeUdmabuf` exists.
		return FailFromErrno("creating a dmabuf from a sealed memfd");
	}

	// The memfd goes here, before the mapping, and that ordering is the interesting part rather
	// than tidiness: the driver holds the pages from `UDMABUF_CREATE` onward, so the file is no
	// longer what keeps them alive, and a target that held both would cost two descriptors per
	// image for nothing. Mapping the dmabuf rather than the memfd is what makes that true — the
	// two mappings are coherent, and only one of them survives its file being closed.
	memfd->Reset();

	void* pixels = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf.Get(), 0);

	// A dmabuf that will not map is still a usable target: a Vulkan device imports the descriptor
	// and never touches this pointer. What is lost is the consumer's ability to look, which
	// `DmabufBuffer::IsMapped` reports and which is not this layer's to call fatal.
	Mapping mapping = pixels == MAP_FAILED ? Mapping{} : Mapping{ static_cast<std::byte*>(pixels), bytes };

	// Described as linear whatever was asked for: a caller that passed `ModifierInvalid` said it
	// did not know, and the target description says what the buffer is.
	const PixelFormat described{ .Code = format.Code, .Reserved = 0, .Modifier = ModifierLinear };

	return DmabufBuffer{ std::move(dmabuf), size, described, stride, std::move(mapping) };
}
} // namespace

Result<Fd> OpenUdmabufDevice()
{
	Fd device{ ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC) };

	if (!device.IsValid())
	{
		// EACCES here is the ordinary answer on a machine with no seat rather than a misconfiguration,
		// and the context says which node so that the reader knows it is not a DRM device.
		return FailFromErrno("opening /dev/udmabuf");
	}

	return device;
}

Result<Fd> OpenAndProbeUdmabuf()
{
	Result<Fd> device = OpenUdmabufDevice();

	if (!device)
	{
		return device;
	}

	// One page, allocated and dropped. Cheap enough to cost nothing at startup and real enough to be
	// the whole operation: the ioctl is what distinguishes a node with a driver behind it from one
	// without, and opening tells you neither.
	const Result<DmabufBuffer> probe = AllocateFrom(
		device->Borrow(), PixelSize<DeviceSpace>{ 1, 1 }, PixelFormat{ FormatXrgb8888, 0, ModifierLinear }
	);

	if (!probe)
	{
		return std::unexpected{ probe.error() };
	}

	return device;
}

bool IsUdmabufAvailable() noexcept
{
	return OpenAndProbeUdmabuf().has_value();
}

std::uint32_t BytesPerPixel(std::uint32_t code) noexcept
{
	switch (code)
	{
		case FormatXrgb8888:
		case FormatArgb8888:
		case FormatXrgb2101010:
		case FormatArgb2101010:
			return 4;
		default:
			return 0;
	}
}

std::size_t UdmabufImageBytes(PixelSize<DeviceSpace> size, std::uint32_t stride) noexcept
{
	if (size.IsEmpty() || stride == 0)
	{
		return 0;
	}

	const std::size_t page = PageSize();
	const std::size_t bytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(size.Height);

	return ((bytes + page - 1) / page) * page;
}

bool UdmabufAllocator::Supports(PixelFormat format) const noexcept
{
	if (BytesPerPixel(format.Code) == 0)
	{
		return false;
	}

	return format.Modifier == ModifierLinear || format.Modifier == ModifierInvalid;
}

Result<DmabufBuffer> UdmabufAllocator::Allocate(PixelSize<DeviceSpace> size, PixelFormat format)
{
	if (!m_Device.IsValid())
	{
		return Failure(ENODEV, "allocating from a udmabuf allocator with no device");
	}

	if (!Supports(format))
	{
		return Failure(EINVAL, "allocating a virtual output target in a format udmabuf cannot produce");
	}

	return AllocateFrom(m_Device.Borrow(), size, format);
}
