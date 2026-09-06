#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "Core/Result.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "Render/Device.h"
#include "Render/Vulkan.h"
#include "Seam/Importer.h"

// Seam/Importer.h's dispatch half on a Vulkan device: a client's buffer becoming something a
// fragment can sample.
//
// **It belongs to the device and not to a renderer, which is the one structural choice in this
// file.** `Blit` implements `IRenderer` and `ITextureImporter` on one object, and the seam describes
// that as the shape; it is a description of the CPU renderer rather than a rule. A `VulkanRenderer`
// is per output — the composition root builds one for each — so an importer hung off one would give
// a two-monitor machine two of everything a window is made of: two `VkImage`s over one dmabuf, two
// descriptor sets, and, for a `wl_shm` client, two device-side copies of the same pixels. What
// decision 131 actually rests on is that the *id space* survives a device rebuild, and this holds
// exactly as long as the device does — so a migration re-adopts against the same ids either way, and
// the only thing per-renderer ownership would buy is the duplication.
//
// **What that costs is the retirement, and it is why `ITextureFence` exists.** One importer with N
// renderers cannot ask "has my work finished" of itself; it has to ask every renderer that could have
// recorded the image. The alternative was a device-wide timeline that every renderer signals, which
// is a larger change to decision 108's `SyncPoint` export than the problem justifies — and the
// renderers are few, small in number, and already know the answer.
//
// **Mapped buffers need `VK_EXT_host_image_copy` and are refused without it**, which is a real
// limitation stated rather than worked around. Filling a device image the ordinary way is a staging
// buffer and `vkCmdCopyBufferToImage`, which is a queue submission — and `Adopt` runs on the dispatch
// thread while the `SCHED_FIFO` frame thread is submitting composites to the only queue gyro creates.
// Sharing it behind a lock is a priority inversion that lands as a dropped frame every time a client
// posts a software buffer; a second queue is a second submission order, which Render/Device.cpp's
// queue selection declines for a reason unrelated to this and still good. Host image copy removes the
// question instead of answering it: the driver writes tiled pixels on the calling thread, with no
// queue in the picture at all.
//
// The two fallbacks were both looked at and both are worse than the refusal. A linear host-visible
// image samples on every frame from memory the GPU reads across the bus, on exactly the older
// hardware least able to absorb it — and it still needs one queue-side layout transition, so it does
// not even buy the property it was reached for. A transition performed by the frame thread at first
// use makes "first" a thing N renderers have to agree about, ordered across submissions that are not
// ordered. Both are more machinery than `wl_shm` on a pre-2023 driver is worth while there is no
// protocol layer to feed either. Every current Mesa driver and both proprietary ones carry the
// extension; where it is missing, the report reaches the dispatch thread, which is the party
// Seam/Importer.h says can act on it.
//
// **Dmabuf sources need none of that**, because nothing has to be written: the descriptor becomes a
// `VkImage` under its explicit modifier, and the only queue-side work is the ownership acquire from
// `VK_QUEUE_FAMILY_FOREIGN_EXT` that the frame thread already emits for every render target it
// touches. It emits one per sampled texture per frame for the same reason it does for targets — it
// is idempotent, it needs no first-time bookkeeping, and the alternative is state shared between the
// two threads to save a barrier that costs nothing.

// SPEC: how many images one device holds at once.
//
// It bounds the descriptor pool rather than estimating a working set: a set is thirty-two bytes of
// driver-side allocation, so a thousand of them is nothing against a machine that can hold a
// thousand windows, and a refusal here is a client's commit answered with an error rather than a
// picture that silently stops updating. Fixed because `Adopt` may not grow a pool a frame is
// currently allocating from.
inline constexpr std::uint32_t MaxTextureImages = 1024;

