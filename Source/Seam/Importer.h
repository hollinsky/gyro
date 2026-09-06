#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

#include "Core/Result.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "Seam/RenderTarget.h"

// How a texture comes to exist: the renderer's dispatch half.
//
// Seam/Renderer.h said for a long while that this was deliberately absent, and it was right about the
// reason — a `wl_buffer` arrives on the dispatch thread and turning it into a device image must not
// happen inside the frame section — but the absence had a cost the entry did not name. `Blit` grew the
// verb privately anyway, for the firmware logo the splash continues, and a private verb on one
// renderer is a verb the composition root cannot call across a device migration. This file is that
// verb at the waist. See Docs/Decisions.md decision 131.
//
// **A separate interface rather than two more methods on `IRenderer`, because the thread is the
// difference.** `IRenderer` is frame-side: `Record` is decision 29's `C` and may not allocate, and the
// two calls that may — `BindTargets` and `ReleaseTargets` — still run on the frame thread, at a target
// invalidation. Everything here runs on the *dispatch* thread instead, while the frame thread is
// composing from the last snapshot. Hanging a dispatch verb off a frame-side interface is the shape
// decision 73 caught on `IPresenter` — the seam provisioned for one side and had no room for the other
// — and the repair there was a second path rather than a wider first one. One object implements both,
// which is `Publication`'s `Reader` and `Publisher` and `Animation`'s `Solve` and `Author`: split by
// direction, not by purity.
//
// **The importer does not mint. It is handed an id and binds pixels to it.** Core/Texture.h has the id
// space dispatch-side, and it has to stay there for a reason that outlives any one renderer: decision
// 41 destroys and rebuilds the renderer on every device migration, so an id space owned by whichever
// renderer is current would rename every image at exactly the handoff that is supposed to be
// invisible. What migration costs instead is a re-adoption of every live texture against the *same*
// ids, driven by the composition root because it is the only party that sees both renderers.
//
// **Retirement is two rules, and the entry only wrote one.** *(Corrected 2026-08-23.)* Docs/Open.md
// asked how a release can be safe while the frame thread may still hold the id in a list it is
// recording from, and Publication/Return.h's watermark answers exactly that: it is the sequence the
// frame thread is rendering from, and everything strictly below it is the dispatch side's to reclaim.
// So `Forget` is an ordinary dispatch-side reclamation alongside the buffer releases already derived
// there, rather than a third channel — which decision 45 would have made a design error rather than an
// addition.
//
// What the entry got wrong is the word *sampling*. A texture below the watermark is one the frame
// thread will not **record** again, and for a renderer that finishes on the CPU before `Record`
// returns those are the same statement — which `Blit` does, which is why one rule looked like enough
// for as long as there was one implementation. A device's are not the same statement: the frame thread
// submits a command buffer and moves on, so the watermark passes a snapshot whose composite is still
// executing on the queue. An image freed there is freed underneath a read in flight, and what it looks
// like is a window that corrupts or a device lost, on the frame after somebody closed something —
// which is the hardest moment there is to attribute it to. Frame/Loop.h already says this in the same
// breath as the post it makes: *a buffer recorded into a texture is held by GPU work rather than by
// the snapshot*.
//
// So the rule splits by who is able to know the answer. **The caller says when the scene has stopped
// naming an id; the implementation says when its own work has stopped reading one.** Only the
// watermark can carry the first and only the renderer holds the second, so `Forget` below is a promise
// to release rather than a release. This is not the refcounted table decision 131 rejected: what
// defers is the destruction of a device object, checked where the renderer is already asked whether a
// frame is done, and nothing is added to the path a fragment is sampled on.
//
// **A failed import is a surface that never reaches a frame**, which is the third thing that entry
// left open. `Adopt` reports to the dispatch thread, and the dispatch thread is what decides whether a
// snapshot names the id at all — so a refusal is answered by not publishing the surface, or by
// answering the client's commit with a protocol error, and the frame path never learns there was a
// question. The alternative was a frame that draws nothing in that rectangle, which spends a frame to
// report a condition the party that can act on it already had in hand.

// Memory a CPU reads, and the const is the whole reason this is not `MappedImage`.
//
// A target is memory the renderer *writes*; a source is somebody else's — a `wl_shm` pool the client
// is still drawing into, the decoded firmware logo, the console's own grid. Handing a writer a mutable
// pointer into a client's buffer is the kind of thing that works for years and then corrupts a
// window's own picture, so the two spellings differ by the one word that stops it.
struct MappedPixels
{
	const std::byte* Pixels = nullptr;
	std::uint32_t Stride = 0;
	std::uint32_t Reserved = 0;

