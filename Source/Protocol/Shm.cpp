#include "Protocol/Shm.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <optional>
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
// already, and the add is then a no-op that reports success. A descriptor that refuses both — GTK's
// own pool, a `shm_open` file, a file on disk — is one gyro will not map, and is read rather than
// refused.
[[nodiscard]] bool SealAgainstShrinking(const Fd& fd) noexcept
{
	const int seals = ::fcntl(fd.Borrow().Value, F_GET_SEALS);

	if (seals >= 0 && (seals & F_SEAL_SHRINK) != 0)
	{
		return true;
	}

	return ::fcntl(fd.Borrow().Value, F_ADD_SEALS, F_SEAL_SHRINK) == 0;
}

// How many bytes the file behind `fd` actually holds, or nothing where it is not a file pixels can be
// read out of — a pipe, a socket, a directory. Both a `memfd` and a `shm_open` file are ordinary
// files, so this refuses only what could never have worked.
[[nodiscard]] std::optional<std::size_t> FileBytes(const Fd& fd) noexcept
{
	struct ::stat status = {};

	if (::fstat(fd.Borrow().Value, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0)
	{
		return std::nullopt;
	}

	return static_cast<std::size_t>(status.st_size);
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

	const std::optional<std::size_t> held = FileBytes(fd);

	if (!held.has_value())
	{
		refusal = Refusal::Unreadable;

		return nullptr;
	}

	// The client named more of the file than there is. Refused whichever path this pool was going to
	// take, because the mapped one would fault on the first frame and the read one would come up short
	// on every one of them — and a client is better told now than told nothing and shown nothing.
	if (*held < size)
	{
		refusal = Refusal::Short;

		return nullptr;
	}

	// Everything below the seal is the *unmapped* pool: `pread` on the descriptor, where a truncation
	// is a short read rather than a fault.
	if (!SealAgainstShrinking(fd))
	{
		return std::shared_ptr<PoolMapping>{ new PoolMapping{ std::move(fd), nullptr, size } };
	}

	// Read-only, which is `MappedPixels` one layer down saying the same thing in the type system: these
	// are somebody else's pixels and nothing in gyro writes them.
	void* const mapped = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd.Borrow().Value, 0);

	if (mapped == MAP_FAILED)
	{
		refusal = Refusal::Mapping;

		return nullptr;
	}

	// Not `make_shared`: the constructor is private, and it is private because a mapping made without
	// the two checks above is the one thing this class exists to prevent.
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

	// **The seal does not cover this and that was the hole.** `F_SEAL_SHRINK` promises the file will
	// not get shorter; it promises nothing about a client that asks gyro to map more of it than it ever
	// wrote, which faults exactly the same way. So the file is measured again at every resize, and the
	// mapped path is only ever as large as what is behind it.
	const std::optional<std::size_t> held = FileBytes(m_Fd);

	if (!held.has_value() || *held < size)
	{
		return false;
	}

	if (m_Pixels == nullptr)
	{
		m_Size = size;

		return true;
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

std::span<const std::byte> PoolMapping::Mapped(std::size_t offset, std::size_t bytes) const noexcept
{
	if (m_Pixels == nullptr || offset > m_Size || bytes > m_Size - offset)
	{
		return {};
	}

	return { m_Pixels + offset, bytes };
}

std::span<const std::byte> PoolMapping::Copy(std::size_t offset, std::size_t bytes) const
{
	if (offset > m_Size || bytes > m_Size - offset)
	{
		return {};
	}

	m_Scratch.resize(bytes);

	std::size_t done = 0;

	while (done < bytes)
	{
		const ssize_t read =
			::pread(m_Fd.Borrow().Value, m_Scratch.data() + done, bytes - done, static_cast<off_t>(offset + done));

		if (read < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}

			return {};
		}

		// Zero is the end of the file, which is a client that truncated the pool under a buffer it had
		// already described. The frame is lost and the compositor is not, which is the whole trade.
		if (read == 0)
		{
			return {};
		}

		done += static_cast<std::size_t>(read);
	}

	return m_Scratch;
}

std::span<const std::byte> ClientShmBuffer::Pixels() const
{
	if (m_Pool == nullptr)
	{
		return {};
	}

	const auto needed = static_cast<std::size_t>(m_Stride) * static_cast<std::size_t>(m_Size.Height);
	const std::span<const std::byte> mapped = m_Pool->Mapped(m_Offset, needed);

	return mapped.empty() ? m_Pool->Copy(m_Offset, needed) : mapped;
}

Result<TextureId> ClientShmBuffer::Adopt(ITextures& textures, SyncTimelinePoint release)
{
	// **Never set, and the assertion is the comment.** A `wl_shm` buffer answers `SupportsExplicitSync`
	// with false, so a client that named a timeline point for one was ended with `unsupported_buffer`
	// before the commit reached here. The parameter exists because the verb is one verb.
	static_cast<void>(release);

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
	// `wl_shm_pool` gained an error enumeration of its own in wayland 1.26, naming the two faults a
	// `create_buffer` can have. They are the codes `wl_shm` already carried and the same values, so what
	// reaches the client is unchanged and the call sites below now say which interface they are refusing
	// on.
	const auto refuse =
		[this](Wayland::Server::WlShmPoolError code, const char* why) -> Wayland::Server::WlBufferHandler* {
		Object().PostError(code, why);

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
			Wayland::Server::WlShmPoolError::InvalidFormat,
			"wl_shm_pool.create_buffer with a format wl_shm never advertised"
		);
	}

	if (offset < 0 || width <= 0 || height <= 0 || stride <= 0)
	{
		return refuse(
			Wayland::Server::WlShmPoolError::InvalidStride, "wl_shm_pool.create_buffer with a negative or empty extent"
		);
	}

	const auto rowBytes = static_cast<std::size_t>(width) * BytesPerPixel;
	const auto rows = static_cast<std::size_t>(height);
	const auto pitch = static_cast<std::size_t>(stride);

	if (pitch < rowBytes)
	{
		return refuse(
			Wayland::Server::WlShmPoolError::InvalidStride,
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
			Wayland::Server::WlShmPoolError::InvalidStride,
			"wl_shm_pool.create_buffer reaching past the end of its pool"
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
		// A pool only grows, which is the protocol's own rule — and it grows no further than the file
		// behind it, which is the rule a client asking for the fault would otherwise get around.
		//
		// **The code is `wl_shm`'s `invalid_fd` and the interface is the pool's**, which is what
		// libwayland's own `wl_shm` posts for a failed resize and therefore what a client library reads
		// there. `wl_shm_pool`'s enumeration names only the two `create_buffer` faults, so a resize failure
		// has no code of its own to use — cast rather than mistyped as one of the two it is not.
		Object().PostError(
			static_cast<Wayland::Server::WlShmPoolError>(Wayland::Server::WlShmError::InvalidFd),
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
			case PoolMapping::Refusal::Unreadable:
				Object().PostError(
					Wayland::Server::WlShmError::InvalidFd,
					"wl_shm.create_pool with a descriptor gyro cannot read pixels out of"
				);

				break;

			case PoolMapping::Refusal::Short:
				Object().PostError(
					Wayland::Server::WlShmError::InvalidFd,
					"wl_shm.create_pool naming more bytes than the descriptor holds"
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
