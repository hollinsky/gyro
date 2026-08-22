#pragma once

#include <string_view>

#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Seam/RenderTarget.h"
#include "Virtual/Buffer.h"

// Where a virtual output's images come from.
//
// **It is an interface with two implementations and it is deliberately not at the waist.**
// Docs/Structure.md's rule for `Seam` is *every interface with more than one implementation and the
// data crossing it*, and the second half is what excludes this: nothing outside this module ever
// names an allocator. `Frame` drives `IPresenter`, the presenter picks its own memory, and a
// dmabuf allocator appearing in `Seam` would widen the control waist for a type the portable tier
// must never construct. Same test that keeps `Frame`'s evaluator inside `Frame`.
//
// **The two providers differ in what they can produce, not in how they are called.** `udmabuf` turns
// a sealed `memfd` into a dmabuf with no device at all, which makes it linear-only — and linear is
// the only modifier lavapipe imports, so the pairing is exact rather than lucky (Docs/Decisions.md
// decision 40). GBM on a render node produces whatever the driver tiles, and is deferred with the
// dependency it costs; see decision 102.
//
// **Allocation is unbounded and allocating by contract**, which is not a hedge: it opens a
// descriptor, maps pages, and may talk to a driver. It runs where `IRenderer::BindTargets` runs — at
// a target invalidation, never inside the frame section — and the presenter that calls it is
// obliged to do so from the same place.
class IDmabufAllocator
{
public:
	IDmabufAllocator() = default;

	virtual ~IDmabufAllocator() = default;

	// Neither copied nor moved, for `IPresenter`'s reason one layer down: a provider holds a device
	// descriptor, and the presenter that uses one holds its address for as long as its targets live.
	IDmabufAllocator(const IDmabufAllocator&) = delete;
	IDmabufAllocator& operator=(const IDmabufAllocator&) = delete;
	IDmabufAllocator(IDmabufAllocator&&) = delete;
	IDmabufAllocator& operator=(IDmabufAllocator&&) = delete;

	// One image. `EINVAL` for a size or format this provider cannot express, `ENOMEM` where the
	// allocation failed, and whatever the device reported otherwise — the same vocabulary
	// `IRenderer::BindTargets` answers with, because a caller that cannot allocate a target set and a
	// renderer that cannot bind one are in the same position.
	[[nodiscard]] virtual Result<DmabufBuffer> Allocate(PixelSize<DeviceSpace> size, PixelFormat format) = 0;

	// Whether `Allocate` would accept this format, without allocating to find out. A presenter asks
	// before it tears down a working target set for a reconfiguration it cannot honour.
	[[nodiscard]] virtual bool Supports(PixelFormat format) const noexcept = 0;

	// For a log line naming which provider a target set came from. Two providers that both worked on
	// the developer's machine and one that does not exist in CI is the failure this makes legible.
	[[nodiscard]] virtual std::string_view Name() const noexcept = 0;
};
