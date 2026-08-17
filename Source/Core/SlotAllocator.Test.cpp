#include "Core/SlotAllocator.h"

#include <cstdint>
#include <optional>
#include <random>
#include <source_location>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Testing/Test.h"

// The runtime half of SlotAllocator.h's contract. The compile-time half is the consteval block at
// the foot of that header, which already walks allocate, free, reuse, staleness, and refusal at
// capacity across two slots. What is left is what a constant expression cannot reach: the free
// list's ordering once there are enough slots for it to have one, the retirement branch, and the
// behaviour under a churn no hand-written sequence would cover.

// Allocation a test has already established is possible. Reporting and returning null keeps a
// broken fixture producing the harness's own diagnostic rather than terminating on an optional.
template<typename Allocator>
static typename Allocator::Id
Take(Allocator& allocator, const std::source_location& where = std::source_location::current())
{
	const std::optional<typename Allocator::Id> id = allocator.Allocate();
	if (!id)
	{
		ReportFailure("allocator.Allocate()", "index space exhausted", where);
		return {};
	}

	return *id;
}

GYRO_TEST(SlotAllocator, ReuseIsLastFreedFirst)
{
	SlotAllocator<EntityTag> allocator{ 8 };

	const EntityId first = Take(allocator);
	const EntityId second = Take(allocator);
	const EntityId third = Take(allocator);

	GYRO_REQUIRE(allocator.Free(first));
	GYRO_REQUIRE(allocator.Free(second));
	GYRO_REQUIRE(allocator.Free(third));

	// The documented order, and the reason the free list is a stack rather than a queue: the slot
	// touched most recently is the one most likely still in cache when it comes back.
	GYRO_CHECK_EQ(Take(allocator).Index, third.Index);
	GYRO_CHECK_EQ(Take(allocator).Index, second.Index);
	GYRO_CHECK_EQ(Take(allocator).Index, first.Index);

	// And the index space did not grow to serve any of them.
	GYRO_CHECK_EQ(allocator.SlotCount(), 3u);
}

GYRO_TEST(SlotAllocator, ExhaustionIsRefusedAndRecoverable)
{
	constexpr std::uint32_t Capacity = 64;
	SlotAllocator<EntityTag> allocator{ Capacity };

	std::vector<EntityId> ids;
	for (std::uint32_t i = 0; i < Capacity; ++i)
	{
		ids.push_back(Take(allocator));
	}

	GYRO_REQUIRE_EQ(allocator.LiveCount(), Capacity);
	GYRO_CHECK(!allocator.Allocate().has_value());
	GYRO_CHECK_EQ(allocator.SlotCount(), Capacity);

	// Refusal is not a terminal state. Decision 27 answers exhaustion by killing the client that
	// caused it, which hands the slots back, so the allocator has to keep working afterwards.
	GYRO_REQUIRE(allocator.Free(ids[10]));

	const std::optional<EntityId> replacement = allocator.Allocate();
	GYRO_REQUIRE(replacement.has_value());
	GYRO_CHECK_EQ(replacement->Index, ids[10].Index);
	GYRO_CHECK(!allocator.IsValid(ids[10]));
}

GYRO_TEST(SlotAllocator, ASlotRetiresRatherThanWrapping)
{
	// Generation 5 stands in for 0xFFFFFFFF. The arithmetic under test is identical; only the
	// number of frees needed to reach it differs, and at the real value that number is two billion.
	SlotAllocator<EntityTag, 5> allocator{ 1 };

	const EntityId first = Take(allocator);
	GYRO_REQUIRE(allocator.Free(first));

	const EntityId second = Take(allocator);
	GYRO_REQUIRE_EQ(second.Index, first.Index);
	GYRO_REQUIRE(second != first);
	GYRO_REQUIRE(allocator.Free(second));

	// The slot reached the limit and was not returned to the free list, so with a capacity of one
	// the allocator is now permanently empty rather than handing out a third id on the same slot —
	// which is what would alias `first` if the generation had wrapped instead.
	GYRO_CHECK(!allocator.Allocate().has_value());
	GYRO_CHECK_EQ(allocator.LiveCount(), 0u);
	GYRO_CHECK(!allocator.IsValid(first));
	GYRO_CHECK(!allocator.IsValid(second));

	// Retired, not forgotten: the index stays reserved, so anything sized by SlotCount still has a
	// place for it rather than shrinking under a live array.
	GYRO_CHECK_EQ(allocator.SlotCount(), 1u);
}

