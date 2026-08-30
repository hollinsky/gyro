#include "Scene/Store.h"

#include <optional>

#include "Core/Clock.h"
#include "Core/Handle.h"
#include "Geometry/Scale.h"
#include "Scene/Entity.h"
#include "Scene/Output.h"
#include "Testing/Test.h"
#include "World/Content.h"
#include "World/Node.h"

// Docs/Decisions.md decision 111's store: a slot map for identity and intrusive links for the tree,
// with a top level that is a list rather than a root.

namespace
{
// The scene's clock. Nothing here reads it — a commit is what clamps an origin against dispatch's now —
// but the store holds one for its whole life rather than taking one per transaction.
ManualClock Clock;

[[nodiscard]] NodeProperties Panel(float width, float height)
{
	return { .Extent = { width, height } };
}
} // namespace

GYRO_TEST(SceneStore, TheTopLevelIsAListAndItKeepsTheOrderItWasBuiltIn)
{
	SceneStore store{ Clock };

	const EntityId first = store.CreateContainer({}, {}).value();
	const EntityId second = store.CreateContainer({}, {}).value();
	const EntityId third = store.CreateContainer({}, {}).value();

	GYRO_REQUIRE(store.FirstRoot() == first);
	GYRO_CHECK(store.Find(first)->NextSibling == second);
	GYRO_CHECK(store.Find(second)->NextSibling == third);
	GYRO_CHECK(store.Find(third)->NextSibling.IsNull());

	// Appended rather than prepended, because decision 55 makes the list order the z order and a
	// window that just opened is in front of the ones that were already there.
	GYRO_CHECK(store.Find(first)->Parent.IsNull());
	GYRO_CHECK_EQ(store.Count(), std::uint32_t{ 3 });
}

GYRO_TEST(SceneStore, AChildLinksUnderItsParentAndTheParentKnowsBothEnds)
{
	SceneStore store{ Clock };

	const EntityId menu = store.CreateContainer({}, {}).value();
	const EntityId below = store.CreateImage(menu, Panel(100.0F, 40.0F), ImageContent{}).value();
	const EntityId above = store.CreateImage(menu, Panel(80.0F, 30.0F), ImageContent{}).value();

	GYRO_REQUIRE(store.Find(menu)->FirstChild == below);
	GYRO_CHECK(store.Find(menu)->LastChild == above);
	GYRO_CHECK(store.Find(below)->NextSibling == above);
	GYRO_CHECK(store.Find(above)->Parent == menu);

	// The top level holds the container alone: a child is not a root.
	GYRO_CHECK(store.FirstRoot() == menu);
	GYRO_CHECK(store.Find(menu)->NextSibling.IsNull());
}

GYRO_TEST(SceneStore, AKindArrivesWithItsPayloadAndNeverWithoutIt)
{
	SceneStore store{ Clock };

	ImageContent image{};
	image.Frame = { { 1.0F, 2.0F }, { 3.0F, 4.0F } };

	SolidContent solid{};
	solid.Red = 0.25F;

	const EntityId container = store.CreateContainer({}, {}).value();
	const EntityId drawn = store.CreateImage({}, Panel(10.0F, 10.0F), image).value();
	const EntityId filled = store.CreateSolid({}, Panel(10.0F, 10.0F), solid).value();

	// A node's `Content` is a position in whichever run its `Kind` selects (decision 95), so the two
	// are one fact — which is what four constructors buy over one that took a kind and an index.
	GYRO_CHECK(store.Find(container)->Kind == NodeKind::Container);
	GYRO_CHECK_EQ(store.Find(container)->Content, NoContent);

	GYRO_REQUIRE(store.Find(drawn)->Kind == NodeKind::Image);
	GYRO_REQUIRE_EQ(store.Images().size(), std::size_t{ 1 });
	GYRO_CHECK(store.Images()[store.Find(drawn)->Content] == image);

	GYRO_REQUIRE(store.Find(filled)->Kind == NodeKind::Solid);
	GYRO_REQUIRE_EQ(store.Solids().size(), std::size_t{ 1 });
	GYRO_CHECK(store.Solids()[store.Find(filled)->Content] == solid);
}

