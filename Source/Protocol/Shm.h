#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "Protocol/Buffer.h"
#include "Scene/Textures.h"
#include "Wayland/Server/Wayland.h"

// `wl_shm`: a client's pixels, in the only way a client can hand gyro pixels today.
//
// **The pool is mapped read-only and gyro copies out of it, which is what makes `wl_buffer.release`
// immediate.** The protocol lets a compositor hold a buffer for as long as it is drawing from it, and
// a toolkit that has to wait for the release is a toolkit that keeps a second buffer to draw the next
// frame into. gyro copies at commit — `Scene/Textures.h` already promises the bytes need not outlive
// the call — so the release goes back in the same step and a client with one buffer never stalls.
// What that costs is a copy per commit on the dispatch thread, which owes no deadline.
//
// **A pool must be shrink-sealed, and gyro seals it if the client did not.** This is the one place a
// client can kill the compositor: the mapping is a window onto the client's file, and a client that
// truncates the file underneath it turns gyro's next read into `SIGBUS` — which, for a compositor
// that is the system's only one, is every user's session ending because one application asked it to.
// The usual answer is a `SIGBUS` handler, and the reason not to take it is that it is a signal handler
// that has to know which mapping faulted and substitute pages under it, on a process running
// `SCHED_FIFO`. `F_SEAL_SHRINK` removes the condition instead of catching it, costs one `fcntl`, and
// is already what the client library does — Mesa and libwayland both create the pool as a `memfd` with
// `MFD_ALLOW_SEALING` and seal it themselves.
//
// A descriptor that will not take the seal is refused with `invalid_fd`, which ends that one client
// and no other. The alternative was to map it anyway and hope, and the shape of that bug is a
// compositor that dies minutes later with a stack in whichever unrelated thing touched the page.
//
// **Only `argb8888` and `xrgb8888` are advertised**, which is the pair `wl_shm` requires of every
// compositor and the pair `Scene/Textures.h` takes without asking what a fourcc is. The rest are
// optional and every one of them is a conversion gyro would be doing on the dispatch thread for a
// client that could have asked for the format the screen is already in.

// The `wl_shm` global's advertised version. Version 2 is `wl_shm.release`, a request and not an event,
// so there is nothing owed by naming it.
inline constexpr std::uint32_t ShmVersion = 2;

// SPEC: the largest pool gyro will map for one client. A pool is a mapping in gyro's address space
// that a client sizes, so this bounds what one application can make the compositor commit to — decision
// 27's rule. Two 8K buffers at 32 bits a pixel is a little over a quarter of this, which is the
// largest thing a real toolkit asks for.
inline constexpr std::size_t MaxPoolBytes = 512U * 1024U * 1024U;

// A read-only mapping of a client's pool, and the descriptor it came from.
//
// **Shared, because a pool outlives its `wl_shm_pool`.** The protocol says a buffer keeps the pool
// alive — a client may destroy the pool object the instant after creating a buffer from it, and
// usually does — so the mapping is refcounted between the pool object and every buffer cut from it.
// The descriptor is kept alongside because `wl_shm_pool.resize` remaps from it.
class PoolMapping
{
public:
	// Maps `size` bytes of `fd`, sealing it against shrinking first.
	//
	// Null where the descriptor would not take the seal, where the size is out of range, or where the
	// mapping itself failed — the caller turns each of those into the protocol error it deserves, so
	// the reason comes back beside the pointer.
	enum class Refusal : std::uint8_t
	{
		None,
		Unsealable,
		Size,
		Mapping,
	};

	[[nodiscard]] static std::shared_ptr<PoolMapping> Map(Fd fd, std::size_t size, Refusal& refusal);

	PoolMapping(const PoolMapping&) = delete;
	PoolMapping& operator=(const PoolMapping&) = delete;
	PoolMapping(PoolMapping&&) = delete;
	PoolMapping& operator=(PoolMapping&&) = delete;

	~PoolMapping();

	// Remap at a larger size. Growing only, which is the protocol's own rule and also the seal's.
	[[nodiscard]] bool Grow(std::size_t size);

