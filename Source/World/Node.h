#pragma once

#include <cstdint>
#include <type_traits>

#include "Geometry/NodeTransform.h"
#include "Geometry/Space.h"

// One node of the published scene, as bytes in the snapshot's node run.
//
// **This module exists because two modules that may not see each other both have to name this
// record.** Docs/Decisions.md decision 86 puts the scene on the wire as a preorder run of node
// records, and decision 90 has the frame thread *validate the tree as it walks it* — so `Frame`
// reads these fields, `Scene` writes them, and `Frame` may not depend on `Scene`
// (Docs/Structure.md#the-modules calls that the one edge worth enforcing rather than describing).
// Decision 87's rule settles where such a type goes: below both waists, never in `Seam`. `Core` is
// not available for it, because the record has to name a transform and `Core` may not say
// `Geometry`. See decision 91.
//
// **`Publication` does not name this record and must not start.** The waist carries the node run as
// an offset, a count, and the writer's element size and alignment — the same arrangement it uses for
// spring coefficients, and for the same reason Publication/Snapshot.h gives. `PutNodes` and
// `Nodes<T>()` are templates, so `World` sits beside the waist rather than under it and adding a
// field here does not touch the boundary.
//
// **What is deliberately absent is content.** A node kind, a material, a texture, a color state, and
// the encoding of decision 88's subtree reference are all the shell's scene vocabulary, which
// Docs/Open.md holds open — decision 51's sketch list has already been outgrown once, by decision 88
// adding a kind it does not contain. What is here is what decisions 86, 90, 19, and 55 pin between
// them: the structure the walk traverses and the transform it composes. The rest arrives when the
// vocabulary does, alongside `Material`, and `SnapshotVersion` is what absorbs it.

// A channel that is not moving, in the slots below. Every run is shorter than this by many orders of
// magnitude, so the sentinel costs no representable index — and it is the *default*, which is the
// direction that matters: a record somebody filled in partially names no coefficients rather than
// naming element zero of every run.
inline constexpr std::uint32_t NoCoefficient = 0xFFFF'FFFFu;

// A node, in the preorder run.
//
// An aggregate deliberately, for the reason Core/Handle.h and Animation/Solve/Spring.h give: this is
// a shape the frame side reconstitutes from bytes at an offset rather than through a constructor.
struct Node
{
	// Scoped by the struct rather than by an `enum class`, so the flags combine without a cast and
	// still cannot be spelled without saying `Node::`. A mask rather than bools because the scene
	// vocabulary will add to it, and because two adjacent bools in a published record are two bytes
	// of padding somebody has to remember to zero.
	enum Flag : std::uint32_t
	{
		None = 0,

		// The subtree contributes nothing and the walk skips it — the operation the encoding below
		// exists to make an addition rather than a test per node. Nine workspaces with one visible
		// is what this costs on every frame.
		Hidden = 1u << 0,

		// The subtree composites into an offscreen and is drawn as one image, which is decision 60's
		// group fade. Dispatch declares it and never places it: the offscreen sits at the subtree's
		// *screen-space* bound, which is evaluated and per output, so decision 86 leaves the
		// placement to the frame side and puts only the declaration here.
		Group = 1u << 1,
	};

	// The model transform, in the parent's space.
	//
	// **`Rotation` is meaningful whether or not the node is turning, and the other channels are not.**
	// Docs/Animation.md#transforms anchors the log map at the target, so a rotation spring is over a
	// deviation whose target is zero and a settled one reads (0, 0, 0) — the orientation is the
	// chart's base point and has to cross unconditionally. Translation and scale are the opposite: a
	// spring carries its own `Target`, so while one of those channels is active the value here is
	// redundant and while it is at rest it is the whole answer. Decision 90 states the rule as
	// *a node carries whatever reconstitutes its value*, of which inline-when-settled is the common
	// case rather than the rule itself.
	//
	// `Anchor` and `Projection` are not animated channels and cross unconditionally for the plainer
	// reason that nothing else carries them.
	NodeTransform Transform{};

	// The node's own quad, spanning [0, Width] x [0, Height] at z = 0 in its own space — the
	// convention Geometry/NodeTransform.h already states. It is here rather than with the content
	// because `BoundingRadius` needs it and `Apply` needs that, so the extent is what the *transform*
	// requires and not what the node draws. Typed in surface space because that is the space a node's
	// own coordinates are in, whether or not a client supplied them.
	Size<SurfaceSpace, float> Extent{};

	// The model opacity, on the same terms as translation above: the whole answer at rest, redundant
	// while `OpacitySpring` names a coefficient. Opaque by default, so a record nobody finished is
	// visible rather than invisible — a node that should not be drawn is a bug somebody can see.
	float Opacity = 1.0F;

	// Decision 19's per-subtree time scale, composed down the tree by the walk. One is the identity
	// and every other value multiplies with the parent's.
	float TimeScale = 1.0F;

