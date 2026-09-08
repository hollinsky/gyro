#include "Shell/Canvas.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>

Canvas::~Canvas()
{
	Close();
}

void Canvas::Close() noexcept
{
	// The buffers before the pool, which is the order the protocol asks for: a pool's memory stays
	// alive until every buffer made from it is gone, so destroying it first would leave two objects
	// holding a mapping this function is about to take away.
	for (std::size_t index = 0; index < Count; ++index)
	{
		if (m_Buffers[index].IsValid())
		{
			m_Buffers[index].Destroy();
		}

		m_Buffers[index] = {};

		// Cleared with the buffer it belonged to. A release for a buffer that no longer exists never
		// arrives, so a canvas reopened while the compositor held one would otherwise start life with a
		// permanently busy slot and draw at half the buffering it thinks it has.
		m_Released[index].Busy = false;
	}

	if (m_Pool.IsValid())
	{
		m_Pool.Destroy();
	}

	m_Pool = {};

	if (m_Words != nullptr)
	{
		::munmap(m_Words, m_Bytes);
	}

	m_Words = nullptr;
	m_Bytes = 0;
	m_Current = Count;
	m_Width = 0;
	m_Height = 0;
}

Result<void> Canvas::Open(Wayland::WlShm shm, std::int32_t width, std::int32_t height)
{
	if (width <= 0 || height <= 0)
	{
		return Failure(EINVAL, "sizing the shell's canvas", Subject::Of("{}x{}", width, height));
	}

	// **Before anything is allocated, so a reopen that fails leaves nothing behind.** Every path out of
	// here below is a failure the caller reports and stops on, and holding the old mapping across one
	// would be holding a canvas sized for a screen the bar is no longer on.
	Close();

	const std::size_t stride = static_cast<std::size_t>(width) * 4U;
	const std::size_t frame = stride * static_cast<std::size_t>(height);
	const std::size_t bytes = frame * Count;

	// `MFD_ALLOW_SEALING` is not optional here and the compositor is why. Protocol/Shm.cpp maps a pool
	// whose descriptor takes `F_SEAL_SHRINK` and falls back to reading one that will not — the fallback
	// exists for toolkits gyro does not control, and a shell of gyro's own has no business taking it.
	const int descriptor = ::memfd_create("gyro-shell", MFD_CLOEXEC | MFD_ALLOW_SEALING);

	if (descriptor < 0)
	{
		return Failure(errno, "creating the shell's canvas");
	}

	Fd owned{ descriptor };

	if (::ftruncate(owned.Borrow().Value, static_cast<off_t>(bytes)) != 0)
	{
		return Failure(errno, "sizing the shell's canvas");
	}

	void* const mapped = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, owned.Borrow().Value, 0);

	if (mapped == MAP_FAILED)
	{
		return Failure(errno, "mapping the shell's canvas");
	}

	// The pool keeps the descriptor because `wl_shm.create_pool` takes it, so the mapping above is the
	// only handle on these bytes this process keeps.
	m_Words = static_cast<std::uint32_t*>(mapped);
	m_Bytes = bytes;
	m_Width = width;
	m_Height = height;

	m_Pool = shm.CreatePool(std::move(owned), static_cast<std::int32_t>(bytes));

	if (!m_Pool.IsValid())
	{
		return Failure(EPROTO, "asking the compositor for a buffer pool");
	}

	for (std::size_t index = 0; index < Count; ++index)
	{
		m_Buffers[index] = m_Pool.CreateBuffer(
			static_cast<std::int32_t>(frame * index),
			width,
			height,
			static_cast<std::int32_t>(stride),
			Wayland::WlShmFormat::Argb8888,
			m_Released[index]
		);

		if (!m_Buffers[index].IsValid())
		{
			return Failure(EPROTO, "asking the compositor for a buffer");
		}
	}

	return {};
}

std::span<std::uint32_t> Canvas::Next() noexcept
{
	const std::size_t words = static_cast<std::size_t>(m_Width) * static_cast<std::size_t>(m_Height);

	for (std::size_t index = 0; index < Count; ++index)
	{
		if (!m_Released[index].Busy)
		{
			m_Current = index;

			return { m_Words + words * index, words };
		}
	}

	m_Current = Count;

	return {};
}

Wayland::WlBuffer Canvas::Take() noexcept
{
	if (m_Current >= Count)
	{
		return {};
	}

	m_Released[m_Current].Busy = true;

	return m_Buffers[m_Current];
}