GYRO_TEST(SlotAllocator, RetirementCostsOneSlotAndNothingElse)
{
	// The neighbouring slots keep working, which is what makes retirement a cost of one index
	// rather than the end of the allocator.
	SlotAllocator<EntityTag, 5> allocator{ 3 };

	const EntityId doomed = Take(allocator);
	const EntityId neighbour = Take(allocator);

	GYRO_REQUIRE(allocator.Free(doomed));
	const EntityId reused = Take(allocator);
	GYRO_REQUIRE_EQ(reused.Index, doomed.Index);
	GYRO_REQUIRE(allocator.Free(reused));

	// That slot is gone; the index space still has one slot left to hand out.
	const EntityId fresh = Take(allocator);
	GYRO_CHECK(fresh.Index != doomed.Index);
	GYRO_CHECK(allocator.IsValid(neighbour));
	GYRO_CHECK(allocator.IsValid(fresh));
	GYRO_CHECK_EQ(allocator.LiveCount(), 2u);
	GYRO_CHECK_EQ(allocator.SlotCount(), 3u);
}

GYRO_TEST(SlotAllocator, MovingTheAllocatorKeepsIdsValid)
{
	// Outputs are reconciled on hotplug and on resume, so the thing holding an allocator is
	// entitled to be moved between owners without every id in the system going stale.
	SlotAllocator<EntityTag> source{ 4 };

	const EntityId id = Take(source);
	SlotAllocator<EntityTag> moved = std::move(source);

	GYRO_CHECK(moved.IsValid(id));
	GYRO_CHECK_EQ(moved.LiveCount(), 1u);
	GYRO_CHECK_EQ(moved.Capacity(), 4u);
}

GYRO_TEST(SlotAllocator, ChurnNeverRepeatsAnIdAndNeverRevivesOne)
{
	// The property the type exists for, over a workload no hand-written sequence would reach. A
	// popup opening and closing dozens of times a minute is exactly this shape, and the failure
	// being guarded against — a new entity silently inheriting a dead one's animation state — is
	// invisible when it happens.
	constexpr std::uint32_t Capacity = 64;
	constexpr int Steps = 20'000;

	SlotAllocator<EntityTag> allocator{ Capacity };

	std::unordered_set<EntityId> issued;
	std::vector<EntityId> live;
	std::vector<EntityId> freed;

	// Fixed seed. A test that finds a different bug on every run has found none of them, and a
	// reproducible failure is the only kind worth reporting from CI.
	std::mt19937 random{ 0x9E3779B9u };

	for (int step = 0; step < Steps; ++step)
	{
		if (live.empty() || (live.size() < Capacity && (random() & 1u) == 0u))
		{
			const std::optional<EntityId> id = allocator.Allocate();
			GYRO_REQUIRE(id.has_value());

			// No id is ever issued twice, across every slot and every reuse of it.
			GYRO_REQUIRE(issued.insert(*id).second);
			live.push_back(*id);
		}
		else
		{
			const std::size_t victim = random() % live.size();
			const EntityId id = live[victim];

			live[victim] = live.back();
			live.pop_back();

			GYRO_REQUIRE(allocator.Free(id));

			// Stale on the instant, not at the next allocation.
			GYRO_REQUIRE(!allocator.Free(id));
			freed.push_back(id);
		}
	}

	GYRO_CHECK_EQ(allocator.LiveCount(), live.size());
	GYRO_CHECK(allocator.SlotCount() <= Capacity);

	for (const EntityId id : live)
	{
		GYRO_REQUIRE(allocator.IsValid(id));
	}

	// The one that matters: every one of these names a slot that has since been handed out again,
	// most of them several times over.
	for (const EntityId id : freed)
	{
		GYRO_REQUIRE(!allocator.IsValid(id));
	}

	// Stated so the loop above cannot pass by having never recycled anything.
	GYRO_CHECK(freed.size() > Capacity);
}