// The format an image gyro draws into is reserved in.
//
// **Named here rather than chosen at the allocation, because a renderer has to build its pipelines
// against it before any frame draws one.** Dynamic rendering bakes the attachment's format into the
// pipeline, so a composite that discovered this format when it first went to fill a snapshot would be
// compiling inside a frame — a hundred milliseconds on the thread that owes a picture every refresh,
// landing on the first frame anybody ever closed a window on. `VulkanRenderer::BindTargets` prepares
// this binding alongside the target's, where building one is legal.
//
// **Sixteen-bit float, and it is the narrower candidates that argue for it.** What is kept here is a
// window's last frame, held for the length of a fade and drawn over whatever is behind it:
//
//   - **Eight bits per channel bands on a ten-bit screen.** The live window was composited at the
//     panel's depth, so a snapshot quantised below it is a window that visibly steps as it leaves —
//     the gradient a person was looking at a moment ago, in bands, and only while it is going away.
//   - **`A2R10G10B10` fixes the banding and breaks the corners.** Two bits of alpha is four levels of
//     coverage, which is a rounded corner turning to stairs and a translucent terminal collapsing to
//     opaque for the whole of its exit. There is no packed thirty-two-bit format that is wide in
//     colour and wide in alpha at once.
//
// So it is eight bytes per pixel rather than four, which is the one real cost: an atlas is twice the
// memory it would be, and `AtlasRenderTargetMultiple` denominates capacity in render targets rather
// than bytes, so the number of windows that fit is unchanged and it is the footprint that moves.
// `Render/Unfused.h` already reaches for this format for the same reason and states the same trade.
inline constexpr VkFormat StorageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// SPEC: how many distinct images one output may sample in one frame.
//
// It bounds the barrier batch a composite opens with, and it is a refusal rather than a resize
// because decision 36 forbids growing anything inside the frame section. A hundred and twenty-eight
// distinct client surfaces visible on one panel in one frame is a desktop nobody has — a busy tiling
// session is tens — and a scene past it is refused at the pre-flight walk with the rest, where
// nothing has been recorded yet.
inline constexpr std::uint32_t MaxSampledImages = 128;

// SPEC: how many renderers may register for retirement, and it is an output count wearing a
// different name. Eight is well past the panels one machine drives and costs one pointer each.
inline constexpr std::uint32_t MaxTextureFences = 8;

// What the importer needs of a renderer in order to know when the device has finished reading an
// image.
//
// **It is two questions rather than one because they are asked from different threads.** `Submitted`
// is read on the dispatch thread at the moment a texture is given up, to stamp it with the work that
// might still name it; `Reached` is read later, on the same thread, to find out whether that work has
// landed. Neither is on the frame path — the frame thread's only involvement is the store inside its
// own submit, which it was making anyway.
//
// A separate interface rather than a `VulkanRenderer*` because the dependency would otherwise run
// both ways: a renderer names this file to sample, and this file would name the renderer to retire.
class ITextureFence
{
public:
	ITextureFence() = default;

	virtual ~ITextureFence() = default;

	ITextureFence(const ITextureFence&) = delete;
	ITextureFence& operator=(const ITextureFence&) = delete;
	ITextureFence(ITextureFence&&) = delete;
	ITextureFence& operator=(ITextureFence&&) = delete;

	// The highest timeline value this renderer has ever submitted. Written by the frame thread at a
	// submission and read here, so the implementation carries whatever ordering that needs.
	[[nodiscard]] virtual std::uint64_t Submitted() const noexcept = 0;

	// Whether the device has finished the work that value names. False where the renderer cannot
	// know — which is answered as *not yet*, since a retirement that waits one iteration too long
	// costs an image and one that runs one too early costs the picture.
	[[nodiscard]] virtual bool Reached(std::uint64_t value) const noexcept = 0;
};

// What a frame needs of an adopted texture, which is a descriptor set and, for an imported dmabuf,
// the image the acquire barrier names.
//
// Returned by value rather than as a pointer into the table, so that nothing the frame thread holds
// outlives the lookup that produced it.
struct BoundTexture
{
	VkDescriptorSet Set = VK_NULL_HANDLE;

	// Null for a texture this device wrote itself, which needs no ownership transfer because it never
	// left. Set for an imported dmabuf, whose pixels belong to a client and whose queue-family
	// ownership has to come back before a shader reads it.
	VkImage Foreign = VK_NULL_HANDLE;

	PixelSize<BufferSpace> Size{};

	[[nodiscard]] bool IsValid() const noexcept { return Set != VK_NULL_HANDLE; }
};

