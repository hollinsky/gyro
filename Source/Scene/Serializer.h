#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Animation/Solve/Ramp.h"
#include "Animation/Solve/Spring.h"
#include "Geometry/AxisTransform.h"
#include "Geometry/NodeTransform.h"
#include "Publication/Publisher/Publisher.h"
#include "Publication/Snapshot.h"
#include "Scene/Entity.h"
#include "Scene/Store.h"
#include "World/Content.h"
#include "World/Node.h"

// The store, turned into the bytes the frame thread walks.
//
// Docs/Decisions.md decision 86 is the format — a preorder run of node records, each naming the length
// of its own subtree, with model values inline and coefficients by reference — and this is the one
// place that produces it. `Publication/Publisher/Publisher.h` lays the runs out and owns the storage;
// this file decides what goes in them.
//
// **Preorder plus subtree length is what the walk on the other side is shaped for.** The frame side
// scans a contiguous run with an explicit transform stack, and skipping a hidden or wholly-culled
// subtree is an addition rather than a test per node — nine workspaces with one visible is what that
// costs on every frame. The length falls out of the walk here for the reason decision 111 gives: one
// entity is one record, so there is no second accounting of what an entity contributed before its
// children's could be added to it.
//
// **A channel crosses as a coefficient exactly where it is moving.** Decision 86 makes the runs the
// *active* set and decision 98 makes `NoCoefficient` the statement that a channel is at rest, so the
// question this file asks of each channel is `Animatable::IsAtRest`, and a still desktop publishes no
// coefficients at all. The model value goes inline either way, because the inline value is redundant
// while a spring is active and is the whole answer when it is not — one line rather than a case.
//
// **The published index is not the store's index, for either kind of payload.** An entity's `Content`
// is a position in the store's own per-kind array and a reference's target is an `EntityId`; what
// crosses is a position in a run this walk is building and a *node index* respectively. Both are
// translations, and doing them here is what lets the store index its content however it likes.
//
// **A reference's target must already have been emitted, and preorder is what guarantees it.** Decision
// 95 requires the target's index to be lower than the reference's, which makes a cycle unrepresentable
// rather than something the frame thread has to detect — and decision 90 declines to rest on a
// detector, because an unbounded traversal inside the frame section on a `SCHED_FIFO` thread ends with
// `RLIMIT_RTTIME` taking every session's UI at once. A reference the walk cannot resolve names no
// target, which `Frame/Evaluator.h` reads as an expansion it will not perform.
//
// **Naive by construction, and Docs/Open.md asks that it stay that way for now.** Every publication is
// a full re-serialisation of the node run, which decision 111 accepts on its own grounds: any insertion
// renumbers indices, enclosing lengths, and every backward reference target, so there is no patch
// smaller than the run. What that entry leaves open is narrower and more useful — during a gesture the
// node run is byte-identical publication to publication while the coefficient run is fresh, and whether
// copying those bytes beats re-walking to produce them is a measurement.
//
// **Two runs are staged empty and neither is an oversight.** Decision 72's driven ramp has no author
// until an interactive transition has one. And the per-output wake schedule of decision 69 is a fold
// over `Animatable::NextWake`, which takes the settling thresholds — geometric ones are in device
// pixels of the finest grid a node intersects, and the policy for opacity and the dressings is open —
// so folding it before those exist would answer the open question by whoever wrote the default first.

class SceneSerializer
{
public:
	// Serialise the store into a publisher ready for `Build`. The reference stays valid until the next
	// call, which is the shape `SnapshotOutbox::Publish` wants: `outbox.Publish(serializer.Serialize(store))`.
	//
	// Dispatch-side, so it may allocate — and after the first few frames of a session it does not,
	// because every vector below keeps its capacity across calls.
	const SnapshotPublisher& Serialize(const SceneStore& store)
	{
		Reset(store);
		Walk(store);
		Stage(store);

		return m_Publisher;
	}

