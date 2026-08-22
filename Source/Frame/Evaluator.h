#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Animation/Solve/Spring.h"
#include "Core/Clock.h"
#include "Core/ColorState.h"
#include "Core/Time.h"
#include "Frame/Admission.h"
#include "Frame/Projection.h"
#include "Geometry/AxisTransform.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Region.h"
#include "Geometry/Space.h"
#include "Publication/Reader/Reader.h"
#include "Seam/Renderer.h"
#include "World/Content.h"
#include "World/Node.h"
// See Docs/Architecture.md#the-frame-loop and decisions 82, 86, 88, 90, 92, 93, 94, and 95.

// What turns a published snapshot into the flat span of draw items a renderer is handed.
//
// Decision 82 settles what crosses the render seam and decision 86 settles what crosses the data
// waist; this file is the whole of what happens between them. The walk is a preorder scan of the
// node run with an explicit stack: it composes the transform chain, evaluates the coefficients a
// node names at the instant the frame is predicted to be presented, projects each drawn node through
// `OutputView`, and appends one `DrawItem` per node that survives.
//
// **It is an interface with one real implementation and it is not at the seam.** `Scene` will
// publish, `Frame` will evaluate, and no backend is ever on the far end of it — Docs/Structure.md
// reserves `Seam` for interfaces with more than one implementation. What the interface buys is the
// loop's own tests, which want a walk that costs a named number of microseconds and draws nothing.
//
// **Everything here runs inside the frame section**, so nothing in it allocates: the item arena is
// sized once at construction and the walk stack is a fixed array. The two consequences worth stating
// are that the arena has a capacity a scene can exhaust, and that the depth cap is a real limit
// rather than a guard against a case nobody has — decision 88's references nest, and decision 90
// declines to let an unbounded walk on a `SCHED_FIFO` thread be prevented by dispatch's good
// behaviour alone.
//
// **What the walk does not do yet, named so that neither absence is read as an oversight.**
// `DrawItem::Sampling` stays at its default: classifying the resample needs the buffer-to-surface
// adapter composed with the node chain, and no such adapter is published yet, so a still window takes
// the resampling path until the protocol layer supplies one. And a node's corner radius stays zero:
// decision 96 rounds the *window geometry rect* rather than the node extent and zeroes the radius for
// a window that is fullscreen or tiled edge to edge, which is layout state that has no carrier in the
// published record — so the frame rect `ImageContent` already holds has nothing to bound yet.

// SPEC: how many draw items one output's frame may hold, and therefore how much the arena costs.
// Four thousand is far past any scene gyro has been pointed at — a busy desktop is a few hundred
// nodes, and an overview of nine workspaces is a few thousand — and it is fixed rather than grown
// because decision 36 forbids allocating inside the frame section.
inline constexpr std::size_t MaxDrawItems = 4096;

// SPEC: how deep the walk may nest, counting reference expansions. A window inside a container inside
// a workspace inside an overview is four; the thumbnails in that overview are references, which is
// five. Thirty-two is an order of magnitude of headroom over the deepest arrangement a shell has been
// asked for, and a subtree deeper than this is dropped rather than drawn — the walk is on a thread
// where an unbounded traversal costs every session's UI at once, so the cap refuses rather than grows.
inline constexpr std::size_t MaxWalkDepth = 32;

// What one output's frame is, once the snapshot has been evaluated at its predicted presentation.
//
// Damage is the new damage this evaluation produced, in device space; the loop unions it into what the
// output has been accumulating rather than replacing it, for the reason Frame/Loop.h's header gives.
struct DrawList
{
	std::span<const DrawItem> Items;
	Region<DeviceSpace> Damage;

	// What building this list cost on the frame thread, measured by the evaluator across its own walk.
	//
	// It is filed whether or not the composite that follows is recorded or presented, because the walk
	// happened either way — where `Submission::RecordCost` is filed only against a submission that
	// succeeded, since a refused record is not a frame's cost. The two differ on purpose.
	Duration EvaluateCost{};
};

struct EvaluateRequest
{
	const SnapshotReader& Snapshot;

	// Which output, as an index into the loop's outputs. It is the snapshot's index too — decision 84
	// has per-output runs cross positionally under a set generation — so this is what an evaluator
	// resolves the output's runs with.
	std::size_t Output = 0;

