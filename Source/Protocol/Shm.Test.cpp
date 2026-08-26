#include "Protocol/Shm.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "Scene/Textures.h"
#include "Testing/Test.h"

// The pool, the seal, and the arithmetic that keeps a client inside it.
//
// **A pool object here has no `wl_resource` behind it**, per Surface.Test.cpp's reason: `PostError` on
// an absent resource does nothing, so a refusal is asserted by what did *not* come back rather than by
// the error the client received. The wire half is Integration/ProtocolRoundTrip.Test.cpp, where the
// error code is what a real client reads.
//
// The bounds cases are the ones worth having. Every product in `create_buffer` overflows a 32-bit
// signed at extents a client is free to name, and an overflow that wrapped would produce a buffer
// which passes its own bounds check and then reads off the end of somebody else's mapping.

namespace
{
// A pool the way a client library makes one: a `memfd` that can be sealed. Deliberately *not* sealed
// here, so that the sealing gyro does is the thing under test.
[[nodiscard]] Fd MakeUnsealedPool(std::size_t size)
{
	const int descriptor = ::memfd_create("gyro-shm-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);

	if (descriptor < 0)
	{
		return {};
	}

	Fd fd{ descriptor };

	if (::ftruncate(descriptor, static_cast<off_t>(size)) != 0)
	{
		return {};
	}

	return fd;
}

// Write a recognisable byte into every pixel of a row, through a writable view of the same file.
void Fill(const Fd& fd, std::size_t size, std::byte value)
{
	void* const writable = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd.Borrow().Value, 0);

	GYRO_REQUIRE(writable != MAP_FAILED);

	std::memset(writable, static_cast<int>(value), size);

	::munmap(writable, size);
}

// The texture space as a fake that remembers what it was handed. It mints real ids, because the
// surface path retires the one it replaces and an id that was never valid would make that untestable.
class RecordingTextures final : public ITextures
{
public:
	using ITextures::Adopt;

	[[nodiscard]] Result<TextureId> Adopt(
		PixelSize<BufferSpace> size,
		std::uint32_t stride,
		std::span<const std::byte> pixels,
		TextureAlpha alpha
	) override
	{
		Size = size;
		Stride = stride;
		Alpha = alpha;
		Pixels.assign(pixels.begin(), pixels.end());

		return TextureId{ ++m_Next, 1 };
	}

	void Retire(TextureId) noexcept override { ++Retired; }

	PixelSize<BufferSpace> Size{};
	std::uint32_t Stride = 0;
	TextureAlpha Alpha = TextureAlpha::Premultiplied;
	std::vector<std::byte> Pixels;
	std::uint32_t Retired = 0;

private:
	std::uint32_t m_Next = 0;
};

// `create_buffer` on a resourceless pool, with the handler owned by the caller.
[[nodiscard]] std::unique_ptr<ClientShmBuffer>
Cut(ClientShmPool& pool,
    std::int32_t offset,
    std::int32_t width,
    std::int32_t height,
    std::int32_t stride,
    Wayland::Server::WlShmFormat format = Wayland::Server::WlShmFormat::Argb8888)
{
	Wayland::Server::WlBufferHandler* const handler = pool.OnCreateBuffer(offset, width, height, stride, format);

	// Always a buffer, never null: null is `no_memory` on the wire and would end the client with the
	// wrong reason. A refused one comes back with no extent.
	return std::unique_ptr<ClientShmBuffer>{ static_cast<ClientShmBuffer*>(handler) };
}
} // namespace

GYRO_TEST(Shm, ADescriptorThatCannotBeSealedIsRefused)
{
	int ends[2] = { -1, -1 };
	GYRO_REQUIRE(::pipe(ends) == 0);

	const Fd reading{ ends[0] };
	const Fd writing{ ends[1] };

	PoolMapping::Refusal refusal = PoolMapping::Refusal::None;

	// A pipe is the blunt case; the one that matters in the field is a `shm_open` file, which maps
	// perfectly well and can be truncated out from under the mapping afterwards. gyro cannot survive
	// the fault that would raise, so it declines the descriptor instead of catching the signal.
	GYRO_CHECK(PoolMapping::Map(Fd{ ::dup(ends[0]) }, 4096, refusal) == nullptr);
	GYRO_CHECK(refusal == PoolMapping::Refusal::Unsealable);
}

