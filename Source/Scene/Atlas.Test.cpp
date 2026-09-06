#include "Scene/Atlas.h"

#include <optional>
#include <vector>

#include "Testing/Test.h"

namespace
{
using Slot = PixelRect<BufferSpace>;

// Whether two reservations overlap, which is the property every other test here is really about: a
// packer that hands the same texels to two exits draws one window's pixels inside another's.
[[nodiscard]] bool Overlaps(Slot left, Slot right) noexcept
{
	return left.Left() < right.Right() && right.Left() < left.Right() && left.Top() < right.Bottom() &&
	       right.Top() < left.Bottom();
}

[[nodiscard]] bool Within(Slot slot, PixelSize<BufferSpace> extent) noexcept
{
	return slot.Left() >= 0 && slot.Top() >= 0 && slot.Right() <= extent.Width && slot.Bottom() <= extent.Height;
}
} // namespace

// The one thing a packer may never do, over a mixture of sizes large enough that shelves are opened,
// filled, and passed over. Checked pairwise rather than by area, because two rectangles can add up to
// less than the atlas and still be on top of each other.
GYRO_TEST(SceneAtlas, NoTwoReservationsEverShareATexel)
{
	constexpr PixelSize<BufferSpace> Extent{ 1024, 1024 };

	ShelfPacker packer{ Extent };
	std::vector<Slot> held;

	for (std::int32_t index = 1; index <= 40; ++index)
	{
		const PixelSize<BufferSpace> wanted{ 40 + (index % 7) * 30, 20 + (index % 5) * 25 };

		if (const std::optional<Slot> slot = packer.Reserve(wanted))
		{
			GYRO_CHECK(Within(*slot, Extent));
			GYRO_CHECK(slot->Extent == wanted);

			held.push_back(*slot);
		}
	}

	GYRO_REQUIRE(held.size() > 8);

	for (std::size_t left = 0; left < held.size(); ++left)
	{
		for (std::size_t right = left + 1; right < held.size(); ++right)
		{
			if (Overlaps(held[left], held[right]))
			{
				GYRO_FAIL(std::format("{} and {} share texels", held[left], held[right]));
			}
		}
	}
}

// **Decision 46's drain property, which everything else it says rests on.** Fragmentation is left
// uncompacted because every occupant dies within one exit, so the pool empties on any idle moment and
// the packer forgets the shape it was in. If a release cycle could leave it permanently shaped by
// what it once held, the argument against a general suballocator would not hold.
GYRO_TEST(SceneAtlas, AtlasReturnsToNothingAfterEveryOccupantHasGone)
{
	ShelfPacker packer{ { 512, 512 } };

	for (std::int32_t round = 0; round < 4; ++round)
	{
		std::vector<Slot> held;

		for (std::int32_t index = 0; index < 6; ++index)
		{
			if (const std::optional<Slot> slot = packer.Reserve({ 60 + index * 10, 30 + index * 20 }))
			{
				held.push_back(*slot);
			}
		}

		GYRO_REQUIRE(held.size() == 6);

		for (const Slot& slot : held)
		{
			GYRO_CHECK(packer.Release(slot));
		}

		GYRO_CHECK(packer.IsEmpty());
		GYRO_CHECK_EQ(packer.Used(), std::int64_t{ 0 });
	}

	// And the whole atlas is available again rather than the top of it, which is the part a shelf
	// packer gets wrong if the vertical span is not reclaimed with the shelves.
	GYRO_CHECK(packer.Reserve({ 512, 512 }).has_value());
}