	// How many outputs the loop is serving, which is the other half of decision 84's rule and the
	// reason it is here rather than being inferred. A per-output run whose length is not this is a run
	// from another output set: no information rather than partial information, and indexing into it
	// puts one output's placement on another one's glass.
	std::size_t Outputs = 1;

	// The composite target's extent, from the output's achieved configuration. It is the half of the
	// view the *hardware* owns, where the placement below it is the world's and arrives in the
	// snapshot; Frame/Projection.h takes them already composed, and this is where they meet.
	PixelSize<DeviceSpace> Resolution{};

	// Decision 36 in one parameter: animations are evaluated at a named instant and never against an
	// ambient now. It is the predicted presentation of the frame being drawn, which is per output
	// because there is no global clock to make it anything else.
	Instant Presentation{};

	// Which composite is about to be recorded. The walk does not read it and decision 94 is why: the
	// floor tier is a cheaper shader over the same items, so the same tree is traversed and the same
	// springs are evaluated whichever verdict came back. It is here because an evaluator that ever
	// wants to know — an effect that has no floor form, a dressing the cheap composite cannot draw —
	// would otherwise have the loop grow a second call.
	RenderMode Mode = RenderMode::Planned;
};

class IEvaluator
{
public:
	IEvaluator() = default;

	virtual ~IEvaluator() = default;

	IEvaluator(const IEvaluator&) = delete;
	IEvaluator& operator=(const IEvaluator&) = delete;
	IEvaluator(IEvaluator&&) = delete;
	IEvaluator& operator=(IEvaluator&&) = delete;

	// Called inside the frame section, so the items must come from storage the evaluator already holds.
	// The span is read before the next call and never after it.
	[[nodiscard]] virtual DrawList Evaluate(const EvaluateRequest& request) = 0;
};

// Draws nothing, which is what the loop's own tests want.
//
// It is not a stub in the sense that it is waiting to be replaced by a real implementation of the same
// thing — it is the floor case, and an output with nothing to draw is one the loop must still
// schedule, present, and idle correctly.
class NullEvaluator final : public IEvaluator
{
public:
	[[nodiscard]] DrawList Evaluate(const EvaluateRequest&) override { return {}; }
};

// The walk itself: decision 86's tree in, decision 82's list out.
class SceneEvaluator final : public IEvaluator
{
public:
	// The clock is held for one reason and it is decision 94's: the party that knows where the work
	// started and stopped is the party that did it. That costs two reads per output where
	// Core/Clock.h asks for one per iteration, and the alternative — the loop bracketing the call —
	// costs the same two reads while putting them where the thing being measured cannot see its own
	// boundaries.
	explicit SceneEvaluator(const IClock& clock) : m_Clock{ &clock } { m_Items.resize(MaxDrawItems); }

	[[nodiscard]] DrawList Evaluate(const EvaluateRequest& request) override
	{
		const Instant started = m_Clock->Now();

		m_Count = 0;
		m_Moving = false;
		m_Truncated = false;

		Walk(request);

		Region<DeviceSpace> damage{};

		if (Changed(request))
		{
			damage.Add(PixelRect<DeviceSpace>{ {}, request.Resolution });
		}

		Remember(request);

		return { .Items = std::span<const DrawItem>{ m_Items.data(), m_Count },
			     .Damage = damage,
			     .EvaluateCost = Elapsed(started, m_Clock->Now()) };
	}

	// Whether the last evaluation ran out of arena. Not an error the loop acts on — a frame is not
	// where that is reported — but the sweep and a log line both want to know that a scene was too
	// large for the figure above rather than that a subtree went missing on its own.
	[[nodiscard]] bool Truncated() const noexcept { return m_Truncated; }

private:
	// No item, in the slots below. The arena is far shorter than this, so the sentinel costs no
	// representable index, and it is what a level that composes into no group carries.
	static constexpr std::uint32_t NoItem = 0xFFFF'FFFFu;

	// No expansion, for a node that is not a reference or whose reference is malformed. Past any run
	// the walk can be handed, for `NoItem`'s reason.
	static constexpr std::size_t NoExpansion = static_cast<std::size_t>(-1);

	// One level of the walk: a run of siblings, the chain they hang off, and what they compose into.
	//
	// A level is pushed per container, per group, and per reference expansion, and the chain is
	// carried by value rather than recomputed — that is what Geometry/NodeTransform.h's
	// `ComposedTransform` is for, and it is what makes a node's four corners cost four multiplies
	// rather than four walks of its ancestors.
	struct Level
	{
		ComposedTransform Chain{};

