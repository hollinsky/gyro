#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Core/Handle.h"
#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Scene/Output.h"
#include "Scene/Textures.h"

// Rectangle allocation inside one exit-snapshot atlas.
//
// Docs/Decisions.md decision 46: **storage is a per-output atlas, reserved at output configuration
// and never grown**, and a snapshot is a rectangle out of a shelf packer plus a blit into that
// subregion. This is the packer half, on its own and with no atlas around it yet — no image, no
// device, no output. What it owns is where things go.
//
// **Shelves, and the argument is the lifetime rather than the packing quality.** Shelf packing is
// the crude one: rows are opened at the height of whatever opened them, occupants fill them left to
// right, and the waste is the difference between the tallest thing on a shelf and everything else on
// it. Decision 46 takes that trade deliberately — every occupant dies within roughly one exit, so the
// pool drains to empty on any idle moment and fragmentation undoes itself without a compaction pass.
// A general suballocator would carry the machinery to solve a problem uniform short lifetime already
// solves, which is the same reasoning decision 9 declines VMA by.
//
// **Rectangles are in `BufferSpace` because that is the space of the texture being sampled**, and
// `World/Content.h`'s `ImageContent::Source` is already typed that way with an atlas slot named in
// its comment. It is not `DeviceSpace`: the atlas is *at* an output's density (52), which is what
// makes the capture correct on each screen a straddling window is on (190), but a position inside a
// texture is not a position on a screen and the two must not be spelled the same.
//
// **Allocation-free after construction, which decision 46 asks for in as many words** — a reservation
// is a free-list pop and nothing more, because it happens on the frame the user closed something.
// The shelf run is a fixed array rather than a vector for that reason, and its bound is a refusal
// rather than a growth.
//
// Dispatch-side. Reservations are taken when a retirement is observed, which is `Scene`'s thread; the
// frame thread reads the rectangle out of the published snapshot and never asks for one.
class ShelfPacker
{
public:
	// The most shelves one atlas will open before it starts refusing.
	//
	// A shelf exists per distinct occupant height, and decision 46's largest legitimate simultaneous
	// retirement is a window plus a few small surfaces — a menu chain, an application closing with a
	// popup open. Thirty-two is an order of magnitude above that, which is how decision 27 sizes a
	// backstop: high enough that reaching it means something is wrong rather than busy.
	//
	// A refusal here is the same shortfall as running out of area and has decision 46's same answer —
	// finish older exits early, which frees shelves as well as space. So the bound needs no policy of
	// its own; it needs only to be a refusal rather than an allocation.
	static constexpr std::size_t MaxShelves = 32;

	constexpr ShelfPacker() noexcept = default;

	// An atlas of this many texels. A degenerate extent is legal and reserves nothing, which is the
	// state an output has before it has been configured.
	explicit constexpr ShelfPacker(PixelSize<BufferSpace> extent) noexcept : m_Extent{ extent } {}

	// Somewhere to put `extent` texels, or nothing.
	//
	// **Nothing is an ordinary answer rather than an error.** Decision 46 resolves exhaustion by
	// hard-settling older exits so their rectangles free, which is a policy about *which* exit to give
	// up and belongs to whoever holds the retiring set. This says only that there is no room.
	//
	// **An open shelf is always preferred to a new one**, which is what shelf packing is and is why the
	// vertical span lasts: a new shelf spends height that nothing can get back until the shelves below
	// it are empty, where an occupant on an open shelf spends only width.
	//
	// **Among the shelves that fit, the shortest.** The alternative is first fit, which is faster on a
	// run this short by an amount nothing can measure and spends the wrong shelf: with a window's shelf
	// and a menu's shelf both open, putting the next menu on the window's leaves the atlas mostly free
	// and unable to take a second window. The choice only arises once a second shelf exists — the first
	// reservation of a given height is what opens one.
	[[nodiscard]] constexpr std::optional<PixelRect<BufferSpace>> Reserve(PixelSize<BufferSpace> extent) noexcept
	{
		if (!extent.IsValid() || extent.IsEmpty() || extent.Width > m_Extent.Width || extent.Height > m_Extent.Height)
		{
			return {};
		}

		std::size_t best = MaxShelves;

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			const Shelf& shelf = m_Shelves[index];

			if (shelf.Height < extent.Height || m_Extent.Width - shelf.Used < extent.Width)
			{
				continue;
			}

			if (best == MaxShelves || shelf.Height < m_Shelves[best].Height)
			{
				best = index;
			}
		}

