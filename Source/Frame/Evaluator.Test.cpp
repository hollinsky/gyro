#include "Frame/Evaluator.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "Animation/Solve/Spring.h"
#include "Core/Clock.h"
#include "Core/FrameSection.h"
#include "Core/Session.h"
#include "Core/Time.h"
#include "Frame/Assign.h"
#include "Geometry/AxisTransform.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Space.h"
#include "Publication/Snapshot.h"
#include "Testing/Test.h"
#include "World/Content.h"
#include "World/Exit.h"
#include "World/Node.h"
#include "World/Root.h"

// The walk, asserted by what each failure looks like on a screen rather than by what it does to an
// array.
//
// A subtree walked in the wrong order is a menu behind the window that opened it. A group whose count
// is off by its nesting is a fade that leaves half a submenu at full opacity. A settled node that does
// not snap is a hairline of wallpaper between two tiled windows. A malformed length that is trusted is
// an unbounded traversal on a `SCHED_FIFO` thread, which is every session's UI at once. Those are the
// assertions below; the arithmetic ones belong to Frame/Projection.Test.cpp and
// Geometry/NodeTransform.Test.cpp, which this deliberately does not repeat.
//
// **The snapshot is assembled here rather than published**, and that is the layering rather than a
// shortcut: `SnapshotPublisher` is a dispatch half and the frame side may not name it, which
// `CheckLayering.cmake` enforces. So this file writes the bytes the way the wire has them and reads
// them back through `SnapshotReader`, which is what the walk actually consumes.
// Source/Integration/SceneRoundTrip.Test.cpp is where the two sides are proved to agree.

namespace
{
// One published snapshot, laid out by hand. The store must outlive every reader taken from it, which
// is why these live on the stack of the test that builds one.
class Wire
{
public:
	template<typename T>
	void Put(SnapshotRun which, std::span<const T> elements)
	{
		Stage(m_Runs[RunIndex(which)], elements);
	}

	template<typename T>
	void PutNodes(std::span<const T> elements)
	{
		Stage(m_Named[0], elements);
	}

	template<typename T>
	void PutViews(std::span<const T> elements)
	{
		Stage(m_Named[1], elements);
	}

	template<typename T>
	void PutImages(std::span<const T> elements)
	{
		Stage(m_Named[2], elements);
	}

	template<typename T>
	void PutSolids(std::span<const T> elements)
	{
		Stage(m_Named[3], elements);
	}

	template<typename T>
	void PutRoots(std::span<const T> elements)
	{
		Stage(m_Named[4], elements);
	}

	template<typename T>
	void PutSessions(std::span<const T> elements)
	{
		Stage(m_Named[5], elements);
	}

	template<typename T>
	void PutExits(std::span<const T> elements)
	{
		Stage(m_Named[6], elements);
	}

	[[nodiscard]] SnapshotReader Read(std::uint64_t sequence = 1)
	{
		std::size_t cursor = sizeof(SnapshotHeader);

		for (Staged& run : m_Runs)
		{
			cursor = Place(run, cursor);
		}

		for (Staged& run : m_Named)
		{
			cursor = Place(run, cursor);
		}

		SnapshotHeader header{};
		header.Sequence = sequence;
		header.ByteSize = static_cast<std::uint32_t>(cursor);

		for (std::size_t index = 0; index < SnapshotRunCount; ++index)
		{
			header.Runs[index] = m_Runs[index].Entry;
		}

		header.Nodes = m_Named[0].Entry;
		header.Views = m_Named[1].Entry;
		header.Images = m_Named[2].Entry;
		header.Solids = m_Named[3].Entry;
		header.Roots = m_Named[4].Entry;
		header.Sessions = m_Named[5].Entry;
		header.Exits = m_Named[6].Entry;

		m_Store.assign((cursor + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t), std::max_align_t{});

		std::byte* const base = reinterpret_cast<std::byte*>(m_Store.data());
		std::memcpy(base, &header, sizeof(SnapshotHeader));

		for (const Staged& run : m_Runs)
		{
			Copy(base, run);
		}

		for (const Staged& run : m_Named)
		{
			Copy(base, run);
		}

		return SnapshotReader{ std::span<const std::byte>{ base, cursor } };
	}

private:
	struct Staged
	{
		std::vector<std::byte> Bytes;
		RunEntry Entry;
	};

	template<typename T>
	static void Stage(Staged& run, std::span<const T> elements)
	{
		run.Bytes.resize(elements.size() * sizeof(T));

		if (!elements.empty())
		{
			std::memcpy(run.Bytes.data(), elements.data(), run.Bytes.size());
		}

		run.Entry = { 0, static_cast<std::uint32_t>(elements.size()), sizeof(T), alignof(T) };
	}

	[[nodiscard]] static std::size_t Place(Staged& run, std::size_t cursor) noexcept
	{
		if (run.Entry.Count == 0)
		{
			return cursor;
		}

		const std::size_t at = Detail::AlignUp(cursor, run.Entry.ElementAlign);
		run.Entry.Offset = static_cast<std::uint32_t>(at);

		return at + run.Bytes.size();
	}

	static void Copy(std::byte* base, const Staged& run) noexcept
	{
		if (run.Entry.Count != 0)
		{
			std::memcpy(base + run.Entry.Offset, run.Bytes.data(), run.Bytes.size());
		}
	}

	std::array<Staged, SnapshotRunCount> m_Runs{};
	std::array<Staged, 7> m_Named{};
	std::vector<std::max_align_t> m_Store;
};

// A clock that moves by a fixed amount every time it is read, so that a walk which reads it twice
// reports exactly one tick. `ManualClock` would report zero, which is indistinguishable from an
// evaluator that never measured itself.
class TickingClock final : public IClock
{
public:
	static constexpr Duration Tick = std::chrono::microseconds{ 7 };

	[[nodiscard]] Instant Now() const noexcept override
	{
		m_Now = Advanced(m_Now, Tick);

		return m_Now;
	}

private:
	mutable Instant m_Now{};
};

constexpr PixelSize<DeviceSpace> Screen{ 1920, 1080 };

// The world shown one-to-one from the global origin, which every test below varies from rather than
// happens to have.
[[nodiscard]] OutputAdapter Placement()
{
	return OutputAdapter::Identity();
}

[[nodiscard]] Node Leaf(NodeKind kind, std::uint32_t content, double x, double y, float width, float height)
{
	Node node{};

	node.Kind = kind;
	node.Content = content;
	node.Transform.Translation = { x, y, 0.0 };
	node.Extent = { width, height };

	return node;
}

[[nodiscard]] Node Image(std::uint32_t content, double x, double y, float width = 100.0F, float height = 50.0F)
{
	return Leaf(NodeKind::Image, content, x, y, width, height);
}

[[nodiscard]] Node Container(std::uint32_t subtree, double x = 0.0, double y = 0.0)
{
	Node node{};

	node.SubtreeLength = subtree;
	node.Transform.Translation = { x, y, 0.0 };

	return node;
}

[[nodiscard]] ImageContent Texel(std::uint32_t id)
{
	ImageContent content{};

	content.Texture = TextureId{ id, 1 };

	return content;
}

// One evaluation of one scene on one output, which is what every test here asks for.
[[nodiscard]] EvaluateRequest Frame(const SnapshotReader& snapshot, Instant at = {})
{
	return { .Snapshot = snapshot, .Output = 0, .Outputs = 1, .Resolution = Screen, .Presentation = at };
}

[[nodiscard]] const DrawGroup* AsGroup(const DrawItem& item)
{
	return std::get_if<DrawGroup>(&item.Content);
}

[[nodiscard]] const DrawTexture* AsTexture(const DrawItem& item)
{
	return std::get_if<DrawTexture>(&item.Content);
}
} // namespace