		std::size_t Index = 0; // the next node to visit
		std::size_t End = 0;   // one past the last node of this run

		float TimeScale = 1.0F;
		float Opacity = 1.0F;

		// Whether the chain down to this level names no moving geometry. Decision 67's snap is
		// unconditional in what it applies to and conditional on this: a node under a parent that is
		// mid-flight is mid-flight, and snapping it would fight the animation a pixel at a time.
		bool Settled = true;

		// The `DrawGroup` this run composites into, or `NoItem`. Backpatched on the way out, because
		// a group's member count and its offscreen's placement are both facts about a subtree that
		// has not been walked yet when the item is emitted.
		std::uint32_t Group = NoItem;

		// The screen-space bound of everything emitted at or under this level, which is what the
		// group's own quad is. Empty until something lands in it.
		bool Bounded = false;
		Rect<DeviceSpace> Bounds{};
	};

	void Walk(const EvaluateRequest& request)
	{
		const std::span<const Node> nodes = request.Snapshot.Nodes<Node>();
		const std::span<const OutputAdapter> views = request.Snapshot.Views<OutputAdapter>();

		// Decision 84's rule, and the whole of the check that is available before the header carries a
		// set generation: a run that is not this output set's is no information rather than partial
		// information, and a scene with no placement has no output to be drawn on.
		if (nodes.empty() || views.size() != request.Outputs || request.Output >= views.size())
		{
			return;
		}

		const OutputView view{ views[request.Output], request.Resolution };

		const Runs runs{
			.Translations = request.Snapshot.Run<Spring<Vector3<double>>>(SnapshotRun::Translation),
			.Scales = request.Snapshot.Run<Spring<Vector3<float>>>(SnapshotRun::Scale),
			.Rotations = request.Snapshot.Run<Spring<RotationVector>>(SnapshotRun::Rotation),
			.Opacities = request.Snapshot.Run<Spring<float>>(SnapshotRun::Opacity),
			.Images = request.Snapshot.Images<ImageContent>(),
			.Solids = request.Snapshot.Solids<SolidContent>(),
		};

		m_Depth = 1;
		m_Stack[0] = Level{ .Chain = view.Root(), .Index = 0, .End = nodes.size() };

		// Where the current top-level subtree's items begin. Exhausting the arena rolls back to it, so
		// that what a frame drops is whole windows rather than the second half of one — see `Emit`.
		std::size_t mark = 0;

		while (m_Depth != 0)
		{
			Level& level = m_Stack[m_Depth - 1];

			if (level.Index >= level.End)
			{
				Close();
				continue;
			}

			if (m_Depth == 1)
			{
				mark = m_Count;
			}

			const std::size_t index = level.Index;
			const Node& node = nodes[index];

			// Decision 90's check, and the one that has to come first: a subtree claiming more nodes
			// than the run it sits in has left the walk with no way to find its own end. The level is
			// abandoned rather than the frame, so the siblings already emitted survive and the outer
			// levels close normally.
			if (node.SubtreeLength > level.End - index - 1)
			{
				level.Index = level.End;
				continue;
			}

			// Where this level resumes once the subtree below is done. Set before anything is pushed,
			// so that a level restored later cannot resume inside its own child.
			level.Index = node.Past(index);

			if (node.IsHidden())
			{
				// The operation decision 86's encoding exists to make an addition rather than a test
				// per node. Nine workspaces with one visible is what this saves on every frame.
				continue;
			}

			if (!Visit(node, index, nodes, runs, view, request))
			{
				m_Count = mark;
				m_Truncated = true;
				m_Depth = 0;

				return;
			}
		}
	}

	// The coefficient runs a walk resolves against, gathered once per call rather than per node.
	struct Runs
	{
		std::span<const Spring<Vector3<double>>> Translations;
		std::span<const Spring<Vector3<float>>> Scales;
		std::span<const Spring<RotationVector>> Rotations;
		std::span<const Spring<float>> Opacities;
		std::span<const ImageContent> Images;
		std::span<const SolidContent> Solids;
	};

