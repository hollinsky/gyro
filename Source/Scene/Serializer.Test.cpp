#include "Scene/Serializer.h"

#include <cstddef>
#include <cstdint>
#include <span>

#include "Animation/Author/Bundle.h"
#include "Animation/Author/Motion.h"
#include "Animation/Author/Retarget.h"
#include "Core/Clock.h"
#include "Core/Time.h"
#include "Geometry/Scale.h"
#include "Publication/Publisher/Publisher.h"
#include "Publication/Reader/Reader.h"
#include "Scene/Commit.h"
#include "Scene/Entity.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Testing/Test.h"
#include "World/Content.h"
#include "World/Node.h"

// What the store becomes on the wire: Docs/Decisions.md decision 86's preorder run, with the active
// coefficients beside it and the per-kind payloads beside those.
//
// The walk that consumes this is Frame's and is asserted against it in Source/Integration. What is
// checked here is the serialisation itself, which is the half of the crossing that has an answer a
// test can write down.

namespace
{
// The scene's clock, parked at the epoch. A commit clamps its origin forward to dispatch's own now, so
// a running clock here would move every `t₀` these tests write to whenever the suite happened to run.
ManualClock Clock;

[[nodiscard]] NodeProperties Panel(float width, float height)
{
	return { .Extent = { width, height } };
}

[[nodiscard]] SceneOutput Primary()
{
	return { .Density = Scale::FromInteger(1), .Grid = { 1920, 1080 } };
}
} // namespace

