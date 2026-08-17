#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "Core/Handle.h"

// Identity allocation, separated from storage.
//
// This is the slot map decision 15 names, minus the map: it owns generations and the free list and
// owns no payload at all. The split is not tidiness. Entity data is meant to be walked as arrays —
// the evaluation pass touches only what is moving, and the publisher emits contiguous runs — so a
// container that owned a T per slot would decide the layout of everything keyed by an id, in favour
// of the one layout the design specifically does not want. Instead the allocator hands out indices,
// and whatever wants something per entity keeps its own array and indexes it.
//
// Dispatch-side only. Entities and outputs are created and destroyed on the dispatch thread, which
// is permitted to allocate and to block; the frame thread never holds one of these, and validates
// the ids it resolves against the published snapshot's own arrays instead.
//
// Generations are parity-coded: even means the slot is live, odd means it is free, and the first
// generation any slot carries is 2. One number therefore says both which occupant and whether there
// is one, which keeps validation to a single load. Two things fall out of starting at 2 rather than
// at 0 — generation 0 becomes unreachable, which is what makes a default-constructed handle null
// with no index reserved to mean it; and a handle carrying an odd generation can be rejected
// outright instead of matching the free slot that happens to hold that number.

// RetirementGeneration is the odd value a slot climbs to and stops at, and it is a parameter with
// the only sensible default rather than a constant because the branch reading it is otherwise
// unreachable — two billion frees of one slot. A branch no test can enter is a branch that is
// wrong, and decision 36's argument for mechanical enforcement applies to this as much as to the
// frame path. A test names a small odd limit; nothing else ever names it.
template<HandleTag Tag, std::uint32_t RetirementGeneration = std::numeric_limits<std::uint32_t>::max()>
class SlotAllocator
{
	static_assert(RetirementGeneration % 2 == 1, "Retirement lands on a free slot's generation, which is odd");
	static_assert(RetirementGeneration >= 3, "A slot has to be usable at least once before it retires");

public:
	using Id = Handle<Tag>;

	// Capacity bounds the index space rather than estimating the working set: it is the backstop
	// that turns unbounded growth into a stated refusal, and decision 27 sizes that kind of limit
	// an order of magnitude above anything real. Nothing is reserved up front, so asking for a
	// large one costs nothing until the slots are used.
	explicit constexpr SlotAllocator(std::uint32_t capacity) noexcept : m_Capacity{ capacity } {}

	// Two allocators handing out ids into the same arrays is not a thing anyone means to do.
	SlotAllocator(const SlotAllocator&) = delete;
	SlotAllocator& operator=(const SlotAllocator&) = delete;
	SlotAllocator(SlotAllocator&&) noexcept = default;
	SlotAllocator& operator=(SlotAllocator&&) noexcept = default;

	// Nothing is returned when the index space is exhausted, which is a refusal the caller has to
	// answer for — by killing the client that caused it, per decision 27 — rather than a null id
	// that flows onward and fails somewhere less attributable.
	[[nodiscard]] constexpr std::optional<Id> Allocate()
	{
		if (!m_FreeList.empty())
		{
			// Last freed, first reused. The alternative orderings buy a longer wait before an index
			// recurs, which the generation already makes worthless, and give up the one real
			// benefit: the slot most recently touched is the slot most likely still in cache.
			const std::uint32_t index = m_FreeList.back();
			m_FreeList.pop_back();

			++m_LiveCount;
			return Id{ index, ++m_Generations[index] };
		}

		if (m_Generations.size() >= m_Capacity)
		{
			return std::nullopt;
		}

		const auto index = static_cast<std::uint32_t>(m_Generations.size());
		m_Generations.push_back(FirstGeneration);

		++m_LiveCount;
		return Id{ index, FirstGeneration };
	}

