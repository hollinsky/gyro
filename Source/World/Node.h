#pragma once

#include <cstdint>
#include <type_traits>

#include "Geometry/NodeTransform.h"
#include "Geometry/Space.h"
#include "World/Elevation.h"
#include "World/Material.h"

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
// **This file does not include World/Content.h, and the omission is the design.** Decision 95 keeps
// what a leaf draws in per-kind runs beside the node run, so a node addresses content by *index* and
// never names the record — exactly as it addresses a spring by index below without including
// Animation/Solve/Spring.h. The include looks obviously missing and adding it would be the union this
// record refuses: the common node is a container or a group with nothing to draw, and it would drag
// the payload through cache in order not to use it.
//
// **What is deliberately absent is now smaller and worth naming.** Decision 95 settles the scene
// vocabulary that Docs/Open.md carried as decision 51's sketch, and it lands here as four fields.
// What it explicitly leaves out is clipping — a node's children are not clipped to its extent, and
// `Group` is not a substitute, since its offscreen sits at the subtree's screen-space bound and so
// contains the overflow rather than cutting it — and any blend mode but `over`. Both stay in
// Docs/Open.md. What stays open *inside* the vocabulary is the contents of the two dressing enums
// rather than their shape, which World/Material.h and World/Elevation.h each say for themselves.

// A channel that is not moving, in the slots below. Every run is shorter than this by many orders of
// magnitude, so the sentinel costs no representable index — and it is the *default*, which is the
// direction that matters: a record somebody filled in partially names no coefficients rather than
// naming element zero of every run.
inline constexpr std::uint32_t NoCoefficient = 0xFFFF'FFFFu;

// The same sentinel for the same reason, one field down: a node that draws nothing addresses no
// content. Every per-kind run is shorter than this by many orders of magnitude, so it costs no
// representable position — and it is the *default*, which is again the direction that matters. A
// record somebody filled in partially names no content rather than naming element zero of every run,
// which would be a container silently wearing the first window's pixels.
inline constexpr std::uint32_t NoContent = 0xFFFF'FFFFu;

// What a node is: decision 95's closed set of four, and the run its `Content` indexes.
//
// **Free rather than nested inside `Node`, and that is a language constraint rather than a taste.**
// The member is `Kind`, and a data member cannot share a name with a nested type declared in the same
// class — so nesting it would force the member to be called something else, and every read site to
// say a word that is not the word. `Node::Flag` above is nested for the opposite reason: it is scoped
// so the flags combine without a cast and cannot be spelled without `Node::`.
enum class NodeKind : std::uint8_t
{
	// Subnodes and no content. Forced rather than convenient: `wl_subsurface.place_below` names the
	// parent surface itself as a legal reference, and decision 55 makes z the list order, so the only
	// encoding of a subsurface beneath its parent's own pixels is a container holding the
	// below-subsurfaces, the parent's own surface, and the above-subsurfaces in that order. An
	// ordinary toplevel is therefore already one, as is every opacity group, workspace, and overview
	// grid. Without the kind a container would be a fully transparent `Solid`: one draw item per
	// container per frame, sampling nothing and covering nothing.
	Container,

	// `Content` is a position in the image run. A live client surface and a compositor-owned snapshot
	// are the same kind, which World/Content.h argues at length: exit pixels swap one for the other
	// while the closing spring is running, and a kind that changes mid-transition restarts the
	// collapse a person is watching.
	Image,

	// `Content` is a position in the solid run. Mostly gyro's own — the background before a wallpaper,
	// the firmware colour the handoff continues into, a letterbox fill.
	Solid,

