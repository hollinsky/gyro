#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/SlotAllocator.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "Scene/Textures.h"
#include "Seam/Allocator.h"
#include "Seam/Importer.h"
#include "Seam/RenderTarget.h"
#include "Seam/Scanout.h"

// Who mints a texture id and who holds what it names.
//
// Docs/Open.md's *how a texture is minted, and who holds it* left two things after
// [decision
// 131](../../Docs/Decisions.md#131-texture-import-is-a-second-interface-and-a-texture-retires-on-the-watermark)
// answered the shape: the Vulkan arm, and the minter. This is the minter, and it is four
// responsibilities that only look separable until one of them is put somewhere else.
//
// **It mints, because the id space cannot belong to a renderer.** Decision 41 destroys and rebuilds
// the renderer on device migration, so ids owned by whichever renderer is current would be renamed at
// exactly the handoff that is supposed to be invisible. Core/Texture.h says the space is dispatch-side;
// this is the object that says so out loud.
//
// **It holds the pixels, because Seam/Importer.h borrows them.** `Blit` records a pointer and samples
// it on every frame that names the id — for the console's grid that is thirty megabytes it would
// otherwise copy per line — so somebody has to keep the memory alive for as long as the id is live and
// then some. The author is the wrong somebody: that would put the watermark rule in every scene that
// ever draws a picture. What it costs is one copy on the dispatch thread, which owes no deadline.
// *(A client's `wl_shm` pool is that same copy, and it buys something extra there: gyro stops needing
// the client's memory the moment `Adopt` returns, so `wl_buffer.release` goes back in the same step
// and a toolkit drawing into one buffer never waits for a second.)*
//
// **It retires on the watermark, which is Publication/Publisher/Outbox.h's own rule with a different
// payload.** `Reclaim` there returns snapshot buffers whose sequence is strictly below the watermark;
// this returns textures the same way, in the same step, for the same reason — the frame thread is
// composing from a published sequence, and a texture that sequence names is one it may be sampling
// right now. What makes the rule cheap is that it was already being applied to something else.
//
// **It adopts into every renderer rather than one.** A renderer is per output (`BoundOutput` in the
// composition root says why), so a texture a scene can draw anywhere has to exist everywhere it might
// be drawn. Two panels on one GPU is two `Blit`s today and genuinely two imports the day the second
// panel is on the second card, so the loop is the honest arrangement rather than a temporary one.
//
// **A gym is not what any of that is for.** The first caller happens to be one; the second is the boot
// splash, and the third is `Protocol` — where the author is a client and the only thing that changes
// is where the bytes came from.

class TextureRegistry final : public ITextures
{
public:
	// SPEC: how many images one system may hold at once. It bounds the index space rather than
	// estimating a working set, per `MaxEntities`' reason — and it is well above what a renderer will
	// take, since `Blit::MaxImages` is eight and refuses beyond it. A refusal here is the caller's to
	// answer for.
	static constexpr std::uint32_t MaxTextures = 4096;

	// The renderers to import into. Borrowed and outlived by nothing here — the composition root owns
	// both the renderers and this, and `Rebind` is how a set that changed arrives.
	// The renderers to import into, the layouts an author may advertise, and the scanout importer where
	// there is one. Borrowed and outlived by nothing here — the composition root owns all four, and
	// `Rebind` is how a set that changed arrives.
	//
	// **The scanout importer is separate from the list and refusals from it are not failures**, which
	// is decision 153's whole shape said in one signature. A renderer that will not take a buffer is a
	// window that cannot be drawn; a *display engine* that will not take one is a window that is drawn
	// by the GPU instead, which is what every window already is. So it is offered to and its answer is
	// discarded, and what promotion costs when it is refused is a composite gyro was doing anyway.
	//
	// **The allocator is where gyro's own pixels come from, and it is optional for the same reason the
	// scanout importer is.** An author hands over bytes; whether those bytes end up in a heap vector or
	// in an allocation a display engine can read is not the author's question and not a second verb —
	// see `Promote`. Where there is none, or where it refuses, the bytes stay in gyro's memory and the
	// image is composited, which is what every one of them did before.
	explicit TextureRegistry(
		std::span<ITextureImporter* const> importers,
		std::span<const TextureFormat> formats = {},
		IScanoutImporter* scanout = nullptr,
		IDmabufAllocator* allocator = nullptr
	)
		: m_Importers{ importers.begin(), importers.end() }, m_Formats{ formats.begin(), formats.end() },
		  m_Scanout{ scanout }, m_Allocator{ allocator }, m_Ids{ MaxTextures }
	{}