GYRO_TEST(Evaluator, OneImageNodeIsOneItem)
{
	Wire wire;
	const std::array nodes{ Image(0, 10.0, 20.0) };
	const std::array images{ Texel(7) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });

	const DrawItem& item = list.Items[0];
	const DrawTexture* const texture = AsTexture(item);

	GYRO_REQUIRE(texture != nullptr);
	GYRO_CHECK_EQ(texture->Texture, TextureId{ 7, 1 });
	GYRO_CHECK_EQ(item.Extent, Size<SurfaceSpace>{ 100.0F, 50.0F });
	GYRO_CHECK_EQ(item.Shape.Bounds(), Rect<DeviceSpace>::FromEdges({ 10.0F, 20.0F }, { 110.0F, 70.0F }));
	GYRO_CHECK_EQ(item.Opacity, 1.0F);

	// Decision 94's figure, and the reason the evaluator holds a clock at all.
	GYRO_CHECK_EQ(list.EvaluateCost, TickingClock::Tick);
}

GYRO_TEST(Evaluator, AHiddenSubtreeIsNotWalked)
{
	Wire wire;
	std::array nodes{ Container(2), Image(0, 0.0, 0.0), Image(0, 0.0, 0.0), Image(0, 300.0, 0.0) };
	nodes[0].Flags = Node::Hidden;

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	// The two nodes under the hidden container are gone and the sibling after it is not, which is the
	// whole of what nine workspaces with one visible costs.
	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds().Left(), 300.0F);
}

GYRO_TEST(Evaluator, AParentsPixelsAreBeneathItsChildren)
{
	Wire wire;
	// A node with subnodes is a container in decision 95's vocabulary, but the ordering being asserted
	// is the one that matters for a parent that draws: preorder is the painter's order, so the parent
	// lands first and the child composites over it.
	std::array nodes{ Image(0, 0.0, 0.0), Image(1, 5.0, 5.0) };
	nodes[0].SubtreeLength = 1;

	const std::array images{ Texel(1), Texel(2) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(AsTexture(list.Items[0])->Texture, TextureId{ 1, 1 });
	GYRO_CHECK_EQ(AsTexture(list.Items[1])->Texture, TextureId{ 2, 1 });

	// The child's transform is in the parent's space, so its five units are five more than the
	// parent's rather than five from the output's origin.
	GYRO_CHECK_EQ(list.Items[1].Shape.Bounds().Left(), 5.0F);
}

GYRO_TEST(Evaluator, AChildComposesWithItsParentsTransform)
{
	Wire wire;
	std::array nodes{ Container(1, 40.0, 60.0), Image(0, 5.0, 5.0) };

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds(), Rect<DeviceSpace>::FromEdges({ 45.0F, 65.0F }, { 145.0F, 115.0F }));
}

GYRO_TEST(Evaluator, AGroupCountsItsWholeRunAndSitsOnItsSubtreesBound)
{
	Wire wire;
	std::array nodes{ Container(2), Image(0, 0.0, 0.0), Image(0, 300.0, 200.0) };
	nodes[0].Flags = Node::Group;

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 3 });

	const DrawGroup* const group = AsGroup(list.Items[0]);

	GYRO_REQUIRE(group != nullptr);
	GYRO_CHECK_EQ(group->Count, std::uint32_t{ 2 });

	// The offscreen sits at the subtree's screen-space bound, which is a fact about the members and
	// therefore not knowable when the item was emitted. A group placed at anything smaller clips the
	// window it was supposed to fade.
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds(), Rect<DeviceSpace>::FromEdges({ 0.0F, 0.0F }, { 400.0F, 250.0F }));
}

GYRO_TEST(Evaluator, ANestedGroupIsInsideItsParentsRun)
{
	Wire wire;
	// A menu, its panel, its open submenu, and that submenu's panel — the worked example decision 86
	// is written against, with both menus declaring a group.
	std::array nodes{ Container(3), Image(0, 0.0, 0.0), Container(1, 20.0, 20.0), Image(0, 0.0, 0.0) };
	nodes[0].Flags = Node::Group;
	nodes[2].Flags = Node::Group;

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	// outer group, the menu's panel, inner group, the submenu's panel.
	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 4 });

	// `Count` is the length of the run and not a child count, so the outer group holds three items
	// where it has two children. The two readings differ the moment anything nests, and the child
	// count is the reading that leaves a submenu at full opacity through its parent's fade.
	GYRO_CHECK_EQ(AsGroup(list.Items[0])->Count, std::uint32_t{ 3 });
	GYRO_CHECK_EQ(AsGroup(list.Items[2])->Count, std::uint32_t{ 1 });
}

GYRO_TEST(Evaluator, AGroupSpendsItsOpacityOnceAndAContainerSpendsItPerNode)
{
	Wire wire;
	std::array grouped{ Container(1), Image(0, 0.0, 0.0) };
	grouped[0].Flags = Node::Group;
	grouped[0].Opacity = 0.5F;

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ grouped });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 2 });

	// Decision 60 in two numbers: the fade is spent at the offscreen and the member is drawn opaque
	// into it. A member also at 0.5 is the double-dimming that makes a cross-fade look wrong at both
	// ends of the transition.
	GYRO_CHECK_EQ(list.Items[0].Opacity, 0.5F);
	GYRO_CHECK_EQ(list.Items[1].Opacity, 1.0F);

	Wire plain;
	std::array ungrouped{ Container(1), Image(0, 0.0, 0.0) };
	ungrouped[0].Opacity = 0.5F;
	ungrouped[1].Opacity = 0.5F;

	plain.PutNodes(std::span<const Node>{ ungrouped });
	plain.PutImages(std::span<const ImageContent>{ images });
	plain.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader second = plain.Read();
	SceneEvaluator other{ clock };
	const DrawList flat = other.Evaluate(Frame(second));

	// Without a group there is no offscreen to spend it at, so the alpha multiplies down the tree —
	// which is exactly the reading decision 60 says a shell wanting a correct fade must avoid by
	// declaring a group.
	GYRO_REQUIRE_EQ(flat.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(flat.Items[0].Opacity, 0.25F);
}

GYRO_TEST(Evaluator, AReferenceExpandsTheSubtreeItNamesUnderItsOwnTransform)
{
	Wire wire;
	// The overview arrangement decision 88 describes: the real window near the top of the run under a
	// hidden container, and the thumbnail below it pointing back.
	std::array nodes{ Container(1), Image(0, 0.0, 0.0), Leaf(NodeKind::Reference, 1, 500.0, 400.0, 0.0F, 0.0F) };
	nodes[0].Flags = Node::Hidden;

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	// One item: the original is hidden by hiding its parent, and the reference draws it again where
	// the thumbnail is. A reference that expanded the flags rather than the node would draw nothing,
	// which is an empty overview.
	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds(), Rect<DeviceSpace>::FromEdges({ 500.0F, 400.0F }, { 600.0F, 450.0F }));
}

GYRO_TEST(Evaluator, AReferenceThatPointsForwardIsNotExpanded)
{
	Wire wire;
	// The rule decision 95 states as an authoring order: a target index below the reference's own is
	// what makes a cycle unrepresentable rather than something this walk has to detect.
	const std::array nodes{ Leaf(NodeKind::Reference, 1, 0.0, 0.0, 0.0F, 0.0F), Image(0, 0.0, 0.0) };

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	// The image is still drawn once, as itself. The reference contributes nothing.
	GYRO_CHECK_EQ(list.Items.size(), std::size_t{ 1 });
}

