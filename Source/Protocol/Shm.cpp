#include "Protocol/Shm.h"

#include <fcntl.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstdint>
#include <utility>

// Sealing arrived after the rest of `fcntl.h` and a toolchain may still be describing an older
// kernel's header. The numbers are the kernel's ABI and cannot move.
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#endif

#ifndef F_GET_SEALS
#define F_GET_SEALS 1034
#endif

#ifndef F_SEAL_SHRINK
#define F_SEAL_SHRINK 0x0002
#endif

namespace
{
// Bytes one pixel takes in the two layouts gyro advertises. Both are a 32-bit little-endian word, and
// the only difference between them is whether the top byte means anything.
constexpr std::size_t BytesPerPixel = 4;

[[nodiscard]] bool IsAdvertised(Wayland::Server::WlShmFormat format) noexcept
{
	return format == Wayland::Server::WlShmFormat::Argb8888 || format == Wayland::Server::WlShmFormat::Xrgb8888;
}

// Make the file unable to shrink, whether or not the client thought to.
//
// A client library that created the pool as a `memfd` with `MFD_ALLOW_SEALING` has usually sealed it
// already, and the add is then a no-op that reports success. A descriptor that refuses both — a
// `shm_open` file, a regular file, a pipe somebody sent for the fun of it — is one gyro declines to
// map, because the compositor cannot survive the fault a truncation would raise on the frame after.
[[nodiscard]] bool SealAgainstShrinking(const Fd& fd) noexcept
{
	const int seals = ::fcntl(fd.Borrow().Value, F_GET_SEALS);

	if (seals >= 0 && (seals & F_SEAL_SHRINK) != 0)
	{
		return true;
	}

	return ::fcntl(fd.Borrow().Value, F_ADD_SEALS, F_SEAL_SHRINK) == 0;
}
} // namespace

std::shared_ptr<PoolMapping> PoolMapping::Map(Fd fd, std::size_t size, Refusal& refusal)
{
	refusal = Refusal::None;

	if (size == 0 || size > MaxPoolBytes)
	{
		refusal = Refusal::Size;

		return nullptr;
	}

	if (!SealAgainstShrinking(fd))
	{
		refusal = Refusal::Unsealable;

		return nullptr;
	}

	// Read-only, which is `MappedPixels` one layer down saying the same thing in the type system: these
	// are somebody else's pixels and nothing in gyro writes them.
	void* const mapped = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd.Borrow().Value, 0);

	if (mapped == MAP_FAILED)
	{
		refusal = Refusal::Mapping;

		return nullptr;
	}

	// Not `make_shared`: the constructor is private, and it is private because a mapping that was not
	// sealed first is the one thing this class exists to prevent.
	return std::shared_ptr<PoolMapping>{ new PoolMapping{
		std::move(fd), static_cast<const std::byte*>(mapped), size } };
}

PoolMapping::~PoolMapping()
{
	if (m_Pixels != nullptr)
	{
		// The cast away from const is the unmap wanting the address rather than the pixels; nothing here
		// writes through it.
		::munmap(const_cast<std::byte*>(m_Pixels), m_Size);
	}
}

bool PoolMapping::Grow(std::size_t size)
{
	if (size <= m_Size || size > MaxPoolBytes)
	{
		return false;
	}

	void* const moved = ::mremap(const_cast<std::byte*>(m_Pixels), m_Size, size, MREMAP_MAYMOVE);

	if (moved == MAP_FAILED)
	{
		return false;
	}

	m_Pixels = static_cast<const std::byte*>(moved);
	m_Size = size;

	return true;
}

std::span<const std::byte> ClientShmBuffer::Pixels() const noexcept
{
	if (m_Pool == nullptr)
	{
		return {};
	}

	const std::span<const std::byte> pool = m_Pool->Bytes();
	const auto needed = static_cast<std::size_t>(m_Stride) * static_cast<std::size_t>(m_Size.Height);

	if (m_Offset > pool.size() || needed > pool.size() - m_Offset)
	{
		return {};
	}

	return pool.subspan(m_Offset, needed);
}

Result<TextureId> ClientShmBuffer::Adopt(ITextures& textures)
{
	const std::span<const std::byte> pixels = Pixels();

	if (pixels.empty())
	{
		return Failure(EINVAL, "a wl_shm buffer whose pool no longer holds the rows it names");
	}

	// The two formats gyro advertises differ in one byte's meaning, which is exactly what
	// `TextureAlpha` carries — so an opaque window costs no pass over its own pixels to say so.
	return textures.Adopt(
		m_Size,
		m_Stride,
		pixels,
		m_Format == Wayland::Server::WlShmFormat::Argb8888 ? TextureAlpha::Premultiplied : TextureAlpha::None
	);
}

