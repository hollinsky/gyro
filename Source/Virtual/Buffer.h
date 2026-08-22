#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <utility>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Seam/RenderTarget.h"

// One allocated image, owned: the dmabuf, and the CPU mapping of it where there is one.
//
// **This is the thing Seam/RenderTarget.h declines to be.** A `RenderTarget` is a *description* — it
// is copied by value, read per frame, and its descriptor is borrowed, because the party that owns the
// memory is the presenter and the party reading the description is the renderer. Something has to be
// the owner on the presenter's side, and this is it: `Describe()` produces the borrowed view the seam
// carries, and the buffer outlives every copy of it by construction, because the presenter holds the
// buffer and the seam's contract is that a target is valid until `TargetsInvalidated`.
//
// **The mapping is optional and is not how the renderer reaches the pixels.** A Vulkan device imports
// the descriptor; nothing composites through this pointer. What it is for is the consumer — a test
// that inspects what was drawn, a writer that puts a frame on disk — and `udmabuf` gives it for free
// because the pages are an ordinary `memfd` underneath. An allocator that cannot map hands back a
// buffer with none, and the consumer that wanted one finds out at the seam rather than by reading a
// null pointer.
//
// See Docs/Decisions.md decision 102 for why the buffers are gyro's to allocate at all.

// An `mmap`, owned. Small enough that a general-purpose mapping type would be more machinery than the
// two callers want, and specific in the one way that matters: `munmap` needs the length back, so the
// pointer and the length are one value or the unmapping is a caller's obligation to remember.
class Mapping
{
public:
	Mapping() = default;

	// Takes ownership. Explicit for `Fd`'s reason: constructing one is a claim, and an implicit
	// conversion would let a borrowed address become an owner in an argument list.
	explicit Mapping(std::byte* pixels, std::size_t length) noexcept : m_Pixels{ pixels }, m_Length{ length } {}

	~Mapping() { Reset(); }

	Mapping(const Mapping&) = delete;
	Mapping& operator=(const Mapping&) = delete;

	Mapping(Mapping&& other) noexcept
		: m_Pixels{ std::exchange(other.m_Pixels, nullptr) }, m_Length{ std::exchange(other.m_Length, 0) }
	{}

	Mapping& operator=(Mapping&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			m_Pixels = std::exchange(other.m_Pixels, nullptr);
			m_Length = std::exchange(other.m_Length, 0);
		}

		return *this;
	}

	[[nodiscard]] bool IsValid() const noexcept { return m_Pixels != nullptr; }

	[[nodiscard]] std::span<std::byte> Bytes() const noexcept { return { m_Pixels, m_Length }; }

	// Unmaps what is held. Defined in Buffer.cpp, and a failing `munmap` is dropped for the reason
	// Core/Fd.cpp drops a failing `close`: the only way it fails is a length that never described a
	// mapping, which is a bug here rather than a condition the caller can act on.
	void Reset() noexcept;

private:
	std::byte* m_Pixels = nullptr;
	std::size_t m_Length = 0;
};

// A dmabuf and everything needed to describe it at the seam.
//
// One plane, and that is a real limit rather than a placeholder. Every format gyro renders a
// composite into is single-plane; `NV12` is in Seam/RenderTarget.h because a *client* may commit one
// and an encoder may want one, and neither of those is an output's own target. `MaxImagePlanes` stays
// four at the seam because that is what `AddFB2` takes.
class DmabufBuffer
{
public:
	DmabufBuffer() = default;

	DmabufBuffer(
		Fd descriptor,
		PixelSize<DeviceSpace> size,
		PixelFormat format,
		std::uint32_t stride,
		Mapping mapping
	) noexcept
		: m_Descriptor{ std::move(descriptor) }, m_Mapping{ std::move(mapping) }, m_Size{ size }, m_Format{ format },
		  m_Stride{ stride }
	{}

	DmabufBuffer(const DmabufBuffer&) = delete;
	DmabufBuffer& operator=(const DmabufBuffer&) = delete;