	// False means the id was already stale — freed twice, or held past what it named. It is
	// reported rather than assumed away because ids legitimately outlive their entities here: an
	// exit animation keeps one alive after the model dropped it, and snapshot pressure can destroy
	// that entity early. A caller that knows better should say so at its own call site.
	constexpr bool Free(Id handle)
	{
		if (!IsValid(handle))
		{
			return false;
		}

		const std::uint32_t freed = ++m_Generations[handle.Index];
		--m_LiveCount;

		// A slot whose next live generation would wrap is retired rather than recycled. That costs
		// one index per two billion uses of that one slot, which even on the hottest slot in the
		// system is not reachable in a machine's life; the alternative is an id from exactly that
		// many allocations ago quietly validating, which is a bug nobody would ever find.
		if (freed != RetirementGeneration)
		{
			m_FreeList.push_back(handle.Index);
		}

		return true;
	}

	[[nodiscard]] constexpr bool IsValid(Id handle) const noexcept
	{
		if (handle.Index >= m_Generations.size())
		{
			return false;
		}

		// The parity test is what makes an id carrying a free slot's odd generation fail instead of
		// matching it. Minted ids are always even, so it rejects only what was never minted.
		return (handle.Generation & 1u) == 0 && m_Generations[handle.Index] == handle.Generation;
	}

	// The validated index, which is the only form in which an index should reach an array. Reaching
	// for handle.Index directly is how the check this type exists to make unskippable gets skipped.
	[[nodiscard]] constexpr std::optional<std::uint32_t> IndexOf(Id handle) const noexcept
	{
		if (!IsValid(handle))
		{
			return std::nullopt;
		}

		return handle.Index;
	}

	[[nodiscard]] constexpr std::uint32_t LiveCount() const noexcept { return m_LiveCount; }

	[[nodiscard]] constexpr std::uint32_t Capacity() const noexcept { return m_Capacity; }

	// The high-water mark of the index space, and therefore what any array keyed by one of these
	// ids has to be able to address. Not the live count: a freed slot keeps its index reserved.
	[[nodiscard]] constexpr std::uint32_t SlotCount() const noexcept
	{
		return static_cast<std::uint32_t>(m_Generations.size());
	}

private:
	static constexpr std::uint32_t FirstGeneration = 2;

	// Two arrays rather than one interleaved { generation, nextFree } record. Validation is the hot
	// operation and it reads only the generation, so four bytes a slot keeps twice as many slots on
	// a cache line as the pair would; the free list is touched once per create and once per destroy
	// and has no business sharing those lines. Where the pair wins — every slot free at once — the
	// two are equal, so there is no case where interleaving is cheaper.
	std::vector<std::uint32_t> m_Generations;
	std::vector<std::uint32_t> m_FreeList;

	std::uint32_t m_Capacity;
	std::uint32_t m_LiveCount = 0;
};

namespace Detail
{
// The whole type is constexpr, so its contract is checked by evaluating it rather than by asserting
// on its shape. The vectors do not escape the evaluation, which is what makes this legal.
consteval bool SlotAllocatorContract()
{
	SlotAllocator<EntityTag> allocator{ 2 };

	const EntityId first = allocator.Allocate().value();
	const EntityId second = allocator.Allocate().value();

	// Capacity is a refusal, not a suggestion.
	if (first == second || allocator.Allocate().has_value())
	{
		return false;
	}

	if (!allocator.IsValid(first) || allocator.IndexOf(second) != 1 || allocator.LiveCount() != 2)
	{
		return false;
	}

	// Freeing twice is caught, and the id goes stale on the first one.
	if (!allocator.Free(first) || allocator.Free(first))
	{
		return false;
	}

	if (allocator.IsValid(first) || allocator.IndexOf(first).has_value() || allocator.LiveCount() != 1)
	{
		return false;
	}

	// The slot comes back; the id does not.
	const EntityId reused = allocator.Allocate().value();

	if (reused.Index != first.Index || reused == first || allocator.IsValid(first))
	{
		return false;
	}

	// The neighbour is untouched, and a retired slot would have grown the index space instead.
	if (!allocator.IsValid(second) || allocator.SlotCount() != 2)
	{
		return false;
	}

	return allocator.IsValid(reused) && !allocator.IsValid(EntityId{});
}
} // namespace Detail

static_assert(Detail::SlotAllocatorContract());
