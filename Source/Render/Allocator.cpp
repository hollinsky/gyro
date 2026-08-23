#include "Render/Allocator.h"

#include <memory>
#include <utility>

namespace
{
// The exported image, kept alive behind the description taken out of it.
//
// It holds nothing else and does nothing else. Seam/Buffer.h's backing is deliberately an empty
// interface — what has to survive is a whole object with a destructor of its own, and `Render` is
// the only module that can name this one.
class ExportedBacking final : public IDmabufBacking
{
public:
	explicit ExportedBacking(ExportedImage image) noexcept : m_Image{ std::move(image) } {}

private:
	ExportedImage m_Image;
};
} // namespace

Result<DmabufBuffer> VulkanAllocator::Allocate(PixelSize<DeviceSpace> size, PixelFormat format)
{
	// A list of one, because the ranking is the caller's. Seam/Allocator.h has the argument: the host
	// ordered the candidates and this device votes on them one at a time.
	const std::uint64_t modifier = format.Modifier;

	Result<ExportedImage> image = m_Device->Export(size, format.Code, { &modifier, 1 });

	if (!image)
	{
		return std::unexpected{ image.error() };
	}

	// Taken before the move, and it borrows the descriptor the backing is about to own — which is the
	// lifetime Seam/Buffer.h's second constructor exists to tie together.
	const RenderTarget described = image->Target();

	++Allocations;

	return DmabufBuffer{ described, std::make_unique<ExportedBacking>(std::move(*image)) };
}