GYRO_TEST(Evaluator, ASubtreeLengthThatOverrunsItsRunAbandonsThatLevel)
{
	Wire wire;
	std::array nodes{ Image(0, 0.0, 0.0), Container(9), Image(0, 300.0, 0.0) };
	nodes[1].SubtreeLength = 9; // claims nine nodes where one is left

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	// Decision 90's check, and what survives it: the sibling emitted before the malformed record is
	// still there, the walk terminates, and nothing past the run is read. The alternative is an
	// unbounded traversal inside the frame section, whose survivable outcome is RLIMIT_RTTIME taking
	// every session's UI at once.
	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds().Left(), 0.0F);
}

GYRO_TEST(Evaluator, PastTheDepthCapTheSubtreeIsDroppedAndTheFrameIsNot)
{
	Wire wire;
	std::vector<Node> nodes;
	constexpr std::size_t Depth = MaxWalkDepth + 4;

	// A chain of containers one deeper than the walk will follow, with a drawn leaf at the bottom.
	for (std::size_t level = 0; level < Depth; ++level)
	{
		nodes.push_back(Container(static_cast<std::uint32_t>(Depth - level), 1.0, 0.0));
	}

	nodes.push_back(Image(0, 0.0, 0.0));
	nodes.push_back(Image(0, 700.0, 0.0)); // a sibling of the whole chain, at the top level

	nodes[0].SubtreeLength = static_cast<std::uint32_t>(Depth);

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	// The leaf at the bottom of the too-deep chain is gone; the sibling beside the chain is not. A cap
	// that ended the walk would take the rest of the screen with it.
	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds().Left(), 700.0F);
}

GYRO_TEST(Evaluator, ASpringIsEvaluatedAtThePredictedPresentation)
{
	Wire wire;
	std::array nodes{ Image(0, 0.0, 0.0) };
	nodes[0].TranslationSpring = 0;

	const Instant origin = Monotonic::FromNanoseconds(1'000'000);
	const Spring<Vector3<double>> spring{ .Origin = origin,
		                                  .Parameters = { .Frequency = 20.0, .Damping = 1.0 },
		                                  .Target = { 400.0, 0.0, 0.0 },
		                                  .Offset = { -400.0, 0.0, 0.0 },
		                                  .Velocity = {} };

	const std::array springs{ spring };
	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.Put(SnapshotRun::Translation, std::span<const Spring<Vector3<double>>>{ springs });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	// At the spring's own origin the node is where the offset says, which is also the check that the
	// inline value was *not* used: the record still carries a translation of zero.
	const DrawList first = evaluator.Evaluate(Frame(snapshot, origin));

	GYRO_REQUIRE_EQ(first.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(first.Items[0].Shape.Bounds().Left(), 0.0F);

	// A hundred milliseconds later it is well on its way. The value is the spring's, so what is
	// asserted is that the walk asked it at the instant it was handed rather than at an ambient now.
	const Instant later = Advanced(origin, std::chrono::milliseconds{ 100 });
	const float expected =
		static_cast<float>(spring.Evaluate(later).Position.X) + 0.0F; // the node's own translation is zero
	const DrawList second = evaluator.Evaluate(Frame(snapshot, later));

	GYRO_REQUIRE_EQ(second.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(second.Items[0].Shape.Bounds().Left(), expected);
	GYRO_CHECK(expected > 0.0F && expected < 400.0F);
}

GYRO_TEST(Evaluator, ATimeScaleSlowsTheSubtreesOwnClock)
{
	const Instant origin = Monotonic::FromNanoseconds(1'000'000);
	const Instant at = Advanced(origin, std::chrono::milliseconds{ 100 });
	const Instant half = Advanced(origin, std::chrono::milliseconds{ 50 });

	const Spring<Vector3<double>> spring{ .Origin = origin,
		                                  .Parameters = { .Frequency = 20.0, .Damping = 1.0 },
		                                  .Target = { 400.0, 0.0, 0.0 },
		                                  .Offset = { -400.0, 0.0, 0.0 },
		                                  .Velocity = {} };

	Wire wire;
	std::array nodes{ Container(1), Image(0, 0.0, 0.0) };
	nodes[0].TimeScale = 0.5F;
	nodes[1].TranslationSpring = 0;

	const std::array springs{ spring };
	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.Put(SnapshotRun::Translation, std::span<const Spring<Vector3<double>>>{ springs });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot, at));

	// Decision 19's whole content: a subtree at half speed is a hundred milliseconds of wall clock
	// reaching the spring as fifty. Composed down the tree rather than per property, which is why the
	// scale is on the container and the coefficient is on its child.
	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds().Left(), static_cast<float>(spring.Evaluate(half).Position.X));
}

GYRO_TEST(Evaluator, SettledGeometryLandsOnTheGridAndMovingGeometryDoesNot)
{
	Wire wire;
	const std::array nodes{ Image(0, 10.4, 20.6) };
	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	// Decision 67: a node that names no coefficient is at rest, and a rect whose boundary lands
	// between pixels has a partially covered perimeter — which is a halo under a light window and a
	// hairline of wallpaper between two tiled ones.
	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds().Left(), 10.0F);
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds().Top(), 21.0F);

	Wire moving;
	std::array flight{ Image(0, 10.4, 20.6) };
	flight[0].TranslationSpring = 0;

	const Spring<Vector3<double>> spring{ .Origin = {},
		                                  .Parameters = { .Frequency = 20.0, .Damping = 1.0 },
		                                  .Target = { 10.4, 20.6, 0.0 },
		                                  .Offset = {},
		                                  .Velocity = {} };
	const std::array springs{ spring };

	moving.PutNodes(std::span<const Node>{ flight });
	moving.Put(SnapshotRun::Translation, std::span<const Spring<Vector3<double>>>{ springs });
	moving.PutImages(std::span<const ImageContent>{ images });
	moving.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader second = moving.Read();
	SceneEvaluator other{ clock };
	const DrawList flying = other.Evaluate(Frame(second));

	// A node in flight is left where the spring put it. Snapping it would quantize the trajectory and
	// show as a window arriving in steps.
	GYRO_REQUIRE_EQ(flying.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(flying.Items[0].Shape.Bounds().Left(), 10.4F);
}

// `Node::Snap`, which is decision 67's snap asked for by a node that never settles — the pointer.
//
// Both halves are here because either alone is the wrong picture. A cursor left at a fractional device
// position has an outline a pixel and a bit wide whose weight pumps every frame: one dark pixel at one
// sub-pixel phase and two grey ones at the next, which is a glyph that boils rather than glides. A
// cursor whose *rectangles* each snap has been hinted apart, and its shape changes as it crosses the
// grid. So the subtree takes the grid once, at its root, and keeps its own proportions below that.
GYRO_TEST(Evaluator, ASnappedSubtreeTakesTheGridOnceAndNotPerNode)
{
	Wire wire;

	// A moving root, so decision 67's own condition is false throughout and the flag is the only thing
	// that could put this on the grid.
	std::array nodes{ Container(1, 10.4, 20.6), Image(0, 0.3, 0.4, 5.0F, 5.0F) };
	nodes[0].Flags = Node::Snap;
	nodes[0].TranslationSpring = 0;

	const Spring<Vector3<double>> spring{ .Origin = {},
		                                  .Parameters = { .Frequency = 20.0, .Damping = 1.0 },
		                                  .Target = { 10.4, 20.6, 0.0 },
		                                  .Offset = {},
		                                  .Velocity = {} };
	const std::array springs{ spring };
	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.Put(SnapshotRun::Translation, std::span<const Spring<Vector3<double>>>{ springs });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	// The root is on the grid although it is mid-flight, and the child sits at its authored offset from
	// there rather than at a rounded one — 10 + 0.3 and 21 + 0.4, not 10 and 21.
	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds().Left(), 10.3F);
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds().Top(), 21.4F);
}