	TextureRegistry(const TextureRegistry&) = delete;
	TextureRegistry& operator=(const TextureRegistry&) = delete;
	TextureRegistry(TextureRegistry&&) = delete;
	TextureRegistry& operator=(TextureRegistry&&) = delete;

	using ITextures::Adopt;

	[[nodiscard]] Result<TextureId> Adopt(
		PixelSize<BufferSpace> size,
		std::uint32_t stride,
		std::span<const std::byte> pixels,
		TextureAlpha alpha
	) override
	{
		if (const Result<void> described = Describes(size, stride, pixels); !described)
		{
			return std::unexpected{ described.error() };
		}

		if (m_Importers.empty())
		{
			return Failure(ENODEV, "no renderer to import an image into");
		}

		// **Written into an allocation a display engine can read, where the machine has one.** This is the
		// whole of what makes a pointer, a splash or a line of console text promotable: a client's window
		// arrives as a descriptor and is scannable, and everything gyro draws for itself arrived as heap
		// memory and structurally was not — so a cursor over a fullscreen window forced the window off its
		// plane too, the promoted set being a suffix. Nothing above here knows it happened.
		if (Result<TextureId> promoted = Promote(size, stride, pixels, alpha); promoted)
		{
			return promoted;
		}

		const std::optional<TextureId> id = m_Ids.Allocate();

		if (!id)
		{
			return Failure(ENOSPC, "the texture id space is exhausted");
		}

		if (m_Held.size() <= m_Ids.SlotCount())
		{
			m_Held.resize(m_Ids.SlotCount() + 1);
		}

		Held& held = m_Held[id->Index];

		held = Held{};

		held.Id = *id;
		held.Pixels = std::vector<std::byte>{ pixels.begin(), pixels.end() };
		held.Size = size;
		held.Stride = stride;
		held.Alpha = alpha;

		if (const Result<void> imported = Import(held); !imported)
		{
			held = Held{};

			static_cast<void>(m_Ids.Free(*id));

			return std::unexpected{ imported.error() };
		}

		return *id;
	}

	[[nodiscard]] Result<TextureId> Adopt(
		PixelSize<BufferSpace> size,
		TextureFormat format,
		std::span<const TexturePlane> planes,
		ITextureRelease* release
	) override
	{
		if (size.IsEmpty() || !size.IsValid())
		{
			return Failure(EINVAL, "an image with no extent");
		}

		if (planes.empty() || planes.size() > MaxTexturePlanes)
		{
			return Failure(EINVAL, "a plane count no format has");
		}

		if (format.Code == 0)
		{
			return Failure(EINVAL, "a buffer with no format");
		}

		if (m_Importers.empty())
		{
			return Failure(ENODEV, "no renderer to import an image into");
		}

		// **Duplicated before anything else can fail**, because a descriptor is the one thing here that
		// the caller gets back either way: a dup that succeeded and is then abandoned closes cleanly on
		// the way out of this scope, where a dup deferred until after the import would leave the
		// importer holding a descriptor whose owner is about to return an error over it.
		std::vector<Fd> descriptors;

		descriptors.reserve(planes.size());

		for (const TexturePlane& plane : planes)
		{
			Fd duplicated = Duplicate(plane.Descriptor);

			if (!duplicated.IsValid())
			{
				return Failure(EBADF, "a plane descriptor that could not be duplicated");
			}

			descriptors.push_back(std::move(duplicated));
		}

		const std::optional<TextureId> id = m_Ids.Allocate();

		if (!id)
		{
			return Failure(ENOSPC, "the texture id space is exhausted");
		}

		if (m_Held.size() <= m_Ids.SlotCount())
		{
			m_Held.resize(m_Ids.SlotCount() + 1);
		}

		Held& held = m_Held[id->Index];

		held = Held{};

		held.Id = *id;
		held.Descriptors = std::move(descriptors);
		held.Size = size;
		held.Release = release;
		held.Format = PixelFormat{ .Code = format.Code, .Modifier = format.Modifier };
		held.Image.PlaneCount = static_cast<std::uint32_t>(planes.size());

		for (std::size_t index = 0; index < planes.size(); ++index)
		{
			held.Image.Planes[index] = DmabufPlane{ .Descriptor = held.Descriptors[index].Borrow(),
				                                    .Offset = planes[index].Offset,
				                                    .Stride = planes[index].Stride };
		}

		if (const Result<void> imported = Import(held); !imported)
		{
			held = Held{};

			static_cast<void>(m_Ids.Free(*id));

			return std::unexpected{ imported.error() };
		}

		Offer(held);

		return *id;
	}

