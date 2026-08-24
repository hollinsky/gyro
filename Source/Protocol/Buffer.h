#pragma once

#include "Core/Result.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "Scene/Textures.h"
#include "Wayland/Server/Wayland.h"

// What a `wl_surface` needs of a `wl_buffer`, whatever made it.
//
// **`wl_buffer` is the one interface in the core protocol with more than one factory**, which is why
// there is a base here and not for `wl_surface` or `wl_region`. A client's pixels arrive from `wl_shm`
// today and from `zwp_linux_dmabuf_v1` next, and the surface that attaches one has no business knowing
// which — its whole question is *give me an id for these pixels*, and the two answers are a copy out of
// a mapping and an import of a descriptor.
//
// **`Of` is the downcast, and the cast is static because `Implementation` already made it sound.** That
// call checks the interface *and* the dispatch table, so what comes back is a handler gyro itself put
// behind a `wl_buffer` — and the rule this file states is that every one of those is a `ClientBuffer`.
// A `dynamic_cast` would be asking the compiler to re-derive a fact the table already carries, at the
// cost of RTTI on the one path a window's content takes.
class ClientBuffer : public Wayland::Server::WlBufferHandler
{
public:
	// Give these pixels an id.
	//
	// **The buffer is not held afterwards and the caller says so to the client**, which is the whole
	// benefit of gyro copying: `wl_buffer.release` goes back in the same step, so a toolkit drawing into
	// one buffer never waits for a second. What it costs is a copy on the dispatch thread, which owes no
	// deadline.
	//
	// Whatever the texture space refused, which the caller answers to the client rather than dropping —
	// a window that silently never appears is the hardest bug there is to report.
	[[nodiscard]] virtual Result<TextureId> Adopt(ITextures& textures) const = 0;

	// The extent of the pixels, in the buffer's own space.
	[[nodiscard]] virtual PixelSize<BufferSpace> Extent() const noexcept = 0;

	// The implementation behind an id a client named, or null where the id was not a `wl_buffer` gyro
	// made.
	[[nodiscard]] static ClientBuffer* Of(Wayland::Server::WlBuffer buffer) noexcept
	{
		return static_cast<ClientBuffer*>(buffer.Implementation());
	}
};