// The same subtree once it has come to rest. Without the second clause of the rule this is the frame a
// person's eye has settled on and the frame the glyph's shape changes in, because decision 67's
// condition comes true for every descendant at once.
GYRO_TEST(Evaluator, ASettledChildOfASnappedSubtreeIsNotSnappedAgain)
{
	Wire wire;

	std::array nodes{ Container(1, 10.4, 20.6), Image(0, 0.3, 0.4, 5.0F, 5.0F) };
	nodes[0].Flags = Node::Snap;

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds().Left(), 10.3F);
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds().Top(), 21.4F);
}

GYRO_TEST(Evaluator, ADressedContainerDrawsItsDressingAndNamesNoContent)
{
	Wire wire;
	std::array nodes{ Container(0) };
	nodes[0].Extent = { 200.0F, 100.0F };
	nodes[0].Dress = Material::None;

	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	// Undressed, a container has no pixels at all — which is the whole reason decision 95 gives it a
	// kind rather than making it a fully transparent solid.
	GYRO_CHECK_EQ(evaluator.Evaluate(Frame(snapshot)).Items.size(), std::size_t{ 0 });

	// Dressed, the same container emits one item that names no content and draws its dressing over its
	// own extent, which is decision 99's rule. World/Node.h states it as HasContent() || IsDressed() ||
	// IsLifted(); this is the material term, the check above is the first, and the lifted term is the
	// two tests below.
	nodes[0].Dress = Material::Glass;

	Wire dressed;
	dressed.PutNodes(std::span<const Node>{ nodes });
	dressed.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader second = dressed.Read();
	SceneEvaluator other{ clock };
	const DrawList list = other.Evaluate(Frame(second));

	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK(std::holds_alternative<DrawDressing>(list.Items[0].Content));
	GYRO_CHECK_EQ(list.Items[0].Dress, Material::Glass);

	// The dressing lands on the node's *own* extent rather than on a subtree bound, which is the half
	// of decision 99 that a container cannot distinguish from the alternative on its own — it has no
	// children here — but which the extent is the observable of.
	GYRO_CHECK_EQ(list.Items[0].Extent, Size<SurfaceSpace>{ 200.0F, 100.0F });
}

GYRO_TEST(Evaluator, ALiftedReferenceDrawsTheTilesShadowUnderItsExpansion)
{
	Wire wire;
	// Decision 99's overview thumbnail, and the case that says an elevation emits on its own: the tile
	// is lifted and dressed in nothing, so its whole item is the shadow. Undressed and unlifted this
	// reference draws only its expansion, which is the test above.
	std::array nodes{ Container(1), Image(0, 0.0, 0.0), Leaf(NodeKind::Reference, 1, 500.0, 400.0, 200.0F, 100.0F) };
	nodes[0].Flags = Node::Hidden;
	nodes[2].Lift = Elevation::Resting;

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 2 });

	// The shadow first and the window on top of it, because preorder is the painter's order. Reversed,
	// an overview would lay every tile's shadow over the tile beside it.
	GYRO_CHECK(std::holds_alternative<DrawDressing>(list.Items[0].Content));
	GYRO_CHECK_EQ(list.Items[0].Lift, Cast(Elevation::Resting));
	GYRO_CHECK_EQ(list.Items[0].Dress, Material::None);

	// On the tile's own extent rather than on the expansion's, which is the rest of decision 99: the
	// dressing is the node's and is not inherited by what the reference brings in.
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds(), Rect<DeviceSpace>::FromEdges({ 500.0F, 400.0F }, { 700.0F, 500.0F }));
	GYRO_CHECK_EQ(list.Items[1].Shape.Bounds(), Rect<DeviceSpace>::FromEdges({ 500.0F, 400.0F }, { 600.0F, 450.0F }));
}

GYRO_TEST(Evaluator, ALiftedGroupCastsOneShadowAndNotTwo)
{
	Wire wire;
	std::array nodes{ Container(1), Image(0, 0.0, 0.0) };
	nodes[0].Flags = Node::Group;
	nodes[0].Lift = Elevation::Floating;

	// A real extent on the group itself, without which `Draw` declines on the projection and the
	// second shadow this test is looking for cannot appear whatever the emission test says.
	nodes[0].Extent = { 400.0F, 300.0F };

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	// Two items and not three. The group takes the lift along with the opacity, for decision 99's
	// reason that both belong to the flattened result — and the emission test then reads the local
	// the group just zeroed rather than the node's own field. Reading the node would put a second
	// shadow inside the offscreen the first one was lifted out of, visible for the one moment a group
	// exists for.
	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 2 });
	GYRO_CHECK(AsGroup(list.Items[0]) != nullptr);
	GYRO_CHECK_EQ(list.Items[0].Lift, Cast(Elevation::Floating));
	GYRO_CHECK(!list.Items[1].Lift.Draws());
}

GYRO_TEST(Evaluator, AContentIndexPastItsRunDrawsNothing)
{
	Wire wire;
	const std::array nodes{ Image(4, 0.0, 0.0), Image(0, 300.0, 0.0) };
	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	// A node pointing past its run is a bug on the other side of the waist, and a frame is not where
	// it gets reported: the node draws nothing and everything else on the screen is untouched.
	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds().Left(), 300.0F);
}

GYRO_TEST(Evaluator, AnOutputRunThatIsNotThisSetsIsNoInformation)
{
	Wire wire;
	const std::array nodes{ Image(0, 0.0, 0.0) };
	const std::array images{ Texel(1) };
	const std::array views{ Placement(), Placement() }; // two outputs' worth

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	// Decision 84: a per-output run from another output set is not partial information, and indexing
	// into it would put one output's placement on another one's glass — a window jumping to a
	// neighbouring monitor for one frame after a hotplug.
	GYRO_CHECK_EQ(evaluator.Evaluate(Frame(snapshot)).Items.size(), std::size_t{ 0 });
}

GYRO_TEST(Evaluator, TheArenaDropsWholeTopLevelSubtrees)
{
	Wire wire;
	std::vector<Node> nodes;

	// Two top-level subtrees, each a group of enough leaves that the second cannot fit.
	for (std::size_t subtree = 0; subtree < 2; ++subtree)
	{
		Node root = Container(static_cast<std::uint32_t>(MaxDrawItems - 8));
		root.Flags = Node::Group;
		nodes.push_back(root);

		for (std::size_t leaf = 0; leaf < MaxDrawItems - 8; ++leaf)
		{
			nodes.push_back(Image(0, 0.0, 0.0));
		}
	}

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	// The first subtree is whole and the second is absent entirely. A list truncated where the arena
	// happened to end would hand a renderer a group whose offscreen is missing half its members —
	// a window drawn with its menu gone and its fade applied to what is left.
	GYRO_CHECK(evaluator.Truncated());
	GYRO_CHECK_EQ(list.Items.size(), MaxDrawItems - 7);
	GYRO_REQUIRE(!list.Items.empty());
	GYRO_CHECK_EQ(AsGroup(list.Items[0])->Count, static_cast<std::uint32_t>(MaxDrawItems - 8));
}

