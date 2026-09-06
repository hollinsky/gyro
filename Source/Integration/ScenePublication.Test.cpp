#include <cstddef>
#include <span>
#include <vector>

#include "Animation/Author/Animatable.h"
#include "Animation/Author/Bundle.h"
#include "Animation/Author/Motion.h"
#include "Animation/Author/Retarget.h"
#include "Animation/Solve/Spring.h"
#include "Core/Clock.h"
#include "Core/FrameSection.h"
#include "Core/Time.h"
#include "Frame/Evaluator.h"
#include "Geometry/AxisTransform.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Scale.h"
#include "Publication/Publisher/Publisher.h"
#include "Publication/Reader/Reader.h"
#include "Scene/Commit.h"
#include "Scene/Output.h"
#include "Scene/Serializer.h"
#include "Scene/Store.h"
#include "Testing/Test.h"
#include "World/Content.h"
#include "World/Node.h"

// The other half of the crossing, and it stands on both sides for the reason
// Source/Integration/SceneRoundTrip.Test.cpp does.
//
// That file builds Docs/Decisions.md decision 86's encoding by hand — four node records, a spring, a
// content record, a placement — and proves the frame walk reaches the coefficient the node names. It
// had to build them by hand, because when it was written nothing authored one. This file is the same
// scene authored the way a shell would author it, and what it asserts is that the two are the same
// picture.
//
// **Which is the only statement about `Scene` a test can make that is worth much.** The serializer's
// own tests, beside it in Source/Scene, check what the runs contain — subtree lengths, the active set,
// a translated content index. All of that could be self-consistently wrong: a length convention off by
// one, or an index into the run the store keeps rather than the run that crossed, produces a snapshot
// that validates and draws the wrong thing. What settles it is the walk on the far side agreeing,
// against bytes it was handed by two producers that share no code.
//
// **No module may do this**, which is why it is here: `Frame` may not name `Scene`, and `Scene` may
// not name `Frame`. Only the composition root wires an author to a walk, and this is the composition
// root's test.

namespace
{
constexpr PixelSize<DeviceSpace> Screen{ 1920, 1080 };

// The scene's clock, parked at the epoch so that the origins written below are the origins that land: a
// commit clamps its `t₀` forward to dispatch's own now, which against a running clock would be every
// origin in this file.
ManualClock Clock;

// A leaf that draws, so that what the walk reaches shows up as an item rather than only as a visit.
[[nodiscard]] Node Panel(float width, float height)
{
	Node node{};

	node.Kind = NodeKind::Image;
	node.Content = 0;
	node.Extent = { width, height };

	return node;
}

[[nodiscard]] NodeProperties PanelProperties(float width, float height)
{
	return { .Extent = { width, height } };
}

// The same four-node menu, authored through the store: a menu, its panel, its open submenu, and that
// submenu's panel. The submenu is the one thing moving, and it is retargeted through the ordinary
// path, so what is published is what a commit would have published.
struct Authored
{
	SceneStore Store{ Clock };
	EntityId Submenu{};
};

[[nodiscard]] Authored Author(Instant origin)
{
	Authored authored;

	const EntityId menu = authored.Store.CreateContainer({}, {}).value();
	[[maybe_unused]] const EntityId panel =
		authored.Store.CreateImage(menu, PanelProperties(100.0F, 40.0F), ImageContent{}).value();

	authored.Submenu = authored.Store.CreateContainer(menu, NodeProperties{ .Position = { 3.0, 4.0, 0.0 } }).value();
	[[maybe_unused]] const EntityId inner =
		authored.Store.CreateImage(authored.Submenu, PanelProperties(80.0F, 30.0F), ImageContent{}).value();

	{
		SceneCommit commit{ authored.Store, CommitAuthor::Shell, origin, Transition::MatchedMove };

		commit.Move(authored.Submenu, { 40.0, 4.0, 0.0 });
	}

	const SceneOutput primary{ .Density = Scale::FromInteger(1), .Grid = Screen };
	const SceneOutput outputs[] = { primary };

	authored.Store.SetOutputs(outputs);

	return authored;
}

// The same scene as records, which is what Source/Integration/SceneRoundTrip.Test.cpp assembles.
[[nodiscard]] SnapshotBuffer ByHand(const Spring<Vector3<double>>& coefficients)
{
	std::vector<Node> scene(4);

	scene[0].SubtreeLength = 3;
	scene[1] = Panel(100.0F, 40.0F);
	scene[2].SubtreeLength = 1;
	scene[2].TranslationSpring = 0;
	scene[2].Transform.Translation = Vector3<double>{ 40.0, 4.0, 0.0 };
	scene[3] = Panel(80.0F, 30.0F);

	// Two image nodes and two records, because the publisher emits one per node that draws rather than
	// one per distinct payload — which is what makes the hand-built run the same length as the authored
	// one and the indices comparable.
	const ImageContent content[] = { ImageContent{}, ImageContent{} };

	scene[3].Content = 1;

	const OutputAdapter placement = OutputAdapter::Identity();

	return SnapshotPublisher{}
	    .Put<Spring<Vector3<double>>>(SnapshotRun::Translation, { &coefficients, 1 })
	    .PutNodes<Node>(scene)
	    .PutImages<ImageContent>(content)
	    .PutViews<OutputAdapter>({ &placement, 1 })
	    .Build(1);
}

// One output's frame, evaluated inside the frame section so decision 36's allocator aborts if any part
// of the walk touches the heap. The items are copied out because a `DrawList` is a span into the
// evaluator's own arena, and two lists have to exist at once to be compared.
[[nodiscard]] std::vector<DrawItem> Evaluate(const SnapshotReader& reader, Instant probe)
{
	MonotonicClock clock;
	SceneEvaluator evaluator{ clock };

	DrawList list{};
	{
		const FrameSection guard;
		list = evaluator.Evaluate(
			{ .Snapshot = reader, .Output = 0, .Outputs = 1, .Resolution = Screen, .Presentation = probe }
		);
	}

	return { list.Items.begin(), list.Items.end() };
}
} // namespace