	// `Content` is a *node index*, and decision 88's subtree reference is the whole payload, so this
	// kind needs no run at all. One field with two readings, and they are the same reading: a
	// reference's content is a node.
	Reference,
};

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

	// Where this node's content is: a position in whichever run `Kind` selects, or `NoContent`. A
	// `Container` names nothing and that is what the sentinel is for.
	//
	// **When `Kind` is `Reference` this is a node index instead, and it must point backwards.**
	// Decision 95 requires the target's index to be *lower* than this node's, which makes a cycle
	// unrepresentable rather than something the frame thread has to detect — and a detector is what
	// decision 90 declines to rely on, because an unbounded walk inside the frame section on a
	// `SCHED_FIFO` thread ends with `RLIMIT_RTTIME` taking every session's UI at once. A rule that
	// makes the bad state unwritable beats a test that would run per node per frame.
	//
	// What it costs is an authoring order — a subtree is published before every presentation of it —
	// and that is how an overview is written anyway: the real windows near the top of the run under a
	// hidden container, the thumbnails below pointing back at them. A reference expands the referenced
	// root's flags as authored, which is why the originals are hidden by hiding their *parent* rather
	// than each of them.
	std::uint32_t Content = NoContent;

	// Which of the four the node is, and therefore which run `Content` is a position in. See
	// `NodeKind` above for what each one means and why the type is free rather than nested.
	NodeKind Kind = NodeKind::Container;

	// Decision 33's dressing, on every kind rather than being a kind of its own. As a kind, a glass
	// window would be an effect-layer node stacked over a surface node — two transforms and two corner
	// radii that agree only while nothing moves, and every frame in which they disagree is a bright
	// seam around a translucent panel.
	//
	// **Spelled the way Seam/Renderer.h's `DrawItem::Dress` is spelled, and deliberately**, so the
	// published field and the emitted one are read as one thing rather than as two fields that happen
	// to carry the same value.
	Material Dress = Material::None;

	// Decision 96's window shadow, as a named level. Orthogonal to `Dress` and to `Kind`, because a
	// glass panel casts a shadow too — which is why it is a second byte here rather than more
	// enumerators in the first.
	Elevation Lift = Elevation::None;

	// Padding that is spelled, so the tail is the publisher's to value-initialise rather than
	// whatever the arena last held — the obligation Core/Wake.h and Animation/Solve/Spring.h record
	// for their own, and the reason a publisher builds this from a value-initialised object. It is
	// also what lands the record on 128: three one-byte fields and five spelled bytes, rather than
	// three fields and five bytes the compiler inserts where nobody can see them.
	std::uint8_t Reserved[5]{};

	[[nodiscard]] constexpr bool IsHidden() const noexcept { return (Flags & Hidden) != 0; }

	[[nodiscard]] constexpr bool IsGroup() const noexcept { return (Flags & Group) != 0; }

	[[nodiscard]] constexpr bool IsContainer() const noexcept { return Kind == NodeKind::Container; }

	[[nodiscard]] constexpr bool IsReference() const noexcept { return Kind == NodeKind::Reference; }

	// The two kinds whose `Content` is a position in a content run, rather than a node index or
	// nothing.
	//
	// **It is not the emission test, and what separates the two is decision 95's effect layer.** A
	// container dressed `Glass` draws the blurred backdrop and nothing else — that is what makes a
	// material a field rather than a kind — so it emits a draw item while naming no content at all.
	// The walk emits for `HasContent() || IsDressed() || IsLifted()`; this predicate says where a
	// node's pixels come from, not whether it has any.
	//
	// **Both dressings count, and separately.** Decision 95 keeps them in one slot because they are
	// orthogonal, so a node lifted but undressed is decision 99's overview thumbnail — a shadow with
	// the referenced window drawn over it — and testing only the material would drop it silently.
	[[nodiscard]] constexpr bool HasContent() const noexcept
	{
		return Kind == NodeKind::Image || Kind == NodeKind::Solid;
	}

	[[nodiscard]] constexpr bool IsDressed() const noexcept { return Dress != Material::None; }

	[[nodiscard]] constexpr bool IsLifted() const noexcept { return Lift != Elevation::None; }

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
// 128 exactly, and the eight decision 95 added are worth what they cost: two cache lines where 120
// straddled, so the walk's indexing becomes a shift rather than a multiply and a node never spans a
// third line. There is no implicit padding in the layout — the three one-byte fields sit together and
// the five spelled ones finish them.
static_assert(
	sizeof(Node) == 128,
	"A transform, an extent, two model scalars, a length, a flag word, five slots, a content index, "
	"three one-byte fields, and the spelled tail"
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
static_assert(Node{}.IsContainer() && !Node{}.IsReference() && !Node{}.HasContent(), "Nothing drawn is nothing named");
static_assert(Node{}.Content == NoContent && !Node{}.IsDressed() && !Node{}.IsLifted());
static_assert(Node{}.Opacity == 1.0F && Node{}.TimeScale == 1.0F);
static_assert(Node{}.Transform.Rotation == Quaternion{}, "The chart's base point, and it crosses always");

static_assert(Node{ .Flags = Node::Hidden | Node::Group }.IsHidden());
static_assert(Node{ .Flags = Node::Hidden | Node::Group }.IsGroup());
static_assert(Node{ .Flags = Node::Group }.IsGroup() && !Node{ .Flags = Node::Group }.IsHidden());

static_assert(Node{ .Content = 0, .Kind = NodeKind::Image }.HasContent());
static_assert(Node{ .Content = 3, .Kind = NodeKind::Reference }.IsReference());
static_assert(!Node{ .Content = 3, .Kind = NodeKind::Reference }.HasContent(), "A reference names a node, not a run");