	void Abandon(const ITextureRelease& release) noexcept override
	{
		for (Held& held : m_Held)
		{
			if (held.Release == &release)
			{
				held.Release = nullptr;
			}
		}
	}

	[[nodiscard]] std::span<const TextureFormat> Formats() const noexcept override { return m_Formats; }

	// **Retired is not gone.** The id stops being one an author draws and stays exactly where it is
	// until `Reclaim` says the frame thread has moved past every snapshot that could name it. The slot
	// is not returned to the allocator either, which matters for a reason the generation does not
	// cover: a renderer keys its table on the whole id, so handing the index back early would put the
	// old entry and its replacement in that table at once, against a bound `Blit::MaxImages` is only
	// eight wide.
	void Retire(TextureId id) noexcept override
	{
		Held* const held = Find(id);

		if (held == nullptr)
		{
			return;
		}

		held->Retired = true;
	}

	// Stamp everything retired since the last call with the sequence about to be published.
	//
	// **That sequence is the first snapshot that cannot name these ids**, because the author gave them
	// up during the `Advance` this publication is serialising — so the last one that could is the one
	// before it, and the frame thread is past that exactly when its watermark reaches this number.
	// Stamping here rather than inside `Retire` is what keeps the author from needing to know a
	// sequence exists.
	//
	// A publish the ring refuses does not disturb this: the outbox supersedes the pending slot under
	// the same unconsumed sequence, so the number stays the one that will eventually carry the scene
	// without these textures in it.
	void Seal(std::uint64_t sequence) noexcept
	{
		for (Held& held : m_Held)
		{
			if (held.Retired && held.Stamp == 0)
			{
				held.Stamp = sequence;
			}
		}
	}

	// Everything the frame thread has moved past goes back — the importers forget it, the pixels are
	// freed, and the slot returns to the allocator.
	//
	// The comparison is `>=` where the outbox's is `<`, and the two say the same thing: a stamp is the
	// first sequence that does *not* name the texture, and a retained buffer's is the sequence it *is*.
	void Reclaim(std::uint64_t watermark) noexcept
	{
		for (Held& held : m_Held)
		{
			if (held.Id.IsNull() || held.Stamp == 0 || watermark < held.Stamp)
			{
				continue;
			}

			for (ITextureImporter* const importer : m_Importers)
			{
				importer->Forget(held.Id);
			}

			// Unconditional, because `IScanoutImporter::Forget` says nothing about an id it never took —
			// which is the same answer every other `Forget` in the system gives, and it means the offer's
			// outcome does not have to be remembered per image.
			if (m_Scanout != nullptr)
			{
				m_Scanout->Forget(held.Id);
			}

			const TextureId id = held.Id;
			ITextureRelease* const release = held.Release;

			held = Held{};

			static_cast<void>(m_Ids.Free(id));

			// **After the slot is clear, not before.** A client answering its own release by attaching
			// the same buffer again lands back in `Adopt` from inside this call, and a registry that had
			// not finished putting the id away would hand the new adoption the slot it is still holding.
			if (release != nullptr)
			{
				release->OnTextureReleased();
			}
		}
	}

	// The renderers were rebuilt, so every live image has to exist again on the new ones.
	//
	// **This is the reason decision 131 put the import verb at the waist at all.** A private verb on one
	// renderer is one the composition root cannot call across a device migration, and a migration that
	// did not re-adopt would come back with every window's pixels gone — which decision 41 requires to
	// be invisible. Re-adoption is possible only because the bytes are held here.
	//
	// A failure leaves the registry pointing at the new set with whatever adopted, and reports the
	// first refusal. There is nothing better available: the old renderers are already gone.
	[[nodiscard]] Result<void> Rebind(std::span<ITextureImporter* const> importers)
	{
		m_Importers.assign(importers.begin(), importers.end());

		Result<void> first{};

		for (Held& held : m_Held)
		{
			if (held.Id.IsNull())
			{
				continue;
			}

			if (const Result<void> imported = Import(held); !imported && first)
			{
				first = std::unexpected{ imported.error() };
			}
		}

		return first;
	}

	// What is drawable and what is waiting on a frame. Nothing in the design reads either; they are a
	// test's way of asserting that reclamation happened rather than that nothing crashed, and the
	// composition root's report line.
	[[nodiscard]] std::uint32_t Live() const noexcept { return m_Ids.LiveCount(); }

	[[nodiscard]] std::size_t Retiring() const noexcept
	{
		std::size_t waiting = 0;

		for (const Held& held : m_Held)
		{
			waiting += (!held.Id.IsNull() && held.Retired) ? std::size_t{ 1 } : std::size_t{ 0 };
		}

		return waiting;
	}

private:
	// One image: the id, the bytes the importers are pointing at, and what they were told those bytes
	// are.
	struct Held
	{
		TextureId Id{};

