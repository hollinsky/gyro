#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "Core/Result.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "Protocol/Sync.h"
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
	// **`release` is the timeline point a `wp_linux_drm_syncobj_surface_v1` named for this adoption**,
	// or nothing for the ordinary case. It is per adoption rather than per buffer for the same reason
	// `wl_buffer.release` is counted rather than sent once: a buffer committed twice before the first
	// frame left the screen has two ids and two points against it, and the buffer is the only party
	// that sees both. Unset on every implementation but the dmabuf one, which is what
	// `SupportsExplicitSync` below already refused the client for.
	[[nodiscard]] virtual Result<TextureId> Adopt(ITextures& textures, SyncTimelinePoint release) = 0;

	// Whether a client may name timeline points for content arriving in this buffer.
	//
	// **False is the copying answer and true is the borrowing one**, which is `ReleasesImmediately`
	// asked from the other end and is not the same question. A `wl_shm` buffer's pixels are *read* by
	// `Adopt` itself, so honouring an acquire point on one would mean deferring the copy — and the copy
	// is what makes the buffer the client's again in the same step. The protocol has a name for
	// declining that, `unsupported_buffer`, and says outright that a compositor need support explicit
	// synchronization only for buffers the dmabuf protocol made. So a software client is told plainly
	// rather than served a contract gyro would have to break the ordering of.
	[[nodiscard]] virtual bool SupportsExplicitSync() const noexcept { return false; }

	// Whether the client may draw into this buffer again the instant `Adopt` returns.
	//
	// **True is the copying answer and false is the borrowing one**, which is the whole difference
	// between the two factories. `wl_shm` pixels are copied into gyro's own memory, so the release goes
	// back in the same step and a toolkit with one buffer never stalls. A `zwp_linux_dmabuf_v1` buffer
	// is *not* copied — that is the point of it — so the memory stays the client's and stays readable by
	// a panel, and telling the client otherwise is a window tearing into itself while it animates. The
	// buffer owes its own release in that case, against the watermark, through `ITextureRelease`.
	[[nodiscard]] virtual bool ReleasesImmediately() const noexcept { return true; }

	// The extent of the pixels, in the buffer's own space.
	[[nodiscard]] virtual PixelSize<BufferSpace> Extent() const noexcept = 0;

	// The rows as the processor can read them, for Scene/Capture.h and nothing else.
	//
	// **Empty is the honest answer for a descriptor and the default says so, and it no longer means
	// *no capture*.** A `wl_shm` buffer is already being read on this thread — the commit copies it —
	// so handing the same span to a capture costs a walk over memory gyro is about to walk anyway. A
	// dmabuf's bytes are tiled under a modifier and are not a picture until somebody detiles them,
	// which nothing on the dispatch thread can do; so an empty span is the offer saying *ask the
	// device*, and Seam/Capture.h reads that image back on the frame thread instead. `MappedAlpha`
	// stays answerable either way, because the client named the format.
	//
	// Borrowed and live only for the call: a pool mapping is the client's memory, and a pool that could
	// not be mapped is a scratch buffer the next read overwrites.
	[[nodiscard]] virtual std::span<const std::byte> MappedRows() { return {}; }

	// Bytes between rows of `MappedRows`, which is the client's own pitch and not `Width * 4`.
	[[nodiscard]] virtual std::uint32_t MappedStride() const noexcept { return 0; }

	// What the top byte of a sample means, stated as an adoption states it.
	[[nodiscard]] virtual TextureAlpha MappedAlpha() const noexcept { return TextureAlpha::Premultiplied; }

	// The implementation behind an id a client named, or null where the id was not a `wl_buffer` gyro
	// made.
	[[nodiscard]] static ClientBuffer* Of(Wayland::Server::WlBuffer buffer) noexcept
	{
		return static_cast<ClientBuffer*>(buffer.Implementation());
	}
};