	// The allocation's own length rather than `Stride * Height`, for `MappedImage`'s reason: the two
	// differ wherever the buffer is page-rounded, and a sampler that derived the bound itself would
	// derive it from the wrong number.
	std::size_t Length = 0;

	[[nodiscard]] constexpr std::span<const std::byte> Bytes() const noexcept { return { Pixels, Length }; }

	friend constexpr bool operator==(MappedPixels, MappedPixels) noexcept = default;
};

// What a texture is imported from. Seam/RenderTarget.h's shape, pointed the other way.
//
// **The same two memory kinds, and discriminated for the same reason.** A `wl_shm` buffer is a
// mapping and a `zwp_linux_dmabuf` buffer is descriptors, and both arrive from the same client on the
// same connection — so unlike a target, where the kind is a property of the backend, here it is a
// property of the individual buffer and both implementations will see both. What the variant buys is
// that an importer refuses the kind it cannot take rather than casting: `Blit` handed descriptors has
// no device to import them onto, and that is `EINVAL` here rather than a black rectangle later.
//
// **No colour state**, for the reason Seam/RenderTarget.h gives and one more. What the texels mean as
// light rides on `DrawItem::Color`, per item, because a composite mixes content that arrived in
// different states — and the tagging comes from the client's `wp_color_management_surface`, which is
// surface state rather than buffer state. A copy here would be a second place to be wrong on the frame
// after a client retags a surface without redrawing it.
struct TextureSource
{
	PixelSize<BufferSpace> Size{};
	PixelFormat Format{};
	std::variant<DmabufImage, MappedPixels> Memory{};

	[[nodiscard]] const DmabufImage* AsDmabuf() const noexcept { return std::get_if<DmabufImage>(&Memory); }

	[[nodiscard]] const MappedPixels* AsMapped() const noexcept { return std::get_if<MappedPixels>(&Memory); }

	[[nodiscard]] bool IsMapped() const noexcept { return std::holds_alternative<MappedPixels>(Memory); }

	// Describable but not importable until it has an extent, a format, and memory. Built in place by
	// the party that demarshalled the buffer, so a half-filled one is worth catching where it is
	// consumed rather than where a plane count of zero eventually samples nothing.
	[[nodiscard]] bool IsValid() const noexcept
	{
		if (Size.IsEmpty() || !Format.IsValid())
		{
			return false;
		}

		if (const DmabufImage* image = AsDmabuf())
		{
			return image->PlaneCount > 0 && image->PlaneCount <= MaxImagePlanes &&
			       image->Planes[0].Descriptor.IsValid();
		}

		if (const MappedPixels* pixels = AsMapped())
		{
			return pixels->Pixels != nullptr && pixels->Length > 0;
		}

		return false;
	}

	friend bool operator==(const TextureSource&, const TextureSource&) = default;
};

class ITextureImporter
{
public:
	ITextureImporter() = default;

	virtual ~ITextureImporter() = default;

	// Neither copied nor moved, for `IRenderer`'s reason: this is the same unit, reached from the other
	// thread. A migration replaces it by destroying and constructing, which is what obliges the
	// composition root to re-adopt rather than to hand the new one a table the old one built.
	ITextureImporter(const ITextureImporter&) = delete;
	ITextureImporter& operator=(const ITextureImporter&) = delete;
	ITextureImporter(ITextureImporter&&) = delete;
	ITextureImporter& operator=(ITextureImporter&&) = delete;

