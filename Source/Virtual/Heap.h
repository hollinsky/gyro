#pragma once

#include <cstdint>
#include <string_view>

#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Seam/Allocator.h"
#include "Seam/RenderTarget.h"
#include "Virtual/Buffer.h"

// Images with no kernel behind them, so that everything above the allocator can be tested anywhere.
//
// **It is `Headless`'s argument applied one layer down.** Docs/Structure.md calls that module *the
// instrument the schedulability sweep runs against, so it must work on a machine with no GPU*; this
// is the same claim about `/dev/udmabuf`, which decision 102 records is `0600 root:kvm` and reachable
// on a workstation only through logind's `uaccess` ACL. The ring, the retirement rule, the
// reconfiguration ordering, the consumer and the frame loop above them are none of them about where
// the memory came from — and pinning their tests to a device a container does not have would make
// them untestable in exactly the environment that entry says will not have it.
//
// **It is a `memfd` and a mapping, which is `UdmabufAllocator` with the one ioctl removed.** That is
// not an economy: it means the descriptor and the mapping describe the same pages, so a consumer
// reading through the mapping sees what a writer put there through the descriptor, and the only
// property this loses against the real provider is the one that needs the driver — being importable
// as a dmabuf. A Vulkan device handed one of these refuses it, correctly, which is why the renderer's
// own tests use the real allocator and gate on it.
//
// **What it must not become is the thing CI runs instead of the real provider.** Docs/Open.md carries
// *how `Virtual`'s tests are gated, and where the dmabuf comes from in CI* precisely because a green
// run that skipped every real test is the rot decision 36's build checks exist to prevent. This
// allocator widens what can be tested everywhere; it does not answer that question, and a suite that
// used it in place of `Udmabuf` would be answering it wrongly.
class HeapAllocator final : public IDmabufAllocator
{
public:
	[[nodiscard]] Result<DmabufBuffer>
	Allocate(PixelSize<DeviceSpace> size, std::uint32_t code, std::span<const std::uint64_t> modifiers) override;

	// The same set `UdmabufAllocator` accepts, reached through the same `BytesPerPixel` — so that the
	// instrument and the real provider cannot disagree about what a stride is, which is the way a
	// stand-in usually goes wrong.
	[[nodiscard]] bool Supports(PixelFormat format) const noexcept override;

	[[nodiscard]] std::string_view Name() const noexcept override { return "heap"; }

	// Fail every allocation with this errno; zero allocates. Sticky rather than one-shot, because a
	// target set is several allocations and a provider that failed the first and served the rest is
	// not a failure mode any real one has. A target set that cannot be built is a state the presenter
	// and the frame loop both have to handle, and scripting it is cheaper than arranging for the
	// kernel to be out of memory.
	int Refuse = 0;

	// How many images this has handed out, for a test that wants to say a reconfiguration really did
	// build a new set rather than reusing the old one.
	std::uint64_t Allocations = 0;
};
