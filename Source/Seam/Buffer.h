#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <utility>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Seam/RenderTarget.h"

// One allocated image, owned: the dmabuf, the CPU mapping of it where there is one, and whatever
// else has to stay alive for the descriptor to keep meaning something.
//
// **This is the thing Seam/RenderTarget.h declines to be.** A `RenderTarget` is a *description* — it
// is copied by value, read per frame, and its descriptor is borrowed, because the party that owns the
// memory is the presenter and the party reading the description is the renderer. Something has to be
// the owner on the presenter's side, and this is it: `Describe()` hands out the borrowed view the
// seam carries, and the buffer outlives every copy of it by construction, because the presenter holds
// the buffer and the seam's contract is that a target is valid until `TargetsInvalidated`.
//
// **It is at the waist because two modules now name one, which is decision 120.** It used to live in
// `Virtual`, on the correct grounds that nothing outside that module ever held an allocator's output.
// A nested output's targets are exported from the Vulkan device, so `Render` produces these and
// `Nested` consumes them, and neither may name the other. What came up with `IDmabufAllocator` is the
// owning half and nothing more: a descriptor, a mapping, and a description. Constructing one is still
// a platform module's business, because only a platform module implements the interface.
//
// **The mapping is optional and is not how the renderer reaches the pixels.** A Vulkan device imports
// the descriptor; nothing composites through this pointer. What it is for is the consumer — a test
// that inspects what was drawn, a writer that puts a frame on disk — and `udmabuf` gives it for free
// because the pages are an ordinary `memfd` underneath. An allocator that cannot map hands back a
// buffer with none, and the consumer that wanted one finds out at the seam rather than by reading a
// null pointer.
//
// **Reading the mapping wants the dma-buf sync ioctl, and that is Linux's**, so it is not here:
// Virtual/Buffer.h has `DmabufRead`, which is a platform module's file precisely because this one has
// to stay in the portable tier for `Nested` and `Render` to share it. See Docs/Decisions.md decisions
// 102 and 120.

// An `mmap`, owned. Small enough that a general-purpose mapping type would be more machinery than the
// callers want, and specific in the one way that matters: `munmap` needs the length back, so the
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

// Whatever else the memory behind a descriptor needs kept alive, named by nothing and destroyed with
// the buffer.
//
// **It exists because one of the two providers owns more than a descriptor.** `udmabuf` and the heap
// allocate a `memfd` and hand it over, so the descriptor really is the whole of the ownership. The
// Vulkan export is a `VkImage` and a `VkDeviceMemory` with a descriptor taken out of it, and closing
// the descriptor alone leaks the allocation on the device — which on a nested session is every target
// of every window, reallocated on every resize.
//
// An empty interface rather than a deleter pair, because what is being kept alive is a whole object
// with a destructor of its own and the module that made it is the only one that can name the type.
// One virtual destructor call per target at teardown, and nothing per frame.
class IDmabufBacking
{
public:
	IDmabufBacking() = default;

	virtual ~IDmabufBacking() = default;

	IDmabufBacking(const IDmabufBacking&) = delete;
	IDmabufBacking& operator=(const IDmabufBacking&) = delete;
	IDmabufBacking(IDmabufBacking&&) = delete;
	IDmabufBacking& operator=(IDmabufBacking&&) = delete;
};

// A dmabuf and everything needed to describe it at the seam.
//
// **The description is stored rather than recomputed, and that is what lets a tiled allocation
// through.** It used to be a size, a format and one stride, with `Describe()` assembling a
// single-plane image out of them — which is every image `udmabuf` can produce and not every image a
// driver can. A compressed modifier lays its metadata out as further planes at further offsets into
// the same allocation, so a buffer that could only say *one stride, offset zero* would either refuse
// those modifiers or describe them wrongly. The device says what it laid out; this carries the
// answer.
class DmabufBuffer
{
public:
	DmabufBuffer() = default;

	// The single-plane form, for a provider whose allocation is a descriptor and nothing else.
	DmabufBuffer(
		Fd descriptor,
		PixelSize<DeviceSpace> size,
		PixelFormat format,
		std::uint32_t stride,
		Mapping mapping
	) noexcept
		: m_Descriptor{ std::move(descriptor) }, m_Mapping{ std::move(mapping) }
	{
		DmabufImage image{};
		image.PlaneCount = 1;
		image.Planes[0] = DmabufPlane{ .Descriptor = m_Descriptor.Borrow(), .Offset = 0, .Stride = stride };

		m_Target = RenderTarget{ .Size = size, .Format = format, .Memory = image };
	}

	// The form a device fills in: it laid the image out, so it says what the layout is, and it hands
	// over the object that has to outlive the descriptions taken from it.
	//
	// The planes in `described` borrow a descriptor the backing owns, which is why the backing is not
	// optional here — a description whose planes name nothing that is kept alive is the defect this
	// constructor exists to make unspellable.
	DmabufBuffer(RenderTarget described, std::unique_ptr<IDmabufBacking> backing) noexcept
		: m_Backing{ std::move(backing) }, m_Target{ described }
	{}