	// Make `id` name these pixels.
	//
	// **Unbounded and allocating, by contract**, and here that is not a concession the way it is for
	// `BindTargets`. This runs on the dispatch thread, which owes no deadline — decision 45 is exactly
	// that the frame thread never waits on it — so a device allocation, a format conversion, or a
	// staging copy is ordinary work rather than something to be careful about.
	//
	// **The memory stays the caller's and must stay valid until `Forget` returns.** The two
	// implementations need it for different lengths of time — a device importer consumes it during this
	// call and owes nothing afterwards, whether it duplicated a descriptor or staged a copy, while a
	// CPU sampler reads the mapping on every frame that names the id — so the contract is the stricter
	// of the two, stated once, rather than a rule that changes with which renderer the machine happens
	// to be running. It is unaffected by `Forget` deferring: what a device holds past that call is its
	// own image, never the caller's memory.
	//
	// **Re-adopting a live id replaces what it names**, which is a console re-laying its grid at a new
	// address across a mode change, and it carries both halves of the rule `Forget` does: the caller
	// waits for the watermark because it stops the id naming pixels a frame may still be recording, and
	// the implementation retires the image it replaced the way it retires a forgotten one. A client's
	// next buffer is a new id rather than a replacement, because that is what makes the old one still
	// drawable while the new one imports.
	//
	// `EINVAL` for a null id, for a source that does not describe the image it claims, and for a
	// memory kind, format, or modifier this importer cannot take. `ENOMEM` where the device or the
	// table has no room. Both are the caller's to answer, and neither reaches a frame.
	[[nodiscard]] virtual Result<void> Adopt(TextureId id, const TextureSource& source) = 0;

	// Make `id` name storage this allocates, with nothing in it yet.
	//
	// **The compositor's own image rather than somebody else's memory, which is why it is a verb and
	// not a third alternative in `TextureSource`.** Every case that variant carries is memory a caller
	// already has and the importer binds to — a client's descriptors, a mapping the compositor
	// uploaded — and the contract on all of it is that the memory stays the caller's. Here there is no
	// caller's memory at all: the pixels do not exist until something draws them, and the party that
	// allocates is the party that will be drawn into.
	//
	// **What wants this is Docs/Decisions.md decision 46's exit atlas.** A window that closes is drawn
	// from a copy of its last frame (decision 20), the copies live in one image per output reserved
	// when that output is configured, and *never* in an image created while a window is closing —
	// decision 36 forbids a `vkCreateImage` on the frame path, and a menu dismissing is exactly the
	// moment a person would see the stall it would cost. So the allocation happens here, on the
	// dispatch thread, at the same moment the output set changes.
	//
	// **The storage must be both drawable into and samplable**, because a snapshot is written by the
	// composite and read by the frames after it. An importer that can offer only one of those refuses.
	//
	// **Contents are undefined until something writes them, and are lost across a device rebuild.**
	// Decision 41 destroys and recreates the renderer on every boot and decision 46 answers that by
	// dropping the retiring set rather than preserving it — hard-settling the springs is entirely
	// CPU-side and therefore survives a GPU that is gone. So a re-adoption of a reserved id is a fresh
	// empty image, not a copy of what it held.
	//
	// **Refused by default, for the descriptor overload's reason one file over.** A texture space with
	// nothing to draw into is an ordinary one rather than an incomplete one, and what a refusal costs
	// is that closing windows cut instead of fading — which is decision 46's own answer to having no
	// room, arriving one step earlier.
	//
	// `EINVAL` for a null id or an extent that is not an image. `ENOMEM` where the device or the table
	// has no room.
	[[nodiscard]] virtual Result<void> Reserve(TextureId id, PixelSize<BufferSpace> size)
	{
		static_cast<void>(id);
		static_cast<void>(size);

		return Failure(ENOTSUP, "this importer has nothing to allocate an image out of");
	}

	// Give up an id, and say nothing about one this does not hold — the same answer a frame gives one.
	//
	// **Called below Publication/Return.h's watermark**, which is the caller's half of the rule and is
	// not something the seam can check: the frame thread composes from a published sequence, and a
	// texture that sequence still names is one it may be recording right now. What makes that half
	// cheap is that the dispatch side already derives the buffer releases and the frame callbacks from
	// the same number, so a texture retires alongside them rather than needing a mechanism of its own.
	//
	// **It returns having promised to release rather than having released**, which is the
	// implementation's half *(2026-08-23)*. The watermark says the scene has stopped naming the id; it
	// does not say the renderer has stopped reading it, because a device composite outlives the frame
	// thread's `Record` by however long its queue takes. So an importer that submits work holds the
	// image until its own completion says otherwise, and reclaims it at a frame boundary rather than
	// inside this call — which must not block, since the dispatch thread has a scene to serialise and
	// waiting here would pace authoring to the GPU. What the caller gets either way is that the id is
	// dead from this call onward.
	//
	// Deliberately not a consequence of anything else. An image outlives the target set it was drawn
	// into, so a mode change that rebinds targets does not change which windows exist.
	virtual void Forget(TextureId id) noexcept = 0;
};
