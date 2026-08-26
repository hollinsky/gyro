#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Seam/Allocator.h"
#include "Seam/Buffer.h"
#include "Seam/RenderTarget.h"

// The display device asked to allocate what it is going to scan out.
//
// **It is the last rung of decision 151's chain, and it exists because a renderer that cannot export
// is a real device rather than a hypothetical one.** lavapipe imports dmabufs and refuses to produce
// one — `vkGetPhysicalDeviceImageFormatProperties2` reports `IMPORTABLE` without `EXPORTABLE` for
// every format and tiling it has — so on a machine whose GPU driver did not come up, the party that
// draws the frame cannot allocate the frame. The card can: `DRM_IOCTL_MODE_CREATE_DUMB` is the
// oldest allocation in the graphics stack, it is on every KMS driver by definition, and what comes
// out of it is a GEM object the display engine was already going to accept.
//
// **Linear and nothing else, which is not a limitation here but the pairing.** A dumb buffer has no
// tiling to state — the ioctl takes a width, a height and a bit depth, and the kernel returns a
// pitch — so this offers `DRM_FORMAT_MOD_LINEAR` alone. That is exactly the one modifier a software
// renderer imports, so the rung that is available when the export rung is not is also the rung that
// produces what the importer accepts. The pairing is why this is the floor rather than a fallback
// with a caveat.
//
// **It is deliberately last, and the cost of being wrong about that is measured.** A dumb buffer on
// a working GPU is a linear target on a part that renders two and a half times faster into a tiled
// one — decision 138's 7.4ms against 2.8ms — which the frame clock absorbs by dropping a tier rather
// than by reporting anything. So this rung is reached only when every better one has declined, and
// `Name()` is what puts the choice in the log. Docs/Open.md carries what that costs a machine whose
// GPU renders but will not export.
//
// **Two references and no ownership.** The card's descriptor belongs to Drm/Device.h and outlives
// every target allocated through it, which is the composition root's sequencing — outputs are
// destroyed before the device. A buffer this hands out owns its own GEM handle and closes it when
// the buffer dies, which is what keeps a target set released at a mode change from leaving objects
// on the card.
namespace Drm
{
class DumbAllocator final : public IDmabufAllocator
{
public:
	// Borrowed, per Core/Fd.h. The card is the composition root's and is closed with the device.
	explicit DumbAllocator(RawFd card) noexcept : m_Card{ card } {}

	[[nodiscard]] Result<DmabufBuffer>
	Allocate(PixelSize<DeviceSpace> size, std::uint32_t code, std::span<const std::uint64_t> modifiers) override;

	// Linear, at a depth this can express. The format's own bit depth is what `CREATE_DUMB` takes, so
	// a fourcc whose pixels are not a whole number of bytes is one this cannot ask for — which is
	// every format gyro composites into, and a refusal for the rest rather than a guess.
	[[nodiscard]] bool Supports(PixelFormat format) const noexcept override;

	[[nodiscard]] std::string_view Name() const noexcept override { return "kms dumb buffer"; }

	// How many images this has handed out, matching the counter on the other providers and there for
	// the same reason: a test that wants to say a reconfiguration really did build a new set.
	std::uint64_t Allocations = 0;

private:
	RawFd m_Card;
};
} // namespace Drm
