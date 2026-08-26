#include "Drm/Dumb.h"

#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <memory>
#include <utility>

#include "Seam/Pixel.h"

namespace
{
// The GEM object behind a descriptor, closed when the buffer that names it dies.
//
// **The handle is kept rather than released after the export, and the reason is the handle table
// rather than the object.** A dmabuf holds its own reference to the underlying object, so destroying
// the dumb handle immediately would not free the pages — but it would make the object unreachable
// through this file description, and Drm/Output.h re-imports the descriptor with
// `drmPrimeFDToHandle` to build a framebuffer out of it. Keeping the handle is the shape that does
// not depend on reasoning about two reference counts, and it costs one ioctl at teardown.
class DumbBacking final : public IDmabufBacking
{
public:
	DumbBacking(RawFd card, std::uint32_t handle, Fd descriptor) noexcept
		: m_Card{ card }, m_Handle{ handle }, m_Descriptor{ std::move(descriptor) }
	{}

	~DumbBacking() override
	{
		if (m_Handle != 0)
		{
			// Dropped rather than reported, for Core/Fd.cpp's reason: the only way this fails is a
			// handle that was never live, which is a defect here and not a condition a caller acts on.
			::drmModeDestroyDumbBuffer(m_Card.Value, m_Handle);
		}
	}

	// The descriptor the target's planes borrow. Held here so that the buffer's whole ownership is one
	// object, which is what Seam/Buffer.h's backing constructor requires.
	[[nodiscard]] RawFd Descriptor() const noexcept { return m_Descriptor.Borrow(); }

private:
	RawFd m_Card;
	std::uint32_t m_Handle = 0;
	Fd m_Descriptor;
};
} // namespace

namespace Drm
{
bool DumbAllocator::Supports(PixelFormat format) const noexcept
{
	return m_Card.IsValid() && format.Modifier == ModifierLinear && DecodableBytesPerPixel(format.Code) != 0;
}

Result<DmabufBuffer>
DumbAllocator::Allocate(PixelSize<DeviceSpace> size, std::uint32_t code, std::span<const std::uint64_t> modifiers)
{
	const PixelFormat format = FirstSupported(*this, code, modifiers);

	if (size.IsEmpty() || !size.IsValid() || !format.IsValid())
	{
		return Failure(EINVAL, "the dumb allocator produces linear buffers and nothing else");
	}

	// The kernel is told a bit depth and answers with a pitch, which is the whole of what a dumb
	// buffer negotiates. Asking for the stride back rather than computing one is not a formality:
	// a driver is free to align a scanout pitch to whatever its display engine fetches in, and a
	// buffer described at the width times the depth would be read one row short of the truth.
	drm_mode_create_dumb request{};
	request.width = static_cast<std::uint32_t>(size.Width);
	request.height = static_cast<std::uint32_t>(size.Height);
	request.bpp = DecodableBytesPerPixel(format.Code) * 8;

	if (::drmIoctl(m_Card.Value, DRM_IOCTL_MODE_CREATE_DUMB, &request) != 0)
	{
		return Failure(errno, "the display device would not allocate a dumb buffer");
	}

	int exported = InvalidFd;

	// `DRM_RDWR` because a consumer may map what comes back, and a descriptor exported read-only is
	// one a renderer's import can still take and a frame dump cannot read.
	if (::drmPrimeHandleToFD(m_Card.Value, request.handle, DRM_CLOEXEC | DRM_RDWR, &exported) != 0)
	{
		const int failure = errno;
		::drmModeDestroyDumbBuffer(m_Card.Value, request.handle);

		return Failure(failure, "the display device would not export a dumb buffer as a dmabuf");
	}

	auto backing = std::make_unique<DumbBacking>(m_Card, request.handle, Fd{ exported });

	DmabufImage image{};
	image.PlaneCount = 1;
	image.Planes[0] = DmabufPlane{ .Descriptor = backing->Descriptor(), .Offset = 0, .Stride = request.pitch };

	const RenderTarget described{ .Size = size, .Format = format, .Memory = image };

	++Allocations;

	return DmabufBuffer{ described, std::move(backing) };
}
} // namespace Drm
