#pragma once

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
// **Retirement is already solved and needs nothing added.** Docs/Open.md asked how a release can be
// safe while the frame thread may still hold the id in a list it is recording from, and the answer was
// built for something else: Publication/Return.h's watermark is the sequence the frame thread is
// rendering from, and everything strictly below it is the dispatch side's to reclaim. A texture last
// named by a snapshot below the watermark is a texture no frame can still be sampling. So `Forget` is
// an ordinary dispatch-side reclamation alongside the buffer releases already derived there, rather
// than a third channel — which decision 45 would have made a design error rather than an addition.
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
	// implementations need it for different lengths of time — a device importer duplicates the
	// descriptor during this call and owes nothing afterwards, while a CPU sampler reads the mapping on
	// every frame that names the id — so the contract is the stricter of the two, stated once, rather
	// than a rule that changes with which renderer the machine happens to be running.
	//
	// **Re-adopting a live id replaces what it names**, which is a console re-laying its grid at a new
	// address across a mode change, and it is subject to the watermark rule `Forget` carries: it stops
	// the id naming pixels the frame thread may still be sampling, so it is the same hazard under a
	// different verb. A client's next buffer is a new id rather than a replacement, because that is
	// what makes the old one still drawable while the new one imports.
	//
	// `EINVAL` for a null id, for a source that does not describe the image it claims, and for a
	// memory kind, format, or modifier this importer cannot take. `ENOMEM` where the device or the
	// table has no room. Both are the caller's to answer, and neither reaches a frame.
	[[nodiscard]] virtual Result<void> Adopt(TextureId id, const TextureSource& source) = 0;

	// Drop what an id names, and say nothing about an id this does not hold — the same answer a frame
	// gives one.
	//
	// **Safe only below Publication/Return.h's watermark**, which is the whole of this verb's
	// difficulty and is the caller's to honour rather than something the seam can check: the frame
	// thread is composing from a published sequence, and a texture that sequence names is one it may be
	// sampling right now. What makes the rule cheap is that the dispatch side already derives the
	// buffer releases and the frame callbacks from the same number, so a texture retires alongside them
	// rather than needing a mechanism of its own.
	//
	// Deliberately not a consequence of anything else. An image outlives the target set it was drawn
	// into, so a mode change that rebinds targets does not change which windows exist.
	virtual void Forget(TextureId id) noexcept = 0;
};