		// A new shelf where none of the open ones will take it, which is also the first reservation's
		// path: an empty packer has no shelves and opens one at the top.
		if (best == MaxShelves)
		{
			if (m_Count == MaxShelves || m_Bottom + extent.Height > m_Extent.Height)
			{
				return {};
			}

			best = m_Count;

			m_Shelves[best] = Shelf{ .Top = m_Bottom, .Height = extent.Height };

			m_Bottom += extent.Height;
			++m_Count;
		}

		Shelf& shelf = m_Shelves[best];
		const PixelRect<BufferSpace> reserved{ { shelf.Used, shelf.Top }, extent };

		shelf.Used += extent.Width;
		++shelf.Live;

		m_Used += Area(extent);
		m_HighWater = m_Used > m_HighWater ? m_Used : m_HighWater;

		return reserved;
	}

	// Give a rectangle back. False for one this packer did not hand out, which is a double release or
	// a rectangle from another output's atlas.
	//
	// **A shelf's width comes back all at once rather than per occupant**, and the occupant count is
	// what says when. Reclaiming the middle of a shelf would need a free list per shelf and a merge
	// rule, to recover space that is about to be recovered anyway — the shelf empties within one exit
	// because everything on it was retired within a few frames of everything else.
	constexpr bool Release(PixelRect<BufferSpace> rectangle) noexcept
	{
		for (std::size_t index = 0; index < m_Count; ++index)
		{
			Shelf& shelf = m_Shelves[index];

			if (shelf.Top != rectangle.Origin.Y || shelf.Live == 0 || rectangle.Extent.Height > shelf.Height)
			{
				continue;
			}

			--shelf.Live;
			m_Used -= Area(rectangle.Extent);

			if (shelf.Live == 0)
			{
				shelf.Used = 0;
			}

			// **The vertical span comes back when the shelves on the end are empty**, which is what
			// keeps a packer from being permanently the shape of the first thing it was asked for. An
			// empty shelf that is not on the end keeps its height and is reused at it; one that is on
			// the end has nothing above it that could care, so the bottom simply moves up and the next
			// reservation opens a shelf at whatever height it needs.
			while (m_Count > 0 && m_Shelves[m_Count - 1].Live == 0)
			{
				m_Bottom = m_Shelves[m_Count - 1].Top;

				--m_Count;
			}

			return true;
		}

		return false;
	}

	// Nothing is reserved. Decision 46 rests on this being reached on any idle moment, and it is the
	// state in which the packer has no shape at all — the next reservation opens a shelf at the top
	// whatever height it wants.
	[[nodiscard]] constexpr bool IsEmpty() const noexcept { return m_Count == 0; }

	[[nodiscard]] constexpr PixelSize<BufferSpace> Extent() const noexcept { return m_Extent; }

	// Texels currently reserved, and the most that were ever reserved at once.
	//
	// **The high-water mark is the instrument Open.md's sizing question asks for.** The atlas multiple
	// is not to be guessed — it is confirmed by watching what a real session actually peaks at, beside
	// the eviction count, the way decision 29 watches the budget. It is counted here because here is
	// where it is known exactly; anywhere else would be a reconstruction.
	[[nodiscard]] constexpr std::int64_t Used() const noexcept { return m_Used; }

	[[nodiscard]] constexpr std::int64_t HighWater() const noexcept { return m_HighWater; }