	DmabufBuffer(const DmabufBuffer&) = delete;
	DmabufBuffer& operator=(const DmabufBuffer&) = delete;

	// **Written out rather than defaulted, and the description is the reason.** Every other member
	// clears itself when it is moved from, because every other member owns something. A `RenderTarget`
	// is trivially copyable, so a defaulted move leaves the source *still describing* a descriptor it
	// has just handed away — and `IsValid()` reads the description, so the husk would answer yes and a
	// target set built from it would hand a renderer a plane pointing at a number the kernel has since
	// given to something else. Cleared here, and asserted in Buffer.Test.cpp.
	DmabufBuffer(DmabufBuffer&& other) noexcept
		: m_Descriptor{ std::move(other.m_Descriptor) }, m_Mapping{ std::move(other.m_Mapping) },
		  m_Backing{ std::move(other.m_Backing) }, m_Target{ std::exchange(other.m_Target, RenderTarget{}) }
	{}

	DmabufBuffer& operator=(DmabufBuffer&& other) noexcept
	{
		if (this != &other)
		{
			m_Descriptor = std::move(other.m_Descriptor);
			m_Mapping = std::move(other.m_Mapping);
			m_Backing = std::move(other.m_Backing);
			m_Target = std::exchange(other.m_Target, RenderTarget{});
		}

		return *this;
	}

	[[nodiscard]] bool IsValid() const noexcept { return m_Target.IsValid(); }

	[[nodiscard]] PixelSize<DeviceSpace> Size() const noexcept { return m_Target.Size; }

	[[nodiscard]] PixelFormat Format() const noexcept { return m_Target.Format; }

	// The first plane's, which is the pixels' — the only stride a single-plane consumer means, and the
	// one `wl_shm`-shaped arithmetic elsewhere in the tree is written against. A consumer that cares
	// about the rest reads `Describe()`.
	[[nodiscard]] std::uint32_t Stride() const noexcept { return Plane(0).Stride; }

	// The first plane's descriptor. Every plane of one of these names the same allocation, because a
	// modifier's extra planes are offsets into it rather than separate buffers.
	[[nodiscard]] RawFd Descriptor() const noexcept { return Plane(0).Descriptor; }

	[[nodiscard]] bool IsMapped() const noexcept { return m_Mapping.IsValid(); }

	// The seam's view of this buffer. Borrowed throughout, which is what Seam/RenderTarget.h's
	// `DmabufPlane` documents and what makes a target description trivially copyable.
	//
	// **It always says dmabuf, and a udmabuf image is honestly both — which is the one thing standing
	// between the heap provider and the CPU renderer.** Seam/RenderTarget.h's memory is a
	// discriminated variant precisely so that a renderer branches rather than casts, and it is
	// explicit that `Blit` handed a dmabuf is a composition-root miswiring: the blitter wants a
	// `MappedImage` and a mapped image *has* one, sitting right there in `m_Mapping`. So the
	// description is not wrong, it is one of two true answers, and this is the one a device can use.
	// Which face an output shows is Virtual/Output.h's `TargetFace`, decided by the root that knows
	// which renderer it built.
	[[nodiscard]] RenderTarget Describe() const noexcept { return m_Target; }

	// The other true answer: this image as memory a CPU writes.
	//
	// A buffer with no mapping describes nothing here — an allocator that cannot map is a real case,
	// and a `MappedImage` with a null pointer is a target a blitter would happily write through.
	[[nodiscard]] RenderTarget DescribeMapped() const noexcept
	{
		const std::span<std::byte> pixels = m_Mapping.Bytes();

		if (pixels.empty())
		{
			return RenderTarget{};
		}

		return RenderTarget{ .Size = m_Target.Size,
			                 .Format = m_Target.Format,
			                 .Memory = MappedImage{ .Pixels = pixels.data(),
			                                        .Stride = Plane(0).Stride,
			                                        .Reserved = 0,
			                                        .Length = pixels.size() } };
	}

	// The mapping, mutable, for whoever filled the buffer without a device. Empty where the allocator
	// gave none.
	[[nodiscard]] std::span<std::byte> Pixels() const noexcept { return m_Mapping.Bytes(); }

private:
	[[nodiscard]] DmabufPlane Plane(std::uint32_t index) const noexcept
	{
		const DmabufImage* const image = m_Target.AsDmabuf();

		return image != nullptr && index < image->PlaneCount ? image->Planes[index] : DmabufPlane{};
	}

	// Declared before the description so that both are destroyed after it, which costs nothing —
	// `RenderTarget` is trivially destructible — and states the direction the borrowing runs in.
	Fd m_Descriptor;
	Mapping m_Mapping;
	std::unique_ptr<IDmabufBacking> m_Backing;

	RenderTarget m_Target{};
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