// A shelf holds occupants shorter than itself and spends its own height on each of them, so where
// something short goes when two shelves would take it is a choice, and it is the choice that decides
// whether an atlas can still hold a window after a busy minute of menus.
GYRO_TEST(SceneAtlas, AShortReservationTakesTheShortestOpenShelfThatFitsIt)
{
	ShelfPacker packer{ { 200, 200 } };

	// A tall shelf with room left over. Nothing else is open yet, so a short reservation would land
	// here whatever the fit rule is — an open shelf is always preferred to a new one, because the
	// vertical span is the axis that runs out first.
	const std::optional<Slot> tall = packer.Reserve({ 150, 100 });

	GYRO_REQUIRE(tall.has_value());
	GYRO_CHECK_EQ(tall->Top(), 0);

	// Too wide for what is left of it, so this opens a second shelf under it — also with room left.
	const std::optional<Slot> shortRow = packer.Reserve({ 150, 20 });

	GYRO_REQUIRE(shortRow.has_value());
	GYRO_CHECK_EQ(shortRow->Top(), 100);

	// And now the choice is real: this fits on either, and it goes on the short one. The tall shelf's
	// remaining width is the only place a second tall thing could go, and spending it on something ten
	// texels high is how an atlas ends up unable to take a window while mostly empty.
	const std::optional<Slot> small = packer.Reserve({ 50, 10 });

	GYRO_REQUIRE(small.has_value());
	GYRO_CHECK_EQ(small->Top(), 100);
}

// Refusal is an ordinary answer. Decision 46 resolves exhaustion by finishing older exits early,
// which is a policy about which exit to give up and belongs to whoever holds the retiring set — so
// what the packer owes is a clean no, and an atlas that still works afterwards.
GYRO_TEST(SceneAtlas, AFullAtlasRefusesAndIsUnharmedByHavingBeenAsked)
{
	ShelfPacker packer{ { 100, 100 } };

	const std::optional<Slot> whole = packer.Reserve({ 100, 100 });
	GYRO_REQUIRE(whole.has_value());

	GYRO_CHECK(!packer.Reserve({ 1, 1 }).has_value());
	GYRO_CHECK(!packer.Reserve({ 100, 100 }).has_value());

	// Bigger than the atlas is the same answer as no room in it, rather than a different kind of
	// failure: an output cannot snapshot a surface larger than the atlas it reserved, and a caller
	// that has to tell the two apart would be a caller writing the eviction policy twice.
	GYRO_CHECK(!packer.Reserve({ 200, 10 }).has_value());

	GYRO_CHECK(packer.Release(*whole));
	GYRO_CHECK(packer.Reserve({ 100, 100 }).has_value());
}

// A rectangle this packer did not hand out. Both spellings of it: one that was already given back,
// and one that never came from here at all — which is a slot from another output's atlas, and the
// case that exists because decision 190 puts a straddling window in two of them.
GYRO_TEST(SceneAtlas, ReleasingSomethingElsesRectangleChangesNothing)
{
	ShelfPacker packer{ { 256, 256 } };

	const std::optional<Slot> slot = packer.Reserve({ 64, 64 });
	GYRO_REQUIRE(slot.has_value());

	GYRO_CHECK(!packer.Release(Slot{ { 0, 128 }, { 64, 64 } }));
	GYRO_CHECK(packer.Release(*slot));
	GYRO_CHECK(!packer.Release(*slot));
	GYRO_CHECK(packer.IsEmpty());
}

// The instrument Open.md's sizing question asks for: the multiple is not to be guessed, it is
// confirmed against what a real session peaks at. So the peak has to survive the drain that follows
// it, or what gets read back is whatever the atlas happened to be holding when somebody looked.
GYRO_TEST(SceneAtlas, TheHighWaterMarkOutlivesTheOccupantsThatSetIt)
{
	ShelfPacker packer{ { 512, 512 } };

	const std::optional<Slot> first = packer.Reserve({ 100, 100 });
	const std::optional<Slot> second = packer.Reserve({ 200, 100 });

	GYRO_REQUIRE(first.has_value());
	GYRO_REQUIRE(second.has_value());

	GYRO_CHECK_EQ(packer.Used(), std::int64_t{ 30'000 });
	GYRO_CHECK_EQ(packer.HighWater(), std::int64_t{ 30'000 });

	GYRO_CHECK(packer.Release(*second));

	GYRO_CHECK_EQ(packer.Used(), std::int64_t{ 10'000 });
	GYRO_CHECK_EQ(packer.HighWater(), std::int64_t{ 30'000 });
}