	// One node: evaluate it, place it, emit it, and descend. False where the arena ran out, which is
	// the one condition that ends the walk rather than the node.
	[[nodiscard]] bool Visit(
		const Node& node,
		std::size_t index,
		std::span<const Node> nodes,
		const Runs& runs,
		const OutputView& view,
		const EvaluateRequest& request
	)
	{
		const Level& level = m_Stack[m_Depth - 1];

		const float timeScale = level.TimeScale * node.TimeScale;
		const float inherited = level.Opacity;

		NodeTransform transform = node.Transform;
		bool settled = level.Settled;

		// **A node carries whatever reconstitutes its value**, which is decision 90's rule and the
		// reason each channel is a branch rather than a table. Where a channel names a coefficient the
		// spring is the answer and the inline value is stale; where it names none the inline value is
		// the whole answer and the channel is at rest. That second reading is also this walk's test
		// for settledness: dispatch drops a coefficient when its spring comes to rest, so a node that
		// names none has nothing in flight — no thresholds crossed the waist and none are needed here.
		//
		// A node that names a coefficient the run does not hold falls back to its inline value and is
		// still counted as moving, which is the conservative direction: a malformed run must not put
		// the compositor to sleep with something on screen that was meant to be moving.
		if (node.IsTranslating())
		{
			settled = false;

			if (node.TranslationSpring < runs.Translations.size())
			{
				const Spring<Vector3<double>>& spring = runs.Translations[node.TranslationSpring];
				transform.Translation =
					spring.Evaluate(Sample(request.Presentation, spring.Origin, timeScale)).Position;
			}
		}

		if (node.IsScaling())
		{
			settled = false;

			if (node.ScaleSpring < runs.Scales.size())
			{
				const Spring<Vector3<float>>& spring = runs.Scales[node.ScaleSpring];
				transform.Scale = spring.Evaluate(Sample(request.Presentation, spring.Origin, timeScale)).Position;
			}
		}

		// The rotation channel is the one that composes rather than replaces: the spring is over the
		// geodesic deviation from the node's own orientation, which is the chart's base point and
		// crosses whether or not anything is turning. See Geometry/NodeTransform.h's `FromDeviation`.
		if (node.IsRotating())
		{
			settled = false;

			if (node.RotationSpring < runs.Rotations.size())
			{
				const Spring<RotationVector>& spring = runs.Rotations[node.RotationSpring];
				const RotationVector deviation =
					spring.Evaluate(Sample(request.Presentation, spring.Origin, timeScale)).Position;

				transform.Rotation = Quaternion::FromDeviation(deviation, node.Transform.Rotation);
			}
		}

		// Decision 72's driven regime moves geometry without this walk being able to say which channel
		// it moves — the binding arrives with the regime. What is knowable now is that something is in
		// flight, which is what the snap and the damage below actually ask.
		if (node.IsDriven())
		{
			settled = false;
		}

		float opacity = node.Opacity;

		if (node.IsFading())
		{
			m_Moving = true;

			if (node.OpacitySpring < runs.Opacities.size())
			{
				const Spring<float>& spring = runs.Opacities[node.OpacitySpring];
				opacity = spring.Evaluate(Sample(request.Presentation, spring.Origin, timeScale)).Position;
			}
		}

		m_Moving = m_Moving || !settled;

		const float radius = transform.BoundingRadius(node.Extent.Width, node.Extent.Height);
		ComposedTransform chain = level.Chain.Push(transform, radius);

		if (settled)
		{
			chain = Snapped(chain);
		}

		// A group's own opacity is the flattened result's, so it is spent once at the offscreen and
		// never again inside it — which is the entire difference decision 60 draws between a group
		// fade and per-node alpha. A subtree that is not a group has no offscreen to spend it at, so
		// the alpha multiplies down and the overlap shows through, which is what that decision says a
		// shell asking for a correct fade must declare a group to avoid.
		float own = inherited * opacity;
		float descend = own;
		Material dress = node.Dress;
		Elevation lift = node.Lift;
		std::uint32_t group = NoItem;

		// **A group takes the node's dressing along with its opacity, and for the same reason.** Both
		// belong to the flattened result: a glass window that declares a group blurs what is behind the
		// *group*, and leaving the material on the member as well would blur it twice — visibly, at the
		// one moment the window is also fading.
		if (node.IsGroup())
		{
			DrawItem item{};
			item.Content = DrawGroup{};
			item.Opacity = own;
			item.Dress = dress;
			item.Lift = lift;

			group = Emit(item);

			if (group == NoItem)
			{
				return false;
			}

			own = 1.0F;
			descend = 1.0F;
			dress = Material::None;
			lift = Elevation::None;
		}

		Drawn drawn{};

		// **What emits an item is content or *either* dressing**, which is World/Node.h's own rule and
		// not a reading of it: a container dressed `Glass` draws the blurred backdrop and names no
		// content while doing it, which is the case decision 95 makes a material a field for.
		//
		// **The elevation is the second dressing and counts on its own.** Decision 95 puts both in one
		// slot precisely because they are orthogonal — a glass panel casts a shadow too — so a node
		// lifted but undressed is a shadow with nothing over it, which is decision 99's overview
		// thumbnail: the tile's shadow, then the window on top of it from the expansion. Testing only
		// the material would drop that silently, which is the failure decision 99 refuses when it
		// declines to treat a dressed reference as malformed.
		//
		// **Both are the locals rather than the node's own predicates**, because a group above has
		// already taken them and zeroed them here. Reading `node.IsLifted()` would draw the shadow a
		// second time, inside the offscreen it was just lifted out of.
		if (node.HasContent() || dress != Material::None || lift != Elevation::None)
		{
			drawn = Draw(node, chain, view, own, dress, lift, runs);

			if (drawn.Full)
			{
				return false;
			}
		}

		const std::size_t expansion = Expansion(node, index, nodes);
		const bool expands = expansion != NoExpansion;
		const bool descends = expands || (node.SubtreeLength != 0 && !node.IsReference());

		// A node's own pixels belong to the group it declared rather than to the run outside it, so
		// where there is a level about to be pushed the bound is seeded into that level and reaches
		// the parent when it closes. Without a group the two are the same place.
		if (drawn.Bounded && group == NoItem)
		{
			Accumulate(drawn.Bounds);
		}

		if (!descends && group == NoItem)
		{
			return true;
		}

		if (m_Depth == m_Stack.size())
		{
			// Past the cap the subtree is dropped whole rather than partially, which is the same
			// answer the malformed-length check gives and for the same reason. A group already emitted
			// closes over what it got, which is its own item and nothing else.
			if (group != NoItem)
			{
				Backpatch(group, drawn.Bounded, drawn.Bounds);
			}

			return true;
		}

		const std::size_t first = expands ? expansion : index + 1;
		const std::size_t past = expands ? nodes[expansion].Past(expansion) : node.Past(index);

		m_Stack[m_Depth] = Level{ .Chain = chain,
			                      .Index = first,
			                      .End = past,
			                      .TimeScale = timeScale,
			                      .Opacity = descend,
			                      .Settled = settled,
			                      .Group = group,
			                      .Bounded = group != NoItem && drawn.Bounded,
			                      .Bounds = drawn.Bounds };
		++m_Depth;

		return true;
	}

