#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "Geometry/Space.h"

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
