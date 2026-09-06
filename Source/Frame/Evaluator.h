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
#include "Core/Session.h"
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
#include "World/Exit.h"
#include "World/Node.h"
#include "World/Root.h"
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
// **What the walk does not do yet, named so that the absence is not read as an oversight.** A node's
// corner radius stays zero: decision 96 rounds the *window geometry rect* rather than the node
// extent and zeroes the radius for a window that is fullscreen or tiled edge to edge, which is layout
// state that has no carrier in the published record — so the frame rect `ImageContent` already holds
// has nothing to bound yet.

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

// SPEC: how many closing windows one output may report a snapshot for in one frame.
//
// It is `Publication/Return.h`'s `ReleasesPerReport` over the same population — the windows that can
// start leaving one screen inside a single frame — because a window's last frame is exactly what a
// hold on its buffer was keeping alive. Sixteen is well past what a person closes at once and past
// what an application's teardown lands in any one frame.
//
// **Past it, the seventeenth window waits for one of the sixteen to finish, not for the next frame.**
// The walk takes them in tree order and cannot skip the ones already drawn, because it does not know
// which those are — `Frame/Capture.h` holds that and holds it downstream of here. So a screen with
// more than sixteen windows leaving at once reports the same sixteen every frame until one of their
// exits ends and its entry leaves the run. What the window waiting draws in the meantime is itself:
// it still has its own pixels until its client takes them away, and decision 20's copy exists for the
// moment after that. Raising the figure is the fix if a shell is ever built that closes more than this
// at once, and lowering the cost is not — the walk touches this run once per closing window per frame.
inline constexpr std::size_t MaxExitCaptures = 16;

// Where one closing window is on this screen and where its picture goes.
//
// **The walk is what knows this, and it is the only thing that does.** Decision 20 draws a window
// that is leaving from a copy of its last frame and decision 46 keeps that copy in a rectangle
// reserved when the retirement was observed; what neither side could say until now is *which pixels*
// — a toplevel is a container (111), so a window is a run of items rather than one image, and the
// run's extent is a fact about a preorder walk in flight. `World/Exit.h` carries the rectangle in and
// this carries the run out, so the two meet on the thread that draws.
//
// The run indexes `DrawList::Items` and is the closing window's own subtree, in the order the
// composite would have drawn it. `Seam/Renderer.h`'s `SnapshotCapture` is this record minus the
// reservation, which is the frame thread's own bookkeeping rather than anything a renderer is told.
struct ExitCapture
{
	// The atlas image, and the rectangle within it, exactly as reserved.
	TextureId Into;
	PixelRect<BufferSpace> Slot{};

	// Where that rectangle's contents are on the screen right now: the closing node's own quad, which
	// is the rectangle `Scene/Reach.h` measured when the slot was taken. The two agreeing is what
	// makes the copy a translation rather than a resample — see `Seam/Renderer.h`.
	Rect<DeviceSpace> Source{};

	// Which reservation this is, from `World/Exit.h`. What `Frame/Capture.h` remembers, so that a
	// window's picture is taken once rather than once a frame for the length of its exit.
	std::uint32_t Reservation = 0;

	// The window's items: where its subtree starts in the list and how many items long it is. Never
	// zero — a window that emitted nothing has no picture to keep and is not reported at all.
	std::uint32_t First = 0;
	std::uint32_t Count = 0;

	friend constexpr bool operator==(ExitCapture, ExitCapture) noexcept = default;
};

// What one output's frame is, once the snapshot has been evaluated at its predicted presentation.
//
// Damage is the new damage this evaluation produced, in device space; the loop unions it into what the
// output has been accumulating rather than replacing it, for the reason Frame/Loop.h's header gives.
struct DrawList
{
	std::span<const DrawItem> Items;
	Region<DeviceSpace> Damage;

	// The closing windows on this output whose last frame is worth keeping, each naming a run of
	// `Items`. Empty on every frame nothing is leaving, which is nearly all of them.
	std::span<const ExitCapture> Captures;

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
		m_CaptureCount = 0;
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
			     .Captures = std::span<const ExitCapture>{ m_Captures.data(), m_CaptureCount },
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