private:
	// One row. `Used` is how far along it the next occupant goes, and `Live` is how many are on it —
	// the second is what decides when the first goes back to zero.
	struct Shelf
	{
		std::int32_t Top = 0;
		std::int32_t Height = 0;
		std::int32_t Used = 0;
		std::uint32_t Live = 0;
	};

	[[nodiscard]] static constexpr std::int64_t Area(PixelSize<BufferSpace> extent) noexcept
	{
		return static_cast<std::int64_t>(extent.Width) * static_cast<std::int64_t>(extent.Height);
	}

	std::array<Shelf, MaxShelves> m_Shelves{};
	std::size_t m_Count = 0;

	// The first row nothing has been opened at. Shelves are stacked from the top, so this is also how
	// much of the atlas has ever been divided up.
	std::int32_t m_Bottom = 0;

	PixelSize<BufferSpace> m_Extent{};

	std::int64_t m_Used = 0;
	std::int64_t m_HighWater = 0;
};

// The packer is a value with no resources behind it, which is what lets an output hold one by value
// and lets these be compile-time claims rather than a test that has to run.
static_assert(ShelfPacker{}.IsEmpty());
static_assert(!ShelfPacker{}.Reserve({ 1, 1 }).has_value(), "An atlas with no extent has nowhere to put anything");

static_assert([] {
	ShelfPacker packer{ { 256, 256 } };

	const std::optional<PixelRect<BufferSpace>> first = packer.Reserve({ 100, 50 });
	const std::optional<PixelRect<BufferSpace>> second = packer.Reserve({ 100, 50 });

	// Side by side on one shelf, which is the whole of what a shelf is.
	return first == PixelRect<BufferSpace>{ { 0, 0 }, { 100, 50 } } &&
	       second == PixelRect<BufferSpace>{ { 100, 0 }, { 100, 50 } };
}());

static_assert([] {
	ShelfPacker packer{ { 256, 256 } };

	static_cast<void>(packer.Reserve({ 200, 50 }));

	// Too wide for what is left of that shelf, so a second one opens below it rather than the
	// reservation being refused with the atlas mostly empty.
	return packer.Reserve({ 200, 20 }) == PixelRect<BufferSpace>{ { 0, 50 }, { 200, 20 } };
}());

static_assert([] {
	ShelfPacker packer{ { 256, 256 } };

	const std::optional<PixelRect<BufferSpace>> only = packer.Reserve({ 10, 10 });

	return only.has_value() && !packer.IsEmpty() && packer.Release(*only) && packer.IsEmpty();
}());

// SPEC: how much atlas each output gets, as a multiple of its own render target.
//
// Decision 46 denominates capacity this way rather than in bytes, because a byte count is right only
// on the machine it was tuned on: as a multiple of the output's render target it scales with
// resolution and with monitor count, and it is expressed in the cost the rest of the design already
// reasons about.
//
// **Two is a starting value and is labelled so.** That entry declines to guess the number and
// [Open.md](../../Docs/Open.md) says how it will be settled: against the largest *legitimate*
// simultaneous retirement — an application closing with a menu open is a window plus two small
// surfaces, a nested menu chain is three or four small ones — and then confirmed by instrumentation
// rather than by argument, watching the per-output high-water and eviction count the way decision 29
// watches the budget. One render target holds a maximised window exactly; the second is the room for
// everything that leaves beside it. An eviction outside a stress test means this is wrong.
inline constexpr std::int32_t AtlasRenderTargetMultiple = 2;

// One entity's rectangle on one output, and which reservation it is.
//
// **Two facts rather than one because the second cannot be derived from the first.** A shelf gives
// the same texels to the next occupant the moment the one before it is finished, so a rectangle names
// a *place* and never an occupant — and the party that has to tell one occupant from the next is the
// frame thread, which sees only what is published. `World/Exit.h` carries the count across for that
// reason and says what goes wrong without it.
struct ExitSlot
{
	PixelRect<BufferSpace> Rectangle{};

	// Counted once for the whole process, so no two reservations anywhere are ever the same. Zero is
	// no reservation, which is what a default-constructed one is.
	std::uint32_t Reservation = 0;

	friend constexpr bool operator==(ExitSlot, ExitSlot) noexcept = default;
};

