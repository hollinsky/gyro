#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "Core/Result.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"

// What an author sees of the texture id space: pixels go in, an id comes back, and the id is given up
// again when the author stops drawing it.
//
// **This is declared here and implemented in `Dispatch`, which is the direction the module graph
// requires and also the honest one.** `Dispatch/Textures.h` holds the registry — it is the half that
// can name `Seam`, mint against the watermark, and re-adopt across a device rebuild — and `Dispatch`
// depends on `Gym`, so the interface an author calls cannot live beside its implementation. That is
// not a workaround: an author is a *caller* of the texture space in the same way it is a caller of
// `Scene`, and what it needs to know is two verbs.
//
// **A gym does not name `Seam`, and this file is what keeps that true.** Decision 87's rule is that
// neither `Protocol` nor `Scene` may name the control waist; a gym stands where clients will stand, so
// a gym reaching for `TextureSource` or a format code would be building the shape the protocol layer
// is forbidden from copying. So the pixels cross as an extent, a stride and bytes, and *the party that
// adopts names the format* — which is the same division that will hold when the bytes are a client's
// `wl_shm` pool instead of a card gyro drew for itself.
//
// **One id space and no second one, per Core/Texture.h.** A gym's card, a client's surface and the
// snapshot atlas are all ids from here, because Docs/Animation.md#exit-pixels has a window's live
// surface become a compositor-owned snapshot mid-transition and an id space per origin would make that
// a different node rather than the same one.
class ITextures
{
public:
	ITextures() = default;

	virtual ~ITextures() = default;

	ITextures(const ITextures&) = delete;
	ITextures& operator=(const ITextures&) = delete;
	ITextures(ITextures&&) = delete;
	ITextures& operator=(ITextures&&) = delete;

	// Give these pixels an id.
	//
	// **The bytes are read during the call and need not outlive it**, which is the opposite of
	// Seam/Importer.h's contract one layer down and is deliberate: the importer borrows, so *something*
	// has to hold the pixels for as long as an id names them, and an author holding them is an author
	// that has to understand the watermark. The registry holds them instead. What that costs is a copy
	// on the dispatch thread, which owes no deadline.
	//
	// The layout is eight bits a channel with alpha in the top byte of a 32-bit little-endian word —
	// Gym/Card.h's own spelling, and the only one an author writes today. `stride` is bytes per row,
	// which is not `width * 4` the moment anything is padded.
	//
	// `EINVAL` for an extent, a stride or a span that do not describe each other; whatever the importer
	// answered where it refused; `ENOSPC` where the id space or a renderer's table is full.
	[[nodiscard]] virtual Result<TextureId>
	Adopt(PixelSize<BufferSpace> size, std::uint32_t stride, std::span<const std::byte> pixels) = 0;

	// Stop drawing this id. It stays valid for the frames already published that name it, and the
	// registry is what waits — Seam/Importer.h's watermark rule is one party's to honour and this is not
	// that party.
	//
	// Says nothing about an id it does not hold, which is `Forget`'s answer one layer down and for the
	// same reason: a double retire is not a condition an author can act on.
	virtual void Retire(TextureId id) noexcept = 0;
};