		// One of the two is filled and the other is empty, which is `TextureSource`'s variant kept as two
		// fields rather than as one: `Rebind` has to say the same thing about the same memory on a new
		// device, and what distinguishes them is `Descriptors` being non-empty.
		std::vector<std::byte> Pixels;
		std::vector<Fd> Descriptors;

		PixelSize<BufferSpace> Size{};
		std::uint32_t Stride = 0;

		// The planes, pointing into `Descriptors`. Rebuilt nowhere: a `vector<Fd>` that never grows after
		// `Adopt` keeps the same descriptor numbers for the whole of the image's life.
		DmabufImage Image{};

		// The client's own fourcc and modifier, for a descriptor. Left invalid for mapped pixels, where
		// `Alpha` below is what the format is derived from instead.
		PixelFormat Format{};

		// Who to tell when this is reclaimed, or null where nobody is owed anything. Borrowed, and cleared
		// by `Abandon` where the party goes away first.
		ITextureRelease* Release = nullptr;

		// What the author said its top byte means. Held rather than folded into the fourcc at adoption
		// because `Rebind` imports the same bytes again onto a new renderer and has to say the same
		// thing about them.
		TextureAlpha Alpha = TextureAlpha::Premultiplied;

		// Zero until `Seal`, which is why sequences start at one.
		std::uint64_t Stamp = 0;

		bool Retired = false;
	};

	// gyro's own pixels in an allocation a display engine can scan out, or nothing.
	//
	// **A copy into a dmabuf rather than a second verb on `ISceneTextures`.** The author-facing question
	// is *here are some bytes and here is what the top one means*, and it has exactly one honest answer
	// on every machine; which memory those bytes land in is a property of the hardware underneath and
	// changes when a card is swapped. So an author states the same thing it always did and this decides,
	// which also means the splash, the recovery console, `Text/Label` and the gym cards get promotable
	// without one of them being edited.
	//
	// **Every refusal is ordinary and the caller falls through to the heap.** No allocator, a provider
	// that will not do linear, an allocation that failed, a buffer nothing can map: all of them mean the
	// image is composited, which is where it already was. The mapping is the one worth naming — a
	// provider may hand back a perfectly good scanout allocation that the CPU cannot write into, and
	// pixels written into nothing is the failure this refuses to have.
	//
	// The row copy is the point of the stride: an allocator names its own pitch, because a display
	// engine fetches in a width of its choosing, and an author's rows are packed to whatever it wrote.
	[[nodiscard]] Result<TextureId>
	Promote(PixelSize<BufferSpace> size, std::uint32_t stride, std::span<const std::byte> pixels, TextureAlpha alpha)
	{
		if (m_Allocator == nullptr)
		{
			return Failure(ENODEV, "no allocator to put an authored image in");
		}

		const std::uint32_t code = alpha == TextureAlpha::Premultiplied ? FormatArgb8888 : FormatXrgb8888;
		const std::array<std::uint64_t, 1> linear{ ModifierLinear };

		Result<DmabufBuffer> buffer = m_Allocator->Allocate({ size.Width, size.Height }, code, linear);

		if (!buffer)
		{
			return std::unexpected{ buffer.error() };
		}

		const std::span<std::byte> destination = buffer->Pixels();

		if (destination.empty())
		{
			return Failure(ENOTSUP, "an authored image needs an allocation the processor can write into");
		}

		const std::uint32_t pitch = buffer->Stride();
		const std::size_t row = static_cast<std::size_t>(size.Width) * 4U;

		if (destination.size() < static_cast<std::size_t>(pitch) * static_cast<std::size_t>(size.Height))
		{
			return Failure(ERANGE, "an allocation smaller than the image it was made for");
		}

		for (std::int32_t line = 0; line < size.Height; ++line)
		{
			const std::size_t source = static_cast<std::size_t>(line) * stride;

			std::copy_n(
				pixels.begin() + static_cast<std::ptrdiff_t>(source),
				row,
				destination.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(line) * pitch)
			);
		}

		const std::array<TexturePlane, 1> planes{ TexturePlane{
			.Descriptor = buffer->Descriptor(), .Offset = 0, .Stride = pitch } };