	// What the last serialisation produced. Nothing in the design reads these — they are here so a test
	// can assert that the active set is the active set rather than infer it from a draw list.
	[[nodiscard]] std::span<const Node> Nodes() const noexcept { return m_Nodes; }
	[[nodiscard]] std::size_t ActiveTranslations() const noexcept { return m_Translations.size(); }
	[[nodiscard]] std::size_t ActiveScales() const noexcept { return m_Scales.size(); }
	[[nodiscard]] std::size_t ActiveRotations() const noexcept { return m_Rotations.size(); }
	[[nodiscard]] std::size_t ActiveOpacities() const noexcept { return m_Opacities.size(); }

private:
	// One node whose subtree is still being emitted: where its record is, and where the walk goes when
	// that subtree finishes.
	struct Open
	{
		std::uint32_t Index = 0;
		EntityId Next{};
	};

	void Reset(const SceneStore& store)
	{
		m_Nodes.clear();
		m_Translations.clear();
		m_Scales.clear();
		m_Rotations.clear();
		m_Opacities.clear();
		m_Images.clear();
		m_Solids.clear();
		m_Views.clear();
		m_Open.clear();

		// Where each entity's record landed, indexed by the entity's slot, so a reference can be
		// resolved to a node index. Refilled rather than patched, because a slot that was not reached
		// this time must not read as the position it held last time — which for a reference whose
		// target has been detached is a thumbnail of whatever now occupies that node index.
		m_Published.assign(store.SlotCount(), NoContent);
	}

	// Preorder over the tree, iteratively. Iteratively rather than recursively because the depth is the
	// author's — a shell that nests a thousand containers would be a stack overflow in the one process
	// on the machine that must not have one, where here it is a vector that grows.
	void Walk(const SceneStore& store)
	{
		EntityId cursor = store.FirstRoot();

		while (!cursor.IsNull() || !m_Open.empty())
		{
			const Entity* entity = cursor.IsNull() ? nullptr : store.Find(cursor);

			// A sibling chain ends at a null link, and a stale one ends it too. The second cannot happen
			// while nothing is destroyed, and it is written as an ending rather than a skip because a
			// chain whose links no longer name live entities has no next to go to.
			if (entity == nullptr)
			{
				if (m_Open.empty())
				{
					return;
				}

				const Open done = m_Open.back();
				m_Open.pop_back();

				// A run length over nodes, not counting the node itself — `Seam/Renderer.h`'s
				// `DrawGroup::Count` convention exactly, which decision 86 asks for by name so that the
				// published tree and the emitted draw list are read in one idiom rather than two.
				m_Nodes[done.Index].SubtreeLength = static_cast<std::uint32_t>(m_Nodes.size() - done.Index - 1);

				cursor = done.Next;
				continue;
			}

			const auto index = static_cast<std::uint32_t>(m_Nodes.size());

			m_Nodes.push_back(Emit(*entity, store, index));
			m_Published[cursor.Index] = index;

			m_Open.push_back({ .Index = index, .Next = entity->NextSibling });

			cursor = entity->FirstChild;
		}
	}

	// One entity's record: the fields it carries itself, plus the three that are positions in runs this
	// walk is building.
	[[nodiscard]] Node Emit(const Entity& entity, const SceneStore& store, std::uint32_t index)
	{
		Node node = entity.Record();

		if (!entity.Translation.IsAtRest())
		{
			node.TranslationSpring = Append(m_Translations, entity.Translation.Coefficients());
		}

		if (!entity.Scale.IsAtRest())
		{
			node.ScaleSpring = Append(m_Scales, entity.Scale.Coefficients());
		}

		// The rotation channel is the one that composes rather than replaces: the spring is over the
		// geodesic deviation from the orientation the record already carries, which is the chart's base
		// point. See `Geometry/NodeTransform.h`'s `FromDeviation`, which is the other end of this.
		if (!entity.Turn.IsAtRest())
		{
			node.RotationSpring = Append(m_Rotations, entity.Turn.Coefficients());
		}

		if (!entity.Opacity.IsAtRest())
		{
			node.OpacitySpring = Append(m_Opacities, entity.Opacity.Coefficients());
		}

		switch (entity.Kind)
		{
			case NodeKind::Container:
				break;

			case NodeKind::Image:
				node.Content = Copy(m_Images, store.Images(), entity.Content);
				break;

			case NodeKind::Solid:
				node.Content = Copy(m_Solids, store.Solids(), entity.Content);
				break;

			case NodeKind::Reference:
				node.Content = Resolve(entity.Target, store, index);
				break;
		}

		return node;
	}

