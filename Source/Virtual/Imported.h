#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Seam/Allocator.h"
#include "Seam/Buffer.h"
#include "Seam/RenderTarget.h"

// Images gyro did not allocate: the ring a client registered its own output with.
//
// **Decision 200 is what this file is.** Every other provider behind `IDmabufAllocator` makes memory;
// this one hands over memory that already exists, because on a client-registered virtual output the
// party that knows what the pixels are *for* is the party that allocated them. An encoder imports
// into a VA-API or V4L2 device and what that device accepts is a fact about hardware gyro never
// opened, so gyro takes the descriptors and renders into them.
//
// **It is a provider rather than a second lifecycle on the presenter, and that is the whole design
// choice here.** Virtual/Output.h already builds a ring by asking an allocator once per target at a
// transition, drops the set on a reconfiguration, and reports why it is empty through `Status()`. An
// output whose images arrive from outside needs precisely those behaviours and no others — so the
// smaller change is to answer the question the presenter already asks, rather than to give it a
// second way to acquire a set that its reconfiguration path would then have to choose between.
//
// **What that borrows from `IDmabufAllocator` and what it stretches.** Seam/Allocator.h describes a
// provider as the device, one per machine, allocating unboundedly on demand. This is per output and
// finite: it hands out what it was given, in order, and then answers `ENOENT`. Every clause that
// matters still holds — a caller offers candidates and is told which it got, substitution within the
// offered set is legal and outside it is not, the call happens at a target invalidation and never
// inside the frame section — and the clause that does not is the one nothing depends on. Naming it
// here rather than in review is the point of the paragraph.
//
// **Running out is the renegotiation, and it is deliberate.** gyro cannot resize a ring it did not
// allocate, so a mode change on a registered output is not something the presenter can honour alone.
// Adopting a new configuration drops the old images and asks for new ones; this answers `ENOENT`; the
// output comes up with an empty target set and a status saying so, which is a state the frame loop
// already tolerates between `TargetsInvalidated` and `Reconfigured`. `Restock` is how the client's
// next ring arrives — the presenter holds this by address for life, so refilling has to reach the
// object rather than replace it.

// One plane of an image a client described, with the descriptor owned by the time it reaches here.
//
// A client sends a descriptor per plane over `SCM_RIGHTS`, so the fd gyro holds is its own copy and
// closing it is right. The planes of one image usually name the same allocation at different offsets,
// which is why they are separate descriptors here rather than one: a client is entitled to send four
// dups of the same file and a compositor is not entitled to assume it did.
struct ImportedPlane
{
	Fd Descriptor;
	std::uint32_t Offset = 0;
	std::uint32_t Stride = 0;
};

// Everything an imported image owns. The descriptions handed to the renderer borrow these, so they
// have to outlive every copy of a `RenderTarget` taken from the buffer, which is exactly what
// Seam/Buffer.h's backing is for.
class ImportedBacking final : public IDmabufBacking
{
public:
	explicit ImportedBacking(std::array<Fd, MaxImagePlanes> descriptors) noexcept
		: m_Descriptors{ std::move(descriptors) }
	{}

	[[nodiscard]] RawFd Borrow(std::size_t plane) const noexcept
	{
		return plane < m_Descriptors.size() ? m_Descriptors[plane].Borrow() : RawFd{};
	}

private:
	std::array<Fd, MaxImagePlanes> m_Descriptors{};
};

// Take ownership of what a client described and produce the buffer a target set is built from.
//
// `EINVAL` for a plane count outside `[1, MaxImagePlanes]`, a descriptor that names nothing, or a
// size or format that is not a description of an image — the checks a compositor owes a client that
// can say anything at all. There is no mapping: these pages belong to another process's allocator and
// nothing here may assume they are `mmap`-able, so a `Mapped` target face over an imported ring is a
// target set that fails to build rather than one that writes through a null pointer.
[[nodiscard]] Result<DmabufBuffer>
ImportBuffer(PixelSize<DeviceSpace> size, PixelFormat format, std::span<ImportedPlane> planes);

class ImportedTargets final : public IDmabufAllocator
{
public:
	ImportedTargets() = default;

	explicit ImportedTargets(std::vector<DmabufBuffer> ring) noexcept : m_Ring{ std::move(ring) } {}

	// Hand over the next image the client supplied.
	//
	// **The candidate list is checked rather than honoured, which is the one place this reads
	// backwards from a real provider.** A device is handed every modifier its caller can accept and
	// picks; there is nothing to pick here, so the question becomes whether what the client already
	// allocated is in the set the caller would have accepted. A mismatch is `EINVAL` and it means the
	// renderer and the client disagree about the image — which is a negotiation that went wrong at
	// registration, and it must not reach the point where a device imports the wrong layout.
	[[nodiscard]] Result<DmabufBuffer>
	Allocate(PixelSize<DeviceSpace> size, std::uint32_t code, std::span<const std::uint64_t> modifiers) override;

	[[nodiscard]] bool Supports(PixelFormat format) const noexcept override;

	[[nodiscard]] std::string_view Name() const noexcept override { return "imported"; }

	// The client's next ring, after a reconfiguration invalidated the last one. Replaces whatever is
	// left rather than appending: a half-consumed ring plus a fresh one is a target set assembled from
	// two mode sets, which is the failure this whole path exists to prevent.
	void Restock(std::vector<DmabufBuffer> ring) noexcept
	{
		m_Ring = std::move(ring);
		m_Next = 0;
	}

	// How many images are left to hand out. What a caller checks before it invalidates a working set
	// for a configuration this cannot furnish.
	[[nodiscard]] std::size_t Remaining() const noexcept { return m_Ring.size() - m_Next; }

private:
	[[nodiscard]] const DmabufBuffer* Next() const noexcept
	{
		return m_Next < m_Ring.size() ? &m_Ring[m_Next] : nullptr;
	}

	std::vector<DmabufBuffer> m_Ring{};
	std::size_t m_Next = 0;
};
