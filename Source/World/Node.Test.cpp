#include "World/Node.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Testing/Test.h"

// The runtime half of Node.h's contract. The compile-time half — the defaults, the flag mask, the
// size and alignment of the published record — is the static_assert block at the foot of that header
// and is not repeated here.
//
// What is left is the encoding itself, which is only convincing against a tree somebody could
// actually author: a run length is indistinguishable from a child count until something nests, and
// that is precisely the shape Docs/Decisions.md decision 86 says the frame side walks. So the fixture
// is the worked example Seam/Renderer.h uses for the same encoding — a menu, its panel, its open
// submenu, and that submenu's panel — and the tests below are the two operations a walk performs on
// it: descend, and skip whole.

namespace
{
// Preorder, with each node's subtree length being the count of nodes that follow it inside its own
// subtree. Read down the indices: the menu owns everything after it, the submenu owns its panel, and
// both panels are leaves.
//
//   0  menu           subtree 3   -> children at 1 and 2
//   1    panel        subtree 0
//   2    submenu      subtree 1   -> child at 3
//   3      panel      subtree 0
//
// A child *count* on the menu would read 2 and lose the two nodes underneath the submenu, which is
// the failure this fixture exists to make visible rather than plausible.
enum Index : std::size_t
{
	Menu = 0,
	MenuPanel = 1,
	Submenu = 2,
	SubmenuPanel = 3,
};

[[nodiscard]] std::vector<Node> BuildMenu()
{
	std::vector<Node> nodes(4);

	nodes[Menu].SubtreeLength = 3;
	nodes[MenuPanel].SubtreeLength = 0;
	nodes[Submenu].SubtreeLength = 1;
	nodes[SubmenuPanel].SubtreeLength = 0;

	return nodes;
}

// What a walk does, reduced to the one thing being asserted: visit in preorder, and step over a
// hidden subtree whole rather than testing every node in it.
[[nodiscard]] std::vector<std::size_t> Visit(const std::vector<Node>& nodes)
{
	std::vector<std::size_t> visited;

	std::size_t index = 0;
	while (index < nodes.size())
	{
		if (nodes[index].IsHidden())
		{
			index = nodes[index].Past(index);
			continue;
		}

		visited.push_back(index);
		++index;
	}

	return visited;
}
} // namespace

GYRO_TEST(Node, SubtreeLengthIsARunLengthAndNotAChildCount)
{
	const std::vector<Node> nodes = BuildMenu();

	// The menu's run is everything after it, nested submenu included. Stated as the half-open range
	// a renderer reads, because that is the reading Seam/Renderer.h's DrawGroup shares and the whole
	// reason decision 86 borrows the convention.
	GYRO_CHECK_EQ(nodes[Menu].Past(Menu), std::size_t{ 4 });
	GYRO_CHECK_EQ(nodes[Submenu].Past(Submenu), std::size_t{ 4 });
	GYRO_CHECK_EQ(nodes[MenuPanel].Past(MenuPanel), std::size_t{ 2 });

	// The distinction the fixture exists for: two children, four nodes.
	GYRO_CHECK_EQ(nodes[Menu].SubtreeLength, std::uint32_t{ 3 });
}

GYRO_TEST(Node, HidingASubtreeSkipsEverythingUnderIt)
{
	std::vector<Node> nodes = BuildMenu();

	GYRO_CHECK_EQ(Visit(nodes).size(), std::size_t{ 4 });

	// Hiding the submenu takes its panel with it — the operation decision 86 chose this encoding for,
	// and the one a parent-index encoding cannot express as an addition.
	nodes[Submenu].Flags |= Node::Hidden;

	const std::vector<std::size_t> withSubmenuHidden = Visit(nodes);
	GYRO_REQUIRE_EQ(withSubmenuHidden.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(withSubmenuHidden[0], std::size_t{ Menu });
	GYRO_CHECK_EQ(withSubmenuHidden[1], std::size_t{ MenuPanel });

	// Hiding the root leaves nothing, and reaches the end of the run exactly rather than past it.
	nodes[Menu].Flags |= Node::Hidden;
	GYRO_CHECK_EQ(Visit(nodes).size(), std::size_t{ 0 });
}

GYRO_TEST(Node, ChannelSlotsAreIndependent)
{
	Node node;

	// A node whose position is animating and whose opacity is not: the common case, and the reason
	// decision 86 puts an index beside a model value per channel rather than a spring per node.
	node.TranslationSpring = 7;

	GYRO_CHECK(node.IsTranslating());
	GYRO_CHECK(!node.IsFading());
	GYRO_CHECK(!node.IsScaling());
	GYRO_CHECK(!node.IsRotating());

	// And the model value the other channels keep is still the whole answer for them.
	GYRO_CHECK_EQ(node.Opacity, 1.0F);
}