	DmabufBuffer(DmabufBuffer&&) noexcept = default;
	DmabufBuffer& operator=(DmabufBuffer&&) noexcept = default;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return m_Descriptor.IsValid() && !m_Size.IsEmpty() && m_Format.IsValid() && m_Stride > 0;
	}

	[[nodiscard]] PixelSize<DeviceSpace> Size() const noexcept { return m_Size; }

	[[nodiscard]] PixelFormat Format() const noexcept { return m_Format; }

	[[nodiscard]] std::uint32_t Stride() const noexcept { return m_Stride; }

	[[nodiscard]] RawFd Descriptor() const noexcept { return m_Descriptor.Borrow(); }

	[[nodiscard]] bool IsMapped() const noexcept { return m_Mapping.IsValid(); }

	// The seam's view of this buffer. Borrowed throughout, which is what Seam/RenderTarget.h's
	// `DmabufPlane` documents and what makes a target description trivially copyable.
	[[nodiscard]] RenderTarget Describe() const noexcept
	{
		DmabufImage image{};
		image.PlaneCount = 1;
		image.Planes[0] = DmabufPlane{ .Descriptor = m_Descriptor.Borrow(), .Offset = 0, .Stride = m_Stride };

		return RenderTarget{ .Size = m_Size, .Format = m_Format, .Memory = image };
	}

	// The mapping, mutable, for whoever filled the buffer without a device. Empty where the allocator
	// gave none.
	[[nodiscard]] std::span<std::byte> Pixels() const noexcept { return m_Mapping.Bytes(); }

	// Bracket a CPU read of the mapping, which `DMA_BUF_IOCTL_SYNC` is what the kernel asks for even
	// where the exporter's implementation of it does nothing.
	//
	// **It is an object rather than two calls because the end is the half that gets forgotten**, and a
	// missed `SYNC_END` on an exporter that does have cache maintenance is a stale read on one machine
	// and a correct one everywhere it was tested. Failures are dropped, per `Mapping::Reset` above:
	// there is no recovery from a sync that will not start, and the destructor could not report one
	// anyway. An unmapped buffer makes this a no-op rather than an error, so a consumer that wants to
	// look asks for the bytes and gets none.
	class CpuRead
	{
	public:
		explicit CpuRead(const DmabufBuffer& buffer) noexcept;

		~CpuRead();

		CpuRead(const CpuRead&) = delete;
		CpuRead& operator=(const CpuRead&) = delete;
		CpuRead(CpuRead&&) = delete;
		CpuRead& operator=(CpuRead&&) = delete;

		[[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return m_Bytes; }

	private:
		RawFd m_Descriptor;
		std::span<const std::byte> m_Bytes;
	};

private:
	Fd m_Descriptor;
	Mapping m_Mapping;
	PixelSize<DeviceSpace> m_Size{};
	PixelFormat m_Format{};
	std::uint32_t m_Stride = 0;
};

// Prints as device(1920x1080) XR24 mod 0x0 fd 7 stride 7680 mapped, which is the line a target-set
// dump wants: the seam's own formatter says everything but who owns it and whether it can be read.
template<>
struct std::formatter<DmabufBuffer>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const DmabufBuffer& buffer, Context& context) const
	{
		return std::format_to(
			context.out(),
			"{} {} {} stride {} {}",
			buffer.Size(),
			buffer.Format(),
			buffer.Descriptor(),
			buffer.Stride(),
			buffer.IsMapped() ? "mapped" : "unmapped"
		);
	}
};

// The contract everything downstream assumes. An owner that copied would close a descriptor twice and
// unmap a mapping twice, and the second of each lands on whatever took the number or the address.
static_assert(!std::is_copy_constructible_v<Mapping> && !std::is_copy_constructible_v<DmabufBuffer>);
static_assert(std::is_nothrow_move_constructible_v<DmabufBuffer> && std::is_nothrow_move_assignable_v<DmabufBuffer>);
static_assert(std::formattable<DmabufBuffer, char>);

// A default-constructed buffer describes nothing, which is what keeps a failed allocation out of a
// target set — asserted in Buffer.Test.cpp rather than here, because a buffer owns a mapping and a
// mapping owns an `munmap`, so neither is constructible in a constant expression. That is the cost of
// owning, and it is the reason the *description* is the trivially copyable half.