GYRO_TEST(SceneSerializer, ASubtreeLengthCountsNodesAndNotChildren)
{
	SceneStore store{ Clock };

	// A menu holding its panel and an open submenu, which is holding its own — the same worked example
	// World/Node.Test.cpp and Source/Integration/SceneRoundTrip.Test.cpp use.
	const EntityId menu = store.CreateContainer({}, {}).value();
	GYRO_CHECK(store.CreateImage(menu, Panel(100.0F, 40.0F), ImageContent{}).has_value());
	const EntityId submenu = store.CreateContainer(menu, {}).value();
	GYRO_CHECK(store.CreateImage(submenu, Panel(80.0F, 30.0F), ImageContent{}).has_value());

	SceneSerializer serializer;
	serializer.Serialize(store);

	const std::span<const Node> nodes = serializer.Nodes();

	GYRO_REQUIRE_EQ(nodes.size(), std::size_t{ 4 });

	// Preorder: menu, its panel, the submenu, the submenu's panel. The lengths are run lengths over
	// nodes and the two differ the moment anything nests — the menu holds two children and three nodes.
	GYRO_CHECK_EQ(nodes[0].SubtreeLength, std::uint32_t{ 3 });
	GYRO_CHECK_EQ(nodes[1].SubtreeLength, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(nodes[2].SubtreeLength, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(nodes[3].SubtreeLength, std::uint32_t{ 0 });

	GYRO_CHECK_EQ(nodes[1].Extent, Size<SurfaceSpace, float>{ 100.0F, 40.0F });
	GYRO_CHECK_EQ(nodes[3].Extent, Size<SurfaceSpace, float>{ 80.0F, 30.0F });

	// A skip is `index += 1 + SubtreeLength`, so every subtree has to end inside the run. This is the
	// comparison the frame side makes per node, made once here against what produced it.
	for (std::size_t index = 0; index < nodes.size(); ++index)
	{
		GYRO_CHECK(nodes[index].Past(index) <= nodes.size());
	}
}

GYRO_TEST(SceneSerializer, SiblingsAtTheTopLevelAreARunAndNotAForest)
{
	SceneStore store{ Clock };

	const EntityId first = store.CreateContainer({}, {}).value();
	GYRO_CHECK(store.CreateSolid(first, Panel(4.0F, 4.0F), SolidContent{}).has_value());
	GYRO_CHECK(store.CreateContainer({}, {}).has_value());

	SceneSerializer serializer;
	serializer.Serialize(store);

	// Frame/Evaluator.h starts at index zero and runs to the end of the node run treating that span as
	// siblings, which is why the store's top level is a list and not a root: a distinguished root would
	// be a node every walk pays for and no reader needs.
	GYRO_REQUIRE_EQ(serializer.Nodes().size(), std::size_t{ 3 });
	GYRO_CHECK_EQ(serializer.Nodes()[0].SubtreeLength, std::uint32_t{ 1 });
	GYRO_CHECK_EQ(serializer.Nodes()[2].SubtreeLength, std::uint32_t{ 0 });
}

GYRO_TEST(SceneSerializer, OnlyAMovingChannelCrossesAsACoefficient)
{
	SceneStore store{ Clock };

	const EntityId still = store.CreateContainer({}, {}).value();
	const EntityId moving = store.CreateContainer({}, {}).value();

	SceneSerializer serializer;
	serializer.Serialize(store);

	// A still desktop publishes no coefficients at all, which is the whole of what the active set of
	// decision 86 buys — the alternative makes a scene where nothing is happening cost the same to
	// publish as one where everything is.
	GYRO_CHECK_EQ(serializer.ActiveTranslations(), std::size_t{ 0 });
	GYRO_CHECK(!serializer.Nodes()[0].IsTranslating());

	{
		SceneCommit commit{ store, CommitAuthor::Shell, Instant{} };

		GYRO_REQUIRE(commit.Move(moving, { 40.0, 0.0, 0.0 }, Animate(Motion::Standard)));
		GYRO_REQUIRE(commit.Fade(moving, 0.0F, Animate(Motion::Standard)));
	}

	serializer.Serialize(store);

	GYRO_REQUIRE_EQ(serializer.ActiveTranslations(), std::size_t{ 1 });
	GYRO_CHECK_EQ(serializer.ActiveOpacities(), std::size_t{ 1 });
	GYRO_CHECK_EQ(serializer.ActiveScales(), std::size_t{ 0 });

	// The index is a position within its *own* channel's array, which is decision 90's one run per
	// channel: pointing a scale index at a rotation spring stops being expressible rather than caught.
	GYRO_CHECK_EQ(serializer.Nodes()[1].TranslationSpring, std::uint32_t{ 0 });
	GYRO_CHECK_EQ(serializer.Nodes()[1].OpacitySpring, std::uint32_t{ 0 });
	GYRO_CHECK(!serializer.Nodes()[1].IsScaling());

	// And the entity that is not moving still names nothing, which is decision 98's statement that a
	// node at rest is a node naming no coefficient.
	GYRO_CHECK(!serializer.Nodes()[0].IsTranslating());
	GYRO_CHECK(still == store.FirstRoot());
}

GYRO_TEST(SceneSerializer, TheContentIndexIsTranslatedAndNeverForwarded)
{
	SceneStore store{ Clock };

	ImageContent first{};
	first.Frame = { { 1.0F, 0.0F }, { 2.0F, 2.0F } };

	ImageContent second{};
	second.Frame = { { 5.0F, 0.0F }, { 2.0F, 2.0F } };

	const EntityId container = store.CreateContainer({}, {}).value();
	GYRO_CHECK(store.CreateImage(container, Panel(2.0F, 2.0F), first).has_value());
	GYRO_CHECK(store.CreateImage({}, Panel(2.0F, 2.0F), second).has_value());

	SceneSerializer serializer;
	const SnapshotPublisher& publisher = serializer.Serialize(store);

	const SnapshotBuffer buffer = publisher.Build(1);
	const SnapshotReader reader{ buffer.Bytes() };

	GYRO_REQUIRE(reader.IsValid());

	const std::span<const Node> nodes = reader.Nodes<Node>();
	const std::span<const ImageContent> images = reader.Images<ImageContent>();

	GYRO_REQUIRE_EQ(nodes.size(), std::size_t{ 3 });
	GYRO_REQUIRE_EQ(images.size(), std::size_t{ 2 });

	// The published run is built by the walk that emits it, so an entity's own content index and the
	// one that crosses agree only by accident. Here they happen to, which is why the assertion is that
	// each node reaches *its own* record rather than that the numbers match.
	GYRO_CHECK(images[nodes[1].Content] == first);
	GYRO_CHECK(images[nodes[2].Content] == second);
	GYRO_CHECK_EQ(nodes[0].Content, NoContent);
}

GYRO_TEST(SceneSerializer, AReferenceResolvesBackwardsOrNotAtAll)
{
	SceneStore store{ Clock };

	const EntityId window = store.CreateImage({}, Panel(100.0F, 40.0F), ImageContent{}).value();
	const EntityId tile = store.CreateReference({}, Panel(100.0F, 40.0F), window).value();

	SceneSerializer serializer;
	serializer.Serialize(store);

	GYRO_REQUIRE_EQ(serializer.Nodes().size(), std::size_t{ 2 });

	// Decision 95 requires the target's index to be lower than the reference's, which makes a cycle
	// unrepresentable rather than something the frame thread has to detect — and decision 90 declines
	// to rest on a detector, since an unbounded walk inside the frame section on a SCHED_FIFO thread
	// ends with RLIMIT_RTTIME taking every session's UI at once.
	GYRO_CHECK(serializer.Nodes()[1].IsReference());
	GYRO_CHECK_EQ(serializer.Nodes()[1].Content, std::uint32_t{ 0 });
	GYRO_CHECK(tile != window);
}

GYRO_TEST(SceneSerializer, AForwardReferenceNamesNothing)
{
	SceneStore store{ Clock };

	// Authored the wrong way round: the tile is emitted before the window it presents, because the top
	// level is a list in the order it was built. What the overview actually does is put the real
	// windows near the top of the run under a hidden container and the thumbnails below them.
	const EntityId tile =
		store.CreateReference({}, Panel(100.0F, 40.0F), EntityId{ .Index = 1, .Generation = 2 }).value();
	const EntityId window = store.CreateImage({}, Panel(100.0F, 40.0F), ImageContent{}).value();

	SceneSerializer serializer;
	serializer.Serialize(store);

	GYRO_REQUIRE_EQ(serializer.Nodes().size(), std::size_t{ 2 });

	// The id is live and the node index is not below the reference's, so it names nothing rather than
	// naming a node the walk would have to bound. Frame/Evaluator.h refuses the expansion a second time
	// on the same comparison; this is the side that makes the byte unwritable.
	GYRO_CHECK(store.IsLive(window));
	GYRO_CHECK(store.Find(tile)->Target == window);
	GYRO_CHECK_EQ(serializer.Nodes()[0].Content, NoContent);
}

GYRO_TEST(SceneSerializer, TheViewsRunIsOnePlacementPerOutputInOutputOrder)
{
	SceneStore store{ Clock };

	GYRO_CHECK(store.CreateContainer({}, {}).has_value());

	SceneOutput secondary = Primary();
	secondary.Bounds = { { 1920.0, 0.0 }, { 1920.0, 1080.0 } };

	const SceneOutput both[] = { Primary(), secondary };
	store.SetOutputs(both);

	SceneSerializer serializer;
	const SnapshotBuffer buffer = serializer.Serialize(store).Build(1);
	const SnapshotReader reader{ buffer.Bytes() };

	GYRO_REQUIRE(reader.IsValid());

	const std::span<const OutputAdapter> views = reader.Views<OutputAdapter>();

	GYRO_REQUIRE_EQ(views.size(), std::size_t{ 2 });
	GYRO_CHECK(views[0] == OutputAdapter::Identity());
	GYRO_CHECK_EQ(views[1].Translation.X, -1920.0);
}

GYRO_TEST(SceneSerializer, ASecondSerialisationKeepsNothingOfTheFirst)
{
	SceneStore store{ Clock };

	const EntityId moving = store.CreateContainer({}, {}).value();
	const EntityId panel = store.CreateImage(moving, Panel(10.0F, 10.0F), ImageContent{}).value();

	{
		SceneCommit commit{ store, CommitAuthor::Shell, Instant{} };

		GYRO_REQUIRE(commit.Move(moving, { 40.0, 0.0, 0.0 }, Animate(Motion::Standard)));
	}

	SceneSerializer serializer;
	serializer.Serialize(store);

	GYRO_REQUIRE_EQ(serializer.ActiveTranslations(), std::size_t{ 1 });

	// The same channel written again with no motion, which is how a commit spells *land it here*. What
	// the serialisation has to notice is that the run it staged last time is not this scene's.
	{
		SceneCommit commit{ store, CommitAuthor::Shell, Instant{} };

		GYRO_REQUIRE(commit.Move(moving, { 40.0, 0.0, 0.0 }, Immediate()));
	}

	const SnapshotBuffer buffer = serializer.Serialize(store).Build(2);
	const SnapshotReader reader{ buffer.Bytes() };

	GYRO_REQUIRE(reader.IsValid());

	// The publisher is reused across calls, so a run left unstaged would be the previous scene's — a
	// node pointing at a spring that belonged to something else, which draws a plausible wrong picture
	// rather than failing.
	GYRO_CHECK(reader.Run<Spring<Vector3<double>>>(SnapshotRun::Translation).empty());
	GYRO_REQUIRE_EQ(reader.Nodes<Node>().size(), std::size_t{ 2 });
	GYRO_CHECK(!reader.Nodes<Node>()[0].IsTranslating());
	GYRO_CHECK_EQ(reader.Images<ImageContent>().size(), std::size_t{ 1 });
	GYRO_CHECK(panel != moving);
}
