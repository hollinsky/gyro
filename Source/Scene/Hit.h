#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "Core/Handle.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Space.h"
#include "Scene/Entity.h"
#include "Scene/Input.h"
#include "Scene/Reach.h"
#include "Scene/Store.h"
#include "World/Node.h"

// What is under the pointer, asked on the dispatch side.
//
// **It is here rather than in `Protocol` because the tree, the transforms and the z order are the
// world's.** Decision 51 gives gyro hit-testing as mechanism; what a hit test needs is the composed
// chain of every node and the order they are drawn in, and both are the store's. Putting the walk in
// `Protocol` would mean a second traversal of a tree that module does not own, disagreeing with the
// picture the moment the two are edited apart — and it would leave the splash and the recovery
// console, which have no client behind them, with no way to be pointed at.
//
// **The frontmost node under the pointer is the last one drawn over it, so the walk is the draw order
// and the last hit wins.** Composition is strict tree order with no depth buffer (55), and decision 55
// makes the sibling list the z order with the last root frontmost — so a preorder walk visits nodes in
// exactly the order a renderer paints them, and keeping the last one that contains the point is
// therefore the topmost by construction. It cannot disagree with what is on screen, because it is the
// same order.
//
// **Rejected: walking backwards and stopping at the first hit.** It is the algorithm every compositor
// writes and it is the one this store cannot afford: decision 111 links children forwards only, so
// reversing a sibling chain means either a previous-sibling link on every entity — four bytes on every
// node in the world to serve the pointer — or collecting each chain into scratch on the way down,
// which allocates on the path a mouse motion travels. What early exit would save is the tail of a walk
// whose whole length is the world, once per dispatch iteration, on the thread that is about to
// serialise that same world anyway.
//
// **Three things pass the pointer through rather than catching it**, and each is a rule stated
// somewhere else arriving here:
//
// - A node that accepts nothing, which is every node until an author says otherwise
//   ([Input.h](Input.h)). The cursor is the case that makes this load-bearing.
// - A retiring node ([Entity.h](Entity.h) names hit-testing as one of the three readers of that flag).
//   A window that is closing is on screen for as long as its exit takes, and clicking the ghost of an
//   application that has exited is worse than clicking through to whatever is behind it.
// - Anything hidden, or under something hidden. `Node::Hidden` skips the subtree in the frame walk, so
//   a window on an inactive workspace draws nothing and catches nothing.
//
// **What is deliberately absent is clipping**, because the scene vocabulary has none —
// [Open.md](../../Docs/Open.md) carries it. A child outside its parent's extent is drawn, so it is
// hit, and the two agree. When clipping lands it lands in both places or in neither.

// A node and where on it the pointer landed.
struct SceneHit
{
	// Null where nothing accepted the point, which is the ordinary answer over the background.
	EntityId Node{};

	// In the node's own space, real-valued and unrounded — this is what becomes a `wl_pointer.motion`,
	// and `wl_fixed` is the only place it is ever quantized. Decision 52's rule about the grid belonging
	// to an output, on the one coordinate that travels back out to a client.
	Point<SurfaceSpace> Local{};

	[[nodiscard]] constexpr explicit operator bool() const noexcept { return !Node.IsNull(); }

	friend constexpr bool operator==(const SceneHit&, const SceneHit&) noexcept = default;
};

namespace Detail
{
// Whether this node catches the point, and where on it. The chain is the node's own, already composed
// down from the top level, so global space is what it is being unprojected out of.
[[nodiscard]] inline bool Catches(
	const NodeInput& input,
	const Entity& entity,
	const ComposedTransform& chain,
	Point<GlobalSpace> point,
	Point<SurfaceSpace>& local
)
{
	const Unprojected found = chain.Unproject(point);

	// A quad with no area, or a point that has passed through the eye of some perspective above this
	// node — the same cull `Projected` names, arriving on the other side of the arithmetic.
	if (!found.IsVisible())
	{
		return false;
	}

	// Half-open in both axes, which is `Scene/Reach.h`'s convention and `Geometry/Shape.h`'s: the column
	// at the right edge belongs to whatever is beyond it, so two windows sharing an edge do not both
	// answer for the pixel between them.
	if (found.Local.X < 0.0F || found.Local.X >= entity.Extent.Width || found.Local.Y < 0.0F ||
	    found.Local.Y >= entity.Extent.Height)
	{
		return false;
	}

	// The extent bounds the shape rather than the other way round, so a client that states a region
	// larger than its surface gets the surplus ignored — which is what the protocol says happens.
	if (input.Shape && !input.Shape->Contains(found.Local))
	{
		return false;
	}

	local = found.Local;

	return true;
}
} // namespace Detail