	// Where this node's children are, as a first index, or `nodes.size()` for a node whose children
	// are simply the run that follows it.
	//
	// **A reference is expanded and its own subtree is not walked.** Decision 95 makes `Content` the
	// whole payload of the kind — a reference's content *is* a node — so children beside the expansion
	// are not a shape the vocabulary has, and walking them would invent one. The backwards rule is
	// what makes the expansion safe: the target's index is lower than this node's, so a cycle is
	// unrepresentable rather than something this walk has to detect, and decision 90 declines to rest
	// on a detector for exactly the traversal that would not terminate.
	[[nodiscard]] static std::size_t
	Expansion(const Node& node, std::size_t index, std::span<const Node> nodes) noexcept
	{
		if (!node.IsReference())
		{
			return NoExpansion;
		}

		const std::size_t target = node.Content;

		if (target >= index || target >= nodes.size() || nodes[target].Past(target) > nodes.size())
		{
			return NoExpansion;
		}

		return target;
	}

	// What emitting one node's item produced: whether the arena refused it, and the device-space bound
	// it landed on, which is what an enclosing group's offscreen is placed over.
	struct Drawn
	{
		bool Full = false;
		bool Bounded = false;
		Rect<DeviceSpace> Bounds{};
	};

	// Project one drawn node and append its item.
	[[nodiscard]] Drawn Draw(
		const Node& node,
		const ComposedTransform& chain,
		const OutputView& view,
		float opacity,
		Material dress,
		Elevation lift,
		const Runs& runs
	)
	{
		const std::optional<Quad> quad = view.Project(chain, node.Extent);

		// Absence is the whole answer: a node behind the viewer, a back face, or a quad that misses
		// the target is gone, and its children are not — they carry their own extents and are asked
		// the same question separately.
		if (!quad)
		{
			return {};
		}

		DrawItem item{};

		item.Shape = *quad;
		item.Extent = node.Extent;
		item.Opacity = opacity;
		item.Dress = dress;
		item.Lift = lift;

		// A node with no content run to index is one that got here on its dressing alone, and
		// `DrawDressing` is what it draws: a quad, an extent, and a material, with nothing of the
		// node's own underneath it.
		if (node.Kind == NodeKind::Image)
		{
			if (node.Content >= runs.Images.size())
			{
				return {};
			}

			const ImageContent& content = runs.Images[node.Content];

			item.Content = DrawTexture{ .Texture = content.Texture, .Source = content.Source };
			item.Color = content.Color;
		}
		else if (node.Kind == NodeKind::Solid)
		{
			if (node.Content >= runs.Solids.size())
			{
				return {};
			}

			const SolidContent& content = runs.Solids[node.Content];

			item.Content = DrawSolid{ content.Red, content.Green, content.Blue, content.Alpha };
			item.Color = content.Color;
		}

		if (Emit(item) == NoItem)
		{
			return { .Full = true };
		}

		return { .Bounded = true, .Bounds = quad->Bounds() };
	}

