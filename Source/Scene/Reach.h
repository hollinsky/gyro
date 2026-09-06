#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "Core/Handle.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Space.h"
#include "Scene/Entity.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "World/Node.h"

// Which outputs a node's pixels land on, asked on the dispatch side.
//
// **This is decision 32's question — *which outputs does this surface intersect* — and
// `Scene/Output.h` says where the answer comes from**: the output's `Bounds` in global space, not its
// device grid. A surface's frame cadence follows the fastest output it touches, and a client's damage
// is cleared only once every output touching it has shown the sequence carrying it (113), so the two
// consumers fold the same set in opposite directions and neither can be written without it.
//
// **It costs an ancestor walk per node asked about, and that is the whole reason it is a function here
// rather than a column the serializer fills in.** `Scene/Serializer.h`'s header explains at length why
// the wake fold is *not* partitioned per output: doing it there means a composed transform for every
// node in the world on every publication, at input rate, duplicating the walk `Frame/Evaluator.h`
// performs a few milliseconds later. [Open.md](../../Docs/Open.md) carries that as an open cost and
// asks for a measurement rather than a change. Nothing here contradicts it, because what is asked is
// not *every node* but *the handful that committed*: a window that just attached a buffer is one
// entity, its chain is a few levels deep, and the work is proportional to what a client did rather
// than to what the world holds. That is the same axis decision 115 rejects a world scan on.
//
// **It is a static bound and not the swept one.** The bound is where the node is *now*, at the model
// values a commit resolved to, and a node in flight is somewhere else a frame later. That is exact for
// the question being asked — the entity that committed pixels is somewhere definite at the instant it
// is sealed — and it is not the swept bound the wake fold would need, which has to cover every
// position a spring will pass through before it settles. Reading this as having answered that entry
// would be reading it as more than it is.

// `OutputReach` and `MaxReachableOutputs` are `Scene/Output.h`'s, because a mask over the output set
// is a fact about that set rather than about this question. `Scene/Atlas.h` reserves per output and
// cannot include this header — it is held by the store, and this one holds the store — so the two
// would otherwise have spelled the same mask twice.

// SPEC: how deep an ancestor chain may be before the answer is *nowhere*. It is `Frame/Evaluator.h`'s
// `MaxWalkDepth` restated rather than shared, because `Scene` may not name `Frame` — and the two have
// to be the same number for the honest reason that a subtree deeper than the frame walk descends is
// one the frame walk *drops*. A node past the cap is never drawn, so answering *no outputs* here is
// the truth about it rather than a conservatism: nothing will present it, and nothing is owed for it.
inline constexpr std::size_t MaxReachDepth = 32;

// Every output in the set, which is what an unanswerable case folds to.
//
// **The direction is deliberate and it is the cheap side of the trade.** A mask that is too wide costs
// a client one frame it draws and nobody sees; a mask that is too narrow is a window that never hears
// back and stops repainting for the rest of its life. So where the geometry cannot be evaluated — a
// point behind the eye of some perspective above it, a node with no extent to project — every output
// is named and the callback goes out on the first one that flips.
[[nodiscard]] inline OutputReach AllOutputs(std::size_t outputs) noexcept
{
	const std::size_t count = std::min(outputs, MaxReachableOutputs);

	if (count == MaxReachableOutputs)
	{
		return ~OutputReach{};
	}

	return (OutputReach{ 1 } << count) - 1;
}

namespace Detail
{
// The chain from the top level down to this entity, or nothing where it is deeper than the cap or
// hidden anywhere along the way.
//
// Hidden is checked over the whole chain rather than on the node itself, because that is what the
// frame walk does with it: `Node::Hidden` skips the subtree, so a window under a hidden workspace is
// not on any output whatever its own flags say.
struct Ancestry
{
	std::array<EntityId, MaxReachDepth> Line{};
	std::size_t Depth = 0;
	bool Reachable = false;
};

[[nodiscard]] inline Ancestry LineageOf(const SceneStore& store, EntityId id)
{
	Ancestry ancestry{};

	for (EntityId at = id; !at.IsNull();)
	{
		const Entity* const entity = store.Find(at);

		if (entity == nullptr || (entity->Flags & Node::Hidden) != 0)
		{
			return {};
		}

		if (ancestry.Depth == ancestry.Line.size())
		{
			return {};
		}

		ancestry.Line[ancestry.Depth] = at;
		++ancestry.Depth;

		at = entity->Parent;
	}

	ancestry.Reachable = ancestry.Depth != 0;

	return ancestry;
}
} // namespace Detail

// What an entity's own quad covers in global space.
//
// **The rectangle and *whether there is one* are separate fields, because the two callers answer an
// indefinite quad differently.** `Reach` below folds one to every output, so a window gyro cannot
// project still hears back about the frame it drew; a caller picking one output to constrain a window
// against has to know it would be picking blind, and taking a zero rectangle for a real one would put
// every such window on the first screen in the list.
struct Coverage
{
	// The bounding box of the node's four projected corners, in the global space an output's own
	// rectangle is stated in. Meaningful only where `Definite`.
	Rect<GlobalSpace> Bounds{};