GYRO_TEST(Evaluator, DamageIsOwedByAMovingSceneAndByANewPublicationAndByNothingElse)
{
	Wire wire;
	const std::array nodes{ Image(0, 10.0, 20.0) };
	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read(4);
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	// A publication this output has not drawn from is a whole frame owed, first time included.
	GYRO_CHECK(!evaluator.Evaluate(Frame(snapshot)).Damage.IsEmpty());

	// Architecture.md#doing-nothing-must-cost-nothing, from the evaluator's side: the same settled
	// scene, drawn again, owes nothing. A damage rule that reported the whole output every time would
	// keep a still desktop compositing forever.
	GYRO_CHECK(evaluator.Evaluate(Frame(snapshot)).Damage.IsEmpty());

	Wire moving;
	std::array flight{ Image(0, 10.0, 20.0) };
	flight[0].TranslationSpring = 0;

	const std::array springs{ Spring<Vector3<double>>{} };

	moving.PutNodes(std::span<const Node>{ flight });
	moving.Put(SnapshotRun::Translation, std::span<const Spring<Vector3<double>>>{ springs });
	moving.PutImages(std::span<const ImageContent>{ images });
	moving.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader second = moving.Read(4);
	SceneEvaluator other{ clock };

	GYRO_CHECK(!other.Evaluate(Frame(second)).Damage.IsEmpty());
	GYRO_CHECK(!other.Evaluate(Frame(second)).Damage.IsEmpty());
}

GYRO_TEST(Evaluator, TheWalkAllocatesNothing)
{
	Wire wire;
	std::vector<Node> nodes;

	nodes.push_back(Container(6));
	nodes[0].Flags = Node::Group;
	nodes.push_back(Image(0, 0.0, 0.0));
	nodes.push_back(Container(2, 30.0, 30.0));
	nodes.push_back(Image(0, 0.0, 0.0));
	nodes.push_back(Leaf(NodeKind::Solid, 0, 40.0, 40.0, 20.0F, 20.0F));
	nodes.push_back(Leaf(NodeKind::Reference, 1, 600.0, 0.0, 0.0F, 0.0F));
	nodes.push_back(Image(0, 900.0, 0.0));

	const std::array images{ Texel(1) };
	const std::array solids{ SolidContent{} };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutSolids(std::span<const SolidContent>{ solids });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	// Core/DebugAllocator.cpp aborts on an allocation inside this guard, which is decision 36 as a
	// check rather than a convention. Every kind, a group, and a reference in one scene, so the walk
	// takes every branch that could have wanted storage.
	const FrameSection guard;
	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	GYRO_CHECK(!list.Items.empty());
}

// **The pointer's frame, end to end: a node whose texels match its pixels goes on a plane.** This is
// the walk's half of decision 152 — Frame/Assign.h refuses anything whose sampling did not classify,
// so an evaluator that leaves it at the default is a compositor that promotes nothing however idle the
// display engine is. The scene is the cursor's shape rather than a synthetic one: a glyph baked at the
// panel's density, sized at one over it, sitting on the grid.
GYRO_TEST(Evaluator, AnImageThatStatesItsTexelsIsClassifiedAndPromotes)
{
	Wire wire;
	const std::array nodes{ Image(0, 640.0, 360.0, 24.0F, 24.0F) };

	ImageContent glyph = Texel(3);
	glyph.Source = { {}, { 24.0F, 24.0F } };

	const std::array images{ glyph };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK(list.Items[0].Sampling.IsResampleFree());
	GYRO_CHECK_EQ(WhyNotPromoted(list.Items[0], ColorState::Srgb()), PromotionRefusal::None);
}