		// Through the descriptor-taking adoption rather than beside it, so that the duplication, the
		// import, the offer to the display engine and the reclaim are the one path a client's buffer
		// already takes. The buffer dies at the end of this scope and the image does not: what the
		// adoption keeps is a duplicate of the descriptor, which holds the pages on its own.
		return Adopt(size, TextureFormat{ .Code = code, .Modifier = ModifierLinear }, planes, nullptr);
	}

	// The one place the format is named, which is Scene/Textures.h's whole division: an author writes
	// bytes in a layout it states in words, and the party that can name `Seam` says which fourcc that
	// is. Linear because these are the CPU's own pixels and there is no device that tiled them.
	//
	// The two fourccs differ in one byte's meaning and nothing else, which is why the author's word for
	// it is a two-valued enumeration rather than a format it had to learn: an opaque window arrives as
	// `xrgb8888` from every toolkit there is, and the alternative to carrying that here was for the
	// caller to walk the image forcing the byte to full — a second pass over every pixel of every
	// window, to say what one code already says.
	[[nodiscard]] static TextureSource Describe(const Held& held) noexcept
	{
		TextureSource source{};

		source.Size = held.Size;

		if (held.Descriptors.empty())
		{
			source.Format = { .Code = held.Alpha == TextureAlpha::Premultiplied ? FormatArgb8888 : FormatXrgb8888,
				              .Modifier = ModifierLinear };
			source.Memory =
				MappedPixels{ .Pixels = held.Pixels.data(), .Stride = held.Stride, .Length = held.Pixels.size() };
		}
		else
		{
			source.Format = held.Format;
			source.Memory = held.Image;
		}

		return source;
	}

	// Offer this image to the display engine, and discard what it says.
	//
	// **Called once per adoption and never from `Rebind`**, which is the difference between the two
	// devices decision 153 keeps apart: a Vulkan device is destroyed and rebuilt on migration and every
	// live image has to exist again on the new one, and the card gyro scans out of is the same card it
	// was before. Re-offering there would adopt an id the display engine already holds.
	void Offer(const Held& held) noexcept
	{
		if (m_Scanout == nullptr)
		{
			return;
		}

		// A refusal leaves the image exactly where it already is — sampled by the GPU, composited, and on
		// screen. Frame/Assign.h then finds no framebuffer for the id and composites the item, which is
		// the same picture at a different cost. That is the whole reason this is not in the loop below.
		static_cast<void>(m_Scanout->Adopt(held.Id, Describe(held)));
	}

	[[nodiscard]] Result<void> Import(Held& held)
	{
		const TextureSource source = Describe(held);

		for (std::size_t index = 0; index < m_Importers.size(); ++index)
		{
			if (const Result<void> imported = m_Importers[index]->Adopt(held.Id, source); !imported)
			{
				// Undo the ones that took it. A texture that exists on one renderer and not another is a
				// window that is on one panel and missing from the other, which is worse than a refusal
				// the caller can answer by not publishing the node.
				for (std::size_t undo = 0; undo < index; ++undo)
				{
					m_Importers[undo]->Forget(held.Id);
				}

				return imported;
			}
		}

		return {};
	}

	[[nodiscard]] static Result<void>
	Describes(PixelSize<BufferSpace> size, std::uint32_t stride, std::span<const std::byte> pixels) noexcept
	{
		if (size.IsEmpty() || !size.IsValid())
		{
			return Failure(EINVAL, "an image with no extent");
		}

		if (stride < static_cast<std::uint32_t>(size.Width) * 4U)
		{
			return Failure(EINVAL, "a stride narrower than the row it describes");
		}

		if (pixels.size() < static_cast<std::size_t>(stride) * static_cast<std::size_t>(size.Height))
		{
			return Failure(EINVAL, "fewer bytes than the extent and the stride claim");
		}

		return {};
	}

	[[nodiscard]] Held* Find(TextureId id) noexcept
	{
		const std::optional<std::uint32_t> index = m_Ids.IndexOf(id);

		return index ? &m_Held[*index] : nullptr;
	}

	std::vector<ITextureImporter*> m_Importers;

	// What `Formats` answers. Fixed at construction rather than derived here: the intersection is a
	// question about devices, and this module holds importers rather than the devices behind them.
	std::vector<TextureFormat> m_Formats;

	IScanoutImporter* m_Scanout = nullptr;

	// Where an authored image is put so that a display engine can read it. Null on a machine with no
	// provider that maps what it allocates, which is every one of them until `Promote` finds otherwise.
	IDmabufAllocator* m_Allocator = nullptr;

	SlotAllocator<TextureTag> m_Ids;

	// Indexed by an id's slot, and sized to the high-water mark rather than the live count, which is
	// `SceneStore`'s arrangement for the same reason.
	std::vector<Held> m_Held;
};

static_assert(
	MaxTexturePlanes == MaxImagePlanes,
	"An author's plane bound and the waist's are the same bound, and a buffer that fits one must fit the other"
);
