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

Result<DmabufBuffer>
VulkanAllocator::Allocate(PixelSize<DeviceSpace> size, std::uint32_t code, std::span<const std::uint64_t> modifiers)
{
	// Straight through. Seam/Allocator.h has the argument: the caller says which pairs its consumer
	// can import, and the device is the end that knows which of them it draws into quickly.
	Result<ExportedImage> image = m_Device->Export(size, code, modifiers);

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
