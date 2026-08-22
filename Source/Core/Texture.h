#pragma once

#include <string_view>

#include "Core/Handle.h"

// The identity of an image the renderer can sample.
//
// **It is in Core because the two parties are on opposite sides of both waists.** An id is minted
// dispatch-side — Docs/Open.md's *how a texture is minted, and who holds it* has a wl_buffer become a
// device image there, because it must not happen inside the frame section — and it is consumed by
// Seam/Renderer.h's draw item, which is frame-side. Structure.md's rule is that a type a dispatch-side
// module and a frame-side module must both name lives below both waists rather than at one of them, so
// this file is what lets Protocol return an id and Scene store one without either naming Seam. It was
// declared in Seam/Renderer.h until decision 87 read the draw item's fields against who produces each
// of them, and the entry it unblocks had no legal caller for as long as it stayed there.

struct TextureTag
{
	static constexpr std::string_view Name = "Texture";
};

// A client's buffer imported onto the device, a slot in the snapshot atlas, the firmware logo the
// splash continues. One id space rather than one per origin, for `EntityId`'s reason —
// Docs/Animation.md#exit-pixels has a window's live surface become a snapshot mid-transition, and a
// source that changed identity when its pixels changed owner would make that a different node rather
// than the same one.
//
// Generational per Core/Handle.h, which is what makes the interesting failure loud: a client
// destroying a buffer while the frame thread still holds the id resolves to nothing, rather than to
// whatever image took the slot. What a renderer does with a null or stale id is draw nothing and say
// nothing — a frame is not the place to report a lifetime bug, and the frame after it would report
// the same one again.
using TextureId = Handle<TextureTag>;
