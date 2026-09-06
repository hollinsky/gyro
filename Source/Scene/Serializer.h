#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Animation/Solve/Ramp.h"
#include "Animation/Solve/Spring.h"
#include "Core/Session.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Geometry/AxisTransform.h"
#include "Geometry/NodeTransform.h"
#include "Publication/Publisher/Publisher.h"
#include "Publication/Snapshot.h"
#include "Scene/Entity.h"
#include "Scene/Settle.h"
#include "Scene/Store.h"
#include "World/Content.h"
#include "World/Node.h"
#include "World/Root.h"

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
// **A channel crosses as a coefficient exactly where it is still owed a frame.** Decision 86 makes the
// runs the *active* set and decision 98 makes `NoCoefficient` the statement that a channel is at rest,
// so the question this file asks of each channel is `Animatable::NextWake`, and a still desktop
// publishes no coefficients at all. The model value goes inline either way, because the inline value is
// redundant while a spring is active and is the whole answer when it is not — one line rather than a
// case. `IsAtRest` was that question until the wake fold arrived, and the difference between the two is
// the whole of the paragraph on retirement below: a spring inside its thresholds has finished as far as
// the schedule is concerned and is not yet at rest, and publishing it on the strength of the second
// predicate is a scene that draws a coefficient nothing will ever move again.
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
// **A channel that has settled is retired here, and that is why the store arrives non-const.**
// `Animation/Author/Animatable.h` defers "the method that retires a settled spring" to *the publisher
// that owns the array*, and this is that publisher: the array is the runs below. The two questions are
// one question — *is this channel at rest* decides whether a coefficient crosses, and *has it settled*
// is the same question asked with the thresholds in hand — so they are asked together, per channel, in
// one place. Split into a pass of its own it would touch every entity twice per publication, and worse,
// a caller could run one without the other: publishing a coefficient whose wake says settled is a scene
// that never reaches idle, and publishing a wake for a coefficient that was dropped is a scene that
// stops mid-motion. Neither state is reachable from here, because the same branch decides both.
//
// What the retirement actually does is `Animatable::Settle` — the model value is where the spring
// already was to within the threshold, so the *published* value moves by less than decision 54's own
// snap moves it a moment later. Nothing a person can see happens at the instant a channel retires.
//
// **The wake fold is scene-wide and replicated across the outputs, deliberately.** Decision 69 makes
// `Sooner` a monoid so the fold *may* be partitioned per output, and Docs/Animation.md's example for
// why is a cursor blinking on one panel and not the other. That example is a contributor attached to an
// *output*, and every such contributor still partitions exactly: an idle timeout, a client's
// `wp_fifo_v1` pairing, a console blink. The contributor here is attached to a *node*, and partitioning
// it means knowing which outputs the node reaches while it moves — a swept screen-space bound per node,
// composed through the transform chain, recomputed on the dispatch thread at commit rate. That is the
// walk `Frame/Evaluator.h` performs a few milliseconds later, run again on the side that is called at
// input rate rather than at frame rate. So every output is told the whole scene's answer, which wakes a
// panel with nothing moving on it for the length of an animation happening on the panel beside it. The
// cost is real and it is named in Docs/Open.md rather than hidden here: it is a composite, on the same
// device queue the animation is being drawn on.
//
// **Naive by construction, and Docs/Open.md asks that it stay that way for now.** Every publication is
// a full re-serialisation of the node run, which decision 111 accepts on its own grounds: any insertion
// renumbers indices, enclosing lengths, and every backward reference target, so there is no patch
// smaller than the run. What that entry leaves open is narrower and more useful — during a gesture the
// node run is byte-identical publication to publication while the coefficient run is fresh, and whether
// copying those bytes beats re-walking to produce them is a measurement.
//
// **One run is staged empty and it is not an oversight.** Decision 72's driven ramp has no author until
// an interactive transition has one, so it crosses as a declared run of length zero rather than as an
// absence the reader would have to have a second reading for.