GYRO_TEST(Shm, APoolTheClientLeftUnsealedIsSealedBeforeItIsMapped)
{
	constexpr std::size_t Size = 4096;

	Fd fd = MakeUnsealedPool(Size);
	GYRO_REQUIRE(fd.IsValid());

	const int raw = fd.Borrow().Value;

	PoolMapping::Refusal refusal = PoolMapping::Refusal::None;
	const std::shared_ptr<PoolMapping> mapping = PoolMapping::Map(std::move(fd), Size, refusal);

	GYRO_REQUIRE(mapping != nullptr);
	GYRO_CHECK(refusal == PoolMapping::Refusal::None);

	// The seal is what makes every read below safe for the rest of the pool's life, so it is asserted
	// against the kernel rather than inferred from the map having succeeded.
	const int seals = ::fcntl(raw, F_GET_SEALS);
	GYRO_REQUIRE(seals >= 0);
	GYRO_CHECK((seals & F_SEAL_SHRINK) != 0);

	GYRO_CHECK(::ftruncate(raw, 128) != 0);
}

GYRO_TEST(Shm, ABufferNamesTheRowsItsOffsetAndStrideDescribe)
{
	constexpr std::size_t Size = 64 * 1024;

	Fd fd = MakeUnsealedPool(Size);
	GYRO_REQUIRE(fd.IsValid());

	Fill(fd, Size, std::byte{ 0x5A });

	PoolMapping::Refusal refusal = PoolMapping::Refusal::None;
	ClientShmPool pool{ PoolMapping::Map(std::move(fd), Size, refusal) };

	const std::unique_ptr<ClientShmBuffer> buffer = Cut(pool, 256, 16, 8, 64);
	GYRO_REQUIRE(buffer->Extent() == (PixelSize<BufferSpace>{ 16, 8 }));

	// Rows times stride, starting at the offset — not the whole pool, and not the whole rows of a
	// buffer that only owns part of one.
	GYRO_CHECK_EQ(buffer->Pixels().size(), std::size_t{ 64 * 8 });
	GYRO_CHECK(buffer->Pixels().front() == std::byte{ 0x5A });
}

GYRO_TEST(Shm, ABufferReachingPastItsPoolIsRefused)
{
	constexpr std::size_t Size = 4096;

	Fd fd = MakeUnsealedPool(Size);
	GYRO_REQUIRE(fd.IsValid());

	PoolMapping::Refusal refusal = PoolMapping::Refusal::None;
	ClientShmPool pool{ PoolMapping::Map(std::move(fd), Size, refusal) };

	// Exactly the pool is allowed; one row more is not.
	GYRO_CHECK(Cut(pool, 0, 16, 64, 64)->Extent() == (PixelSize<BufferSpace>{ 16, 64 }));
	GYRO_CHECK(Cut(pool, 0, 16, 65, 64)->Extent().IsEmpty());
	GYRO_CHECK(Cut(pool, 64, 16, 64, 64)->Extent().IsEmpty());
	GYRO_CHECK(Cut(pool, -1, 16, 64, 64)->Extent().IsEmpty());
}

GYRO_TEST(Shm, AStrideNarrowerThanItsOwnRowsIsRefused)
{
	constexpr std::size_t Size = 4096;

	Fd fd = MakeUnsealedPool(Size);
	GYRO_REQUIRE(fd.IsValid());

	PoolMapping::Refusal refusal = PoolMapping::Refusal::None;
	ClientShmPool pool{ PoolMapping::Map(std::move(fd), Size, refusal) };

	// Sixteen pixels is sixty-four bytes in both formats gyro advertises, so a stride of sixty is a
	// buffer whose second row starts inside its first.
	GYRO_CHECK(Cut(pool, 0, 16, 4, 60)->Extent().IsEmpty());
	GYRO_CHECK(Cut(pool, 0, 16, 4, 64)->Extent() == (PixelSize<BufferSpace>{ 16, 4 }));
}

