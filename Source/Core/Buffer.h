#pragma once

#include <string_view>

#include "Core/Handle.h"

// The identity of a client buffer the compositor is holding.
//
// **It is here rather than in `Publication` for Core/Texture.h's reason, and it took a minter to
// notice.** *(Moved 2026-08-23.)* Publication/Return.h declared it, with the argument that *the waist
// is where the schema both halves bind to belongs* — which is right about the schema and wrong about
// the type. Decision 87's rule is that a type a dispatch-side module and a frame-side module must both
// name lives below *both* waists, and this is the same shape `TextureId` was in when it lived in
// `Seam`: `Protocol` mints one when it imports a `wl_buffer` and consumes one when the release comes
// back, and Docs/Structure.md gives `Protocol` `Core`, `Geometry` and `Scene` and deliberately not the
// publication waist. Declared where it was, the module that mints the id could not say its name.
//
// Nothing caught it for as long as nothing minted one, which is the general shape of the miss rather
// than an accident: a type with no producer has no call sites to fail a layering check.
//
// **The record it rides in stays put.** `FrameReport` is the schema the two halves bind to and belongs
// at the waist; what moved is the identity, exactly as `TextureId` moved out of `Seam` while
// `TextureSource` stayed.

struct BufferTag
{
	static constexpr std::string_view Name = "Buffer";
};

// A client buffer, named so that a release cannot free the wrong one.
//
// The identity is the dispatch side's — it mints one when it imports a client buffer and consumes one
// when the hold comes back — and the frame thread only carries it across. Generational per
// Core/Handle.h because Docs/Architecture.md admits cross-thread lifetime as the one genuinely new
// cost of the split: a client may destroy a surface while the frame thread holds its buffer, and a
// stale release must compare unequal rather than name whatever occupies the slot now.
//
// Distinct from `TextureId` and not a spelling of it. A texture is the device image a renderer
// samples; a buffer is the client memory behind it, and the two have different lifetimes on purpose —
// Seam/Importer.h's importer *borrows* the memory, so the id that says *the compositor is done with
// these bytes* cannot be the id that says *this image is no longer named by a scene*.
using BufferId = Handle<BufferTag>;