// The exit-snapshot atlases, one per output, and the reservations standing in them.
//
// Decision 190: **a snapshot belongs to a surface on an output.** A retiring surface takes a
// rectangle in the atlas of every output it is on, each at that output's own density, so a window
// leaving across a seam is captured correctly on both screens rather than sampled across from one.
//
// **All or nothing across the outputs a surface is on.** A reservation that succeeded on one screen
// and failed on the other would be the same window leaving two different ways at the same time, in
// the one configuration where both renderings are in view at once — which is the artefact 190 is
// about, arriving through the failure path instead of through the storage. So a partial reservation
// is given back and the answer is no, and no is decision 46's ordinary shortfall with its ordinary
// answer: finish the exit at once rather than draw it wrong.
//
// **Keyed by entity, held here rather than on the entity.** A `Scene/Entity.h` record is walked by
// everything and is the wrong place for a few bytes that matter to the handful of nodes currently
// dying. The retiring set is small by construction — decision 46 admits bounded-lifetime occupants
// only — so a run scanned linearly is shorter than an index would be.
class ExitAtlases
{
public:
	// Where the storage comes from, or nothing.
	//
	// **A separate call rather than a constructor argument, because the two facts arrive at different
	// moments.** A store exists as soon as there is a clock; a texture space exists once the
	// composition root has a renderer to put images on. `Dispatch/Loop.h` has both in hand at `Open`
	// and says so there, once, before the first output set.
	//
	// Null is legal and is what a store in a test has: no storage, so no atlas image, so a closing
	// window cuts rather than fading. That is decision 46's answer to having no room, and it is the
	// same answer whether the room ran out or was never there.
	void Attach(ITextures* storage) noexcept { m_Storage = storage; }

	// Take up an output set. Atlases for outputs that are still here with the same grid are kept along
	// with what is reserved in them; everything else goes.
	//
	// **A grid change is a new atlas rather than a resized one**, which is decision 46's *reserved at
	// output configuration and never grown* read literally. A mode change is not a moment to be
	// preserving exits across: the window that was leaving was leaving on a screen that no longer has
	// those dimensions, and its rectangle was in that screen's texels.
	void Configure(std::span<const SceneOutput> outputs)
	{
		m_Kept.clear();
		m_Replaced.clear();

		for (const SceneOutput& output : outputs)
		{
			const Atlas* const existing = Find(output.Id);
			const PixelSize<BufferSpace> extent = ExtentFor(output);

			if (existing != nullptr && existing->Packer.Extent() == extent && existing->Density == output.Density)
			{
				m_Kept.push_back(*existing);

				continue;
			}

			if (existing != nullptr)
			{
				m_Replaced.push_back(output.Id);
				Give(existing->Image);
			}

			m_Kept.push_back(
				Atlas{ .Output = output.Id,
			           .Packer = ShelfPacker{ extent },
			           .Density = output.Density,
			           .Image = Take(extent) }
			);
		}

		// An old atlas whose output is not in the new set is a monitor that has gone, and its image goes
		// with it — a hundred and thirty megabytes at 4K, held for a screen nobody can reserve on. A replaced one was
		// already given up above, where its output was found and its shape was not.
		for (const Atlas& atlas : m_Atlases)
		{
			const auto survives = [&atlas](const Atlas& kept) { return kept.Output == atlas.Output; };

			if (std::find_if(m_Kept.begin(), m_Kept.end(), survives) == m_Kept.end())
			{
				Give(atlas.Image);
			}
		}

		m_Atlases.swap(m_Kept);

		// **A reservation whose atlas is gone is not a reservation**, and there are two ways for it to
		// be gone: the output was unplugged, or it was reconfigured and given a fresh atlas. The second
		// is the one that looks like nothing happened — the id is still in the set, so a check that only
		// asked whether the output still exists would leave a rectangle addressing texels in a texture
		// that no longer has them.
		//
		// Either way the exit is still on screen and now has nowhere to be captured, which `SlotFor`
		// reports by answering nothing. That is the same answer a refusal gives, so a caller has one
		// case rather than three.
		std::erase_if(m_Slots, [this](const Slot& slot) {
			return Find(slot.Output) == nullptr ||
			       std::find(m_Replaced.begin(), m_Replaced.end(), slot.Output) != m_Replaced.end();
		});
	}