GYRO_TEST(SceneStore, AReferenceNamesAnIdAndNotACopy)
{
	SceneStore store{ Clock };

	const EntityId window = store.CreateImage({}, Panel(100.0F, 40.0F), ImageContent{}).value();
	const EntityId tile = store.CreateReference({}, Panel(100.0F, 40.0F), window).value();

	// Decision 88: a reference is not a second identity, so a switcher tile and the window it shows are
	// one object by construction rather than two the system keeps in agreement.
	GYRO_CHECK(store.Find(tile)->Target == window);
	GYRO_CHECK_EQ(store.Find(tile)->Content, NoContent);
	GYRO_CHECK(store.Find(tile)->Kind == NodeKind::Reference);
}

GYRO_TEST(SceneStore, AReferenceTakesNoChildren)
{
	SceneStore store{ Clock };

	const EntityId window = store.CreateImage({}, Panel(100.0F, 40.0F), ImageContent{}).value();
	const EntityId tile = store.CreateReference({}, {}, window).value();

	// Refused here so it is unwritable rather than something the frame walk has to be careful about: a
	// reference's content *is* a node, so `Frame` descends into the expansion and never into the run
	// beside it, and a child attached here would be authored, serialised, paid for, and never drawn.
	GYRO_CHECK(!store.CreateContainer(tile, {}).has_value());
	GYRO_CHECK_EQ(store.Count(), std::uint32_t{ 2 });
}

GYRO_TEST(SceneStore, AParentThatIsNotLiveIsARefusalRatherThanAPromotion)
{
	SceneStore store{ Clock };

	const EntityId forged{ .Index = 4, .Generation = 2 };

	// Silently promoting the child to the top level would put it on screen, which is the failure worth
	// having a refusal for — a caller answers for it, per decision 27, rather than finding out by
	// looking at the display.
	GYRO_CHECK(!store.CreateContainer(forged, {}).has_value());
	GYRO_CHECK(!store.CreateImage(forged, {}, ImageContent{}).has_value());
	GYRO_CHECK(store.FirstRoot().IsNull());
	GYRO_CHECK_EQ(store.Count(), std::uint32_t{ 0 });

	// And the refused image left no payload behind, which is what would otherwise make the store's
	// content array grow on every rejected create.
	GYRO_CHECK(store.Images().empty());
}

GYRO_TEST(SceneStore, TheIndexSpaceIsARefusalRatherThanAnEstimate)
{
	SceneStore store{ Clock, 2 };

	GYRO_CHECK(store.CreateContainer({}, {}).has_value());
	GYRO_CHECK(store.CreateContainer({}, {}).has_value());
	GYRO_CHECK(!store.CreateContainer({}, {}).has_value());

	GYRO_CHECK_EQ(store.SlotCount(), std::uint32_t{ 2 });
}

GYRO_TEST(SceneStore, AStaleIdResolvesToNothing)
{
	SceneStore store{ Clock };

	const EntityId live = store.CreateContainer({}, {}).value();
	const EntityId stale{ .Index = live.Index, .Generation = live.Generation + 2 };

	GYRO_CHECK(store.Find(live) != nullptr);
	GYRO_CHECK(store.Find(stale) == nullptr);
	GYRO_CHECK(!store.IsLive(stale));
	GYRO_CHECK(store.Find(EntityId{}) == nullptr);
}