class SceneSerializer
{
public:
	SceneSerializer() = default;

	// The thresholds this serializer settles against, for a test that wants to vary them. `Scene/Settle.h`
	// carries the numbers and the argument for each; nothing in the design passes anything but the
	// default.
	explicit SceneSerializer(SettlePolicy policy) noexcept : m_Policy{ policy } {}

	// Serialise the store into a publisher ready for `Build`. The reference stays valid until the next
	// call, which is the shape `SnapshotOutbox::Publish` wants: `outbox.Publish(serializer.Serialize(store))`.
	//
	// Dispatch-side, so it may allocate — and after the first few frames of a session it does not,
	// because every vector below keeps its capacity across calls.
	//
	// **The store is not const**, for the reason the header gives at length: a channel that has settled
	// is retired on the same visit that decides whether it crosses.
	//
	// The instant everything is judged at is the store's own clock rather than an output's predicted
	// presentation, which is the conservative direction and the only one available: dispatch's now is at
	// or before every presentation the frame thread will evaluate for, so a channel retired here had
	// already settled by every instant that will read it. Decision 57's one reader of the timebase is
	// what `SceneStore::Now` is.
	const SnapshotPublisher& Serialize(SceneStore& store)
	{
		Reset(store);
		Sweep(store);
		Walk(store);
		Stage(store);

		return m_Publisher;
	}

	// What the last serialisation folded to: what the scene as a whole still owes, before it was
	// replicated across the outputs. Nothing in the design reads this — the frame thread reads the
	// per-output schedule in the snapshot header — and it is here so a test can assert the fold rather
	// than infer it from the bytes.
	[[nodiscard]] Wake SceneWake() const noexcept { return m_Wake; }

	// When *dispatch* has to look at this scene again, which is a different question from the one above
	// and asked on the other side of the boundary.
	//
	// **Without it the two halves of settling do not close, and the failure swaps one bug for another.**
	// A scene is published when a commit resolves. A free-running animation commits once and then nothing
	// commits again, so the snapshot the frame thread holds says *every frame, forever* and no later
	// publication ever contradicts it — the compositor that used to draw one frame and stop would instead
	// draw every frame and never stop, which is the same invariant broken from the other end.
	// Docs/Architecture.md#doing-nothing-must-cost-nothing is what both violate.
	//
	// So the answer is the earliest instant at which some active channel comes to rest — `SettlesAt`, the
	// analytic settle decision 11 exists for, folded by the same monoid. It is `Timed` rather than
	// `Continuous` because dispatch has exactly one thing to do at that instant and nothing to do
	// between: re-serialise, retire whatever finished, and publish a scene that owes less. A spring that
	// never settles saturates the instant, which reads here as *no republication is owed*, and is correct
	// for the one motion in the design that genuinely never stops — decision 69's sentinel collision
	// avoided rather than met, because this is the fold over *retirements* and not over frames.
	//
	// **Nothing arms it yet**, and that is deliberate rather than unfinished: there is no dispatch event
	// loop in the tree to arm anything, and inventing one here would fix its shape from this file. What
	// exists is the number, computed where the coefficients are already in hand, so the loop that
	// eventually arms it is reading a fold rather than inventing a second one.
	[[nodiscard]] Wake Republish() const noexcept { return m_Republish; }

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
		m_At = store.Now();
		m_Thresholds = SettleThresholdSet{ store.Outputs(), m_Policy };
		m_Wake = Wake::Never();
		m_Republish = Wake::Never();

		m_Nodes.clear();
		m_Translations.clear();
		m_Scales.clear();
		m_Rotations.clear();
		m_Opacities.clear();
		m_Images.clear();
		m_Solids.clear();
		m_Views.clear();
		m_Wakes.clear();
		m_Roots.clear();
		m_Sessions.clear();
		m_Open.clear();