		// Whether some node at or above this level carried `Node::Snap`, and therefore whether the
		// grid has already been taken for this subtree.
		//
		// **A snapped subtree is placed on the grid once, as a body, and never again inside itself.**
		// That is the whole content of the flag: a glyph is a few dozen rectangles whose sub-pixel
		// relationships are the drawing, so rounding each of them on its own hints them apart. Without
		// this the cursor is body-snapped while it moves and rect-snapped the instant it stops, since
		// decision 67's condition comes true for every descendant at once — which is a pointer whose
		// shape ticks as it comes to rest, in the one frame a person's eye has settled on it.
		bool Snapped = false;

		// The `DrawGroup` this run composites into, or `NoItem`. Backpatched on the way out, because
		// a group's member count and its offscreen's placement are both facts about a subtree that
		// has not been walked yet when the item is emitted.
		std::uint32_t Group = NoItem;

		// The screen-space bound of everything emitted at or under this level, which is what the
		// group's own quad is. Empty until something lands in it.
		bool Bounded = false;
		Rect<DeviceSpace> Bounds{};

		// The snapshot the node that pushed this level owes, carried down and filed on the way out.
		// Its `First` is already set and its `Count` is not, for `Group`'s reason one field over: a
		// window's run ends where its subtree does, and that is not known when the window is met.
		// `Reservation` of zero is a level nothing is closing, which is every level nearly always.
		ExitCapture Exit{};
	};

	void Walk(const EvaluateRequest& request)
	{
		const std::span<const Node> nodes = request.Snapshot.Nodes<Node>();
		const std::span<const OutputAdapter> views = request.Snapshot.Views<OutputAdapter>();
		const std::span<const SceneRoot> roots = request.Snapshot.Roots<SceneRoot>();
		const std::span<const SceneAssignment> sessions = request.Snapshot.Sessions<SceneAssignment>();

		// Decision 84's rule, and the whole of the check that is available before the header carries a
		// set generation: a run that is not this output set's is no information rather than partial
		// information, and a scene with no placement has no output to be drawn on.
		if (nodes.empty() || views.size() != request.Outputs || request.Output >= views.size())
		{
			return;
		}

		const OutputView view{ views[request.Output], request.Resolution };

		// Which session this output is showing, and therefore which roots it draws. Decision 84's length
		// test as everywhere else, resolving to `None` rather than returning: an output with no
		// assignment published for it is showing gyro's own scene, which is a scene rather than an
		// error, and it is what every output is showing between boot and the first agent's offer.
		const SceneAssignment assignment =
			sessions.size() == request.Outputs ? sessions[request.Output] : SceneAssignment{};
		const SessionId shown = assignment.Shown;

		// Where the root run has been read up to. A preorder walk meets its roots in increasing node
		// order and the run is written in that order, so one forward pass over it serves the whole walk.
		std::size_t root = 0;

		const Runs runs{
			.Translations = request.Snapshot.Run<Spring<Vector3<double>>>(SnapshotRun::Translation),
			.Scales = request.Snapshot.Run<Spring<Vector3<float>>>(SnapshotRun::Scale),
			.Rotations = request.Snapshot.Run<Spring<RotationVector>>(SnapshotRun::Rotation),
			.Opacities = request.Snapshot.Run<Spring<float>>(SnapshotRun::Opacity),
			.Images = request.Snapshot.Images<ImageContent>(),
			.Solids = request.Snapshot.Solids<SolidContent>(),
			.Exits = request.Snapshot.Exits<ExitSnapshot>(),
		};

		// Decision 188's cross-fade, resolved once for the whole walk: an output moving from one session
		// to another composites both, live, and the one it is leaving enters at the coefficient's value
		// instead of at one. Live on both sides rather than a photograph of the outgoing half, because
		// there is no photograph of a frame nobody has drawn — so a film playing when a laptop is locked
		// goes on playing as it leaves the screen.
		//
		// **A faded session with no coefficient behind it is not drawn**, which is decision 90's
		// rule that the frame thread validates what it walks rather than trusting it: the pair is
		// retired together on the far side, so a run that says otherwise has drifted, and the safe way
		// to be wrong is the steady state — the screen a person is arriving at rather than the one they
		// are leaving.
		SessionId fading = SessionId::None;
		float fade = 0.0F;

		if (assignment.Fading != SessionId::None && assignment.Fade < runs.Opacities.size())
		{
			const Spring<float>& spring = runs.Opacities[assignment.Fade];

			// **Clamped, and it is decision 43's anti-spoofing property being kept rather than a
			// defensive habit.** That argument survives a transition only if the coefficient stays
			// inside the two ends it was authored between, and the other half of it is the catalog: the
			// motion this is authored under does not overshoot, and one that did would be visible as a
			// locked screen briefly showing the desktop again.
			fade = std::clamp(spring.Evaluate(Sample(request.Presentation, spring.Origin, 1.0F)).Position, 0.0F, 1.0F);
			fading = assignment.Fading;

			// A transition in flight is the frame loop's reason to come back, exactly as a moving node
			// is. Nothing else would say so: no node names this spring.
			m_Moving = true;
		}

		m_Depth = 1;
		m_Stack[0] = Level{ .Chain = view.Root(), .Index = 0, .End = nodes.size() };

		// Where the current top-level subtree's items begin. Exhausting the arena rolls back to it, so
		// that what a frame drops is whole windows rather than the second half of one — see `Emit`.
		std::size_t mark = 0;

		// And what it had already promised to photograph. A run rolled back names items that are no
		// longer there, so the snapshots taken inside it go with it — a window dropped from this frame
		// is one whose picture is taken on a frame that draws it, rather than one whose rectangle is
		// filled from whatever ended up at those indices.
		std::size_t promised = 0;

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
				promised = m_CaptureCount;
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

			// Decision 21's partition, and it is asked at depth one only because that is the only depth
			// it can be asked at: a root's session is the session of everything under it, or the
			// partition would be a second tree cutting across this one. The skip is the same addition
			// `Hidden` above is, so a session nobody is showing costs one comparison per root and not
			// one per window in it — which is what makes *many sessions connected, one presented
			// locally* a property of the frame rather than an intention.
			//
			// **`None` is shown on every output rather than on none.** That is the pointer glyph, which
			// Scene/Cursor.h makes the last root so that it draws over whatever else is on screen, and
			// it is the splash and the console for the same reason. An output showing `None` therefore
			// draws exactly gyro's own roots, which is the two halves agreeing without a third state.
			if (m_Depth == 1)
			{
				const SessionId owner = Owner(roots, root, index);

				// The strength this root's whole subtree enters at: full for gyro's own and for the
				// session being shown, the coefficient's value for the one carrying the transition, and
				// nothing at all for anybody else's. Two comparisons and a multiply, which is the whole
				// cost of decision 188 in the walk.
				//
				// **gyro's own is tested first and is never faded**, which is what keeps the pointer on
				// the screen: `None` is the background at the back of the paint order and the glyph at
				// the front, so it is not a layer and a transition may not take it as one.
				//
				// **The faded session is tested before the shown one, and that ordering is the whole of
				// the fade-in.** The two are equal while a session is arriving over what is already
				// there — unlocking a screen — and testing `shown` first would draw it at full strength
				// on the first frame, which is the cut this ordering exists to stop being.
				if (owner == SessionId::None)
				{
					m_Stack[0].Opacity = 1.0F;
				}
				else if (owner == fading)
				{
					m_Stack[0].Opacity = fade;
				}
				else if (owner == shown)
				{
					m_Stack[0].Opacity = 1.0F;
				}
				else
				{
					continue;
				}
			}

			if (!Visit(node, index, nodes, runs, view, request))
			{
				m_Count = mark;
				m_CaptureCount = promised;
				m_Truncated = true;
				m_Depth = 0;

				return;
			}
		}
	}

	// Which session a root belongs to, advancing the cursor as the walk advances.
	//
	// **The run names the node index and this compares it rather than trusting the ordinal**, which is
	// decision 90's rule that the frame thread validates what it walks applied one run over. A root run
	// that had drifted from the node run by one entry would otherwise draw one session's windows on
	// another session's screen, silently and for as long as both were connected — so a root the run
	// does not name is `None` and is shown, which fails towards a scene a person can see and act on.
	[[nodiscard]] static SessionId
	Owner(std::span<const SceneRoot> roots, std::size_t& cursor, std::size_t index) noexcept
	{
		while (cursor < roots.size() && roots[cursor].Node < index)
		{
			++cursor;
		}

		return cursor < roots.size() && roots[cursor].Node == index ? roots[cursor].Session : SessionId::None;
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
		std::span<const ExitSnapshot> Exits;
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

		// **A node that asks for the grid gets it whether or not it has settled**, which is the whole
		// of `Node::Snap`. The pointer is the node it exists for and is never settled while somebody
		// is using it, so decision 67's condition alone would leave its outline pumping for exactly
		// as long as anybody is looking at it — and the second clause is the other half, because a
		// subtree already placed on the grid must not be placed on it again one rectangle at a time.
		const bool snapping = level.Snapped || node.IsSnapped();

		if ((settled || node.IsSnapped()) && !level.Snapped)
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

		// **The one place a level becomes numbers.** Decision 104 has the draw list carry the derived
		// shadow rather than the elevation, so the resolution happens here, on the walk that is already
		// reading the node — and a renderer is never told which level it is drawing.
		Shadow lift = Cast(node.Lift);
		std::uint32_t group = NoItem;

		// **Where this window's picture would start, taken before anything of it is emitted.** A
		// closing window is kept as the run of items it is about to draw — its own quad, its dressing,
		// its shadow and everything hanging under it — so the run opens here and closes when the walk
		// leaves the subtree, whether that is at the group below, at the level pushed at the end, or
		// immediately for a window with nothing under it.
		const ExitCapture pending = Reserved(node, index, chain, view, runs.Exits, request.Output, m_Count);

		// **And the picture stops at the window, so its own shadow is not drawn into it.** The slot is
		// the window's rectangle and nothing more, so a shadow emitted here would be scissored off at
		// the window's edges — but squaring it off is not what is wanted either. A shadow is not the
		// window's pixels: it is this height applied to this quad, and the frame that draws the
		// snapshot back has the node and can cast it again around the same rectangle, which is what
		// decision 105 asks for when it says the shadow animates with its window rather than being
		// carried along by it.
		//
		// **Above the group for the group's own reason**, one field over: a group takes the level with
		// it, so leaving this until after would bake the shadow into the flattened result instead.
		// Only the closing node's own is dropped — a lift below it casts inside the picture, where it
		// belongs and where nothing clips it.
		if (pending.Reservation != 0)
		{
			lift = {};
		}

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
			lift = {};
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
		if (node.HasContent() || dress != Material::None || lift.Draws())
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
			File(pending);

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

			File(pending);

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
			                      .Snapped = snapping,
			                      .Group = group,
			                      .Bounded = group != NoItem && drawn.Bounded,
			                      .Bounds = drawn.Bounds,
			                      .Exit = pending };
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
		Shadow lift,
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

			// **The one kind that has texels, and therefore the only one this is asked of.** A solid, a
			// group's offscreen and a bare dressing have no image to be sharp or soft, and decision 152
			// refuses all three on their content before it ever reads this — so an unreduced class is
			// the honest answer for them rather than an omission.
			item.Sampling = Classify(chain, node.Extent, content.Source);
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

		File(done.Exit);

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

	// The snapshot a node is closing under on this output, opened at the item it is about to emit.
	//
	// **Every step of the scan is checked rather than trusted**, which is decision 90's rule that the
	// frame thread validates what it walks: the run is a start position and a contiguity contract, so
	// an entry naming another node ends it and an index past the run's end is simply no snapshot.
	// Getting that wrong would copy one window's rectangle out of another window's pixels, which is a
	// wrong picture rather than a missing one.
	//
	// **The source is the node's own quad and not its subtree's bound**, because that is the rectangle
	// the reservation was measured from — `Scene/Reach.h` takes the four projected corners of the
	// closing node, and `Seam/Renderer.h` moves the run by the difference between the two origins. The
	// two agreeing is what makes the copy a translation, so measuring anything else here would slide
	// the picture inside its own rectangle by however far the subtree overhangs.
	//
	// **Which is also why the closing node's own shadow is not in the picture**, and the caller drops
	// it rather than this rectangle growing to hold it. A shadow reaches past the quad it is cast
	// from, so a slot the size of the quad squares it off at the window's own edges — but the fix is
	// not a bigger slot. The shadow is not the window's pixels; it is arithmetic on the window's
	// rectangle and its height, and the frame that draws the snapshot back has both. Casting it there
	// is what keeps decision 105's promise that the shadow animates with its window: a shadow baked
	// into the picture is scaled along with it, so a window shrinking as it leaves would carry a
	// shrinking shadow, when what a person expects is one cast fresh from where the window now is.
	//
	// The rectangle stopping at the quad is what leaves `Scene/Atlas.h` reserving from
	// `Scene/Reach.h`'s bound with nothing added, and the elevation a level rather than a distance on
	// the side that could not spell the distance anyway — `Scene` may not name `Seam`, where the one
	// light's numbers live.
	[[nodiscard]] static ExitCapture Reserved(
		const Node& node,
		std::size_t index,
		const ComposedTransform& chain,
		const OutputView& view,
		std::span<const ExitSnapshot> exits,
		std::size_t output,
		std::size_t first
	) noexcept
	{
		if (!node.IsExiting())
		{
			return {};
		}

		for (std::size_t at = node.Exit; at < exits.size() && exits[at].Node == index; ++at)
		{
			const ExitSnapshot& snapshot = exits[at];

			// A null texture is an output whose atlas the device had no room for and an empty slot is
			// a packer that had none, which decision 46 answers the same way: the window cuts instead
			// of fading, and the frames it would have faded over cost nothing.
			if (snapshot.Output != output || snapshot.Reservation == 0 || snapshot.Texture.IsNull() ||
			    snapshot.Slot.IsEmpty())
			{
				continue;
			}

			const std::optional<Quad> quad = view.Project(chain, node.Extent);

			if (!quad)
			{
				return {};
			}

			return { .Into = snapshot.Texture,
				     .Slot = snapshot.Slot,
				     .Source = quad->Bounds(),
				     .Reservation = snapshot.Reservation,
				     .First = static_cast<std::uint32_t>(first),
				     .Count = 0 };
		}

		return {};
	}

	// File one closing window's run, now that the walk knows where it ends.
	//
	// **A window that emitted nothing is not reported**, and the difference matters on screen: a
	// reported run of no items would clear its rectangle to nothing, mark the reservation filled, and
	// leave the window fading from an empty rectangle for the length of its exit. Not reporting it
	// means the window cuts, which is the picture decision 46 already accepts, and means the snapshot
	// is taken on the first frame the window does draw something.
	void File(const ExitCapture& pending) noexcept
	{
		if (pending.Reservation == 0 || m_Count <= pending.First || m_CaptureCount == m_Captures.size())
		{
			return;
		}

		m_Captures[m_CaptureCount] = pending;
		m_Captures[m_CaptureCount].Count = static_cast<std::uint32_t>(m_Count - pending.First);
		++m_CaptureCount;
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

	// Decision 67's snap, applied to the node's own origin in the output's device grid — and
	// `Node::Snap`'s, which is the same arithmetic asked for by a node that never settles.
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

	// The closing windows this frame found. An array rather than a vector because it is half a
	// kilobyte and decision 36 forbids the allocation either way.
	std::array<ExitCapture, MaxExitCaptures> m_Captures{};
	std::size_t m_CaptureCount = 0;

	std::array<Level, MaxWalkDepth> m_Stack{};
	std::size_t m_Depth = 0;

	std::array<Memory, MaxOutputs> m_Seen{};

	bool m_Moving = false;
	bool m_Truncated = false;
};
