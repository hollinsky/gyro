#include <cstddef>
#include <span>
#include <vector>

#include "Animation/Author/Animatable.h"
#include "Animation/Solve/Spring.h"
#include "Core/Clock.h"
#include "Core/FrameSection.h"
#include "Core/Time.h"
#include "Frame/Evaluator.h"
#include "Geometry/AxisTransform.h"
#include "Geometry/NodeTransform.h"
#include "Publication/Publisher/Publisher.h"
#include "Publication/Reader/Reader.h"
#include "Testing/Test.h"
#include "World/Content.h"
#include "World/Node.h"

// The structural half of the crossing, and it stands on both sides for the same reason the spring
// round trip beside it does.
//
// Publication depends on neither World nor Animation, so no single module may publish a real node
// record next to a real coefficient and then walk the one to reach the other — which is the whole of
// what Docs/Decisions.md decision 86 puts on the wire. Decision 91 is why the record lives in World:
// Scene writes it, Frame walks it, and Frame may not name Scene, so the record sits below both waists
// and the waist itself carries it as bytes.
//
// **What is being proved is that the walk reaches the coefficient, not that the bytes survived.** The
// bytes surviving is the publisher's round trip and is proven beside the publisher. Here the node's
// channel slot is an index into a run the node does not point at — the direction decision 90 fixes,
// the node reaching for the spring and never the other way round — and a walk that gets that
// indirection wrong produces a plausible wrong picture rather than a failure.
//
// **The walk is `Frame`'s own and not a model of it.** *(Revised 2026-08-22.)* This file carried a
// reduced traversal until the evaluator existed, which proved the indirection against a walk that
// could drift from the one that draws. `SceneEvaluator` is now what runs here, so what is checked is
// the pair a frame actually depends on: the publisher's layout and the reader's, over records neither
// module names. Frame/Evaluator.Test.cpp is where the walk's own behaviour is asserted, against bytes
// it assembles itself — because the frame half may not name the publisher.

namespace
{
// Docs/Decisions.md decision 86's encoding at its smallest interesting size, and the same worked
// example World/Node.Test.cpp uses: a menu, its panel, its open submenu, and that submenu's panel.
// The submenu is the one thing moving.
enum Index : std::size_t
{
	Menu = 0,
	MenuPanel = 1,
	Submenu = 2,
	SubmenuPanel = 3,
};

constexpr PixelSize<DeviceSpace> Screen{ 1920, 1080 };

// A leaf that draws, so that what the walk reaches shows up as an item rather than only as a visit.
[[nodiscard]] Node Panel(float width, float height)
{
	Node node{};

	node.Kind = NodeKind::Image;
	node.Content = 0;
	node.Extent = { width, height };

	return node;
}
} // namespace

GYRO_TEST(SceneRoundTrip, TheWalkReachesTheCoefficientTheNodeNames)
{
	// Authored through the ordinary path, so what is published is what a commit would have published.
	Animatable<Vector3<double>> position{ Vector3<double>{ 3.0, 4.0, 0.0 } };
	position.AnimateTo(Vector3<double>{ 40.0, 4.0, 0.0 }, ParametersFromResponse(0.4, 1.0), Instant{});

	const Spring<Vector3<double>> coefficients = position.Coefficients();

	std::vector<Node> scene(4);
	scene[Menu].SubtreeLength = 3;
	scene[MenuPanel] = Panel(100.0F, 40.0F);
	scene[Submenu].SubtreeLength = 1;
	scene[Submenu].TranslationSpring = 0;
	scene[SubmenuPanel] = Panel(80.0F, 30.0F);

	// The settled model value the submenu also carries. It is redundant while the spring is active,
	// which is the point: the walk must take the coefficient and not this.
	scene[Submenu].Transform.Translation = Vector3<double>{ -1.0, -1.0, -1.0 };

	const ImageContent content{};
	const OutputAdapter placement = OutputAdapter::Identity();

	const SnapshotBuffer buffer = SnapshotPublisher{}
	                                  .Put<Spring<Vector3<double>>>(SnapshotRun::Translation, { &coefficients, 1 })
	                                  .PutNodes<Node>(scene)
	                                  .PutImages<ImageContent>({ &content, 1 })
	                                  .PutViews<OutputAdapter>({ &placement, 1 })
	                                  .Build(1);

	const SnapshotReader reader{ buffer.Bytes() };
	GYRO_REQUIRE(reader.IsValid());
	GYRO_REQUIRE_EQ(reader.Nodes<Node>().size(), std::size_t{ 4 });

	constexpr Instant Probe = Monotonic::FromNanoseconds(120'000'000);

	MonotonicClock clock;
	SceneEvaluator evaluator{ clock };

	// The walk runs inside the frame section, so decision 36's allocator aborts if any part of it
	// touches the heap. The comparisons come after, because a failed check formats its operands.
	DrawList list{};
	{
		const FrameSection guard;
		list = evaluator.Evaluate(
			{ .Snapshot = reader, .Output = 0, .Outputs = 1, .Resolution = Screen, .Presentation = Probe }
		);
	}

	// The menu's panel and the submenu's, in preorder. The two containers draw nothing, which is what
	// decision 95 gives the kind for.
	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 2 });

	// The coefficient the submenu's slot named, evaluated on the frame side, placed the panel under it
	// — decision 50's promise, reached through the node rather than by index, and arriving as pixels.
	const Vector3<double> fromModel = position.PresentationState(Probe).Position;

	GYRO_CHECK_EQ(list.Items[1].Shape.Bounds().Left(), static_cast<float>(fromModel.X));
	GYRO_CHECK_EQ(list.Items[1].Shape.Bounds().Top(), static_cast<float>(fromModel.Y));

	// And a node at rest reads its inline value, which for the menu's panel is the identity — where
	// the submenu's model value, had the walk taken it, would have put this one at -1 as well.
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds().Left(), 0.0F);
	GYRO_CHECK_EQ(list.Items[0].Extent, Size<SurfaceSpace>{ 100.0F, 40.0F });
}

GYRO_TEST(SceneRoundTrip, AHiddenSubtreeCostsOneStep)
{
	std::vector<Node> scene(4);
	scene[Menu].SubtreeLength = 3;
	scene[MenuPanel] = Panel(100.0F, 40.0F);
	scene[Submenu].SubtreeLength = 1;
	scene[Submenu].Flags |= Node::Hidden;
	scene[SubmenuPanel] = Panel(80.0F, 30.0F);

	const ImageContent content{};
	const OutputAdapter placement = OutputAdapter::Identity();

	const SnapshotBuffer buffer = SnapshotPublisher{}
	                                  .PutNodes<Node>(scene)
	                                  .PutImages<ImageContent>({ &content, 1 })
	                                  .PutViews<OutputAdapter>({ &placement, 1 })
	                                  .Build(1);

	const SnapshotReader reader{ buffer.Bytes() };
	GYRO_REQUIRE(reader.IsValid());

	MonotonicClock clock;
	SceneEvaluator evaluator{ clock };

	DrawList list{};
	{
		const FrameSection guard;
		list = evaluator.Evaluate({ .Snapshot = reader, .Output = 0, .Outputs = 1, .Resolution = Screen });
	}

	// The submenu's panel is never visited, and finding that out was an addition rather than a test —
	// which is what nine workspaces with one visible costs on every frame.
	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(list.Items[0].Extent, Size<SurfaceSpace>{ 100.0F, 40.0F });
}