// Where a point of global space falls on one *named* entity, whatever is over it.
//
// **This is what a grab is made of.** Once a button is down the coordinates belong to the surface that
// was under the pointer when it went down, wherever the pointer travels afterwards: a person dragging
// a scrollbar past the edge of its window is still dragging that scrollbar, and a toolkit tracking the
// drag needs numbers that keep going rather than numbers that stop at the frame. So this asks about a
// stated entity rather than about whatever is frontmost, and it answers outside the extent as readily
// as inside it — clamping would be gyro deciding a gesture had ended.
//
// It is deliberately *not* filtered by what the entity accepts. An input shape says where a surface
// takes the pointer, which is a question about where a press lands; a grab already landed, and a
// finger sliding over a corner the client cut out of its region must not make the drag stutter.
//
// Nothing where the entity names nothing live, is hidden or under something hidden, sits deeper than
// the frame walk descends, or lies edge-on to the viewer. The ancestor walk is `Scene/Reach.h`'s, for
// the same reason it is one there: what is asked about is the handful of nodes somebody is pointing
// at rather than the world.
[[nodiscard]] inline std::optional<Point<SurfaceSpace>>
LocalOn(const SceneStore& store, EntityId id, Point<GlobalSpace> point)
{
	const Detail::Ancestry ancestry = Detail::LineageOf(store, id);

	if (!ancestry.Reachable)
	{
		return std::nullopt;
	}

	ComposedTransform chain{};

	for (std::size_t level = ancestry.Depth; level != 0; --level)
	{
		const Entity* const entity = store.Find(ancestry.Line[level - 1]);

		if (entity == nullptr)
		{
			return std::nullopt;
		}

		const Node record = entity->Record();

		chain =
			chain.Push(record.Transform, record.Transform.BoundingRadius(record.Extent.Width, record.Extent.Height));
	}

	const Unprojected found = chain.Unproject(point);

	if (!found.IsVisible())
	{
		return std::nullopt;
	}

	return found.Local;
}

// The topmost node accepting the pointer at this point, and where on it.
//
// Iterative, with an explicit chain of ancestors, for the reason `SceneStore::Retire` is: the depth is
// an author's to choose and the one process on this machine that cannot overflow a stack is this one.
// The cap is `Scene/Reach.h`'s `MaxReachDepth` — the frame walk's depth restated — and a subtree past
// it is not hit for the same reason it is not drawn.
[[nodiscard]] inline SceneHit HitTest(const SceneStore& store, Point<GlobalSpace> point)
{
	// The chain *above* each open node, so that popping back to a sibling restores what the sibling's
	// own push should start from.
	struct Level
	{
		EntityId At{};
		ComposedTransform Above{};
	};

	std::array<Level, MaxReachDepth> ancestors{};
	std::size_t depth = 0;

	ComposedTransform above{};
	SceneHit found{};

	for (EntityId at = store.FirstRoot(); !at.IsNull();)
	{
		const Entity* const entity = store.Find(at);

		// A link naming nothing live ends the chain, which is how the frame walk reads one too — the
		// alternative is a hit test that outlives a window the picture has already dropped.
		if (entity == nullptr)
		{
			break;
		}

		EntityId next = entity->NextSibling;

		if ((entity->Flags & Node::Hidden) == 0)
		{
			const Node record = entity->Record();
			const ComposedTransform chain = above.Push(
				record.Transform, record.Transform.BoundingRadius(record.Extent.Width, record.Extent.Height)
			);

			const NodeInput* const input = entity->Retiring ? nullptr : store.InputFor(at);

			if (input != nullptr)
			{
				Point<SurfaceSpace> local{};

				if (Detail::Catches(*input, *entity, chain, point, local))
				{
					// Last wins: this node is drawn over everything visited before it.
					found = SceneHit{ .Node = at, .Local = local };
				}
			}

			if (!entity->FirstChild.IsNull() && depth < ancestors.size())
			{
				ancestors[depth] = Level{ .At = at, .Above = above };
				++depth;

				above = chain;
				at = entity->FirstChild;

				continue;
			}
		}

		// Out of children: take the sibling, climbing until there is one. Siblings share the chain above
		// them, so restoring it is what the popped level carries.
		while (next.IsNull() && depth != 0)
		{
			--depth;

			const Entity* const parent = store.Find(ancestors[depth].At);

			above = ancestors[depth].Above;
			next = parent != nullptr ? parent->NextSibling : EntityId{};
		}

		at = next;
	}

	return found;
}
