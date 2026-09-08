#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Wayland/Wayland.h"

// The pixels a shell draws into, and the `wl_buffer` gyro reads them out of.
//
// **Two buffers rather than one, and it is not premature.** A launcher redraws on every keystroke,
// and one buffer means each redraw waits for the compositor to say it has finished with the last —
// which on a compositor holding a frame while it composites the one behind it is a character
// appearing later than it was typed. Two is enough because nothing here draws faster than a person
// types.
//
// **`ARGB8888` and no other format**, because it is the one `wl_shm` guarantees and because the
// transparency is the point: gyro draws the material behind this surface and it is seen exactly where
// these words have a zero alpha. A shell that filled the buffer opaque would get an opaque launcher,
// which the protocol says out loud.
//
// Opened rather than constructed, and neither copied nor moved, for the reason every listener in this
// tree carries: the connection holds the address of the `wl_buffer` listeners below for as long as
// those objects are bound, so a canvas that moved would leave the dispatcher pointing at the husk.
class Canvas
{
public:
	Canvas() = default;
	~Canvas();

	Canvas(const Canvas&) = delete;
	Canvas& operator=(const Canvas&) = delete;

	// Creates the pool and both buffers at this size in device pixels. Refuses an empty extent, which
	// is the size a shell computes when it bound no output and did not notice.
	//
	// **Callable again, and that is what a scale change costs.** The pool is one allocation sized at
	// open, so a bar told it is now on a 2x screen has no way to honour that except to throw the
	// mapping away and make another — see `Close`. Nothing is preserved across it: the caller redraws
	// immediately, because the only reason to have resized is that it is about to.
	[[nodiscard]] Result<void> Open(Wayland::WlShm shm, std::int32_t width, std::int32_t height);

	// The words to draw the next frame into. Empty where both buffers are still held by the
	// compositor, which is a caller drawing faster than the screen refreshes — something to skip
	// rather than to wait on, since the frame it would have drawn is superseded by the next one.
	[[nodiscard]] std::span<std::uint32_t> Next() noexcept;

	// The buffer `Next` drew into, marked busy until the compositor releases it. Attach, damage and
	// commit stay the caller's, because a shell wanting a frame callback or a material change on the
	// same commit has to put them in between.
	[[nodiscard]] Wayland::WlBuffer Take() noexcept;

	[[nodiscard]] std::int32_t Width() const noexcept { return m_Width; }

	[[nodiscard]] std::int32_t Height() const noexcept { return m_Height; }

	[[nodiscard]] bool IsValid() const noexcept { return m_Words != nullptr; }

	// Whether the canvas already stands at this size, so a configure that changed nothing costs no
	// reallocation. gyro re-sends a configure whenever the world moves under a bar that is up, which on
	// a busy desk is often.
	[[nodiscard]] bool Is(std::int32_t width, std::int32_t height) const noexcept
	{
		return IsValid() && m_Width == width && m_Height == height;
	}

private:
	// Gives back the pool, the buffers and the mapping, leaving the canvas as though it had never been
	// opened.
	//
	// **The buffers are destroyed even where the compositor still holds one**, which is safe for the
	// narrow reason that the caller attaches a new buffer in the same breath: a `wl_buffer` destroyed
	// while attached makes that surface's contents undefined, and a surface about to be committed with
	// a different buffer has no contents to lose. What would not be safe is destroying the pool and
	// keeping the buffers, which is why this does both or neither.
	void Close() noexcept;

	// What a `wl_buffer` says about itself: one event, and it is the one that matters — whether these
	// words may be written again.
	class Released final : public Wayland::WlBufferListener
	{
	public:
		void OnRelease() override { Busy = false; }

		bool Busy = false;
	};

	static constexpr std::size_t Count = 2;

	Fd m_Descriptor;
	Wayland::WlShmPool m_Pool;
	Wayland::WlBuffer m_Buffers[Count];
	Released m_Released[Count];
	std::uint32_t* m_Words = nullptr;
	std::size_t m_Bytes = 0;
	std::size_t m_Current = Count;
	std::int32_t m_Width = 0;
	std::int32_t m_Height = 0;
};
