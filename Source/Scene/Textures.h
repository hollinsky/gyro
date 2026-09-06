#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>

#include "Core/Fd.h"
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

// A pixel layout named the way whoever allocated it named it: a DRM fourcc and a DRM modifier.
//
// **This is the one place decision 87's rule is broken, and it is broken because the client owns the
// word.** Everywhere else an author says what its bytes mean in prose — `TextureAlpha` above is the
// whole of it — and the party that can name `Seam` turns that into a format. That division holds
// exactly as long as the author *chose* the layout, which is true of a gym's card and of the console's
// grid and is not true here: `zwp_linux_buffer_params_v1.create` carries a fourcc and a modifier as
// wire arguments, chosen by the client's own allocator, and there is no prose an author could
// translate them into that does not throw away the thing that makes them work. A modifier is an opaque
// token whose entire purpose is that nobody in the middle interprets it.
//
// So the author relays rather than decides, and the same two numbers come *back* through `Formats`
// below so it has something honest to advertise. What decision 87 was protecting against — a module
// that authors a world reasoning about pixel layouts — is still true of every other verb here.
struct TextureFormat
{
	std::uint32_t Code = 0;

	// DRM's own token. Not defaulted to zero-meaning-linear: zero *is* linear, and there is no value
	// that means *unset*, so a half-filled one is a lie rather than a placeholder. The party that fills
	// this always has a client's number to put in it.
	std::uint64_t Modifier = 0;

	friend constexpr bool operator==(TextureFormat, TextureFormat) noexcept = default;
};

// One plane of a buffer somebody else allocated, in the layout they laid it out in.
//
// **Borrowed for the duration of the call, like the pixels above.** The descriptor is duplicated by
// whoever takes it if it needs to outlive the call, which it does — a dmabuf is not copied, so the
// memory has to stay reachable for as long as an id names it. What that dup costs is a file
// descriptor per plane per live buffer, which is the price of not copying a window every frame.
struct TexturePlane
{
	RawFd Descriptor{};
	std::uint32_t Offset = 0;
	std::uint32_t Stride = 0;
};

// SPEC: the most planes one buffer may have. Four is what every DRM format there is uses at most, and
// it is `Seam/RenderTarget.h`'s `MaxImagePlanes` said again on the side of the waist that may not name
// it — the two are checked against each other where they meet.
inline constexpr std::size_t MaxTexturePlanes = 4;

// Who is told when nobody is reading a texture's memory any more.
//
// **This exists because a dmabuf is not copied, and that is the whole difference.** A `wl_shm` buffer
// is copied at `Adopt` and handed straight back, so the client may draw into it again immediately;
// descriptors are *borrowed*, so a client told the same thing would be drawing into the buffer the
// panel is scanning out — which reaches a person as a window tearing into itself, on the frame after
// it started animating, and reaches a log as nothing at all.
//
// So the release is owed, and the number that says when is the watermark: the frame thread has moved
// past every snapshot that could name the id. That is the same number the registry already retires
// against, which is what makes this an observer on an existing step rather than a mechanism of its
// own — decision 45's rule that a third channel would be a design error.
//
// A caller that goes away first says so with `Abandon`, because the registry holds a bare pointer and
// a client destroying its `wl_buffer` is ordinary rather than exceptional.
class ITextureRelease
{
public:
	ITextureRelease() = default;

	virtual ~ITextureRelease() = default;

	ITextureRelease(const ITextureRelease&) = delete;
	ITextureRelease& operator=(const ITextureRelease&) = delete;
	ITextureRelease(ITextureRelease&&) = delete;
	ITextureRelease& operator=(ITextureRelease&&) = delete;

	// One id that named this memory has been reclaimed. Called once per successful `Adopt`, on the
	// dispatch thread, and never after `Abandon`.
	virtual void OnTextureReleased() noexcept = 0;
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

	// Give me an id naming storage of this size that gyro owns and nothing has drawn into yet.
	//
	// **The one verb here that takes no pixels, because the pixels are gyro's own to produce.** The
	// snapshot atlas is the caller — a window that closes is drawn from a copy of its last frame, and
	// Docs/Decisions.md decision 46 keeps those copies in one image per output allocated when that
	// output is configured, because an image created at the moment a window closes is a stall on
	// exactly the frame a person is watching something go away. Everything else here hands over pixels
	// that already exist; this asks for room.
	//
	// **One id space and no second one**, per the header above and Core/Texture.h: a closing window's
	// node goes on naming an id, and the id changing from the client's surface to a rectangle of the
	// atlas is what makes an exit the same node rather than a different one.
	//
	// **Contents are undefined until something draws into them**, and they do not survive a device
	// being replaced — decision 41 rebuilds the renderer on every boot and decision 46 drops the
	// retiring set rather than preserving it, so an author gets an empty image back and no promise
	// about what was in it.
	//
	// `EINVAL` for an extent that is not an image; `ENOSPC` where the id space is full; whatever the
	// importer answered where it refused.
	//
	// **Answered by refusing where nothing implements it**, on the descriptor overload's terms: a
	// texture space with nothing to draw into is an ordinary one, and what a refusal costs is that
	// closing windows cut rather than fade — decision 46's own answer to running out of room, one step
	// earlier.
	[[nodiscard]] virtual Result<TextureId> Reserve(PixelSize<BufferSpace> size)
	{
		static_cast<void>(size);

		return Failure(ENOTSUP, "this texture space has no storage of its own to give");
	}

