#include "Scene/Atlas.h"

#include <array>
#include <optional>
#include <vector>

#include "Core/Clock.h"
#include "Scene/Commit.h"
#include "Scene/Serializer.h"
#include "Scene/Store.h"
#include "Testing/Test.h"

namespace
{
using Slot = PixelRect<BufferSpace>;

// Two screens side by side in global space, the second at twice the density — which is the laptop and
// the external monitor that decision 190 is about, and the pairing where "one atlas sampled by the
// other" would have to resample.
[[nodiscard]] SceneOutput Screen(OutputId id, double left, std::int32_t scale)
{
	return { .Id = id,
		     .Bounds = { { left, 0.0 }, { 1000.0, 1000.0 } },
		     .Density = Scale::FromInteger(scale),
		     .Grid = { 1000 * scale, 1000 * scale } };
}

// A texture space that mints ids and counts what is outstanding, standing in for
// `Dispatch/Textures.h` — which is the real one and belongs to a module this may not name.
class FakeStorage final : public ITextures
{
public:
	using ITextures::Adopt;

	[[nodiscard]] Result<TextureId>
	Adopt(PixelSize<BufferSpace>, std::uint32_t, std::span<const std::byte>, TextureAlpha) override
	{
		return Failure(ENOTSUP, "this space takes no pixels");
	}

	[[nodiscard]] Result<TextureId> Reserve(PixelSize<BufferSpace> size) override
	{
		if (m_Refusing)
		{
			return Failure(ENOMEM, "this space is refusing");
		}

		const TextureId id{ m_Next++, 1 };

		Live.push_back(id);
		Allocated.push_back(size);

		return id;
	}

	void Retire(TextureId id) noexcept override
	{
		std::erase(Live, id);
		Retired.push_back(id);
	}

	void Refuse(bool refusing) noexcept { m_Refusing = refusing; }