// What Seam/Capture.h's texture read needs of an adopted image, which is everything the copy has to
// state and nothing the frame path does.
//
// Separate from `BoundTexture` rather than three more fields on it: that one is returned per drawn
// item on every frame of every session, and this one is asked for on the frames a person presses a
// key on.
struct ReadableTexture
{
	VkImage Handle = VK_NULL_HANDLE;
	PixelSize<BufferSpace> Size{};

	// The client's own fourcc and modifier, as adopted. It is what says whether the top byte of the
	// rows that come back is coverage or padding, and gyro never chose it.
	PixelFormat Format{};

	[[nodiscard]] bool IsValid() const noexcept { return Handle != VK_NULL_HANDLE; }
};

class VulkanTextures final : public ITextureImporter
{
public:
	// **Construction cannot fail, and `Status()` is where the reason lives** — `VulkanRenderer`'s
	// shape, for its reason and one more. A machine whose driver refused a descriptor pool should come
	// up with a compositor that draws solids rather than abort before anything reaches the screen; and
	// a table that had to be told to build itself is one a composition root can forget to tell, which
	// is a null set layout reaching a pipeline creation as an error about the wrong thing.
	explicit VulkanTextures(VulkanDevice& device);

	~VulkanTextures() override;

	VulkanTextures(const VulkanTextures&) = delete;
	VulkanTextures& operator=(const VulkanTextures&) = delete;
	VulkanTextures(VulkanTextures&&) = delete;
	VulkanTextures& operator=(VulkanTextures&&) = delete;

	// Why this table cannot hold anything, where it cannot. Success once construction completed.
	[[nodiscard]] const Result<void>& Status() const noexcept { return m_Status; }

	// The layout every textured pipeline is built against. Null where `Status` failed, which is what
	// makes a renderer over a broken table fail at its own construction rather than at a draw.
	[[nodiscard]] VkDescriptorSetLayout SetLayout() const noexcept { return m_SetLayout; }

	// Seam/Importer.h's two verbs, on the dispatch thread.
	//
	// `EINVAL` for a null id, for a source that does not describe the image it claims, and for a
	// format this device will not sample. `ENOTSUP` for a mapped source on a device with no host
	// image copy, which is the one refusal this file adds to the seam's list and is named in the
	// message. `ENOMEM` where the device or the table has no room.
	[[nodiscard]] Result<void> Adopt(TextureId id, const TextureSource& source) override;

	// Decision 46's exit atlas, allocated here because it may not be allocated on the frame path.
	//
	// `EINVAL` for a null id or an extent that is not an image; `ENOMEM` where no device-local memory
	// type takes a colour attachment, or where the table has no room.
	[[nodiscard]] Result<void> Reserve(TextureId id, PixelSize<BufferSpace> size) override;

	void Forget(TextureId id) noexcept override;

	// A renderer that might be recording an image when it is given up. Registered by the composition
	// root, which is the only party that sees both. Registering none is legal and means nothing is
	// deferred — which is correct for a table nobody draws from, and wrong for one somebody does, so
	// it is the root's job the way every other wiring here is.
	void Attach(ITextureFence& fence) noexcept;

	// Stop asking this renderer. Everything currently waiting on it is swept first, with a device
	// wait, because a renderer being torn down is exactly the moment its timeline stops answering.
	void Detach(ITextureFence& fence) noexcept;

	// What an id names, on the frame thread.
	//
	// **Total, allocating nothing, and lock-free by the watermark rather than by an atomic.** The
	// dispatch thread only ever writes a slot no published snapshot names — `Adopt` takes a free one,
	// `Forget` clears one the watermark has released — and the frame thread only ever reads slots the
	// snapshot it is composing from does name. The two sets are disjoint by Publication/Return.h's
	// rule, which is the same argument `Blit`'s table already runs on and the reason decision 131
	// could reject refcounting.
	//
	// A stale or unknown id comes back invalid, and Core/Texture.h says what a renderer does with
	// that: draw nothing and say nothing.
	[[nodiscard]] BoundTexture Find(TextureId id) const noexcept;