Wayland::Server::WlBufferHandler* ClientShmPool::OnCreateBuffer(
	std::int32_t offset,
	std::int32_t width,
	std::int32_t height,
	std::int32_t stride,
	Wayland::Server::WlShmFormat format
)
{
	// `wl_shm_pool` declares no error enumeration of its own, so the code is `wl_shm`'s — which is what
	// every client library reads it as, because the two interfaces share one error space.
	const auto refuse = [this](Wayland::Server::WlShmError code, const char* why) -> Wayland::Server::WlBufferHandler* {
		Object().PostError(static_cast<std::uint32_t>(code), why);

		// Not null: null is `no_memory` and ends the client with the wrong reason. The buffer is created
		// and immediately irrelevant, because the error above has already ended the connection.
		return new ClientShmBuffer{ m_Mapping, 0, PixelSize<BufferSpace>{}, 0, Wayland::Server::WlShmFormat::Argb8888 };
	};

	// A pool whose mapping was refused: the client has already been ended and this is whatever it had
	// queued behind the request that ended it.
	if (m_Mapping == nullptr)
	{
		return new ClientShmBuffer{ nullptr, 0, PixelSize<BufferSpace>{}, 0, Wayland::Server::WlShmFormat::Argb8888 };
	}

	if (!IsAdvertised(format))
	{
		return refuse(
			Wayland::Server::WlShmError::InvalidFormat,
			"wl_shm_pool.create_buffer with a format wl_shm never advertised"
		);
	}

	if (offset < 0 || width <= 0 || height <= 0 || stride <= 0)
	{
		return refuse(
			Wayland::Server::WlShmError::InvalidStride, "wl_shm_pool.create_buffer with a negative or empty extent"
		);
	}

	const auto rowBytes = static_cast<std::size_t>(width) * BytesPerPixel;
	const auto rows = static_cast<std::size_t>(height);
	const auto pitch = static_cast<std::size_t>(stride);

	if (pitch < rowBytes)
	{
		return refuse(
			Wayland::Server::WlShmError::InvalidStride,
			"wl_shm_pool.create_buffer with a stride narrower than its own rows"
		);
	}

	// Computed in `size_t` against the mapping's own length rather than in the wire's `int32_t`: every
	// one of these products overflows a 32-bit signed at extents a client is free to name, and an
	// overflow here is a buffer that passes its bounds check and reads off the end of the pool.
	const std::size_t span = pitch * rows;
	const auto start = static_cast<std::size_t>(offset);

	if (start > m_Mapping->Size() || span > m_Mapping->Size() - start)
	{
		return refuse(
			Wayland::Server::WlShmError::InvalidStride, "wl_shm_pool.create_buffer reaching past the end of its pool"
		);
	}

	return new ClientShmBuffer{
		m_Mapping, start, PixelSize<BufferSpace>{ width, height }, static_cast<std::uint32_t>(stride), format
	};
}

void ClientShmPool::OnResize(std::int32_t size)
{
	if (m_Mapping == nullptr)
	{
		return;
	}

	if (size <= 0 || !m_Mapping->Grow(static_cast<std::size_t>(size)))
	{
		// A pool that only grows is the protocol's own rule, and it is also the seal's: a client that
		// asks to shrink one is asking for the fault the seal exists to make impossible.
		Object().PostError(
			static_cast<std::uint32_t>(Wayland::Server::WlShmError::InvalidFd),
			"wl_shm_pool.resize to a size this pool cannot take"
		);
	}
}

void ClientShm::OnBound()
{
	Object().Format(Wayland::Server::WlShmFormat::Argb8888);
	Object().Format(Wayland::Server::WlShmFormat::Xrgb8888);
}

Wayland::Server::WlShmPoolHandler* ClientShm::OnCreatePool(Fd fd, std::int32_t size)
{
	PoolMapping::Refusal refusal = PoolMapping::Refusal::None;
	std::shared_ptr<PoolMapping> mapping;

	if (size > 0)
	{
		mapping = PoolMapping::Map(std::move(fd), static_cast<std::size_t>(size), refusal);
	}
	else
	{
		refusal = PoolMapping::Refusal::Size;
	}

	if (mapping == nullptr)
	{
		switch (refusal)
		{
			case PoolMapping::Refusal::Unsealable:
				Object().PostError(
					Wayland::Server::WlShmError::InvalidFd,
					"wl_shm.create_pool with a descriptor that cannot be sealed against shrinking"
				);

				break;

			case PoolMapping::Refusal::Size:
				Object().PostError(
					Wayland::Server::WlShmError::InvalidStride, "wl_shm.create_pool with a size gyro will not map"
				);

				break;

			case PoolMapping::Refusal::None:
			case PoolMapping::Refusal::Mapping:
				Object().PostError(
					Wayland::Server::WlShmError::InvalidFd, "wl_shm.create_pool with a descriptor that would not map"
				);

				break;
		}

		// The client is already ended; the pool exists so that libwayland has something behind the id it
		// is holding rather than a `no_memory` on top of the error just posted.
		return new ClientShmPool{ nullptr };
	}

	return new ClientShmPool{ std::move(mapping) };
}

Wayland::Server::WlShmHandler* ShmGlobal::OnBind(wl_client& client, std::uint32_t version)
{
	(void)client;
	(void)version;

	return new ClientShm{};
}
