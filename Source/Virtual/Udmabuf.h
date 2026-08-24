#pragma once

#include <cstddef>
#include <string_view>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Seam/Allocator.h"
#include "Seam/RenderTarget.h"
#include "Virtual/Buffer.h"

// Real dmabufs on a machine with no GPU.
//
// `memfd_create`, `ftruncate`, `F_SEAL_SHRINK`, `UDMABUF_CREATE`. The result is a genuine dmabuf: a
// Vulkan device imports it through `VK_EXT_external_memory_dma_buf` like any other, and lavapipe —
// which accepts `DRM_FORMAT_MOD_LINEAR` and nothing else — accepts exactly what this can produce. So
// the whole render path runs where there is no display, no seat, and no GPU driver, which is what
// Docs/Decisions.md decision 102 is for.
//
// **What the kernel actually requires, confirmed by running it rather than read off a header.** The
// memfd must be sealed against shrinking, or `UDMABUF_CREATE` refuses it — the seal is what makes
// the pages safe to pin. The size must be a whole number of pages, and an unaligned one is `EINVAL`
// rather than a rounding. The memfd may be closed as soon as the dmabuf exists, because the driver
// holds the pages, so a target costs one descriptor and not two. And the dmabuf is directly
// `mmap`-able, coherent with the memfd's own mapping, which is why a consumer inspecting a composited
// frame needs no readback and no staging buffer.
//
// **The awkward half is permission rather than capability**, and it points the opposite way from
// where a reader would look for it. `/dev/udmabuf` is `0600 root:kvm`; a workstation reaches it only
// because logind puts a `uaccess` ACL on it for the seat-local user, so an SSH session with no seat
// or a CI container has the kernel support and not the access. That is why opening the device is a
// separate, fallible step whose `EACCES` a caller can report as *skipped* rather than *failed*, and
// why Docs/Open.md carries the question of what CI should do about it.

// The device, opened. Separate from the allocator so that the allocator's constructor cannot fail —
// which is what lets it be a member rather than something behind a `Result` and a `unique_ptr`, and
// what gives the one interesting error a place to be reported with its errno intact.
[[nodiscard]] Result<Fd> OpenUdmabufDevice();

// Whether this machine can produce a dmabuf this way at all, for a test or a startup probe that wants
// to say *skipped, no udmabuf here* rather than fail.
//
// **It allocates one page and throws it away, and the weaker forms are all wrong.** A `stat` reports
// the node's existence and says nothing about the `uaccess` ACL. Opening the node reports the ACL and
// says nothing about whether a driver is behind it — a stale device node in a container opens
// perfectly and then answers `ENOTTY` to the only ioctl that matters. The one honest answer to "can
// I" is having done it, which is what this does.
[[nodiscard]] bool IsUdmabufAvailable() noexcept;

// The device opened and probed in one step: an allocator that is known to work, or the error that
// stopped it. This is what a caller that wants to *use* udmabuf should ask for, since it collapses
// the two failures — no access, and no driver — into the one answer either forces.
//
// It is a `Result<Fd>` rather than a `Result<UdmabufAllocator>` because an allocator is neither
// copyable nor movable, being an `IDmabufAllocator`; the caller constructs one from this in place.
[[nodiscard]] Result<Fd> OpenAndProbeUdmabuf();

class UdmabufAllocator final : public IDmabufAllocator
{
public:
	// Takes the device descriptor from `OpenUdmabufDevice`. Ownership, because the allocator outlives
	// every buffer it made and the device is what makes another one.
	explicit UdmabufAllocator(Fd device) noexcept : m_Device{ std::move(device) } {}

	[[nodiscard]] Result<DmabufBuffer>
	Allocate(PixelSize<DeviceSpace> size, std::uint32_t code, std::span<const std::uint64_t> modifiers) override;

	// Linear or unspecified, and nothing else. A tiled modifier is not something this can produce and
	// saying so here is what keeps a presenter from tearing down a working target set to find out.
	//
	// `ModifierInvalid` is accepted and produces a linear buffer described as linear, because
	// Seam/RenderTarget.h is explicit that invalid means *unknown* rather than *none* — a caller that
	// did not state one gets the truth back in the target description rather than its own vagueness.
	[[nodiscard]] bool Supports(PixelFormat format) const noexcept override;

	[[nodiscard]] std::string_view Name() const noexcept override { return "udmabuf"; }

	// Whether the device is open. A default-constructed `Fd` reaches here only from a caller that
	// ignored `OpenUdmabufDevice`'s failure, and `Allocate` answers `ENODEV` rather than handing an
	// invalid descriptor to an ioctl.
	[[nodiscard]] bool IsValid() const noexcept { return m_Device.IsValid(); }

private:
	Fd m_Device;
};

// The bytes one image needs, rounded up to whole pages because `UDMABUF_CREATE` refuses anything
// else. Exposed because it is what a test asserts against and what a capacity estimate multiplies:
// a target set is this times the ring depth, and on a virtual output that is the whole of what the
// backend costs in memory.
[[nodiscard]] std::size_t UdmabufImageBytes(PixelSize<DeviceSpace> size, std::uint32_t stride) noexcept;

// Bytes per pixel for the formats this provider will allocate. Zero for anything else, which is what
// `Supports` reads and what makes an unknown format a refusal rather than a stride of nought.
[[nodiscard]] std::uint32_t BytesPerPixel(std::uint32_t code) noexcept;
