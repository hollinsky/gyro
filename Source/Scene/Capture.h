#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "Geometry/Space.h"
#include "Scene/Textures.h"

// The other half of Input/Chord.h's screenshot: the buffer a client committed, beside the picture the
// panel was showing.
//
// **A screenshot alone cannot say whose fault a stale pixel is.** Seam/Capture.h reads a target back
// after forcing a full composite, so its bytes are provably the bytes that went to the panel — and
// that is exactly why it cannot answer the next question. When a row of an old window title survives a
// repaint, the picture shows the survival and nothing else: gyro composites whole outputs
// (Frame/Evaluator.h's `Changed`) and copies whole `wl_shm` buffers (Protocol/Shm.cpp), so neither of
// gyro's two chances to drop a row is a chance it takes, and yet the row is on the glass. What splits
// it is the buffer the client handed over for that frame. If the stale row is already in the buffer,
// the client repainted too little and the question moves to what gyro told it about reuse; if the
// buffer is clean, the loss is downstream of the copy and it is ours.
//
// **The damage rectangles are the reason this is worth more than the pixels.** A client's repaint is
// its own damage applied to whatever the buffer already held, and Protocol/Surface.cpp accumulates
// what it was told and then drops it on the floor at the end of the commit. Recording it here turns
// *is this client repainting too little* from an inference into a read: the rectangles either cover
// the row that went stale or they do not.
//
// **`wl_shm` only, and the boundary is honest rather than temporary.** Those pixels are copied at
// commit and are gyro's the moment `Adopt` returns, so offering them costs a walk over memory the
// process already owns. A `zwp_linux_dmabuf_v1` buffer is borrowed, under a modifier that makes its
// bytes something other than a picture, and reading one back means a map or a blit on the dispatch
// thread — a path that half works would be worse than a gap somebody can see in the log.
//
// **Here rather than at the waist for Scene/Textures.h's reason**, which is decision 87: `Protocol`
// mints the thing being captured and may not name `Seam`, and the party that implements this is the
// composition root. So the verb is declared below both and the alpha is stated in Core/Texture.h's
// vocabulary rather than as a fourcc, exactly as an adoption already states it.

// One committed buffer, offered for the length of one call.
//
// Every span is borrowed and none of it outlives `Offer` — the pixels are a client's pool mapping and
// the rectangles are about to be cleared by the commit that produced them.
struct SurfaceCapture
{
	// Which surface, as the wire names it. The client's own object id, so a capture can be read beside
	// a protocol log without a table in between.
	std::uint32_t Surface = 0;

	// What the buffer says it is. `Stride` is the client's, which is not `Width * 4` in general.
	PixelSize<BufferSpace> Size{};
	std::uint32_t Stride = 0;
	TextureAlpha Alpha = TextureAlpha::Premultiplied;

	// The rows, at `Stride` apart.
	std::span<const std::byte> Pixels;

	// What the client said it changed, in buffer coordinates, in the order it said it.
	//
	// **Not reduced, and not unioned with the surface-local rectangles.** `Geometry/Region.h` collapses
	// to a bounding box when it runs out of room, and a bounding box answers *did the client repaint
	// around here* when the question is *did it repaint this row*. The surface-space damage is the
	// client's other spelling and reconciling the two is Protocol/Surface.h's job at a point this is
	// not; a capture that did it here would be reporting gyro's arithmetic rather than the client's
	// claim.
	std::span<const PixelRect<BufferSpace>> Damage;
};

// Where a committed buffer goes. Implemented by the composition root, held by Protocol/Context.h for
// the length of a host's life, and null on every run that did not ask for captures.
class ISurfaceCapture
{
public:
	ISurfaceCapture() = default;
	ISurfaceCapture(const ISurfaceCapture&) = delete;
	ISurfaceCapture& operator=(const ISurfaceCapture&) = delete;

	virtual ~ISurfaceCapture() = default;

	// Is anything owed a buffer?
	//
	// **Asked before the pixels are fetched**, which is the whole reason it is separate from `Offer`:
	// a `wl_shm` pool that could not be mapped is read with `pread` into a scratch buffer, so asking
	// for the rows of every commit on every frame would put a copy of every window on the dispatch
	// thread for a key nobody pressed.
	[[nodiscard]] virtual bool Wanted() const noexcept = 0;

	// Take one, or decline it. Declining is ordinary — one capture per surface per press, and a writer
	// still busy with the last one is a reason to say nothing rather than to wait.
	virtual void Offer(const SurfaceCapture& buffer) noexcept = 0;
};
