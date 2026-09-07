#include "Virtual/Imported.h"

#include <algorithm>
#include <cerrno>
#include <memory>
#include <utility>

Result<DmabufBuffer> ImportBuffer(PixelSize<DeviceSpace> size, PixelFormat format, std::span<ImportedPlane> planes)
{
	if (planes.empty() || planes.size() > MaxImagePlanes)
	{
		return Failure(EINVAL, "an imported image has between one and MaxImagePlanes planes");
	}

	if (size.IsEmpty() || !size.IsValid() || !format.IsValid())
	{
		return Failure(EINVAL, "an imported image needs a size and a format");
	}

	std::array<Fd, MaxImagePlanes> descriptors{};
	DmabufImage image{};
	image.PlaneCount = static_cast<std::uint32_t>(planes.size());

	for (std::size_t index = 0; index < planes.size(); ++index)
	{
		if (!planes[index].Descriptor.IsValid() || planes[index].Stride == 0)
		{
			return Failure(EINVAL, "an imported plane names no descriptor or no stride");
		}

		descriptors[index] = std::move(planes[index].Descriptor);
	}

	auto backing = std::make_unique<ImportedBacking>(std::move(descriptors));

	// Borrowed off the backing rather than off the descriptors above, because the backing is what
	// outlives the description — Seam/Buffer.h's whole reason for existing, and the ordering that
	// makes a plane pointing at a closed descriptor unspellable here.
	for (std::size_t index = 0; index < planes.size(); ++index)
	{
		image.Planes[index] = DmabufPlane{ .Descriptor = backing->Borrow(index),
			                               .Offset = planes[index].Offset,
			                               .Stride = planes[index].Stride };
	}

	const RenderTarget described{ .Size = size, .Format = format, .Memory = image };

	if (!described.IsValid())
	{
		return Failure(EINVAL, "an imported image does not describe a target");
	}

	return DmabufBuffer{ described, std::move(backing) };
}

Result<DmabufBuffer>
ImportedTargets::Allocate(PixelSize<DeviceSpace> size, std::uint32_t code, std::span<const std::uint64_t> modifiers)
{
	const DmabufBuffer* const next = Next();

	if (next == nullptr)
	{
		// Not a defect on its own: it is what a reconfiguration looks like from here, and the caller
		// turns it into an empty target set with a status rather than into a failure to come up.
		return Failure(ENOENT, "the client's ring has no more images");
	}

	if (next->Size() != size || next->Format().Code != code)
	{
		return Failure(EINVAL, "the client's image is not the size or format this output renders");
	}

	if (std::ranges::find(modifiers, next->Format().Modifier) == modifiers.end())
	{
		return Failure(EINVAL, "the client's image has a modifier this output cannot render into");
	}

	return std::move(m_Ring[m_Next++]);
}

bool ImportedTargets::Supports(PixelFormat format) const noexcept
{
	const DmabufBuffer* const next = Next();

	return next != nullptr && next->Format() == format;
}