	// A reference's target, as a node index strictly below this one, or `NoContent`.
	//
	// The two refusals are one refusal: a target that has not been emitted yet is a forward reference,
	// and a target that was never reached at all is a subtree hanging off nothing. Both would have to be
	// detected on the frame side to be safe there, which decision 90 will not have, so both stop here —
	// and `Frame/Evaluator.h` refuses the expansion a second time anyway, on the same comparison.
	[[nodiscard]] std::uint32_t Resolve(EntityId target, const SceneStore& store, std::uint32_t index) const noexcept
	{
		if (!store.IsLive(target) || target.Index >= m_Published.size())
		{
			return NoContent;
		}

		const std::uint32_t at = m_Published[target.Index];

		return at < index ? at : NoContent;
	}

	void Stage(const SceneStore& store)
	{
		for (const SceneOutput& output : store.Outputs())
		{
			m_Views.push_back(output.Placement());
		}

		// Every run is staged on every serialisation, including the empty ones. The publisher is reused
		// across calls, so a run left unstaged would be the previous scene's — which for the coefficient
		// runs is a node pointing at a spring that belonged to something else.
		m_Publisher.Put<Spring<Vector3<double>>>(SnapshotRun::Translation, m_Translations);
		m_Publisher.Put<Spring<Vector3<float>>>(SnapshotRun::Scale, m_Scales);
		m_Publisher.Put<Spring<RotationVector>>(SnapshotRun::Rotation, m_Rotations);
		m_Publisher.Put<Spring<float>>(SnapshotRun::Opacity, m_Opacities);
		m_Publisher.Put<Ramp>(SnapshotRun::DrivenProgress, {});

		m_Publisher.PutNodes<Node>(m_Nodes);
		m_Publisher.PutViews<OutputAdapter>(m_Views);
		m_Publisher.PutImages<ImageContent>(m_Images);
		m_Publisher.PutSolids<SolidContent>(m_Solids);
	}

	// Append one element and hand back where it landed. The position is the index a node record names,
	// which is a position within its *own* channel's array — decision 90's one run per channel, which is
	// what makes pointing a scale index at a rotation spring inexpressible rather than caught.
	template<typename T>
	[[nodiscard]] static std::uint32_t Append(std::vector<T>& run, const T& element)
	{
		const auto at = static_cast<std::uint32_t>(run.size());
		run.push_back(element);

		return at;
	}

	// Copy one per-kind payload out of the store and hand back where it landed. A copy rather than a
	// build, which is decision 111's out-of-line payload paying off: the two sides keep the record in
	// the same shape for unrelated reasons, so the emission is a memcpy.
	//
	// An entity whose content index does not name a record draws nothing rather than drawing element
	// zero of the run — the direction `World/Node.h`'s sentinel is chosen for, since the alternative is
	// a container silently wearing the first window's pixels.
	template<typename T>
	[[nodiscard]] static std::uint32_t Copy(std::vector<T>& run, std::span<const T> from, std::uint32_t at)
	{
		if (at >= from.size())
		{
			return NoContent;
		}

		return Append(run, from[at]);
	}

	SnapshotPublisher m_Publisher;

	std::vector<Node> m_Nodes;

	std::vector<Spring<Vector3<double>>> m_Translations;
	std::vector<Spring<Vector3<float>>> m_Scales;
	std::vector<Spring<RotationVector>> m_Rotations;
	std::vector<Spring<float>> m_Opacities;

	std::vector<ImageContent> m_Images;
	std::vector<SolidContent> m_Solids;
	std::vector<OutputAdapter> m_Views;

	std::vector<Open> m_Open;
	std::vector<std::uint32_t> m_Published;
};