	// Finish the level on top of the stack: close its group over what it received, and hand its bound
	// up, since a nested group's offscreen is content of the one outside it.
	void Close() noexcept
	{
		const Level done = m_Stack[m_Depth - 1];
		--m_Depth;

		if (done.Group != NoItem)
		{
			Backpatch(done.Group, done.Bounded, done.Bounds);
		}

		if (done.Bounded && m_Depth != 0)
		{
			Accumulate(done.Bounds);
		}
	}

	// A group's two facts, filled in once its members are known: how many items follow it, and where
	// the offscreen they compose into sits. The placement is axis-aligned because the members'
	// own projections are already baked into their device-space corners.
	void Backpatch(std::uint32_t item, bool bounded, Rect<DeviceSpace> bounds) noexcept
	{
		m_Items[item].Content = DrawGroup{ static_cast<std::uint32_t>(m_Count - item - 1) };
		m_Items[item].Shape = bounded ? Quad::FromRect(bounds) : Quad{};

		// The offscreen's own extent, so that the group's four corners parameterize it the way every
		// other item's do. It is device-sized because a flattened subtree has no surface behind it —
		// the members' projections are already baked into the bound this is the size of.
		m_Items[item].Extent = { bounded ? bounds.Extent.Width : 0.0F, bounded ? bounds.Extent.Height : 0.0F };
	}

	void Accumulate(Rect<DeviceSpace> bounds) noexcept
	{
		Level& level = m_Stack[m_Depth - 1];

		level.Bounds = level.Bounded ? Enclosing(level.Bounds, bounds) : bounds;
		level.Bounded = true;
	}

	// The smallest rectangle containing both, at the width the projection produced. Geometry/Region.h
	// has the same fold over whole pixels and this is not it: a group's offscreen is placed from real
	// bounds, and rounding them outward here would grow the target by a pixel per nesting level.
	[[nodiscard]] static Rect<DeviceSpace> Enclosing(Rect<DeviceSpace> left, Rect<DeviceSpace> right) noexcept
	{
		return Rect<DeviceSpace>::FromEdges(
			{ std::min(left.Left(), right.Left()), std::min(left.Top(), right.Top()) },
			{ std::max(left.Right(), right.Right()), std::max(left.Bottom(), right.Bottom()) }
		);
	}

	// Append one item, or `NoItem` where the arena is full.
	//
	// **Exhaustion drops the top-level subtree the walk is inside rather than stopping where it
	// stands**, which is what `Walk`'s rollback does with this answer. Truncating mid-subtree would
	// hand a renderer a group whose offscreen is missing half its members — a window drawn with its
	// menu gone and its fade applied to what is left — where dropping the subtree loses a whole window
	// and leaves everything else exactly right. Neither is good; only one of them is legible.
	[[nodiscard]] std::uint32_t Emit(const DrawItem& item) noexcept
	{
		if (m_Count == m_Items.size())
		{
			return NoItem;
		}

		const std::uint32_t at = static_cast<std::uint32_t>(m_Count);

		m_Items[m_Count] = item;
		++m_Count;

		return at;
	}

