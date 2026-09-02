#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

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
// **A mapping is a window onto somebody else's file, and gyro never reads through one it cannot prove
// is backed.** This is the one place a client can kill the compositor: truncate the file under the
// mapping and gyro's next read is a `SIGBUS`, which for the system's only compositor is every user's
// session ending because one application asked it to. The usual answer is a `SIGBUS` handler, and the
// reason not to take it is that it is a signal handler that has to know which mapping faulted and
// substitute pages under it, on a process running `SCHED_FIFO`.
//
// So there are two paths, and which one a pool takes is decided once, when it arrives.
//
// **A descriptor that takes `F_SEAL_SHRINK` is mapped**, which is nearly all of them: Mesa and
// libwayland both create the pool as a `memfd` with `MFD_ALLOW_SEALING`, and where the client did not
// seal it, gyro does. The seal says the file cannot get shorter; `fstat` at the same moment says it is
// already long enough for the size the client named. Together those are a proof rather than a hope,
// and they are checked again at `resize` — because `F_SEAL_SHRINK` says nothing about a client that
// grows the *pool* past a file it never grew, which is the same fault by the other route.
//
// **A descriptor that will not take it is read with `pread` instead**, and is not mapped at all. This
// is not a rare case and refusing it was a bug: GTK creates its pool with a plain `memfd_create` and no
// `MFD_ALLOW_SEALING`, so gyro used to end the connection of every GTK application on the machine —
// Firefox reached the error before it reached a window. A `pread` past the end of a file is a short
// read rather than a fault, so the hazard is gone by construction with no signal handler anywhere: a
// client that truncates its pool loses that one frame with a protocol error, and nothing else on the
// machine notices. What it costs is one copy into a scratch buffer per commit, on the dispatch thread,
// which owes no deadline — and only for the clients that could not be mapped safely.
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
	// Takes `size` bytes of `fd`, mapping it where it can be sealed against shrinking and reading it
	// with `pread` where it cannot.
	//
	// Null where the descriptor is not a file gyro can read pixels out of at all, where it is shorter
	// than the pool the client named, where the size is out of range, or where the mapping itself
	// failed — the caller turns each of those into the protocol error it deserves, so the reason comes
	// back beside the pointer.
	enum class Refusal : std::uint8_t
	{
		None,
		Unreadable,
		Short,
		Size,
		Mapping,
	};

	[[nodiscard]] static std::shared_ptr<PoolMapping> Map(Fd fd, std::size_t size, Refusal& refusal);

	PoolMapping(const PoolMapping&) = delete;
	PoolMapping& operator=(const PoolMapping&) = delete;
	PoolMapping(PoolMapping&&) = delete;
	PoolMapping& operator=(PoolMapping&&) = delete;

	~PoolMapping();

	// Take a larger size. Growing only, which is the protocol's own rule and also the seal's — and
	// refused where the file itself is not that big, because a pool grown past its own backing is the
	// truncation hazard arriving from the other direction.
	[[nodiscard]] bool Grow(std::size_t size);

	// The bytes at `offset`, straight out of the mapping. Empty when this pool is one gyro declined to
	// map, which is what sends the caller to `Copy` — and the two are one question at the call site
	// rather than a flag it has to remember to ask about.
	[[nodiscard]] std::span<const std::byte> Mapped(std::size_t offset, std::size_t bytes) const noexcept;

	// The same bytes, read out of the descriptor into a scratch buffer this pool owns. Empty where the
	// read came up short, which is exactly the case the mapping could not have survived.
	//
	// **One scratch per pool rather than per buffer**, so what a client can make gyro hold is bounded
	// by the pool it already sized rather than by the number of buffers it cuts out of one. The span is
	// good until the next call, which is all `Scene/Textures.h` asks of the pixels it is handed.
	[[nodiscard]] std::span<const std::byte> Copy(std::size_t offset, std::size_t bytes) const;

	[[nodiscard]] std::size_t Size() const noexcept { return m_Size; }

private:
	PoolMapping(Fd fd, const std::byte* pixels, std::size_t size) noexcept
		: m_Fd{ std::move(fd) }, m_Pixels{ pixels }, m_Size{ size }
	{}

	Fd m_Fd;

	// The mapping, or null for a pool being read through its descriptor. There is no second flag: not
	// mapped *is* the unsealed case, and the one place that matters is `Grow`.
	const std::byte* m_Pixels = nullptr;
	std::size_t m_Size = 0;

	// Where `Copy` puts what it read. Mutable because reading a client's pixels does not change the
	// pool, and this is a buffer rather than state.
	mutable std::vector<std::byte> m_Scratch;
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

	[[nodiscard]] Result<TextureId> Adopt(ITextures& textures, SyncTimelinePoint release) override;

	[[nodiscard]] PixelSize<BufferSpace> Extent() const noexcept override { return m_Size; }

	[[nodiscard]] std::uint32_t Stride() const noexcept { return m_Stride; }

	[[nodiscard]] Wayland::Server::WlShmFormat Format() const noexcept { return m_Format; }

	// The rows this buffer names, or an empty span if the pool can no longer produce them — which is a
	// pool that shrank, impossible by construction, or a descriptor that came up short under `pread`,
	// which is the truncation a client is entitled to attempt and the compositor is obliged to survive.
	//
	// Not `noexcept`: the unmapped path allocates, once per pool and per size.
	[[nodiscard]] std::span<const std::byte> Pixels() const;

	// Scene/Capture.h's three, which `wl_shm` is the one factory that can answer: these pixels are the
	// same ones the commit is about to copy, so a capture reads them rather than reconstructing them.
	[[nodiscard]] std::span<const std::byte> MappedRows() override { return Pixels(); }

	[[nodiscard]] std::uint32_t MappedStride() const noexcept override { return m_Stride; }

	[[nodiscard]] TextureAlpha MappedAlpha() const noexcept override
	{
		return m_Format == Wayland::Server::WlShmFormat::Argb8888 ? TextureAlpha::Premultiplied : TextureAlpha::None;
	}

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