	// Whether anything draws this entity at all: live, visible, visibly parented, and no deeper than
	// the frame walk descends.
	bool Reachable = false;

	// Whether a rectangle came out. False for a reachable entity with no extent of its own and for one
	// whose corners the projection cannot answer for.
	bool Definite = false;
};

// The coverage above, composed down the ancestor chain once. See `Reach` below for the fold that was
// this function's only caller until a window needed to be told which screen it is on.
[[nodiscard]] inline Coverage Cover(const SceneStore& store, EntityId id)
{
	const Detail::Ancestry ancestry = Detail::LineageOf(store, id);

	if (!ancestry.Reachable)
	{
		return {};
	}

	// Composed from the top level down, which is the order `ComposedTransform::Push` is written for and
	// the order the lineage above was built in reverse. The root is the identity rather than an output's
	// view: what comes out is global space, where an output's rectangle is stated, so one composition
	// answers for every output instead of one per output.
	ComposedTransform chain{};

	for (std::size_t level = ancestry.Depth; level != 0; --level)
	{
		const Entity* const entity = store.Find(ancestry.Line[level - 1]);

		if (entity == nullptr)
		{
			return {};
		}

		const Node record = entity->Record();

		chain =
			chain.Push(record.Transform, record.Transform.BoundingRadius(record.Extent.Width, record.Extent.Height));
	}

	const Entity* const node = store.Find(id);

	if (node == nullptr)
	{
		return {};
	}

	const Size<SurfaceSpace, float> extent = node->Extent;

	if (extent.IsEmpty())
	{
		return { .Reachable = true };
	}

	// The four corners of the node's own quad, in the winding `Frame/Projection.h` states — the order
	// does not matter to a bounding box, and using the same one keeps the two readings of "the four
	// corners" from being two different things in two files.
	const Vector3<float> local[4]{ { 0.0F, 0.0F, 0.0F },
		                           { extent.Width, 0.0F, 0.0F },
		                           { extent.Width, extent.Height, 0.0F },
		                           { 0.0F, extent.Height, 0.0F } };

	double left = 0.0;
	double top = 0.0;
	double right = 0.0;
	double bottom = 0.0;

	for (std::size_t corner = 0; corner < 4; ++corner)
	{
		const Projected projected = chain.Project(local[corner]);

		if (!projected.IsVisible())
		{
			return { .Reachable = true };
		}

		const double x = projected.Position.X;
		const double y = projected.Position.Y;

		left = corner == 0 ? x : std::min(left, x);
		right = corner == 0 ? x : std::max(right, x);
		top = corner == 0 ? y : std::min(top, y);
		bottom = corner == 0 ? y : std::max(bottom, y);
	}

	return { .Bounds = Rect<GlobalSpace>::FromEdges({ left, top }, { right, bottom }),
		     .Reachable = true,
		     .Definite = true };
}

// The outputs a coverage lands on, as a mask over an output set.
//
// Zero for an entity that names nothing live, one that is hidden or under something hidden, and one
// nested deeper than the frame walk will descend. Every output for a quad the projection cannot
// answer for — see `AllOutputs` above for which way that trade runs.
//
// Taken as a coverage rather than an entity, which is what a caller with
// both halves in hand wants: `Scene/Commit.h` needs the bounds *and* the mask when it reserves exit
// storage, and asking twice would walk the ancestor chain twice for one answer.
[[nodiscard]] inline OutputReach ReachOf(const Coverage& cover, std::span<const SceneOutput> outputs)
{
	if (outputs.empty() || !cover.Reachable)
	{
		return 0;
	}

	if (!cover.Definite)
	{
		return AllOutputs(outputs.size());
	}

	OutputReach reach = 0;

	for (std::size_t index = 0; index < outputs.size() && index < MaxReachableOutputs; ++index)
	{
		const Rect<GlobalSpace>& bounds = outputs[index].Bounds;

		// Half-open in both axes, which is the same rule a scissor rectangle is read under: a window
		// whose right edge is exactly an output's left edge puts no pixel on it, and counting that as an
		// intersection is how a window one pixel off a screen paces itself against a panel it is not on.
		const bool overlaps = cover.Bounds.Left() < bounds.Right() && cover.Bounds.Right() > bounds.Left() &&
		                      cover.Bounds.Top() < bounds.Bottom() && cover.Bounds.Bottom() > bounds.Top();

		if (overlaps)
		{
			reach |= OutputReach{ 1 } << index;
		}
	}

	return reach;
}

// The outputs this entity's own quad lands on. The ordinary spelling, for a caller that wants the mask
// and nothing else.
[[nodiscard]] inline OutputReach Reach(const SceneStore& store, EntityId id)
{
	return ReachOf(Cover(store, id), store.Outputs());
}