GYRO_TEST(ScenePublication, TheAuthoredSceneDrawsWhatTheHandBuiltOneDraws)
{
	constexpr Instant Origin = Instant{};
	constexpr Instant Probe = Monotonic::FromNanoseconds(120'000'000);

	Authored authored = Author(Origin);

	SceneSerializer serializer;
	const SnapshotBuffer published = serializer.Serialize(authored.Store).Build(1);

	const SnapshotReader fromScene{ published.Bytes() };
	GYRO_REQUIRE(fromScene.IsValid());
	GYRO_REQUIRE_EQ(fromScene.Nodes<Node>().size(), std::size_t{ 4 });

	const SnapshotBuffer byHand = ByHand(authored.Store.Find(authored.Submenu)->Translation.Coefficients());

	const SnapshotReader fromRecords{ byHand.Bytes() };
	GYRO_REQUIRE(fromRecords.IsValid());

	const std::vector<DrawItem> authoredItems = Evaluate(fromScene, Probe);
	const std::vector<DrawItem> handItems = Evaluate(fromRecords, Probe);

	// The menu's panel and the submenu's, in preorder. The two containers draw nothing, which is what
	// decision 95 gives the kind for.
	GYRO_REQUIRE_EQ(authoredItems.size(), std::size_t{ 2 });
	GYRO_REQUIRE_EQ(handItems.size(), authoredItems.size());

	for (std::size_t index = 0; index < authoredItems.size(); ++index)
	{
		GYRO_CHECK(authoredItems[index] == handItems[index]);
	}

	// And the item the walk reached through the coefficient is where the model says it is, which is the
	// half of decision 50's promise this file adds to the one beside it: the spring the store published
	// is the spring the store is holding, evaluated on the frame side and arriving as pixels.
	const Vector3<double> fromModel =
		authored.Store.Find(authored.Submenu)->Translation.PresentationState(Probe).Position;

	GYRO_CHECK_EQ(authoredItems[1].Shape.Bounds().Left(), static_cast<float>(fromModel.X));
	GYRO_CHECK_EQ(authoredItems[1].Shape.Bounds().Top(), static_cast<float>(fromModel.Y));
	GYRO_CHECK_EQ(authoredItems[0].Shape.Bounds().Left(), 0.0F);
	GYRO_CHECK_EQ(authoredItems[0].Extent, Size<SurfaceSpace>{ 100.0F, 40.0F });
}

GYRO_TEST(ScenePublication, AHiddenSubtreeCostsOneStepWhereverItWasAuthored)
{
	SceneStore store{ Clock };

	const EntityId menu = store.CreateContainer({}, {}).value();
	GYRO_REQUIRE(store.CreateImage(menu, NodeProperties{ .Extent = { 100.0F, 40.0F } }, ImageContent{}).has_value());

	const EntityId submenu = store.CreateContainer(menu, NodeProperties{ .Flags = Node::Hidden }).value();
	GYRO_REQUIRE(store.CreateImage(submenu, NodeProperties{ .Extent = { 80.0F, 30.0F } }, ImageContent{}).has_value());

	const SceneOutput outputs[] = { SceneOutput{ .Density = Scale::FromInteger(1), .Grid = Screen } };
	store.SetOutputs(outputs);

	SceneSerializer serializer;
	const SnapshotBuffer published = serializer.Serialize(store).Build(1);

	const SnapshotReader reader{ published.Bytes() };
	GYRO_REQUIRE(reader.IsValid());

	// The store publishes a hidden subtree in full and the walk skips it in one addition — which is the
	// arrangement that makes nine workspaces with one visible cost a step rather than a traversal. The
	// authoring side does not prune, because what is hidden on one output is not hidden on another.
	GYRO_REQUIRE_EQ(reader.Nodes<Node>().size(), std::size_t{ 4 });

	const std::vector<DrawItem> items = Evaluate(reader, Instant{});

	GYRO_REQUIRE_EQ(items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(items[0].Extent, Size<SurfaceSpace>{ 100.0F, 40.0F });
}