// And the same node with nothing said about its texels, which is every image the walk cannot count:
// unclassified, and therefore composited. The refusal is named so that a reader of the trace is told
// *sampling* rather than being left to infer it from a plane count of zero.
GYRO_TEST(Evaluator, AnImageThatStatesNoTexelsIsRefusedForSampling)
{
	Wire wire;
	const std::array nodes{ Image(0, 640.0, 360.0, 24.0F, 24.0F) };
	const std::array images{ Texel(3) };
	const std::array views{ Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(WhyNotPromoted(list.Items[0], ColorState::Srgb()), PromotionRefusal::Sampling);
}

// Decision 21's partition, asserted the way the rest of this file is: by what each failure looks like
// on a screen. A root drawn on the wrong output is one person's windows on another person's monitor.
// A root drawn on none is a session that logged in and got a black screen. A partition that costs a
// test per node rather than per root is the frame time of every switched-away session, paid every
// refresh by whoever is actually looking at the machine.
namespace
{
// Two outputs showing the same world, which is what a partition needs in order to differ.
[[nodiscard]] EvaluateRequest On(const SnapshotReader& snapshot, std::size_t output)
{
	return { .Snapshot = snapshot, .Output = output, .Outputs = 2, .Resolution = Screen, .Presentation = {} };
}

constexpr auto First = static_cast<SessionId>(1);
constexpr auto Second = static_cast<SessionId>(2);

// An output in its steady state: showing one session, leaving none, and so carrying no coefficient —
// which is what every entry says outside a transition.
[[nodiscard]] constexpr SceneAssignment Showing(SessionId session) noexcept
{
	return { .Shown = session };
}

// An output mid transition, with the session it is leaving faded by the spring at `fade`.
[[nodiscard]] constexpr SceneAssignment Leaving(SessionId shown, SessionId outgoing, std::uint32_t fade) noexcept
{
	return { .Shown = shown, .Fading = outgoing, .Fade = fade };
}

// The other direction: the session being shown is the one carrying the coefficient, resolving onto
// whatever is underneath it. Unlocking a screen is this — what is underneath is gyro's own.
[[nodiscard]] constexpr SceneAssignment Arriving(SessionId shown, std::uint32_t fade) noexcept
{
	return { .Shown = shown, .Fading = shown, .Fade = fade };
}
} // namespace

GYRO_TEST(Evaluator, ARootIsDrawnOnlyOnAnOutputShowingItsSession)
{
	Wire wire;

	// One window each, and the container above it, so that skipping is proved to take the subtree with
	// it rather than the root alone — a session skipped a node at a time is its windows without their
	// frames, which is worse than either whole answer.
	const std::array nodes{ Container(1), Image(0, 10.0, 10.0), Container(1), Image(0, 400.0, 10.0) };
	const std::array images{ Texel(1) };
	const std::array views{ Placement(), Placement() };
	const std::array roots{ SceneRoot{ .Node = 0, .Session = First }, SceneRoot{ .Node = 2, .Session = Second } };
	const std::array sessions{ Showing(First), Showing(Second) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutRoots(std::span<const SceneRoot>{ roots });
	wire.PutSessions(std::span<const SceneAssignment>{ sessions });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList one = evaluator.Evaluate(On(snapshot, 0));

	GYRO_REQUIRE_EQ(one.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(one.Items[0].Shape.Bounds().Origin, Point<DeviceSpace>{ 10.0F, 10.0F });

	const DrawList two = evaluator.Evaluate(On(snapshot, 1));

	GYRO_REQUIRE_EQ(two.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(two.Items[0].Shape.Bounds().Origin, Point<DeviceSpace>{ 400.0F, 10.0F });
}

// The pointer glyph is the standing case: gyro's own, and on screen over whichever session an output
// is showing. A `None` root that followed the same rule as a session's would be a machine with two
// users logged in and a cursor on neither screen.
GYRO_TEST(Evaluator, ARootOfNoSessionIsDrawnOnEveryOutput)
{
	Wire wire;
	const std::array nodes{ Image(0, 10.0, 10.0), Image(0, 400.0, 10.0) };
	const std::array images{ Texel(1) };
	const std::array views{ Placement(), Placement() };
	const std::array roots{ SceneRoot{ .Node = 0, .Session = First },
		                    SceneRoot{ .Node = 1, .Session = SessionId::None } };

	// The second output is showing gyro's own scene, which is the splash, the console, and the gap
	// between one session and the next.
	const std::array sessions{ Showing(First), Showing(SessionId::None) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutRoots(std::span<const SceneRoot>{ roots });
	wire.PutSessions(std::span<const SceneAssignment>{ sessions });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	// The session's window and the cursor over it.
	GYRO_CHECK_EQ(evaluator.Evaluate(On(snapshot, 0)).Items.size(), std::size_t{ 2 });

	// The cursor alone, over gyro's own scene.
	const DrawList own = evaluator.Evaluate(On(snapshot, 1));

	GYRO_REQUIRE_EQ(own.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(own.Items[0].Shape.Bounds().Origin, Point<DeviceSpace>{ 400.0F, 10.0F });
}

// A scene nobody has partitioned is drawn whole, and this is the assertion that keeps the gate from
// being a behaviour change on the day it lands: everything gyro published before there were sessions
// publishes no roots and no assignment, and must go on looking exactly as it did.
GYRO_TEST(Evaluator, AnUnpartitionedSceneIsDrawnWhole)
{
	Wire wire;
	const std::array nodes{ Image(0, 10.0, 10.0), Image(0, 400.0, 10.0) };
	const std::array images{ Texel(1) };
	const std::array views{ Placement(), Placement() };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	GYRO_CHECK_EQ(evaluator.Evaluate(On(snapshot, 0)).Items.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(evaluator.Evaluate(On(snapshot, 1)).Items.size(), std::size_t{ 2 });
}

// An assignment run that is not the output set's is decision 84's *no information rather than partial
// information*, and here that resolves to gyro's own scene rather than to everything. It fails towards
// the screen that cannot be the wrong person's: showing nothing of a session is a black panel somebody
// reports, and showing a session that may not be this output's is the one outcome the partition exists
// to prevent.
GYRO_TEST(Evaluator, AnAssignmentRunOfTheWrongLengthShowsGyrosOwnSceneOnly)
{
	Wire wire;
	const std::array nodes{ Image(0, 10.0, 10.0), Image(0, 400.0, 10.0) };
	const std::array images{ Texel(1) };
	const std::array views{ Placement(), Placement() };
	const std::array roots{ SceneRoot{ .Node = 0, .Session = First },
		                    SceneRoot{ .Node = 1, .Session = SessionId::None } };
	const std::array sessions{ Showing(First) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutRoots(std::span<const SceneRoot>{ roots });
	wire.PutSessions(std::span<const SceneAssignment>{ sessions });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(On(snapshot, 0));

	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds().Origin, Point<DeviceSpace>{ 400.0F, 10.0F });
}

// The root run names node indices and the walk compares them, so a run that has drifted from the scene
// cannot quietly attribute one session's roots to another. What it does instead is show them, which is
// the direction to fail in: a person sees a window that should not be on their screen and says so, and
// nobody's session is invisible while they wait for somebody to notice.
GYRO_TEST(Evaluator, ARootTheRunDoesNotNameIsShown)
{
	Wire wire;
	const std::array nodes{ Image(0, 10.0, 10.0), Image(0, 400.0, 10.0) };
	const std::array images{ Texel(1) };
	const std::array views{ Placement(), Placement() };

	// One entry, and it names neither root: the first index is past both.
	const std::array roots{ SceneRoot{ .Node = 9, .Session = Second } };
	const std::array sessions{ Showing(First), Showing(First) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutRoots(std::span<const SceneRoot>{ roots });
	wire.PutSessions(std::span<const SceneAssignment>{ sessions });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	GYRO_CHECK_EQ(evaluator.Evaluate(On(snapshot, 0)).Items.size(), std::size_t{ 2 });
}

// The cross-fade, which is decision 188. Every claim here fails as something a person watches happen
// on a laptop lid: a film that freezes the moment the screen locks, a desktop that snaps away instead
// of leaving, or — the one that matters — somebody else's windows still on the glass after it should
// have gone.
namespace
{
// A spring standing still at one value, so that what a test asserts is the walk rather than the
// solver: no offset and no velocity is a position equal to the target at every instant.
[[nodiscard]] Spring<float> Held(float value) noexcept
{
	return { .Target = value };
}
} // namespace

GYRO_TEST(Evaluator, TheSessionAnOutputIsLeavingIsDrawnAtTheFadesValue)
{
	Wire wire;
	const std::array nodes{ Image(0, 10.0, 10.0), Image(0, 400.0, 10.0) };
	const std::array images{ Texel(1) };
	const std::array views{ Placement(), Placement() };
	const std::array roots{ SceneRoot{ .Node = 0, .Session = First }, SceneRoot{ .Node = 1, .Session = Second } };

	// The first output is moving from the second session to the first, a quarter of the way left to go.
	const std::array sessions{ Leaving(First, Second, 0), Showing(First) };
	const std::array fade{ Held(0.25F) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutRoots(std::span<const SceneRoot>{ roots });
	wire.PutSessions(std::span<const SceneAssignment>{ sessions });
	wire.Put<Spring<float>>(SnapshotRun::Opacity, fade);

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(On(snapshot, 0));

	// Both, which is the whole of the decision: the outgoing session is composited live rather than
	// photographed, so what was playing goes on playing as it leaves.
	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 2 });

	// The arriving session at full strength from the first frame, and the departing one at the
	// coefficient. One is a screen a person is being taken to; the other is the one being taken away.
	GYRO_CHECK_EQ(list.Items[0].Opacity, 1.0F);
	GYRO_CHECK_EQ(list.Items[1].Opacity, 0.25F);
	GYRO_CHECK_EQ(list.Items[1].Shape.Bounds().Origin, Point<DeviceSpace>{ 400.0F, 10.0F });

	// And the second output, which was asked to move nothing, shows exactly what it was showing.
	GYRO_CHECK_EQ(evaluator.Evaluate(On(snapshot, 1)).Items.size(), std::size_t{ 1 });
}

// The other direction, and the one the first shape of decision 188 could not express: a session
// arriving over what is already on the screen carries the coefficient itself. Unlocking is this —
// gyro's own background is behind the windows coming back, so fading *it* is a coefficient nobody can
// see and the desktop would cut in at full strength on the first frame.
GYRO_TEST(Evaluator, AnArrivingSessionCarriesTheCoefficientWhenItIsTheOneInFront)
{
	Wire wire;
	const std::array nodes{ Image(0, 10.0, 10.0), Image(0, 400.0, 10.0) };
	const std::array images{ Texel(1) };
	const std::array views{ Placement(), Placement() };

	// gyro's own root first, which is where the background is, and the session's over it.
	const std::array roots{ SceneRoot{ .Node = 0, .Session = SessionId::None },
		                    SceneRoot{ .Node = 1, .Session = First } };

	// A quarter of the way back onto the screen.
	const std::array sessions{ Arriving(First, 0), Showing(SessionId::None) };
	const std::array fade{ Held(0.25F) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutRoots(std::span<const SceneRoot>{ roots });
	wire.PutSessions(std::span<const SceneAssignment>{ sessions });
	wire.Put<Spring<float>>(SnapshotRun::Opacity, fade);

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(On(snapshot, 0));

	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 2 });

	// **gyro's own at full strength and the session at the coefficient**, which is the ordering the
	// walk exists to get right: `Fading` equals `Shown` here, so a test of `Shown` reached first would
	// draw the desktop at one and there would be no transition to see.
	GYRO_CHECK_EQ(list.Items[0].Opacity, 1.0F);
	GYRO_CHECK_EQ(list.Items[1].Opacity, 0.25F);
	GYRO_CHECK_EQ(list.Items[1].Shape.Bounds().Origin, Point<DeviceSpace>{ 400.0F, 10.0F });
}

// The pointer glyph is `None` and is the last root so that it draws over everything (55). A
// transition may never take gyro's own as the faded side, or the cursor would leave the screen every
// time somebody locked it — so `None` is tested before anything else in the walk.
GYRO_TEST(Evaluator, GyrosOwnRootsAreNeverFadedByATransition)
{
	Wire wire;
	const std::array nodes{ Image(0, 10.0, 10.0), Image(0, 400.0, 10.0) };
	const std::array images{ Texel(1) };
	const std::array views{ Placement(), Placement() };
	const std::array roots{ SceneRoot{ .Node = 0, .Session = First },
		                    SceneRoot{ .Node = 1, .Session = SessionId::None } };

	// A run that names gyro's own as the faded session, which nothing authors and the walk must
	// nonetheless not honour — decision 90 has the frame thread validating what it walks.
	const std::array sessions{ SceneAssignment{ .Shown = First, .Fading = SessionId::None, .Fade = 0 },
		                       Showing(SessionId::None) };
	const std::array fade{ Held(0.25F) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutRoots(std::span<const SceneRoot>{ roots });
	wire.PutSessions(std::span<const SceneAssignment>{ sessions });
	wire.Put<Spring<float>>(SnapshotRun::Opacity, fade);

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(On(snapshot, 0));

	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 2 });

	GYRO_CHECK_EQ(list.Items[0].Opacity, 1.0F);
	GYRO_CHECK_EQ(list.Items[1].Opacity, 1.0F);
}

// The cut, which is what suspend takes and what every reassignment does today: no coefficient, so the
// session that was there is simply not there, on the very next frame.
GYRO_TEST(Evaluator, AReassignmentWithNoCoefficientDropsTheOutgoingSessionOutright)
{
	Wire wire;
	const std::array nodes{ Image(0, 10.0, 10.0), Image(0, 400.0, 10.0) };
	const std::array images{ Texel(1) };
	const std::array views{ Placement(), Placement() };
	const std::array roots{ SceneRoot{ .Node = 0, .Session = First }, SceneRoot{ .Node = 1, .Session = Second } };
	const std::array sessions{ Leaving(First, Second, NoCoefficient), Showing(First) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutRoots(std::span<const SceneRoot>{ roots });
	wire.PutSessions(std::span<const SceneAssignment>{ sessions });

	const DrawList list = [&] {
		const SnapshotReader snapshot = wire.Read();
		TickingClock clock;
		SceneEvaluator evaluator{ clock };

		return evaluator.Evaluate(On(snapshot, 0));
	}();

	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(list.Items[0].Shape.Bounds().Origin, Point<DeviceSpace>{ 10.0F, 10.0F });
}

// The pair is retired together on the authoring side, so an outgoing session whose coefficient is not
// in the run is a run that has drifted. This is the one place the partition fails *towards* the
// steady state rather than towards showing a root: a session left composited at a strength nobody
// wrote is somebody else's desktop sitting on a locked screen, which is the failure decision 43
// exists to prevent.
GYRO_TEST(Evaluator, AnOutgoingSessionWhoseCoefficientIsNotInTheRunIsNotDrawn)
{
	Wire wire;
	const std::array nodes{ Image(0, 10.0, 10.0), Image(0, 400.0, 10.0) };
	const std::array images{ Texel(1) };
	const std::array views{ Placement(), Placement() };
	const std::array roots{ SceneRoot{ .Node = 0, .Session = First }, SceneRoot{ .Node = 1, .Session = Second } };

	// One spring in the run and the entry names the sixth.
	const std::array sessions{ Leaving(First, Second, 5), Showing(First) };
	const std::array fade{ Held(0.25F) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutRoots(std::span<const SceneRoot>{ roots });
	wire.PutSessions(std::span<const SceneAssignment>{ sessions });
	wire.Put<Spring<float>>(SnapshotRun::Opacity, fade);

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	GYRO_CHECK_EQ(evaluator.Evaluate(On(snapshot, 0)).Items.size(), std::size_t{ 1 });
}

// A closing window, and what the walk has to say about it that nothing else can.
//
// Decision 20 draws a window that is leaving from a copy of its last frame; decision 46 reserves the
// rectangle that copy goes in when the retirement is observed, on the far side of the waist. What is
// left over is *which pixels* — a toplevel is a container (111), so the window is a run of items and
// the run's extent is a fact about a walk in flight. These assert that run, and each failure has a
// picture: a run that starts a item early is a window that leaves wearing the wallpaper behind it, a
// run one item short is a window whose menu vanishes a frame before the rest of it, and a source
// rectangle that is not the node's own quad is a window that slides sideways inside its own fade.
namespace
{
[[nodiscard]] ExitSnapshot Reserving(std::uint32_t node, std::uint32_t output, std::uint32_t reservation)
{
	return { .Node = node,
		     .Output = output,
		     .Reservation = reservation,
		     .Texture = TextureId{ 900 + reservation, 1 },
		     .Slot = PixelRect<BufferSpace>{ { 0, 0 }, { 100, 60 } } };
}
} // namespace

GYRO_TEST(Evaluator, AClosingWindowNamesItsWholeSubtreeAndNothingBesideIt)
{
	Wire wire;

	// A window ahead of it that must not be in the run, the closing window with two nodes under it,
	// and a window behind it that must not be either.
	std::array nodes{ Image(0, 0.0, 0.0),
		              Container(2, 300.0, 200.0),
		              Image(1, 300.0, 200.0),
		              Image(2, 320.0, 220.0),
		              Image(3, 800.0, 0.0) };
	nodes[1].Extent = { 100.0F, 60.0F };
	nodes[1].Exit = 0;

	const std::array images{ Texel(1), Texel(2), Texel(3), Texel(4) };
	const std::array views{ Placement() };
	const std::array exits{ Reserving(1, 0, 11) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutExits(std::span<const ExitSnapshot>{ exits });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	// The container draws nothing of its own, so the run is its two children.
	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 4 });
	GYRO_REQUIRE_EQ(list.Captures.size(), std::size_t{ 1 });

	const ExitCapture& capture = list.Captures[0];

	GYRO_CHECK_EQ(capture.Reservation, 11U);
	GYRO_CHECK_EQ(capture.Into, TextureId{ 911, 1 });
	GYRO_CHECK_EQ(capture.First, 1U);
	GYRO_CHECK_EQ(capture.Count, 2U);

	// The window's own quad, which is the rectangle the slot was reserved from — not the bound of its
	// children, which reach further right and further down.
	GYRO_CHECK_EQ(capture.Source, Rect<DeviceSpace>::FromEdges({ 300.0F, 200.0F }, { 400.0F, 260.0F }));

	const DrawTexture* const first = AsTexture(list.Items[capture.First]);
	const DrawTexture* const last = AsTexture(list.Items[capture.First + capture.Count - 1]);

	GYRO_REQUIRE(first != nullptr && last != nullptr);
	GYRO_CHECK_EQ(first->Texture, TextureId{ 2, 1 });
	GYRO_CHECK_EQ(last->Texture, TextureId{ 3, 1 });
}

GYRO_TEST(Evaluator, AClosingWindowThatDeclaredAGroupKeepsTheGroupItemInItsOwnRun)
{
	Wire wire;

	std::array nodes{ Container(2, 300.0, 200.0), Image(0, 300.0, 200.0), Image(1, 320.0, 220.0) };
	nodes[0].Extent = { 100.0F, 60.0F };
	nodes[0].Flags |= Node::Group;
	nodes[0].Exit = 0;

	const std::array images{ Texel(1), Texel(2) };
	const std::array views{ Placement() };
	const std::array exits{ Reserving(0, 0, 4) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutExits(std::span<const ExitSnapshot>{ exits });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	// **The group's own item is inside the run, and it has to be.** A group is what the renderer
	// flattens the subtree through, so a run that named only the members would draw the window into
	// its snapshot without the fade, the dressing, or the shadow the group is carrying for it.
	GYRO_REQUIRE_EQ(list.Captures.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(list.Captures[0].First, 0U);
	GYRO_CHECK_EQ(list.Captures[0].Count, 3U);
	GYRO_REQUIRE(AsGroup(list.Items[0]) != nullptr);
}

GYRO_TEST(Evaluator, EachScreenIsToldOnlyAboutItsOwnRectangle)
{
	Wire wire;

	std::array nodes{ Container(1, 10.0, 10.0), Image(0, 10.0, 10.0) };
	nodes[0].Extent = { 100.0F, 60.0F };
	nodes[0].Exit = 0;

	const std::array images{ Texel(1) };
	const std::array views{ Placement(), Placement() };

	// Decision 190: a window straddling the seam is in both atlases, and the two entries are
	// contiguous under the same node.
	const std::array exits{ Reserving(0, 0, 21), Reserving(0, 1, 22) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutExits(std::span<const ExitSnapshot>{ exits });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList left = evaluator.Evaluate(On(snapshot, 0));

	GYRO_REQUIRE_EQ(left.Captures.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(left.Captures[0].Reservation, 21U);

	const DrawList right = evaluator.Evaluate(On(snapshot, 1));

	GYRO_REQUIRE_EQ(right.Captures.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(right.Captures[0].Reservation, 22U);
}

GYRO_TEST(Evaluator, AWindowWithNoRoomReservedForItIsNotReported)
{
	Wire wire;

	std::array nodes{ Container(1, 10.0, 10.0), Image(0, 10.0, 10.0) };
	nodes[0].Extent = { 100.0F, 60.0F };
	nodes[0].Exit = 0;

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	// Decision 46's exhaustion: the packer had nowhere to put it, so the entry carries no texture. The
	// window cuts instead of fading, and no frame is spent finding that out again.
	std::array exits{ Reserving(0, 0, 5) };
	exits[0].Texture = TextureId{};

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutExits(std::span<const ExitSnapshot>{ exits });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	GYRO_CHECK(evaluator.Evaluate(Frame(snapshot)).Captures.empty());
}

GYRO_TEST(Evaluator, AnExitRunNamingSomebodyElsesNodeIsReadAsNoSnapshotAtAll)
{
	Wire wire;

	std::array nodes{ Container(1, 10.0, 10.0), Image(0, 10.0, 10.0) };
	nodes[0].Extent = { 100.0F, 60.0F };
	nodes[0].Exit = 0;

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };

	// Decision 90 at this seam: the entry at the named position belongs to another node, so the scan
	// stops rather than reading it. Trusting it would copy this window's rectangle out of a rectangle
	// reserved for a different window — a wrong picture where this is a missing one.
	const std::array exits{ Reserving(1, 0, 6) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutExits(std::span<const ExitSnapshot>{ exits });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	GYRO_CHECK(evaluator.Evaluate(Frame(snapshot)).Captures.empty());
}

GYRO_TEST(Evaluator, AClosingWindowWithNothingLeftToDrawIsNotReported)
{
	Wire wire;

	// The client took its buffer away on the way out: the container is still closing and there is no
	// longer anything under it. Reporting an empty run would clear the rectangle, mark it filled, and
	// leave the window fading from nothing for the length of its exit.
	std::array nodes{ Container(0, 10.0, 10.0) };
	nodes[0].Extent = { 100.0F, 60.0F };
	nodes[0].Exit = 0;

	const std::array views{ Placement() };
	const std::array exits{ Reserving(0, 0, 7) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutExits(std::span<const ExitSnapshot>{ exits });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	GYRO_CHECK(list.Items.empty());
	GYRO_CHECK(list.Captures.empty());
}

GYRO_TEST(Evaluator, TwoWindowsClosingAtOnceGetOneRunEach)
{
	Wire wire;

	// Two toplevels leaving together — closing a folder of windows, or an application going down.
	// The runs must be disjoint and in walk order, because a run that overlapped its neighbour would
	// put half of one window into the other's rectangle and both would leave wearing each other.
	std::array nodes{ Container(2, 100.0, 100.0),
		              Image(0, 100.0, 100.0),
		              Image(1, 120.0, 120.0),
		              Container(1, 600.0, 100.0),
		              Image(2, 600.0, 100.0) };
	nodes[0].Extent = { 100.0F, 60.0F };
	nodes[0].Exit = 0;
	nodes[3].Extent = { 100.0F, 60.0F };
	nodes[3].Exit = 1;

	const std::array images{ Texel(1), Texel(2), Texel(3) };
	const std::array views{ Placement() };
	const std::array exits{ Reserving(0, 0, 31), Reserving(3, 0, 32) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutExits(std::span<const ExitSnapshot>{ exits });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	GYRO_REQUIRE_EQ(list.Items.size(), std::size_t{ 3 });
	GYRO_REQUIRE_EQ(list.Captures.size(), std::size_t{ 2 });

	GYRO_CHECK_EQ(list.Captures[0].Reservation, 31U);
	GYRO_CHECK_EQ(list.Captures[0].First, 0U);
	GYRO_CHECK_EQ(list.Captures[0].Count, 2U);

	GYRO_CHECK_EQ(list.Captures[1].Reservation, 32U);
	GYRO_CHECK_EQ(list.Captures[1].First, 2U);
	GYRO_CHECK_EQ(list.Captures[1].Count, 1U);
}

GYRO_TEST(Evaluator, AWindowClosingInsideAClosingWindowGetsARunInsideTheOtherOne)
{
	Wire wire;

	// A dialog leaving with the window that owns it. The inner run is contained by the outer one and
	// both are filed, which is the only arrangement that draws right either way round: the dialog's
	// own rectangle holds the dialog, and the window's holds the window with the dialog on it.
	std::array nodes{ Container(2, 0.0, 0.0), Container(1, 20.0, 20.0), Image(0, 20.0, 20.0) };
	nodes[0].Extent = { 200.0F, 120.0F };
	nodes[0].Exit = 0;
	nodes[1].Extent = { 80.0F, 40.0F };
	nodes[1].Exit = 1;

	const std::array images{ Texel(1) };
	const std::array views{ Placement() };
	const std::array exits{ Reserving(0, 0, 41), Reserving(1, 0, 42) };

	wire.PutNodes(std::span<const Node>{ nodes });
	wire.PutImages(std::span<const ImageContent>{ images });
	wire.PutViews(std::span<const OutputAdapter>{ views });
	wire.PutExits(std::span<const ExitSnapshot>{ exits });

	const SnapshotReader snapshot = wire.Read();
	TickingClock clock;
	SceneEvaluator evaluator{ clock };

	const DrawList list = evaluator.Evaluate(Frame(snapshot));

	GYRO_REQUIRE_EQ(list.Captures.size(), std::size_t{ 2 });

	// Filed as the walk leaves each subtree, so the inner one comes out first.
	GYRO_CHECK_EQ(list.Captures[0].Reservation, 42U);
	GYRO_CHECK_EQ(list.Captures[0].First, 0U);
	GYRO_CHECK_EQ(list.Captures[0].Count, 1U);

	GYRO_CHECK_EQ(list.Captures[1].Reservation, 41U);
	GYRO_CHECK_EQ(list.Captures[1].First, 0U);
	GYRO_CHECK_EQ(list.Captures[1].Count, 1U);
}