	// What an id names for a readback, on the frame thread and only on a captured frame.
	//
	// Invalid for an id nothing names and for an image this device wrote itself: a `wl_shm` client's
	// pixels are captured at the commit that copied them, where the damage rectangles still exist, and
	// reading the device's copy back would put the same window on disk twice under two names.
	[[nodiscard]] ReadableTexture Readable(TextureId id) const noexcept;

	// How many ids currently name an image, and how many images are waiting on a renderer to finish
	// with them. Both for a test, which is the only party that can meaningfully assert on either.
	[[nodiscard]] std::uint32_t Held() const noexcept { return m_Count; }

	[[nodiscard]] std::uint32_t Retiring() const noexcept { return m_DoomedCount; }

private:
	// One live texture. The id is kept whole rather than reduced to an index, which is
	// Core/Texture.h's *a stale id draws nothing* in one comparison: a generation that has moved on
	// compares unequal and the lookup misses, where an index alone would resolve to whatever took the
	// slot.
	struct Image
	{
		TextureId Id;
		VkImage Handle = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		VkImageView View = VK_NULL_HANDLE;
		VkDescriptorSet Set = VK_NULL_HANDLE;
		PixelSize<BufferSpace> Size{};

		// As adopted, kept for `Readable` above — the view has already consumed the fourcc and neither
		// it nor the descriptor set can be asked what it was built from.
		PixelFormat Format{};

		// Whether the pixels are a client's, reached through a descriptor this device did not
		// allocate — which is the whole of what the frame thread has to do differently.
		bool Imported = false;
	};

	// An image no id names any more, and the work that may still be reading it.
	//
	// **One value per registered renderer rather than one number**, because the renderers are on
	// separate timelines: a value of 4000 means something different on each, and a single high-water
	// mark would compare one renderer's clock against another's.
	struct Doomed
	{
		VkImage Handle = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		VkImageView View = VK_NULL_HANDLE;
		VkDescriptorSet Set = VK_NULL_HANDLE;

		std::array<std::uint64_t, MaxTextureFences> At{};
		std::uint32_t Count = 0;
	};

	[[nodiscard]] Result<void> AdoptMapped(Image& into, const TextureSource& source);

	[[nodiscard]] Result<void> AdoptDmabuf(Image& into, const TextureSource& source);

	// An empty image this device both draws into and samples from. The half of `AdoptMapped` that
	// creates and binds, with no host copy after it because there is nothing yet to copy.
	[[nodiscard]] Result<void> ReserveStorage(Image& into);

	// The view and the descriptor set over an image both arms have already created and filled.
	[[nodiscard]] Result<void> Describe(Image& into, VkFormat format);

	// Move an image onto the doomed list, stamped with every renderer's current submission. Total:
	// where there is no room to defer, the only remaining answers are to leak or to free something a
	// queue is reading, and a wait is the one that is merely slow.
	void Doom(Image& image) noexcept;

	// Destroy everything the device has finished with. Called at the top of both dispatch-side verbs,
	// which is what makes retirement cost nothing on a system where no texture is ever given up —
	// there is nothing on the list to walk.
	void Sweep() noexcept;

	void Release(Doomed& doomed) noexcept;

	[[nodiscard]] Image* Lookup(TextureId id) noexcept;

	// The sampler, the set layout and the pool — everything that does not depend on any one image.
	[[nodiscard]] Result<void> Build();

	VulkanDevice* m_Device = nullptr;

	VkSampler m_Sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_SetLayout = VK_NULL_HANDLE;
	VkDescriptorPool m_Pool = VK_NULL_HANDLE;

	// Dense: `m_Count` entries, order not meaningful, a removal filling the hole from the end. A
	// linear scan over live textures once per drawn item is what a lookup costs, which is the same
	// trade `Blit` makes over eight and is still cheaper than a hash at these sizes.
	std::array<Image, MaxTextureImages> m_Images{};
	std::uint32_t m_Count = 0;

	std::array<ITextureFence*, MaxTextureFences> m_Fences{};
	std::uint32_t m_FenceCount = 0;

	std::array<Doomed, MaxTextureImages> m_Doomed{};
	std::uint32_t m_DoomedCount = 0;

	Result<void> m_Status{};
};