	// Reserve for one retiring surface, on every output the mask names.
	//
	// `bounds` is the surface's rectangle in global space — `Scene/Reach.h`'s `Cover` computes it and
	// the mask beside it — and each output converts it to its own texels at its own scale. Rounded
	// outward, because a snapshot short of the window by a fraction of a pixel is a hairline of
	// background down an edge for the length of the exit.
	[[nodiscard]] bool Reserve(EntityId id, OutputReach reach, Rect<GlobalSpace> bounds)
	{
		Release(id);

		if (reach == 0 || m_Atlases.empty())
		{
			return false;
		}

		const std::size_t before = m_Slots.size();

		for (std::size_t index = 0; index < m_Atlases.size() && index < MaxReachableOutputs; ++index)
		{
			if ((reach & (OutputReach{ 1 } << index)) == 0)
			{
				continue;
			}

			Atlas& atlas = m_Atlases[index];
			const std::optional<PixelRect<BufferSpace>> slot = atlas.Packer.Reserve(TexelsOf(bounds, atlas.Density));

			if (!slot)
			{
				Release(id);

				return false;
			}

			++m_Next;

			m_Slots.push_back(Slot{ .Entity = id, .Output = atlas.Output, .Rectangle = *slot, .Reservation = m_Next });
		}

		return m_Slots.size() != before;
	}

	// Give back everything this entity reserved. The serialisation pass that frees a finished
	// retirement is the caller, which is what makes the atlas drain with the exits rather than on a
	// sweep of its own.
	void Release(EntityId id) noexcept
	{
		for (const Slot& slot : m_Slots)
		{
			if (slot.Entity != id)
			{
				continue;
			}

			if (Atlas* const atlas = Find(slot.Output); atlas != nullptr)
			{
				static_cast<void>(atlas->Packer.Release(slot.Rectangle));
			}
		}

		std::erase_if(m_Slots, [id](const Slot& slot) { return slot.Entity == id; });
	}

	// Where this entity's snapshot goes on that output, or nothing where it has none — which is a
	// surface that is not retiring, one whose reservation was refused, and one whose output has been
	// unplugged since. All three mean the same thing to a caller: there is no snapshot, so do not
	// draw from one.
	[[nodiscard]] std::optional<ExitSlot> SlotFor(EntityId id, OutputId output) const noexcept
	{
		for (const Slot& slot : m_Slots)
		{
			if (slot.Entity == id && slot.Output == output)
			{
				return ExitSlot{ .Rectangle = slot.Rectangle, .Reservation = slot.Reservation };
			}
		}

		return {};
	}

	// Whether this entity has a rectangle anywhere, which is the question *is a picture owed for it*
	// asked without naming an output.
	//
	// `Scene/Store.h` is the caller and the case is a client destroying the surface its closing window
	// was drawing: the exit ends there unless a snapshot is coming, so what it needs is the existence
	// of a reservation rather than the placement of one.
	[[nodiscard]] bool Holds(EntityId id) const noexcept
	{
		for (const Slot& slot : m_Slots)
		{
			if (slot.Entity == id)
			{
				return true;
			}
		}

		return false;
	}

	// The image this output's rectangles are in, or null where there is none.
	//
	// **Not folded into `SlotFor`, because the two answers become available at different times.** A
	// rectangle is reserved the instant a window is observed to be closing; the pixels in it do not
	// exist until a later frame has room to copy them. A caller that got both together would have
	// every reason to think it could draw one.
	[[nodiscard]] TextureId ImageOn(OutputId output) const noexcept
	{
		const Atlas* const atlas = Find(output);

		return atlas == nullptr ? TextureId{} : atlas->Image;
	}

	[[nodiscard]] std::size_t Count() const noexcept { return m_Slots.size(); }

	// The instrumentation Open.md's sizing question asks for, per output and in texels.
	[[nodiscard]] std::int64_t HighWaterOn(OutputId output) const noexcept
	{
		const Atlas* const atlas = Find(output);

		return atlas == nullptr ? 0 : atlas->Packer.HighWater();
	}