GYRO_TEST(SceneStore, TheOutputSetIsReplacedWholeAndTheGenerationSaysSo)
{
	SceneStore store{ Clock };

	GYRO_CHECK(store.Outputs().empty());
	GYRO_CHECK_EQ(store.OutputGeneration(), std::uint64_t{ 0 });

	const SceneOutput primary{ .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } };
	const SceneOutput secondary{ .Bounds = { { 1920.0, 0.0 }, { 1920.0, 1080.0 } },
		                         .Density = Scale::FromInteger(1),
		                         .Grid = { 1920, 1080 } };

	const SceneOutput both[] = { primary, secondary };

	store.SetOutputs(both);

	GYRO_REQUIRE_EQ(store.Outputs().size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(store.OutputGeneration(), std::uint64_t{ 1 });

	// Hotplug replacing one output with another leaves the count alone, which is exactly the case
	// decision 84's generation exists for: position is the identity across the waist, and nothing else
	// would say the two sides mean the same outputs by it.
	const SceneOutput replaced[] = { primary, primary };

	store.SetOutputs(replaced);

	GYRO_CHECK_EQ(store.Outputs().size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(store.OutputGeneration(), std::uint64_t{ 2 });
}

GYRO_TEST(SceneStore, AssigningASessionMovesOneOutputAndDoesNotRenumberTheSet)
{
	SceneStore store{ Clock };

	const SceneOutput primary{ .Id = OutputId{ 1, 1 }, .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } };
	const SceneOutput secondary{ .Id = OutputId{ 2, 1 },
		                         .Bounds = { { 1920.0, 0.0 }, { 1920.0, 1080.0 } },
		                         .Density = Scale::FromInteger(1),
		                         .Grid = { 1920, 1080 } };

	const SceneOutput both[] = { primary, secondary };

	store.SetOutputs(both);

	// Every output starts showing gyro's own scene, which is what the splash and the recovery console
	// are, rather than showing a session nobody has offered yet.
	GYRO_CHECK(store.Outputs()[0].Session == SessionId::None);
	GYRO_CHECK(store.Outputs()[1].Session == SessionId::None);

	const std::uint64_t before = store.OutputGeneration();

	store.SetOutputSession(OutputId{ 2, 1 }, static_cast<SessionId>(7));

	GYRO_CHECK(store.Outputs()[0].Session == SessionId::None);
	GYRO_CHECK(store.Outputs()[1].Session == static_cast<SessionId>(7));

	// **The set did not change, so the generation must not move.** Decision 84's number answers *do
	// these runs mean my outputs at all*, and bumping it because somebody logged in would invalidate
	// every per-output run in the snapshot over a fact none of them carries.
	GYRO_CHECK_EQ(store.OutputGeneration(), before);

	// A monitor unplugged between an agent offering and the root assigning is an id that is not here,
	// and the assignment is dropped rather than landing on whoever took the slot.
	store.SetOutputSession(OutputId{ 9, 1 }, static_cast<SessionId>(7));

	GYRO_CHECK(store.Outputs()[0].Session == SessionId::None);

	// The session ending returns its outputs to gyro, which is the same field written the other way.
	store.SetOutputSession(OutputId{ 2, 1 }, SessionId::None);

	GYRO_CHECK(store.Outputs()[1].Session == SessionId::None);
	GYRO_CHECK_EQ(store.OutputGeneration(), before);
}

// Raising is the z order written to: the sibling list is the paint order (55), so the raised node
// comes out last and everything it passed keeps its own order.
GYRO_TEST(SceneStore, RaisingAChildPutsItLastAndLeavesTheRestInOrder)
{
	SceneStore store{ Clock };

	const EntityId floor = store.CreateContainer({}, {}).value();
	const EntityId first = store.CreateContainer(floor, {}).value();
	const EntityId second = store.CreateContainer(floor, {}).value();
	const EntityId third = store.CreateContainer(floor, {}).value();

	GYRO_REQUIRE(store.Raise(first));

	GYRO_CHECK(store.Find(floor)->FirstChild == second);
	GYRO_CHECK(store.Find(floor)->LastChild == first);
	GYRO_CHECK(store.Find(second)->NextSibling == third);
	GYRO_CHECK(store.Find(third)->NextSibling == first);
	GYRO_CHECK(store.Find(first)->NextSibling.IsNull());

	// The middle one, which is the case that exercises the predecessor repair rather than the head.
	GYRO_REQUIRE(store.Raise(third));

	GYRO_CHECK(store.Find(floor)->FirstChild == second);
	GYRO_CHECK(store.Find(floor)->LastChild == third);
	GYRO_CHECK(store.Find(second)->NextSibling == first);
	GYRO_CHECK(store.Find(first)->NextSibling == third);
	GYRO_CHECK(store.Find(third)->NextSibling.IsNull());

	// Nothing moved and nothing was unlinked: the common answer, a person clicking about inside the
	// window they are already using.
	GYRO_CHECK(store.Raise(third));
	GYRO_CHECK(store.Find(floor)->LastChild == third);
	GYRO_CHECK(store.Find(second)->NextSibling == first);
}