	[[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return { m_Pixels, m_Size }; }

	[[nodiscard]] std::size_t Size() const noexcept { return m_Size; }

private:
	PoolMapping(Fd fd, const std::byte* pixels, std::size_t size) noexcept
		: m_Fd{ std::move(fd) }, m_Pixels{ pixels }, m_Size{ size }
	{}

	Fd m_Fd;
	const std::byte* m_Pixels = nullptr;
	std::size_t m_Size = 0;
};

// One `wl_buffer` cut out of a pool: an extent, a stride, a format, and an offset into the mapping.
//
// It holds the mapping rather than a pointer into it, because `wl_shm_pool.resize` moves the mapping
// and a buffer made before the resize is still valid after it.
class ClientShmBuffer final : public ClientBuffer
{
public:
	ClientShmBuffer(
		std::shared_ptr<PoolMapping> pool,
		std::size_t offset,
		PixelSize<BufferSpace> size,
		std::uint32_t stride,
		Wayland::Server::WlShmFormat format
	) noexcept
		: m_Pool{ std::move(pool) }, m_Offset{ offset }, m_Size{ size }, m_Stride{ stride }, m_Format{ format }
	{}

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	[[nodiscard]] Result<TextureId> Adopt(ITextures& textures) const override;

	[[nodiscard]] PixelSize<BufferSpace> Extent() const noexcept override { return m_Size; }

	[[nodiscard]] std::uint32_t Stride() const noexcept { return m_Stride; }

	[[nodiscard]] Wayland::Server::WlShmFormat Format() const noexcept { return m_Format; }

	// The rows this buffer names, or an empty span if the pool is no longer large enough to hold them.
	//
	// **That second case cannot happen while the seal holds** and the check is here anyway, because it
	// is the difference between a bug in this file and a read off the end of a mapping. A pool only
	// grows, and the bounds were checked against its size at `create_buffer`.
	[[nodiscard]] std::span<const std::byte> Pixels() const noexcept;

private:
	std::shared_ptr<PoolMapping> m_Pool;

	std::size_t m_Offset = 0;
	PixelSize<BufferSpace> m_Size{};
	std::uint32_t m_Stride = 0;
	Wayland::Server::WlShmFormat m_Format = Wayland::Server::WlShmFormat::Argb8888;
};

// One client's `wl_shm_pool`.
class ClientShmPool final : public Wayland::Server::WlShmPoolHandler
{
public:
	explicit ClientShmPool(std::shared_ptr<PoolMapping> mapping) noexcept : m_Mapping{ std::move(mapping) } {}

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately. The mapping
	// stays alive for as long as a buffer cut from it does.
	void OnDestroy() override {}

	Wayland::Server::WlBufferHandler* OnCreateBuffer(
		std::int32_t offset,
		std::int32_t width,
		std::int32_t height,
		std::int32_t stride,
		Wayland::Server::WlShmFormat format
	) override;

	void OnResize(std::int32_t size) override;

private:
	std::shared_ptr<PoolMapping> m_Mapping;
};

// One client's `wl_shm`.
class ClientShm final : public Wayland::Server::WlShmHandler
{
public:
	void OnGone() override { delete this; }

	// The format list, which is what `wl_shm`'s contract begins with: a client binds and expects to be
	// told what it may draw in before it has asked anything. Two, and both are a 32-bit little-endian
	// word — the pair every compositor must accept, and the pair the texture space takes as it stands.
	//
	// Everything else `wl_shm_format` names is optional, and each one gyro added would be a conversion
	// on the dispatch thread for a client that could have drawn in what the screen is already in.
	void OnBound() override;

	Wayland::Server::WlShmPoolHandler* OnCreatePool(Fd fd, std::int32_t size) override;

	// The resource is already destroyed when this runs, and `OnGone` follows immediately. Pools
	// created through it are unaffected, which the protocol says out loud.
	void OnRelease() override {}
};

// The global itself, owned by whoever advertises it and outliving every client that binds it.
class ShmGlobal final : public Wayland::Server::WlShmBinding
{
public:
	Wayland::Server::WlShmHandler* OnBind(wl_client& client, std::uint32_t version) override;
};