GYRO_TEST(Shm, AnExtentThatWouldOverflowTheWireIsRefused)
{
	constexpr std::size_t Size = 4096;

	Fd fd = MakeUnsealedPool(Size);
	GYRO_REQUIRE(fd.IsValid());

	PoolMapping::Refusal refusal = PoolMapping::Refusal::None;
	ClientShmPool pool{ PoolMapping::Map(std::move(fd), Size, refusal) };

	// `stride * height` is a hair over 2^62 here. Computed in the wire's own `int32_t` it wraps to
	// something small, and a buffer that passed its bounds check would read off the end of the
	// mapping — so the products are done in `size_t` against the pool's real length.
	GYRO_CHECK(Cut(pool, 0, 1, 0x4000'0000, 0x4000'0000)->Extent().IsEmpty());
	GYRO_CHECK(Cut(pool, 0x7FFF'FFFF, 1, 1, 4)->Extent().IsEmpty());
}

GYRO_TEST(Shm, AFormatWlShmNeverAdvertisedIsRefused)
{
	constexpr std::size_t Size = 4096;

	Fd fd = MakeUnsealedPool(Size);
	GYRO_REQUIRE(fd.IsValid());

	PoolMapping::Refusal refusal = PoolMapping::Refusal::None;
	ClientShmPool pool{ PoolMapping::Map(std::move(fd), Size, refusal) };

	GYRO_CHECK(Cut(pool, 0, 4, 4, 16, Wayland::Server::WlShmFormat::Rgb565)->Extent().IsEmpty());
	GYRO_CHECK(
		Cut(pool, 0, 4, 4, 16, Wayland::Server::WlShmFormat::Xrgb8888)->Extent() == (PixelSize<BufferSpace>{ 4, 4 })
	);
}

GYRO_TEST(Shm, APoolGrowsAndABufferCutBeforeTheGrowthStillReads)
{
	constexpr std::size_t Size = 4096;

	Fd fd = MakeUnsealedPool(Size * 4);
	GYRO_REQUIRE(fd.IsValid());

	Fill(fd, Size * 4, std::byte{ 0x11 });

	PoolMapping::Refusal refusal = PoolMapping::Refusal::None;
	const std::shared_ptr<PoolMapping> mapping = PoolMapping::Map(std::move(fd), Size, refusal);
	GYRO_REQUIRE(mapping != nullptr);

	ClientShmPool pool{ mapping };

	const std::unique_ptr<ClientShmBuffer> buffer = Cut(pool, 0, 16, 4, 64);
	GYRO_REQUIRE(!buffer->Extent().IsEmpty());

	// A resize remaps and the address may move. The buffer holds the mapping rather than a pointer
	// into it, which is what keeps a window that was drawn before a toolkit grew its pool from
	// reading a page that has been unmapped.
	GYRO_REQUIRE(mapping->Grow(Size * 2));
	GYRO_CHECK_EQ(mapping->Size(), Size * 2);
	GYRO_CHECK_EQ(buffer->Pixels().size(), std::size_t{ 64 * 4 });
	GYRO_CHECK(buffer->Pixels().front() == std::byte{ 0x11 });

	// Only upwards, which is the protocol's rule and the seal's.
	GYRO_CHECK(!mapping->Grow(Size));
}

GYRO_TEST(Shm, AnOpaqueBufferSaysSoRatherThanCostingAPassOverItsPixels)
{
	constexpr std::size_t Size = 4096;

	Fd fd = MakeUnsealedPool(Size);
	GYRO_REQUIRE(fd.IsValid());

	Fill(fd, Size, std::byte{ 0x7F });

	PoolMapping::Refusal refusal = PoolMapping::Refusal::None;
	ClientShmPool pool{ PoolMapping::Map(std::move(fd), Size, refusal) };

	RecordingTextures textures;

	const std::unique_ptr<ClientShmBuffer> opaque = Cut(pool, 0, 4, 4, 16, Wayland::Server::WlShmFormat::Xrgb8888);
	GYRO_REQUIRE(opaque->Adopt(textures).has_value());

	// The top byte of an `xrgb8888` pixel is whatever the toolkit left there. Carrying that as a word
	// rather than fixing it up is the difference between a window you can see the desktop through and
	// a second pass over every pixel of every frame.
	GYRO_CHECK(textures.Alpha == TextureAlpha::None);
	GYRO_CHECK_EQ(textures.Stride, std::uint32_t{ 16 });
	GYRO_CHECK(textures.Size == (PixelSize<BufferSpace>{ 4, 4 }));
	GYRO_CHECK_EQ(textures.Pixels.size(), std::size_t{ 64 });

	const std::unique_ptr<ClientShmBuffer> translucent = Cut(pool, 0, 4, 4, 16, Wayland::Server::WlShmFormat::Argb8888);
	GYRO_REQUIRE(translucent->Adopt(textures).has_value());

	GYRO_CHECK(textures.Alpha == TextureAlpha::Premultiplied);
}