	[[nodiscard]] bool IsEmpty() const noexcept { return m_Slots.empty(); }

private:
	struct Atlas
	{
		OutputId Output{};
		ShelfPacker Packer{};
		Scale Density{};

		// The storage the rectangles are in, or null where there is none — no texture space attached, or
		// one that had nothing to give. A null image is an atlas that packs perfectly well and has
		// nowhere to put the pixels, which is the same answer to a caller as a refused reservation.
		TextureId Image{};
	};

	struct Slot
	{
		EntityId Entity{};
		OutputId Output{};
		PixelRect<BufferSpace> Rectangle{};
		std::uint32_t Reservation = 0;
	};

	// The atlas is as wide as the output and `AtlasRenderTargetMultiple` times as tall, which is the
	// shape that makes the capacity statement true and keeps a maximised window able to fit: anything
	// that was on the screen is no wider than the screen, so width is never the axis that refuses.
	// Storage of this size, or null. Every refusal is the same refusal to everything above: no texture
	// space, a texture space with no renderer behind it, a device with no room. What a person sees in
	// all of them is a window that cuts instead of fading, which is decision 46's exhaustion answer.
	[[nodiscard]] TextureId Take(PixelSize<BufferSpace> extent) noexcept
	{
		if (m_Storage == nullptr)
		{
			return {};
		}

		const Result<TextureId> image = m_Storage->Reserve(extent);

		return image ? *image : TextureId{};
	}

	void Give(TextureId image) noexcept
	{
		if (m_Storage != nullptr && !image.IsNull())
		{
			m_Storage->Retire(image);
		}
	}

	[[nodiscard]] static PixelSize<BufferSpace> ExtentFor(const SceneOutput& output) noexcept
	{
		return { output.Grid.Width, output.Grid.Height * AtlasRenderTargetMultiple };
	}

	[[nodiscard]] static PixelSize<BufferSpace> TexelsOf(Rect<GlobalSpace> bounds, Scale density) noexcept
	{
		const auto logical = [](double span) {
			return Detail::Saturate(static_cast<std::int64_t>(std::ceil(span > 0.0 ? span : 0.0)));
		};

		return { density.DeviceFromLogical(logical(bounds.Extent.Width), Rounding::Up),
			     density.DeviceFromLogical(logical(bounds.Extent.Height), Rounding::Up) };
	}

	[[nodiscard]] const Atlas* Find(OutputId output) const noexcept
	{
		for (const Atlas& atlas : m_Atlases)
		{
			if (atlas.Output == output)
			{
				return &atlas;
			}
		}

		return nullptr;
	}

	[[nodiscard]] Atlas* Find(OutputId output) noexcept
	{
		return const_cast<Atlas*>(static_cast<const ExitAtlases*>(this)->Find(output));
	}

	// **No destructor giving the images back, deliberately.** The texture space is the registry the
	// dispatch loop holds beside the store, and it is destroyed *first* — so a release at teardown
	// would be a call into a registry that has gone. What it would buy is nothing either: the ids and
	// the images die with the registry a moment earlier. Storage is given back where it is actually
	// lost, which is an output being reconfigured or unplugged while the process goes on running.

	// Where an atlas image comes from. Borrowed, and outlives this — it is the registry the dispatch
	// loop holds beside the store.
	ITextures* m_Storage = nullptr;

	// The last reservation handed out. It counts up and never restarts, including across a hotplug that
	// throws every atlas away — which is the case it exists for, since the rectangles come back looking
	// exactly like the ones that just went. Sixty-four bits would be the cautious width and is not
	// needed: at four billion reservations, one per window closing, a machine has been closing a window
	// every millisecond for seven weeks.
	std::uint32_t m_Next = 0;

	std::vector<Atlas> m_Atlases;

	// Scratch for `Configure`: the set being built, and the outputs whose atlas it threw away.
	std::vector<Atlas> m_Kept;
	std::vector<OutputId> m_Replaced;
	std::vector<Slot> m_Slots;
};
