#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "Animation/Author/Animatable.h"
#include "Animation/Solve/Spring.h"
#include "Core/FrameSection.h"
#include "Core/Time.h"
#include "Geometry/NodeTransform.h"
#include "Publication/Publisher/Publisher.h"
#include "Publication/Reader/Reader.h"
#include "Testing/Test.h"
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

// The preorder walk, reduced to what this test asserts: compose Docs/Decisions.md decision 19's time
// scale down the tree, skip a hidden subtree whole, and evaluate a node's translation where it names
// a coefficient. Everything a real walk also does — the transform chain, culling, emission — is
// Frame's and does not exist yet.
struct Visited
{
	std::size_t Index = 0;
	float TimeScale = 1.0F;
	Vector3<double> Translation{};
};

void Walk(
	std::span<const Node> nodes,
	std::span<const Spring<Vector3<double>>> translations,
	Instant at,
	std::span<Visited> into,
	std::size_t& count
)
{
	// An explicit stack of (past, scale) rather than recursion, which is the shape decision 86 chose
	// preorder-plus-subtree-length for: descending is ++index and skipping is arithmetic.
	std::array<std::pair<std::size_t, float>, 8> stack{};
	std::size_t depth = 0;
	float scale = 1.0F;

	std::size_t index = 0;
	while (index < nodes.size())
	{
		while (depth != 0 && index >= stack[depth - 1].first)
		{
			--depth;
			scale = stack[depth].second;
		}

		const Node& node = nodes[index];

		if (node.IsHidden())
		{
			index = node.Past(index);
			continue;
		}

		const float composed = scale * node.TimeScale;

		Vector3<double> translation = node.Transform.Translation;
		if (node.IsTranslating() && node.TranslationSpring < translations.size())
		{
			translation = translations[node.TranslationSpring].Evaluate(at).Position;
		}

		into[count] = Visited{ index, composed, translation };
		++count;

		if (node.SubtreeLength != 0 && depth < stack.size())
		{
			stack[depth] = { node.Past(index), scale };
			++depth;
			scale = composed;
		}

		++index;
	}
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
	scene[Menu].TimeScale = 0.5F;
	scene[MenuPanel].SubtreeLength = 0;
	scene[Submenu].SubtreeLength = 1;
	scene[Submenu].TranslationSpring = 0;
	scene[SubmenuPanel].SubtreeLength = 0;

	// The settled model value the submenu also carries. It is redundant while the spring is active,
	// which is the point: the walk must take the coefficient and not this.
	scene[Submenu].Transform.Translation = Vector3<double>{ -1.0, -1.0, -1.0 };

	const SnapshotBuffer buffer = SnapshotPublisher{}
	                                  .Put<Spring<Vector3<double>>>(SnapshotRun::Translation, { &coefficients, 1 })
	                                  .PutNodes<Node>(scene)
	                                  .Build(1);

	const SnapshotReader reader{ buffer.Bytes() };
	GYRO_REQUIRE(reader.IsValid());

	const std::span<const Node> nodes = reader.Nodes<Node>();
	const std::span<const Spring<Vector3<double>>> translations =
		reader.Run<Spring<Vector3<double>>>(SnapshotRun::Translation);
	GYRO_REQUIRE_EQ(nodes.size(), std::size_t{ 4 });
	GYRO_REQUIRE_EQ(translations.size(), std::size_t{ 1 });

	constexpr Instant Probe = Monotonic::FromNanoseconds(120'000'000);

	// The walk runs inside the frame section, so decision 36's allocator aborts if any part of it
	// touches the heap. The comparisons come after, because a failed check formats its operands.
	std::array<Visited, 4> visited{};
	std::size_t count = 0;
	{
		const FrameSection guard;
		Walk(nodes, translations, Probe, visited, count);
	}

	GYRO_REQUIRE_EQ(count, std::size_t{ 4 });

	// The coefficient the submenu's slot named, evaluated on the frame side, is bit-identical to the
	// model's own evaluation — decision 50's promise, reached through the node rather than by index.
	const Vector3<double> fromModel = position.PresentationState(Probe).Position;
	GYRO_CHECK_EQ(visited[Submenu].Translation.X, fromModel.X);
	GYRO_CHECK_EQ(visited[Submenu].Translation.Y, fromModel.Y);

	// And a node at rest reads its inline value, which for these three is the identity.
	GYRO_CHECK_EQ(visited[MenuPanel].Translation.X, 0.0);

	// Decision 19's scale composes down the tree and not across siblings: the menu's half applies to
	// everything under it, and the menu itself keeps its own.
	GYRO_CHECK_EQ(visited[Menu].TimeScale, 0.5F);
	GYRO_CHECK_EQ(visited[MenuPanel].TimeScale, 0.5F);
	GYRO_CHECK_EQ(visited[SubmenuPanel].TimeScale, 0.5F);
}

GYRO_TEST(SceneRoundTrip, AHiddenSubtreeCostsOneStep)
{
	std::vector<Node> scene(4);
	scene[Menu].SubtreeLength = 3;
	scene[Submenu].SubtreeLength = 1;
	scene[Submenu].Flags |= Node::Hidden;

	const SnapshotBuffer buffer = SnapshotPublisher{}.PutNodes<Node>(scene).Build(1);
	const SnapshotReader reader{ buffer.Bytes() };
	GYRO_REQUIRE(reader.IsValid());

	std::array<Visited, 4> visited{};
	std::size_t count = 0;
	{
		const FrameSection guard;
		Walk(reader.Nodes<Node>(), {}, Instant{}, visited, count);
	}

	// The submenu's panel is never visited, and finding that out was an addition rather than a test —
	// which is what nine workspaces with one visible costs on every frame.
	GYRO_REQUIRE_EQ(count, std::size_t{ 2 });
	GYRO_CHECK_EQ(visited[0].Index, std::size_t{ Menu });
	GYRO_CHECK_EQ(visited[1].Index, std::size_t{ MenuPanel });
}
