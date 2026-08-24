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
// requires.** `Dispatch/Textures.h` holds the registry — it is the half that can name `Seam`, mint
// against the watermark, and re-adopt across a device rebuild — and `Dispatch` depends on everything an
// author does, so the interface an author calls cannot live beside its implementation. That is not a
// workaround: an author is a *caller* of the texture space in the same way it is a caller of `Scene`,
// and what it needs to know is two verbs.
//
// **It lives in `Scene` because every scene author is handed one**, which is the correction to where it
// started. It began in `Gym` on decision 136's argument — that a gym stands where a client will stand,
// so the interface a gym calls is the one the protocol layer inherits — and that argument was right
// about the *shape* and wrong about the *home*. The second author is the client host, and it is the
// heaviest caller of these two verbs rather than an incidental one: a `wl_shm` pool is pixels arriving
// exactly the way a gym's card arrives. A host reaching into `Gym` for the interface would be the
// module that authors gyro's own scenes standing between a client and its window. So it sits beside
// `Scene/Author.h`, whose signatures name it, and both authors reach one module rather than each other.
//
// **Neither author names `Seam`, and this file is what keeps that true.** Decision 87's rule is that
// neither `Protocol` nor `Scene` may name the control waist, so a caller reaching for `TextureSource`
// or a format code would be building the shape the protocol layer is forbidden from copying. The pixels
// cross as an extent, a stride and bytes, and *the party that adopts names the format* — which is the
// same division that holds whether the bytes are a client's `wl_shm` pool or a card gyro drew for
// itself.
//
// **One id space and no second one, per Core/Texture.h.** A gym's card, a client's surface and the
// snapshot atlas are all ids from here, because Docs/Animation.md#exit-pixels has a window's live
// surface become a compositor-owned snapshot mid-transition and an id space per origin would make that
// a different node rather than the same one.
// What the byte above red means, which is the one thing an author has to say about its pixels that
// the layout alone does not.
//
// **Not a format, deliberately.** Decision 87 keeps a fourcc out of every module that authors a world,
// and this is not one: both values are the same eight-bits-a-channel little-endian word, and the only
// question is whether the top byte carries coverage or is left over. What the answer changes is real
// and visible — a window whose pixels were never given an alpha, drawn as though they had one, is a
// window you can see the desktop through in whatever pattern its toolkit happened to leave behind.
//
// The party that adopts turns this into a fourcc, which is the same division the rest of this file
// rests on.
enum class TextureAlpha : std::uint8_t
{
	// The top byte is coverage, already multiplied into the other three.
	Premultiplied,

	// The top byte means nothing and the image is fully opaque.
	None,
};

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
	// The layout is eight bits a channel in a 32-bit little-endian word, blue lowest, and `alpha` says
	// what the byte above red means — Gym/Card.h's own spelling, and a client's `argb8888` and
	// `xrgb8888` are the same two answers. `stride` is bytes per row, which is not `width * 4` the
	// moment anything is padded.
	//
	// `EINVAL` for an extent, a stride or a span that do not describe each other; whatever the importer
	// answered where it refused; `ENOSPC` where the id space or a renderer's table is full.
	[[nodiscard]] virtual Result<TextureId>
	Adopt(PixelSize<BufferSpace> size, std::uint32_t stride, std::span<const std::byte> pixels, TextureAlpha alpha) = 0;

	// Stop drawing this id. It stays valid for the frames already published that name it, and the
	// registry is what waits — Seam/Importer.h's watermark rule is one party's to honour and this is not
	// that party.
	//
	// Says nothing about an id it does not hold, which is `Forget`'s answer one layer down and for the
	// same reason: a double retire is not a condition an author can act on.
	virtual void Retire(TextureId id) noexcept = 0;
};