	// The nodes following this one that are inside its subtree, **not counting this one** — so a
	// skip is `index += 1 + SubtreeLength` and a leaf is zero.
	//
	// The convention is Seam/Renderer.h's `DrawGroup::Count` exactly, which decision 86 asks for by
	// name: the published tree and the emitted draw list are then read in one idiom rather than two.
	// It is a run length and never a child count, and the two differ the moment anything nests.
	//
	// **The frame thread checks it rather than trusting it.** Decision 88 puts the guarantee
	// dispatch-side and decision 90 declines to let that be the only thing standing between a bug and
	// the machine: a length that overruns what is left of the run is an unbounded walk inside the
	// frame section on a `SCHED_FIFO` thread, whose survivable outcome is `RLIMIT_RTTIME` taking
	// every session's UI at once. One comparison per node, on a walk that already does one.
	std::uint32_t SubtreeLength = 0;

	std::uint32_t Flags = None;

	// Where this node's coefficients are, one slot per channel, or `NoCoefficient`.
	//
	// **Named slots rather than an array indexed by `SnapshotRun`**, and the layering is why. That
	// enum is the waist's schema — Publication/Snapshot.h argues that fixing which runs exist and in
	// what order belongs there — and `World` may not reach it. An array here would have to carry a
	// second spelling of that ordering, which is the objection decision 87 made to giving one fact two
	// spellings. Named, the correspondence is nominal and the compiler checks it where the run is
	// asked for: `Run<Spring<double>>(SnapshotRun::Translation)` indexed by `TranslationSpring`. There
	// is no generic loop over channels to lose, because reconstitution differs per channel anyway —
	// rotation goes back through `FromDeviation` and nothing else does.
	//
	// The index is a position within its *own* channel's array, which is what decision 90's one run
	// per channel buys: pointing a scale index at a rotation spring stops being expressible rather
	// than being caught.
	std::uint32_t TranslationSpring = NoCoefficient;
	std::uint32_t ScaleSpring = NoCoefficient;
	std::uint32_t RotationSpring = NoCoefficient;
	std::uint32_t OpacitySpring = NoCoefficient;

	// Decision 72's driven ramp, which is a distinct closed form rather than a spring — a gesture
	// driving a channel directly. Which channel it drives arrives with the regime; the slot is here
	// for the reason the run is reserved at the waist.
	std::uint32_t DrivenRamp = NoCoefficient;

	// Padding that is spelled, so the tail is the publisher's to value-initialise rather than
	// whatever the arena last held — the obligation Core/Wake.h and Animation/Solve/Spring.h record
	// for their own, and the reason a publisher builds this from a value-initialised object.
	std::uint32_t Reserved = 0;

	[[nodiscard]] constexpr bool IsHidden() const noexcept { return (Flags & Hidden) != 0; }

	[[nodiscard]] constexpr bool IsGroup() const noexcept { return (Flags & Group) != 0; }

	[[nodiscard]] constexpr bool IsTranslating() const noexcept { return TranslationSpring != NoCoefficient; }

	[[nodiscard]] constexpr bool IsScaling() const noexcept { return ScaleSpring != NoCoefficient; }

	[[nodiscard]] constexpr bool IsRotating() const noexcept { return RotationSpring != NoCoefficient; }

	[[nodiscard]] constexpr bool IsFading() const noexcept { return OpacitySpring != NoCoefficient; }

	[[nodiscard]] constexpr bool IsDriven() const noexcept { return DrivenRamp != NoCoefficient; }

	// The index the walk moves to when this node's subtree is skipped whole, given this node's own.
	// Named rather than open-coded because `1 +` is exactly the mistake the run-length-versus-child-count
	// distinction above invites, and it is a mistake that draws a plausible wrong picture.
	[[nodiscard]] constexpr std::size_t Past(std::size_t index) const noexcept { return index + 1 + SubtreeLength; }

	friend constexpr bool operator==(Node, Node) noexcept = default;
};

static_assert(std::is_trivially_copyable_v<Node> && std::is_standard_layout_v<Node>);
static_assert(
	sizeof(Node) == 120,
	"A transform, an extent, two model scalars, a length, a flag word, five slots, and the spelled tail"
);
static_assert(alignof(Node) == 8, "The widest member is a global-space coordinate, and nothing here is wider");

// A record nobody finished names nothing and hides nothing: every channel is at rest, the node is
// visible and opaque, its subtree is empty, and its time runs at the parent's rate. Asserted rather
// than assumed, because the defaults are what a partially-written publisher produces and every one of
// them is chosen to make that case a still node rather than an invisible or frozen one.
static_assert(Node{}.SubtreeLength == 0 && Node{}.Past(7) == 8, "A leaf's subtree is the leaf");
static_assert(!Node{}.IsTranslating() && !Node{}.IsScaling() && !Node{}.IsRotating());
static_assert(!Node{}.IsFading() && !Node{}.IsDriven());
static_assert(!Node{}.IsHidden() && !Node{}.IsGroup());
static_assert(Node{}.Opacity == 1.0F && Node{}.TimeScale == 1.0F);
static_assert(Node{}.Transform.Rotation == Quaternion{}, "The chart's base point, and it crosses always");

static_assert(Node{ .Flags = Node::Hidden | Node::Group }.IsHidden());
static_assert(Node{ .Flags = Node::Hidden | Node::Group }.IsGroup());
static_assert(Node{ .Flags = Node::Group }.IsGroup() && !Node{ .Flags = Node::Group }.IsHidden());
