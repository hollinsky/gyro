#include "Drm/Scanout.h"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cerrno>
#include <format>
#include <span>
#include <utility>

namespace Drm
{

DrmScanout::~DrmScanout()
{
	// Every panel this card drives is gone by the time the device is — the composition root destroys
	// outputs before the device — so there is nothing left reading and the wait is not a wait.
	for (std::uint32_t index = 0; index < m_Count; ++index)
	{
		Destroy(m_Images[index].Framebuffer, { m_Images[index].Handles.data(), m_Images[index].PlaneCount });
	}

	for (std::uint32_t index = 0; index < m_DoomedCount; ++index)
	{
		Destroy(m_Doomed[index].Framebuffer, { m_Doomed[index].Handles.data(), m_Doomed[index].PlaneCount });
	}

	m_Count = 0;
	m_DoomedCount = 0;
}

Result<std::uint32_t> DrmScanout::Acquire(RawFd descriptor) noexcept
{
	std::uint32_t handle = 0;

	if (::drmPrimeFDToHandle(m_Device.Borrow().Value, descriptor.Value, &handle) != 0)
	{
		return Failure(errno, "importing a client buffer onto the display device");
	}

	for (std::uint32_t index = 0; index < m_NamedCount; ++index)
	{
		if (m_Named[index].Handle == handle)
		{
			++m_Named[index].Count;

			return handle;
		}
	}

	if (m_NamedCount == m_Named.size())
	{
		// Unreachable while the image table bounds the plane count, and answered rather than asserted
		// because the alternative is a handle nothing will ever close.
		::drmCloseBufferHandle(m_Device.Borrow().Value, handle);

		return Failure(ENOMEM, "the display device's handle table is full");
	}

	m_Named[m_NamedCount] = Named{ .Handle = handle, .Count = 1 };
	++m_NamedCount;

	return handle;
}

void DrmScanout::Release(std::uint32_t handle) noexcept
{
	for (std::uint32_t index = 0; index < m_NamedCount; ++index)
	{
		if (m_Named[index].Handle != handle)
		{
			continue;
		}

		if (--m_Named[index].Count != 0)
		{
			return;
		}

		::drmCloseBufferHandle(m_Device.Borrow().Value, handle);
		m_Named[index] = m_Named[m_NamedCount - 1];
		--m_NamedCount;

		return;
	}
}

void DrmScanout::Destroy(std::uint32_t framebuffer, std::span<const std::uint32_t> handles) noexcept
{
	if (framebuffer != 0)
	{
		::drmModeRmFB(m_Device.Borrow().Value, framebuffer);
	}

	for (const std::uint32_t handle : handles)
	{
		Release(handle);
	}
}

Result<void> DrmScanout::Adopt(TextureId id, const TextureSource& source)
{
	Sweep();

	if (id.IsNull() || !source.IsValid())
	{
		return Failure(EINVAL, "a scanout import needs a live id and a described image");
	}

	// A `wl_shm` buffer's pixels were copied into gyro's own memory at commit and there is no
	// descriptor underneath. Refused by name rather than turned into an allocation this layer invents.
	const DmabufImage* const image = source.AsDmabuf();

	if (image == nullptr)
	{
		return Failure(EINVAL, "a mapped buffer has nothing a display engine can scan out");
	}

	// Retiring entries count against the table, which is what makes the push in `Forget` below total:
	// every live image has somewhere to go when it is given up, however long a panel holds it.
	if (m_Count + m_DoomedCount >= m_Images.size())
	{
		return Failure(ENOMEM, "the display device holds as many scanout framebuffers as it can");
	}

	// Re-adopting a live id replaces what it names, which carries the same two halves `Forget` does:
	// the caller waited for the watermark, and the framebuffer it replaces retires the way a forgotten
	// one does.
	Forget(id);

	Image built{ .Id = id, .Framebuffer = 0, .Handles = {}, .PlaneCount = 0, .Format = source.Format };

	std::array<std::uint32_t, MaxImagePlanes> strides{};
	std::array<std::uint32_t, MaxImagePlanes> offsets{};
	std::array<std::uint64_t, MaxImagePlanes> layouts{};

	for (std::uint32_t plane = 0; plane < image->PlaneCount; ++plane)
	{
		Result<std::uint32_t> handle = Acquire(image->Planes[plane].Descriptor);

		if (!handle)
		{
			Destroy(0, { built.Handles.data(), built.PlaneCount });

			return std::unexpected{ handle.error() };
		}

		built.Handles[plane] = *handle;
		++built.PlaneCount;

		strides[plane] = image->Planes[plane].Stride;
		offsets[plane] = image->Planes[plane].Offset;
		layouts[plane] = source.Format.Modifier;
	}

	const int added = ::drmModeAddFB2WithModifiers(
		m_Device.Borrow().Value,
		static_cast<std::uint32_t>(source.Size.Width),
		static_cast<std::uint32_t>(source.Size.Height),
		source.Format.Code,
		built.Handles.data(),
		strides.data(),
		offsets.data(),
		layouts.data(),
		&built.Framebuffer,
		source.Format.Modifier != ModifierInvalid ? DRM_MODE_FB_MODIFIERS : 0U
	);

	if (added != 0)
	{
		const int reason = errno;

		Destroy(0, { built.Handles.data(), built.PlaneCount });

		// The ordinary refusal rather than a fault: a buffer laid out for sampling need not be one any
		// plane can read, and what it costs is a surface that is never promoted.
		return Failure(reason, "a display framebuffer for", Subject::Of("{}", source.Format));
	}

	m_Images[m_Count] = built;
	++m_Count;

	return {};
}

void DrmScanout::Forget(TextureId id) noexcept
{
	for (std::uint32_t index = 0; index < m_Count; ++index)
	{
		if (m_Images[index].Id != id)
		{
			continue;
		}

		// The panel may still be reading it, and removing it there would disable the plane rather than
		// free memory — see Seam/Scanout.h. So it goes on the list and `Sweep` decides.
		// There is always room: `Adopt` counts the retiring against the table for exactly this.
		m_Doomed[m_DoomedCount] = Doomed{ .Framebuffer = m_Images[index].Framebuffer,
			                              .Handles = m_Images[index].Handles,
			                              .PlaneCount = m_Images[index].PlaneCount };
		++m_DoomedCount;

		m_Images[index] = m_Images[m_Count - 1];
		--m_Count;

		Sweep();

		return;
	}
}

bool DrmScanout::StillReading(std::uint32_t framebuffer) const noexcept
{
	return std::ranges::any_of(std::span{ m_Holds.data(), m_HoldCount }, [framebuffer](const IScanoutHold* hold) {
		return hold->Holds(framebuffer);
	});
}

void DrmScanout::Sweep() noexcept
{
	std::uint32_t index = 0;

	while (index < m_DoomedCount)
	{
		if (StillReading(m_Doomed[index].Framebuffer))
		{
			++index;

			continue;
		}

		Destroy(m_Doomed[index].Framebuffer, { m_Doomed[index].Handles.data(), m_Doomed[index].PlaneCount });
		m_Doomed[index] = m_Doomed[m_DoomedCount - 1];
		--m_DoomedCount;
	}
}

void DrmScanout::Attach(IScanoutHold& hold) noexcept
{
	if (m_HoldCount == m_Holds.size() || std::ranges::find(std::span{ m_Holds.data(), m_HoldCount }, &hold) !=
	                                         std::span{ m_Holds.data(), m_HoldCount }.end())
	{
		return;
	}

	m_Holds[m_HoldCount] = &hold;
	++m_HoldCount;
}

void DrmScanout::Detach(IScanoutHold& hold) noexcept
{
	for (std::uint32_t index = 0; index < m_HoldCount; ++index)
	{
		if (m_Holds[index] != &hold)
		{
			continue;
		}

		m_Holds[index] = m_Holds[m_HoldCount - 1];
		--m_HoldCount;

		break;
	}

	// An output being torn down is exactly the moment it stops answering, and everything it was the
	// last reader of is free now.
	Sweep();
}

std::uint32_t DrmScanout::Find(TextureId id) const noexcept
{
	for (std::uint32_t index = 0; index < m_Count; ++index)
	{
		if (m_Images[index].Id == id)
		{
			return m_Images[index].Framebuffer;
		}
	}

	return 0;
}

PixelFormat DrmScanout::Layout(TextureId id) const noexcept
{
	for (std::uint32_t index = 0; index < m_Count; ++index)
	{
		if (m_Images[index].Id == id)
		{
			return m_Images[index].Format;
		}
	}

	return {};
}

} // namespace Drm