// A root raises against the top level's own pair, which decision 111 makes a sibling list with nothing
// distinguished above it — so the same two links are repaired and there is no parent to ask.
GYRO_TEST(SceneStore, RaisingARootMovesItToTheFrontOfTheTopLevel)
{
	SceneStore store{ Clock };

	const EntityId floor = store.CreateContainer({}, {}).value();
	const EntityId cursor = store.CreateContainer({}, {}).value();

	GYRO_REQUIRE(store.Raise(floor));

	GYRO_CHECK(store.FirstRoot() == cursor);
	GYRO_CHECK(store.Find(cursor)->NextSibling == floor);
	GYRO_CHECK(store.Find(floor)->NextSibling.IsNull());

	// A stale handle is a refusal rather than a reorder of whatever now holds that slot.
	SceneStore other{ Clock };

	GYRO_CHECK(!other.Raise(floor));
}

// `wl_subsurface.place_above` and `place_below` as a link edit, which is what decision 55 makes them:
// the sibling list is the z order, so a client restacking its own parts moves nothing else.
GYRO_TEST(SceneStore, OrderingPutsASiblingWhereItWasAskedFor)
{
	SceneStore store{ Clock };

	const EntityId window = store.CreateContainer({}, {}).value();
	const EntityId first = store.CreateContainer(window, {}).value();
	const EntityId second = store.CreateContainer(window, {}).value();
	const EntityId third = store.CreateContainer(window, {}).value();

	// To the front, which is what a null reference means and is `place_below` naming the first entry
	// there is.
	GYRO_REQUIRE(store.Order(third, {}));

	GYRO_CHECK(store.Find(window)->FirstChild == third);
	GYRO_CHECK(store.Find(third)->NextSibling == first);
	GYRO_CHECK(store.Find(window)->LastChild == second);

	// And into the middle, which is the case that exercises both repairs at once: the predecessor it
	// left and the one it arrived after.
	GYRO_REQUIRE(store.Order(third, first));

	GYRO_CHECK(store.Find(window)->FirstChild == first);
	GYRO_CHECK(store.Find(first)->NextSibling == third);
	GYRO_CHECK(store.Find(third)->NextSibling == second);
	GYRO_CHECK(store.Find(window)->LastChild == second);

	// To the very end, where the last link is the one that has to move.
	GYRO_REQUIRE(store.Order(first, second));

	GYRO_CHECK(store.Find(window)->LastChild == first);
	GYRO_CHECK(store.Find(first)->NextSibling.IsNull());

	// Already there, which is what a toolkit restating its stacking on every commit asks for.
	GYRO_CHECK(store.Order(first, second));
	GYRO_CHECK(store.Find(window)->LastChild == first);
}

// A reference in somebody else's chain is refused rather than adopted, because moving the node anyway
// would reparent a subsurface into a window that never asked for it.
GYRO_TEST(SceneStore, OrderingRefusesAReferenceThatIsNotASibling)
{
	SceneStore store{ Clock };

	const EntityId window = store.CreateContainer({}, {}).value();
	const EntityId other = store.CreateContainer({}, {}).value();
	const EntityId child = store.CreateContainer(window, {}).value();
	const EntityId stranger = store.CreateContainer(other, {}).value();

	GYRO_CHECK(!store.Order(child, stranger));
	GYRO_CHECK(store.Find(window)->FirstChild == child);
	GYRO_CHECK(store.Find(other)->FirstChild == stranger);

	// Naming itself is refused too, which is what `place_above` calls a protocol error and what would
	// otherwise be a node linked after itself — a cycle in the walk that draws the world.
	GYRO_CHECK(!store.Order(child, child));
	GYRO_CHECK(store.Find(child)->NextSibling.IsNull());
}
