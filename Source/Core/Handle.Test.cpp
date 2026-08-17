#include "Core/Handle.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Testing/Test.h"

// The runtime half of Handle.h's contract. The compile-time half is the static_assert block at the
// foot of that header — size, layout, nullness, ordering, and the tags being distinct types — and
// is not repeated here. What is left is the two pieces a constant expression cannot reach: what a
// handle looks like to whoever reads a log, and whether the hash does anything.

GYRO_TEST(Handle, FormatsAsSlotAndGeneration)
{
	GYRO_CHECK_EQ(std::format("{}", EntityId{ 12, 4 }), std::string{ "Entity#12.4" });
	GYRO_CHECK_EQ(std::format("{}", EntityId{}), std::string{ "Entity#null" });

	// Both halves are printed because a line naming a slot without its generation cannot be told
	// apart from one naming the occupant a few seconds earlier, which is the exact confusion the
	// generation exists to resolve.
	GYRO_CHECK_EQ(std::format("{}", EntityId{ 12, 6 }), std::string{ "Entity#12.6" });
}

GYRO_TEST(Handle, TheTagIsVisibleToTheReader)
{
	// Same bits, different kind. The compiler already refuses to mix them; if they also printed
	// alike then a log would be the one place that distinction is invisible.
	GYRO_CHECK_EQ(std::format("{}", EntityId{ 3, 2 }), std::string{ "Entity#3.2" });
	GYRO_CHECK_EQ(std::format("{}", OutputId{ 3, 2 }), std::string{ "Output#3.2" });
}

GYRO_TEST(Handle, OrderingSortsIntoTraversalOrder)
{
	// Why <=> is defaulted at all. A dirty set arrives in the order things were touched, and
	// walking the entity arrays in index order rather than that one is the difference between a
	// linear read and a random one.
	std::vector<EntityId> handles{ { 4, 2 }, { 1, 6 }, { 4, 4 }, { 0, 2 }, { 1, 2 } };
	std::ranges::sort(handles);

	// Index first, generation only as a tiebreak — the traversal cares about the former and has no
	// opinion at all about the latter, so the whole sequence is the assertion rather than its ends.
	const std::vector<EntityId> ordered{ { 0, 2 }, { 1, 2 }, { 1, 6 }, { 4, 2 }, { 4, 4 } };

	GYRO_CHECK(handles == ordered);
}

GYRO_TEST(Handle, AStaleKeyDoesNotFindItsSuccessor)
{
	// The identity map is dispatch-side and keyed on these, so a handle whose slot has been reused
	// must miss rather than land on whatever moved in. This is job one of decision 15 expressed as
	// a container lookup.
	std::unordered_map<EntityId, int> byEntity;

	byEntity[EntityId{ 4, 4 }] = 7;

	// Subscripted rather than looked up through at(), so the braced key sits at paren depth zero in
	// the macro argument — the shape the preprocessor splits on and the C++ parser puts back.
	GYRO_CHECK_EQ(byEntity[EntityId{ 4, 4 }], 7);

	GYRO_CHECK_EQ(byEntity.count(EntityId{ 4, 4 }), std::size_t{ 1 });
	GYRO_CHECK_EQ(byEntity.count(EntityId{ 4, 2 }), std::size_t{ 0 });
	GYRO_CHECK_EQ(byEntity.count(EntityId{ 4, 6 }), std::size_t{ 0 });
	GYRO_CHECK_EQ(byEntity.count(EntityId{}), std::size_t{ 0 });
}

GYRO_TEST(Handle, GenerationReachesTheLowBitsOfTheHash)
{
	// One slot's whole reuse history, dropped into a table that indexes on the low bits — which is
	// what this codebase would write if it wrote an open-addressed map, and what the finalizer in
	// Handle.h is for. The generation occupies the high word of the value being hashed, so an
	// identity hash puts every one of these in a single bucket. This is the only case that
	// distinguishes mixing from doing nothing, so it is the case worth testing.
	constexpr std::size_t Buckets = 4096;
	constexpr std::uint32_t Count = 4096;

	std::unordered_set<std::size_t> occupied;
	for (std::uint32_t i = 0; i < Count; ++i)
	{
		occupied.insert(std::hash<EntityId>{}(EntityId{ 7, 2 * (i + 1) }) % Buckets);
	}

	// A uniform hash fills 1 - 1/e of the table, near 2590. The identity fills one. The bound is
	// loose because what is being asserted is the difference between those two numbers.
	GYRO_CHECK(occupied.size() > 2'000);
}
