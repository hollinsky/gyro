#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "Core/Result.h"
#include "Core/SlotAllocator.h"
#include "Core/Texture.h"
#include "Geometry/Space.h"
#include "Scene/Textures.h"
#include "Seam/Importer.h"
#include "Seam/RenderTarget.h"

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
	explicit TextureRegistry(std::span<ITextureImporter* const> importers)
		: m_Importers{ importers.begin(), importers.end() }, m_Ids{ MaxTextures }
	{}

	TextureRegistry(const TextureRegistry&) = delete;
	TextureRegistry& operator=(const TextureRegistry&) = delete;
	TextureRegistry(TextureRegistry&&) = delete;
	TextureRegistry& operator=(TextureRegistry&&) = delete;

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

		held = Held{ .Id = *id,
			         .Pixels = std::vector<std::byte>{ pixels.begin(), pixels.end() },
			         .Size = size,
			         .Stride = stride,
			         .Alpha = alpha };

		if (const Result<void> imported = Import(held); !imported)
		{
			held = Held{};

			static_cast<void>(m_Ids.Free(*id));

			return std::unexpected{ imported.error() };
		}

		return *id;
	}

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

			const TextureId id = held.Id;

			held = Held{};

			static_cast<void>(m_Ids.Free(id));
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
		std::vector<std::byte> Pixels;
		PixelSize<BufferSpace> Size{};
		std::uint32_t Stride = 0;

		// What the author said its top byte means. Held rather than folded into the fourcc at adoption
		// because `Rebind` imports the same bytes again onto a new renderer and has to say the same
		// thing about them.
		TextureAlpha Alpha = TextureAlpha::Premultiplied;

		// Zero until `Seal`, which is why sequences start at one.
		std::uint64_t Stamp = 0;

		bool Retired = false;
	};

	// The one place the format is named, which is Scene/Textures.h's whole division: an author writes
	// bytes in a layout it states in words, and the party that can name `Seam` says which fourcc that
	// is. Linear because these are the CPU's own pixels and there is no device that tiled them.
	//
	// The two fourccs differ in one byte's meaning and nothing else, which is why the author's word for
	// it is a two-valued enumeration rather than a format it had to learn: an opaque window arrives as
	// `xrgb8888` from every toolkit there is, and the alternative to carrying that here was for the
	// caller to walk the image forcing the byte to full — a second pass over every pixel of every
	// window, to say what one code already says.
	[[nodiscard]] Result<void> Import(Held& held)
	{
		const TextureSource source{
			.Size = held.Size,
			.Format = { .Code = held.Alpha == TextureAlpha::Premultiplied ? FormatArgb8888 : FormatXrgb8888,
			            .Modifier = ModifierLinear },
			.Memory = MappedPixels{ .Pixels = held.Pixels.data(), .Stride = held.Stride, .Length = held.Pixels.size() },
		};

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

	SlotAllocator<TextureTag> m_Ids;

	// Indexed by an id's slot, and sized to the high-water mark rather than the live count, which is
	// `SceneStore`'s arrangement for the same reason.
	std::vector<Held> m_Held;
};