	// Decision 67's snap, applied to the node's own origin in the output's device grid.
	//
	// **It is the position and not the extent**, because that is what the promise rests on: two tiled
	// windows abut exactly when both their edges land on the grid, and decision 52 already governs the
	// extent from the other side. A node whose chain is rotated or perspective-projected gets a snap
	// that is meaningless and harmless, which is decision 67's own reading of the case.
	//
	// The correction goes into the translation column scaled by the weight at the origin, so it is
	// exact for the orthographic chain every settled node in practice has, and is still the right
	// correction under a projection.
	[[nodiscard]] static ComposedTransform Snapped(ComposedTransform chain) noexcept
	{
		const double weight = chain.M[3][3];

		// The origin has passed through some ancestor's eye, so there is no device position to round
		// to. The node is about to be culled by `Project` for the same reason.
		if (!(weight >= static_cast<double>(Projected::MinimumWeight)))
		{
			return chain;
		}

		chain.M[0][3] = std::round(chain.M[0][3] / weight) * weight;
		chain.M[1][3] = std::round(chain.M[1][3] / weight) * weight;

		return chain;
	}

	// Decision 19's per-subtree time scale, applied where it is the only place it can be applied
	// exactly: to the elapsed time of the spring being evaluated, measured from that spring's own
	// origin. A scale of one is the identity and is the common case, so it costs a comparison.
	//
	// The multiply goes through `DurationFromSeconds` because a time scale is authored — it is the
	// untrusted operand that function exists for, and a scale that is not a number must produce a
	// saturated instant rather than an undefined one.
	[[nodiscard]] static Instant Sample(Instant presentation, Instant origin, float scale) noexcept
	{
		if (scale == 1.0F)
		{
			return presentation;
		}

		return Advanced(
			origin, DurationFromSeconds(ToSeconds(Elapsed(origin, presentation)) * static_cast<double>(scale))
		);
	}

	// Whether this output owes a repaint.
	//
	// **The whole output, or nothing.** A frame is damaged where anything in it moved, or where the
	// world it was drawn from is a newer publication than the one this output last drew — and both
	// conditions are answered for the output rather than per node. That is conservative in the only
	// direction that is safe: too much damage costs bandwidth on a frame that was being composited
	// anyway, and too little leaves a trail of the window that moved.
	//
	// **What it takes to do better is node identity across frames, and the record does not have it.**
	// Per-node damage means comparing where a node was against where it is, which needs the two
	// frames' nodes to be the same node — a published `Handle` rather than a position in a run that
	// dispatch is free to reorder. Client surface damage is the other half and has no carrier either
	// until there is a protocol layer to mint it. Both are worth having and neither is inventable from
	// what crosses the waist today, so this reports the honest bound instead of a plausible one.
	[[nodiscard]] bool Changed(const EvaluateRequest& request) const noexcept
	{
		if (request.Output >= m_Seen.size())
		{
			return true;
		}

		const Memory& seen = m_Seen[request.Output];

		return m_Moving || !seen.Drawn || seen.Sequence != request.Snapshot.Sequence() ||
		       seen.Resolution != request.Resolution;
	}

	void Remember(const EvaluateRequest& request) noexcept
	{
		if (request.Output < m_Seen.size())
		{
			m_Seen[request.Output] = { request.Snapshot.Sequence(), request.Resolution, true };
		}
	}

	// What one output was last drawn from, which is all the history the damage rule above needs.
	struct Memory
	{
		std::uint64_t Sequence = 0;
		PixelSize<DeviceSpace> Resolution{};
		bool Drawn = false;
	};

	const IClock* m_Clock = nullptr;

	// The arena. A vector rather than an array because it is half a megabyte and the composition root
	// constructs the evaluator once, outside every frame section; the capacity never changes after
	// that, so nothing here allocates where decision 36 says nothing may.
	std::vector<DrawItem> m_Items;
	std::size_t m_Count = 0;

	std::array<Level, MaxWalkDepth> m_Stack{};
	std::size_t m_Depth = 0;

	std::array<Memory, MaxOutputs> m_Seen{};

	bool m_Moving = false;
	bool m_Truncated = false;
};