		// Where each entity's record landed, indexed by the entity's slot, so a reference can be
		// resolved to a node index. Refilled rather than patched, because a slot that was not reached
		// this time must not read as the position it held last time — which for a reference whose
		// target has been detached is a thumbnail of whatever now occupies that node index.
		m_Published.assign(store.SlotCount(), NoContent);
	}

	// Decision 114's second step: destroy the retiring subtrees that have finished dying.
	//
	// **It is here and not in the store because settling is this file's question.** The store can say
	// which subtrees are retiring; whether one has *come to rest* is `Animatable::NextWake` against the
	// thresholds `Reset` just resolved from the output set, and those exist for the length of a
	// serialisation. It is the same argument that already put channel retirement in this file — the walk
	// that decides whether a coefficient crosses is the walk that holds the thresholds — applied to the
	// entity instead of to the channel.
	//
	// **Before the walk rather than after it, and that is a frame on the screen.** Swept first, the pass
	// on which an exit finishes is the pass that publishes the scene without it, so the last frame a
	// person sees is the one where the animation ended. Swept afterwards, the finished entity would be in
	// the run this pass is about to build and would need a further publication to leave — and nothing
	// would arm one, because a channel that settles contributes no wake, so the closing window would stop
	// at its last frame and stay on screen until something unrelated republished.
	//
	// The predicate is *settled*, not *at rest*, and the difference is one pass. A spring inside both
	// thresholds is finished as far as the schedule is concerned and does not become `IsAtRest` until
	// something calls `Settle` on it — which for a subtree about to be destroyed is never, since the walk
	// that would have done it is the walk this sweep is keeping the entity out of.
	void Sweep(SceneStore& store)
	{
		m_Dead.clear();

		for (const EntityId root : store.RetiringRoots())
		{
			if (HasFinished(store, root))
			{
				m_Dead.push_back(root);
			}
		}

		// Collected first and destroyed second, because destroying is what edits the list being iterated.
		for (const EntityId root : m_Dead)
		{
			[[maybe_unused]] const bool destroyed = store.Destroy(root);
		}
	}

	// Whether every channel of every entity in this subtree has settled. One still moving keeps the whole
	// subtree alive, which is what decision 114 means by a retiring entity still being evaluated: a window
	// whose own opacity has finished does not vanish out from under a subsurface still sliding away.
	[[nodiscard]] bool HasFinished(const SceneStore& store, EntityId root)
	{
		m_Pending.clear();
		m_Pending.push_back(root);

		while (!m_Pending.empty())
		{
			const EntityId at = m_Pending.back();
			m_Pending.pop_back();

			const Entity* const entity = store.Find(at);

			if (entity == nullptr)
			{
				continue;
			}

			const bool settled = Settled(entity->Translation, m_Thresholds.Translation()) &&
			                     Settled(entity->Scale, m_Thresholds.Scaling()) &&
			                     Settled(entity->Turn, m_Thresholds.Rotation()) &&
			                     Settled(entity->Opacity, m_Thresholds.Opacity());

			if (!settled)
			{
				return false;
			}

			for (EntityId child = entity->FirstChild; !child.IsNull();)
			{
				const Entity* const next = store.Find(child);

				m_Pending.push_back(child);
				child = next != nullptr ? next->NextSibling : EntityId{};
			}
		}

		return true;
	}

	// One channel, asked the same question `Coefficient` asks and answered without the write. At rest is
	// settled trivially; anything else is settled exactly when its wake says so.
	template<typename Channel>
	[[nodiscard]] bool Settled(const Channel& channel, SettleThresholds<typename Channel::Scalar> thresholds) const
	{
		return channel.IsAtRest() || channel.NextWake(m_At, thresholds).Which == Wake::Kind::Settled;
	}

	// Preorder over the tree, iteratively. Iteratively rather than recursively because the depth is the
	// author's — a shell that nests a thousand containers would be a stack overflow in the one process
	// on the machine that must not have one, where here it is a vector that grows.
	void Walk(SceneStore& store)
	{
		EntityId cursor = store.FirstRoot();

		while (!cursor.IsNull() || !m_Open.empty())
		{
			Entity* entity = cursor.IsNull() ? nullptr : store.Mutable(cursor);

			// A sibling chain ends at a null link, and a stale one ends it too. The second is not supposed
			// to be reachable — `SceneStore::Destroy` unlinks before it frees, precisely so that a window
			// closing cannot truncate the chain of the windows behind it — and it is written as an ending
			// rather than a skip because a chain whose links no longer name live entities has no next to
			// go to. Ending early loses the siblings after the break; skipping would follow a link into a
			// reused slot and publish whatever now lives there, which is a closed window's neighbours
			// replaced by something else rather than missing.
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

			// A root is an entity with nothing open above it, which is the whole of what depth means to
			// this walk. Recorded here rather than by a second pass over the store's root chain, because
			// the position a root landed at in the node run is only known while the run is being built.
			if (m_Open.empty())
			{
				// The entity's own answer, and it is asked here rather than of every node because a
				// session belongs to a root — `SceneStore::SetSession` refuses anything else, so a
				// window's own field is never consulted and never has to be kept true.
				m_Roots.push_back(SceneRoot{ .Node = index, .Session = entity->Session });
			}

			m_Nodes.push_back(Emit(*entity, store, index));
			m_Published[cursor.Index] = index;

			m_Open.push_back({ .Index = index, .Next = entity->NextSibling });

			cursor = entity->FirstChild;
		}
	}

	// One entity's record: the fields it carries itself, plus the three that are positions in runs this
	// walk is building.
	[[nodiscard]] Node Emit(Entity& entity, const SceneStore& store, std::uint32_t index)
	{
		Node node = entity.Record();

		node.TranslationSpring = Coefficient(entity.Translation, m_Translations, m_Thresholds.Translation());
		node.ScaleSpring = Coefficient(entity.Scale, m_Scales, m_Thresholds.Scaling());

		// The rotation channel is the one that composes rather than replaces: the spring is over the
		// geodesic deviation from the orientation the record already carries, which is the chart's base
		// point. See `Geometry/NodeTransform.h`'s `FromDeviation`, which is the other end of this.
		//
		// **Retiring it is still `Settle`, and the chart is why that is not obvious.** The deviation's
		// target is zero, so settling drives the deviation to zero and leaves `Orientation` — the base
		// point the record already carries — exactly where it is. A rotation that has finished is
		// therefore the orientation the commit set, published inline, which is what decision 90 means by
		// a node carrying whatever reconstitutes its value.
		node.RotationSpring = Coefficient(entity.Turn, m_Rotations, m_Thresholds.Rotation());
		node.OpacitySpring = Coefficient(entity.Opacity, m_Opacities, m_Thresholds.Opacity());

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

	void Stage(SceneStore& store)
	{
		for (SceneOutput& output : store.MutableOutputs())
		{
			m_Views.push_back(output.Placement());

			// Decision 69's schedule, one entry per output in output order, and every entry the same —
			// the replication this file's header argues for. It is written per output rather than as one
			// value with a count because the *carrier* is per output and stays that way: the day a
			// contributor attached to an output arrives, it folds into its own entry here and nothing
			// downstream changes, which is the property the monoid was chosen for.
			m_Wakes.push_back(m_Wake);

			// Decision 21's assignment and decision 188's transition, which the composition root wrote
			// onto the output and this only carries. `SessionId::None` is an output showing gyro's own
			// scene rather than one nobody has got round to, so it crosses as itself and the frame
			// thread needs no absent case.
			//
			// **The fade goes in the *opacity* run rather than a run of its own**, which is what makes
			// it one comparison and one multiply on the far side: a spring is a spring whether a node
			// named it or an output did, and a second run would be a second length for decision 84 to
			// check and a second thing to leave unstaged. It is the only coefficient in the snapshot
			// nothing in the node run points at.
			const std::uint32_t fade = Coefficient(output.Fade, m_Opacities, m_Thresholds.Opacity());

			// **A fade with no coefficient left is a transition that is over**, and retiring the pair
			// here is decision 122's rule — the walk that decides whether a coefficient still crosses is
			// the walk that has the thresholds in hand. Left set, the outgoing session would be
			// composited at zero for ever: invisible, paid for every frame, and still counted as
			// leaving by everything that asks whether it is on a screen.
			if (fade == NoCoefficient)
			{
				output.Outgoing = SessionId::None;
				output.Fade.SetImmediate(0.0F);
			}

			m_Sessions.push_back(SceneAssignment{ .Shown = output.Session, .Outgoing = output.Outgoing, .Fade = fade });
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
		m_Publisher.PutWakes(m_Wakes);
		m_Publisher.PutViews<OutputAdapter>(m_Views);
		m_Publisher.PutImages<ImageContent>(m_Images);
		m_Publisher.PutSolids<SolidContent>(m_Solids);
		m_Publisher.PutRoots<SceneRoot>(m_Roots);
		m_Publisher.PutSessions<SceneAssignment>(m_Sessions);
	}

	// One channel's whole story: retire it if it has settled, publish it if it has not, and fold what it
	// still owes into the scene's answer.
	//
	// **Three outcomes and one branch**, which is what makes the two representations unable to disagree.
	// At rest there is nothing to do and nothing to say. Settled but not at rest is the case this
	// function exists for — the spring is inside both thresholds and stays there, so it is brought to
	// rest here and crosses as an inline model value, and the schedule hears nothing from it. Still
	// moving is a coefficient in the run and a term in the fold, and the two come from one call to
	// `NextWake` rather than from a predicate and a separate answer that could differ.
	//
	// `Sooner` is decision 69's reduction, applied here rather than at the end, so the fold is over the
	// contributors as the walk meets them and the result is independent of the order it meets them in.
	template<typename Channel, typename Run>
	[[nodiscard]] std::uint32_t
	Coefficient(Channel& channel, Run& run, SettleThresholds<typename Channel::Scalar> thresholds)
	{
		if (channel.IsAtRest())
		{
			return NoCoefficient;
		}

		const Wake wake = channel.NextWake(m_At, thresholds);

		if (wake.Which == Wake::Kind::Settled)
		{
			channel.Settle();

			return NoCoefficient;
		}

		m_Wake = Sooner(m_Wake, wake);
		m_Republish = Sooner(m_Republish, Wake::At(channel.Coefficients().SettlesAt(thresholds)));

		return Append(run, channel.Coefficients());
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

	// Decision 21's partition, both halves: one entry per root and one per output.
	std::vector<SceneRoot> m_Roots;
	std::vector<SceneAssignment> m_Sessions;
	std::vector<Wake> m_Wakes;

	// The sweep's two scratch lists: what finished dying this pass, and the subtree stack that decides it.
	// Members rather than locals for the same reason every run above is, which is that a serialisation
	// after the first few frames of a session allocates nothing.
	std::vector<EntityId> m_Dead;
	std::vector<EntityId> m_Pending;

	// What this serialisation is judged at, and what against. Both are resolved once in `Reset` rather
	// than per node: the instant is the store's clock, which decision 57 makes one read, and the
	// thresholds are a function of the output set, which does not change while a walk is running.
	SettlePolicy m_Policy{};
	SettleThresholdSet m_Thresholds{};
	Instant m_At{};
	Wake m_Wake = Wake::Never();
	Wake m_Republish = Wake::Never();

	std::vector<Open> m_Open;
	std::vector<std::uint32_t> m_Published;
};