	std::vector<TextureId> Live;
	std::vector<TextureId> Retired;
	std::vector<PixelSize<BufferSpace>> Allocated;

private:
	std::uint32_t m_Next = 1;
	bool m_Refusing = false;
};

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

// **Decision 190, and it is the whole reason the atlases are a set rather than one.** A window
// straddling the seam reserves on both screens at once, each at that screen's density — so the
// capture on the high-density monitor is not a resample of the low-density panel's copy.
GYRO_TEST(SceneAtlas, AWindowAcrossTheSeamReservesOnBothScreensAtEachDensity)
{
	ExitAtlases atlases;

	const OutputId panel{ 1, 1 };
	const OutputId monitor{ 2, 1 };
	const std::array outputs{ Screen(panel, 0.0, 1), Screen(monitor, 1000.0, 2) };

	atlases.Configure(outputs);

	// Four hundred logical wide, sitting across the boundary at x = 1000.
	const EntityId window{ 7, 2 };
	const Rect<GlobalSpace> bounds{ { 800.0, 100.0 }, { 400.0, 300.0 } };

	GYRO_REQUIRE(atlases.Reserve(window, 0b11, bounds));
	GYRO_CHECK_EQ(atlases.Count(), std::size_t{ 2 });

	const std::optional<Slot> onPanel = atlases.SlotFor(window, panel);
	const std::optional<Slot> onMonitor = atlases.SlotFor(window, monitor);

	GYRO_REQUIRE(onPanel.has_value());
	GYRO_REQUIRE(onMonitor.has_value());

	// The same window, in each screen's own texels: 400x300 logical is 400x300 at scale 1 and 800x600
	// at scale 2. One atlas holding both would have to pick one of these and stretch it onto the other
	// screen, which is the resample decision 52 forbids being made by storage.
	GYRO_CHECK(onPanel->Extent == PixelSize<BufferSpace>{ 400, 300 });
	GYRO_CHECK(onMonitor->Extent == PixelSize<BufferSpace>{ 800, 600 });
}

// All or nothing across the outputs a surface is on. A reservation that took on one screen and was
// refused on the other is the same window leaving two different ways with both halves in view, which
// is the artefact 190 exists to prevent arriving through the failure path.
GYRO_TEST(SceneAtlas, AReservationRefusedOnOneScreenIsNotHeldOnTheOther)
{
	ExitAtlases atlases;

	const OutputId panel{ 1, 1 };
	const OutputId monitor{ 2, 1 };

	// The second screen is tiny, so a window that fits comfortably on the first cannot be captured on
	// it at all.
	const std::array outputs{ Screen(panel, 0.0, 1),
		                      SceneOutput{ .Id = monitor,
		                                   .Bounds = { { 1000.0, 0.0 }, { 100.0, 100.0 } },
		                                   .Density = Scale::FromInteger(1),
		                                   .Grid = { 100, 100 } } };

	atlases.Configure(outputs);

	const EntityId window{ 7, 2 };

	GYRO_CHECK(!atlases.Reserve(window, 0b11, Rect<GlobalSpace>{ { 800.0, 0.0 }, { 400.0, 300.0 } }));

	// Nothing kept anywhere, including on the screen that had room — and the atlas that took one is
	// back to empty rather than holding a rectangle nobody will ever give back.
	GYRO_CHECK(atlases.IsEmpty());
	GYRO_CHECK(!atlases.SlotFor(window, panel).has_value());
	GYRO_CHECK_EQ(atlases.HighWaterOn(panel), std::int64_t{ 120'000 });
}

// A reservation outlives an output set changing under it, and does not outlive its own output. The
// first is a monitor plugged in beside the window that is leaving; the second is the monitor it was
// leaving on being unplugged mid-exit.
GYRO_TEST(SceneAtlas, AnAtlasSurvivesAHotplugElsewhereAndGoesWithItsOwnOutput)
{
	ExitAtlases atlases;

	const OutputId panel{ 1, 1 };
	const OutputId monitor{ 2, 1 };
	const EntityId window{ 7, 2 };

	const std::array one{ Screen(panel, 0.0, 1) };
	atlases.Configure(one);

	GYRO_REQUIRE(atlases.Reserve(window, 0b1, Rect<GlobalSpace>{ { 0.0, 0.0 }, { 200.0, 200.0 } }));

	const std::array two{ Screen(panel, 0.0, 1), Screen(monitor, 1000.0, 1) };
	atlases.Configure(two);

	// Still there, because that output is still there with the same grid. A window mid-exit does not
	// blink because somebody plugged in a second screen.
	GYRO_CHECK(atlases.SlotFor(window, panel).has_value());

	const std::array none{ Screen(monitor, 1000.0, 1) };
	atlases.Configure(none);

	// And gone with the screen it was on, reported as no snapshot rather than as a rectangle in an
	// atlas that no longer exists.
	GYRO_CHECK(!atlases.SlotFor(window, panel).has_value());
	GYRO_CHECK(atlases.IsEmpty());
}

// A mode change is a new atlas rather than a resized one, which is decision 46's "reserved at output
// configuration and never grown" read literally: the rectangle was in texels of a screen that no
// longer has those dimensions.
GYRO_TEST(SceneAtlas, ChangingAnOutputsGridReplacesItsAtlas)
{
	ExitAtlases atlases;

	const OutputId panel{ 1, 1 };
	const EntityId window{ 7, 2 };

	const std::array before{ Screen(panel, 0.0, 1) };
	atlases.Configure(before);

	GYRO_REQUIRE(atlases.Reserve(window, 0b1, Rect<GlobalSpace>{ { 0.0, 0.0 }, { 200.0, 200.0 } }));

	const std::array after{ SceneOutput{
		.Id = panel, .Bounds = { {}, { 1000.0, 1000.0 } }, .Density = Scale::FromInteger(1), .Grid = { 800, 600 } } };
	atlases.Configure(after);

	GYRO_CHECK(!atlases.SlotFor(window, panel).has_value());
	GYRO_CHECK(atlases.IsEmpty());
}

// **Where the reservation is actually taken, end to end.** Decision 46 takes it when the retirement
// is observed, which is the commit verb that observes one; decision 114 gives it back when the
// subtree is freed, which is the pass that decides the exit is over. Neither is a sweep of its own.
GYRO_TEST(SceneAtlas, RetiringAWindowTakesItsRectangleAndFreeingItGivesItBack)
{
	ManualClock clock{ Monotonic::FromNanoseconds(1'000'000'000) };
	SceneStore store{ clock };

	const OutputId panel{ 1, 1 };
	const std::array outputs{ Screen(panel, 0.0, 1) };

	store.SetOutputs(outputs);

	const std::optional<EntityId> window = store.CreateContainer(store.FirstRoot(), { .Extent = { 300.0F, 200.0F } });

	GYRO_REQUIRE(window.has_value());
	GYRO_CHECK(!store.Atlases().SlotFor(*window, panel).has_value());

	{
		SceneCommit commit{ store, CommitAuthor::Client };

		GYRO_REQUIRE(commit.Retire(*window));
	}

	const std::optional<Slot> slot = store.Atlases().SlotFor(*window, panel);

	GYRO_REQUIRE(slot.has_value());
	GYRO_CHECK(slot->Extent == PixelSize<BufferSpace>{ 300, 200 });

	// The free is the serialisation pass's, and it is what returns the rectangle — so an atlas drains
	// with the exits in it rather than on a sweep that has to be remembered. Nothing on this window is
	// in flight, so the first pass over it finds the exit finished.
	SceneSerializer serializer;

	serializer.Serialize(store);

	GYRO_CHECK(!store.IsLive(*window));
	GYRO_CHECK(store.Atlases().IsEmpty());
}

// Decision 46 reserves exit storage when an output is configured, because an image created at the
// moment a window closes is a stall on exactly the frame somebody is watching. These are the storage
// half of that: one image per output, sized from the output's own render target, and given back the
// moment the screen it belonged to changes shape or goes away.

GYRO_TEST(ExitAtlases, EachScreenGetsAnImageTheSizeOfItsOwnAtlas)
{
	FakeStorage storage;
	ExitAtlases atlases;

	atlases.Attach(&storage);

	const OutputId panel{ 1, 1 };
	const OutputId monitor{ 2, 1 };
	const std::array outputs{ Screen(panel, 0.0, 1), Screen(monitor, 1000.0, 2) };

	atlases.Configure(outputs);

	GYRO_REQUIRE_EQ(storage.Live.size(), std::size_t{ 2 });
	GYRO_CHECK(!atlases.ImageOn(panel).IsNull());
	GYRO_CHECK(!atlases.ImageOn(monitor).IsNull());
	GYRO_CHECK(atlases.ImageOn(panel) != atlases.ImageOn(monitor));

	// As wide as the screen and `AtlasRenderTargetMultiple` times as tall, in that screen's own texels
	// — the scale-2 monitor's atlas is four times the area of the scale-1 panel's, because what has to
	// fit in it is four times as many texels.
	GYRO_CHECK(storage.Allocated[0] == PixelSize<BufferSpace>{ 1000, 1000 * AtlasRenderTargetMultiple });
	GYRO_CHECK(storage.Allocated[1] == PixelSize<BufferSpace>{ 2000, 2000 * AtlasRenderTargetMultiple });
}

GYRO_TEST(ExitAtlases, AnAtlasKeptAcrossAHotplugKeepsItsImageAndOneUnpluggedGivesItBack)
{
	FakeStorage storage;
	ExitAtlases atlases;

	atlases.Attach(&storage);

	const OutputId panel{ 1, 1 };
	const OutputId monitor{ 2, 1 };
	const std::array both{ Screen(panel, 0.0, 1), Screen(monitor, 1000.0, 2) };

	atlases.Configure(both);

	const TextureId kept = atlases.ImageOn(panel);
	const TextureId lost = atlases.ImageOn(monitor);

	const std::array alone{ Screen(panel, 0.0, 1) };

	atlases.Configure(alone);

	// The screen that is still there keeps the image it had, along with everything reserved in it. An
	// allocation per hotplug would be thirty megabytes at 4K, on the thread that has a scene to
	// serialise, every time somebody plugged in a monitor.
	GYRO_CHECK(atlases.ImageOn(panel) == kept);
	GYRO_CHECK(atlases.ImageOn(monitor).IsNull());

	// And the one that went gave its image back rather than leaving it resident for a screen nobody can
	// reserve on.
	GYRO_REQUIRE_EQ(storage.Retired.size(), std::size_t{ 1 });
	GYRO_CHECK(storage.Retired[0] == lost);
	GYRO_CHECK_EQ(storage.Live.size(), std::size_t{ 1 });
}

GYRO_TEST(ExitAtlases, ChangingAnOutputsGridReplacesItsImageAlongWithItsRectangles)
{
	FakeStorage storage;
	ExitAtlases atlases;

	atlases.Attach(&storage);

	const OutputId panel{ 1, 1 };
	const std::array before{ Screen(panel, 0.0, 1) };

	atlases.Configure(before);

	const EntityId window{ 4, 1 };

	GYRO_REQUIRE(atlases.Reserve(window, 0b1, { { 0.0, 0.0 }, { 200.0, 100.0 } }));

	const TextureId old = atlases.ImageOn(panel);
	const std::array after{ Screen(panel, 0.0, 2) };

	atlases.Configure(after);

	// A mode change is a new image and not a resized one, for the reason the rectangles are dropped
	// with it: what was leaving was leaving on a screen that no longer has those dimensions, and its
	// rectangle was in that screen's texels.
	GYRO_CHECK(!atlases.ImageOn(panel).IsNull());
	GYRO_CHECK(atlases.ImageOn(panel) != old);
	GYRO_REQUIRE_EQ(storage.Retired.size(), std::size_t{ 1 });
	GYRO_CHECK(storage.Retired[0] == old);
	GYRO_CHECK(!atlases.SlotFor(window, panel).has_value());
}

GYRO_TEST(ExitAtlases, WithNowhereToPutThePixelsTheRectanglesAreStillPacked)
{
	FakeStorage storage;
	ExitAtlases atlases;

	storage.Refuse(true);
	atlases.Attach(&storage);

	const OutputId panel{ 1, 1 };
	const std::array outputs{ Screen(panel, 0.0, 1) };

	atlases.Configure(outputs);

	const EntityId window{ 4, 1 };

	// The packing is arithmetic and answers whether there would have been room; the image is what a
	// device was willing to give. Keeping the first when the second fails is what lets the sizing
	// question stay measurable on a machine whose driver refused the allocation — and a closing window
	// cuts there, which is decision 46's answer to having no room, arriving one step earlier.
	GYRO_CHECK(atlases.ImageOn(panel).IsNull());
	GYRO_REQUIRE(atlases.Reserve(window, 0b1, { { 0.0, 0.0 }, { 200.0, 100.0 } }));
	GYRO_CHECK(atlases.SlotFor(window, panel).has_value());
	GYRO_CHECK(storage.Live.empty());
}

GYRO_TEST(ExitAtlases, WithNoTextureSpaceAtAllNothingIsAskedForAndNothingBreaks)
{
	ExitAtlases atlases;

	const OutputId panel{ 1, 1 };
	const std::array outputs{ Screen(panel, 0.0, 1) };

	atlases.Configure(outputs);

	const EntityId window{ 4, 1 };

	GYRO_CHECK(atlases.ImageOn(panel).IsNull());
	GYRO_REQUIRE(atlases.Reserve(window, 0b1, { { 0.0, 0.0 }, { 200.0, 100.0 } }));

	atlases.Release(window);

	GYRO_CHECK(atlases.IsEmpty());
}
