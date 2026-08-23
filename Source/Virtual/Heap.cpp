#include "Virtual/Heap.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <utility>

#include "Core/Fd.h"
#include "Virtual/Udmabuf.h"

Result<DmabufBuffer> HeapAllocator::Allocate(PixelSize<DeviceSpace> size, PixelFormat format)
{
	if (Refuse != 0)
	{
		return Failure(Refuse, "scripted refusal");
	}

	if (size.IsEmpty() || !size.IsValid() || !Supports(format))
	{
		return Failure(EINVAL, "the heap allocator refuses this size or format");
	}

	const auto stride = static_cast<std::uint32_t>(size.Width) * BytesPerPixel(format.Code);
	const std::size_t bytes = UdmabufImageBytes(size, stride);

	// Sealed like the real one even though nothing here checks the seal, because the point of this
	// allocator is that the only difference from `UdmabufAllocator` is the ioctl it cannot make. A
	// stand-in that also skipped the sealing would let a bug in that half survive here.
	Fd memory{ ::memfd_create("gyro-heap", MFD_CLOEXEC | MFD_ALLOW_SEALING) };

	if (!memory.IsValid())
	{
		return Failure(errno, "creating a memfd for a heap image");
	}

	if (::ftruncate(memory.Get(), static_cast<off_t>(bytes)) != 0)
	{
		return Failure(errno, "sizing a heap image");
	}

	void* const pixels = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, memory.Get(), 0);

	if (pixels == MAP_FAILED)
	{
		return Failure(errno, "mapping a heap image");
	}

	++Allocations;

	return DmabufBuffer{ std::move(memory), size, format, stride, Mapping{ static_cast<std::byte*>(pixels), bytes } };
}

bool HeapAllocator::Supports(PixelFormat format) const noexcept
{
	return BytesPerPixel(format.Code) != 0 && (format.Modifier == ModifierLinear || format.Modifier == ModifierInvalid);
}