	// Stop drawing this id. It stays valid for the frames already published that name it, and the
	// registry is what waits — Seam/Importer.h's watermark rule is one party's to honour and this is not
	// that party.
	//
	// Says nothing about an id it does not hold, which is `Forget`'s answer one layer down and for the
	// same reason: a double retire is not a condition an author can act on.
	virtual void Retire(TextureId id) noexcept = 0;

	// Give these descriptors an id.
	//
	// **The descriptors are borrowed and the memory is not**, which is the inverse of the overload
	// above and is the reason the two are separate verbs rather than one with a variant in it. Nothing
	// is copied: the implementation duplicates each plane's descriptor, imports the buffer onto every
	// device that will take it, and holds the duplicates until the id is reclaimed — at which point
	// `release` is told, if there is one. The caller's own descriptors are its own again the moment
	// this returns, whether it succeeded or not.
	//
	// **`release` is optional and its absence is not a shortcut.** An author that allocated the buffer
	// itself owes nobody a release; an author relaying a client's buffer owes exactly one, and passing
	// null there would be the tearing bug `ITextureRelease` describes. It must outlive the id or say
	// otherwise through `Abandon`.
	//
	// `EINVAL` for an extent, a plane count or a descriptor that does not describe an image; whatever
	// the importer answered where it refused the format or the modifier; `ENOSPC` where the id space
	// is full.
	//
	// **Answered by refusing where nothing implements it**, unlike the overload above, and the asymmetry
	// is the point: every texture space there is can take bytes, because bytes are what a CPU renderer
	// samples and what the console draws with. Descriptors need a device, and a texture space with none
	// is an ordinary one rather than an incomplete one — so the default is the honest answer rather
	// than a compiler error every stub would discharge by writing the same refusal.
	[[nodiscard]] virtual Result<TextureId> Adopt(
		PixelSize<BufferSpace> size,
		TextureFormat format,
		std::span<const TexturePlane> planes,
		ITextureRelease* release
	)
	{
		static_cast<void>(size);
		static_cast<void>(format);
		static_cast<void>(planes);
		static_cast<void>(release);

		return Failure(ENOTSUP, "this texture space takes no descriptors");
	}

	// Stop calling this. Every id it was registered against stays live and retires normally; what is
	// dropped is only the notification, because the party that wanted it is going away.
	//
	// Says nothing about one it does not hold, for `Retire`'s reason.
	virtual void Abandon(const ITextureRelease& release) noexcept { static_cast<void>(release); }

	// The layouts descriptors may be handed over in, in no particular order.
	//
	// **What an author advertises rather than what a device can do**, and the two differ by one word:
	// this is the intersection over every importer, because an image a scene can draw anywhere has to
	// exist everywhere it might be drawn. A format one panel's device accepts and another's does not is
	// a window that appears on one monitor and is missing from the other, which is worse than a client
	// falling back to a format both take.
	//
	// Empty is a legitimate answer and means *hand over no descriptors*: a machine with no GPU has a
	// CPU renderer that has no device to put a descriptor on, and an author that advertises nothing
	// there is one whose clients draw into shared memory and still get a window.
	[[nodiscard]] virtual std::span<const TextureFormat> Formats() const noexcept { return {}; }

	// The device a client should allocate those layouts against, as the `dev_t` of a DRM node, or zero
	// where there is none.
	//
	// **A number the author relays rather than a device it can name**, which is decision 154's carve-out
	// in decision 87 used a second time and for the same reason: the pair list crosses this interface
	// because the *client* chose it, and the device number crosses because the client is the party that
	// has to open it. Neither is a thing a scene reasons about, and a `dev_t` is as opaque here as a
	// modifier is — it exists to be handed back to `drmGetDeviceFromDevId` on the far side.
	//
	// **Without it the pair list is unusable, which is what made this the difference between a window
	// that runs on the GPU and one that does not.** Mesa has no other channel left: `wl_drm` is gone
	// from its Wayland platform, so a compositor that lists formats and names no device leaves every
	// GL and Vulkan client on llvmpipe. See [Dmabuf.h](../Protocol/Dmabuf.h).
	//
	// Zero is the honest answer on a machine with no GPU, and it travels with an empty `Formats` —
	// together they say *hand over no descriptors*, and a client draws into shared memory.
	[[nodiscard]] virtual std::uint64_t MainDevice() const noexcept { return 0; }
};
