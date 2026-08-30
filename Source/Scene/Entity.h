#pragma once

#include <cstdint>

#include "Animation/Author/Animatable.h"
#include "Core/Handle.h"
#include "Core/Session.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Space.h"
#include "World/Elevation.h"
#include "World/Material.h"
#include "World/Node.h"

// What the dispatch side holds, one per published node.
//
// Docs/Decisions.md decision 111: **there is one kind of object.** An entity is the authoring side's
// record, a `World/Node.h` `Node` is what it publishes, and the two are one to one — not because a
// window is one node, but because every part of one needs identity anyway. A toplevel is a container
// holding its below-subsurfaces, its own surface, and its above-subsurfaces (decision 95), and each of
// those parts has a buffer to release, a frame callback to fire, and damage of its own, all of which
// decision 75 requires dispatch to *derive* from a presented sequence. Parts that need identity are
// entities, and the entity-owns-a-subtree shape has a level nobody uses.
//
// **The difference from the record it publishes is that a channel here is an `Animatable` and there
// it is a value or an index.** Decision 86 puts model values inline and coefficients by reference, and
// decision 98 makes *names no coefficient* the statement that a channel is at rest — so which of the
// two a channel crosses as is a fact about this record's springs at the instant of publication, and
// `Scene/Serializer.h` is where that is read. Nothing here holds a run index, because a run index is a
// position in a serialisation that does not exist yet.
//
// **The per-kind payload is out of line, and the reason is not the wire's.** Decision 95 keeps content
// in per-kind runs so the frame walk does not drag a payload through cache; here it is only that most
// entities have none — a container has no texture, no damage, and no colour state, and
// `Geometry/Region.h` alone is a quarter of a kilobyte at its fixed capacity. Two unrelated arguments,
// one layout, which is what makes the publisher's per-kind emission a copy rather than a build.
//
// **Mutation is `Scene/Commit.h`'s and there is no setter here.** Decision 89 makes setting a model
// value *be* a retarget under the commit's shared origin, so a staged value is the shape that decision
// killed — and what a channel below offers is `AnimateTo`, which is that retarget and nothing else.
// What has not arrived with it is decision 89's phase two, the half whose inputs are the rest of the
// commit: the differ, the match set, and the atlas reservation. Decision 114's retiring flag has
// arrived ahead of them — it is below, it is set eagerly at the write like every phase-one write, and
// what stayed behind in phase two is only the cancellation, which is a retire and a re-create in one
// transaction annihilating before either spends anything.
//
// **And there is no client damage**, which is a different kind of absence: decision 113 puts a
// `Region<BufferSpace>` on the image entity and a run of its own on the wire, and both land when
// `Protocol` mints the first rectangle.

// What an author says a node is: everything an entity carries that is not the tree, the kind, or the
// payload the kind selects.
//
// **The initial values are at rest, which is what makes the first commit against an entity ordinary.**
// `Animation/Author/Animatable.h` states it for one property — a node is constructed carrying the
// state the shell would have set had it been asked — and this is that for all four channels at once.
// It is not a way in past decision 89: a value set here has no motion to interrupt, because there is
// nothing yet that could be watching it.
struct NodeProperties
{
	// The three sprung transform channels, at rest. `Orientation` is the *chart's base point* rather
	// than a fourth start value: decision 90's rule is that a node carries whatever reconstitutes its
	// value, and a rotation is sprung over a geodesic deviation anchored at its target, so a settled
	// one is a zero deviation over this quaternion and this crosses whether or not anything is turning.
	Vector3<double> Position{};
	Vector3<float> Scale{ 1.0F, 1.0F, 1.0F };
	Quaternion Orientation{};

	// Not animated channels, and they cross unconditionally for the plain reason that nothing else
	// carries them. See `Geometry/NodeTransform.h`.
	Vector3<float> Anchor{};
	Perspective Projection{};

	// The node's own quad, in the space a node's own coordinates are in whether or not a client
	// supplied them.
	Size<SurfaceSpace, float> Extent{};

	float Opacity = 1.0F;

	// Decision 19's per-subtree time scale. One is the identity.
	float TimeScale = 1.0F;

	// `Node::Hidden` and `Node::Group`, spelled as the record spells them so the published field and
	// the authored one are read as one thing.
	std::uint32_t Flags = Node::None;

	Material Dress = Material::None;
	Elevation Lift = Elevation::None;
};

// One entity: what an author said, plus the tree it sits in.
//
// **The links are handles rather than pointers**, so a stale link compares unequal instead of naming
// whatever occupies the slot now — decision 15 doing its job on the structure as well as on the ids.
// Links rather than a contiguous child array because the commonest structural change in a desktop is a
// sibling reorder: clicking a window raises it, and decision 55 makes z the list order, so that is a
// relink here and a move of everything above it there.
//
// **One array rather than links beside the entities in a second one.** `Core/SlotAllocator.h` hands
// out indices precisely so that whatever wants something per entity keeps its own array — and here the
// walk that reads a node's links is the same walk that reads its channels, on the same visit, so
// splitting them would guarantee two cache misses where one array costs one.
struct Entity
{
	constexpr Entity() = default;

	constexpr explicit Entity(const NodeProperties& properties) noexcept
		: Translation{ properties.Position }, Scale{ properties.Scale }, Opacity{ properties.Opacity },
		  Orientation{ properties.Orientation }, Anchor{ properties.Anchor }, Projection{ properties.Projection },
		  Extent{ properties.Extent }, TimeScale{ properties.TimeScale }, Flags{ properties.Flags },
		  Dress{ properties.Dress }, Lift{ properties.Lift }
	{}

	// Parent, first child, and next sibling are decision 111's three. `LastChild` is a fourth and it is
	// the append that pays for it: a scene is built one window at a time onto the end of a sibling list,
	// and without a tail the construction of a workspace is quadratic in the windows on it. It is
	// derived and it is maintained, which decision 16 usually refuses — the exemption is that it is a
	// position in a list rather than a value anything reads, so the failure mode of letting it drift is
	// a link the store repairs rather than a picture that disagrees with itself.
	EntityId Parent{};
	EntityId FirstChild{};
	EntityId LastChild{};
	EntityId NextSibling{};

	// The four sprung channels. `Turn` is the log-map deviation against a target of zero, which is the
	// construction `Animation/Author/Animatable.h` describes and declines to offer as a call: a new
	// orientation is a new chart, so retargeting one goes through `Geometry`'s transport rather than
	// through `AnimateTo`.
	Animatable<Vector3<double>> Translation{};
	Animatable<Vector3<float>> Scale{ Vector3<float>{ 1.0F, 1.0F, 1.0F } };
	Animatable<RotationVector> Turn{};
	Animatable<float> Opacity{ 1.0F };

	Quaternion Orientation{};
	Vector3<float> Anchor{};
	Perspective Projection{};

	Size<SurfaceSpace, float> Extent{};
	float TimeScale = 1.0F;
	std::uint32_t Flags = Node::None;

	NodeKind Kind = NodeKind::Container;
	Material Dress = Material::None;
	Elevation Lift = Elevation::None;

	// Decision 114's retiring set, which is this flag and not a container. The author that created this
	// entity has gone away, so nothing will ever write to it again — but it is still drawn, still
	// published, and still at the position it had, because an exit animation is a motion on a node and a
	// node that had been moved somewhere else to die would have to be spliced back into the preorder run
	// to be serialised at all.
	//
	// **What it means is a predicate three dispatch-side readers apply**: layout skips it, focus will not
	// land on it, and hit-testing passes through it. All three are on this side of the waist, which is
	// why it is not a `Node::` flag — the frame thread draws a retiring node exactly as it draws any
	// other, and a bit on the published record would be a bit nothing over there ever reads.
	//
	// **It is also what schedules the entity's destruction**, in `Scene/Serializer.h`: the free happens
	// when every channel in the retiring subtree has settled, which before there is an exit catalog is
	// the very next serialisation and afterwards is the frame the exit finishes on. One rule, stated
	// once, correct on both sides of that catalog landing.
	bool Retiring = false;

	// Where this entity's payload is, in whichever of the store's per-kind arrays `Kind` selects, or
	// `NoContent` for a container or a reference. It is *not* the position the node record will carry:
	// the published runs are built by the walk that emits them, so the two numbers agree only by
	// accident and the serializer translates.
	std::uint32_t Content = NoContent;

	// The session whose scene the subtree rooted here belongs to, read at the top level and nowhere
	// else.
	//
	// **A session is a property of a root, and everything under one is in it by being under it.** That
	// is what `World/Root.h` publishes and it is why this is not asked of a window: a client's entity is
	// parented into its session's floor (141), so the floor answers for it and a second copy on every
	// descendant would be a second thing to keep true. `SceneStore::SetSession` refuses an entity that
	// has a parent for exactly that reason, rather than storing a value the serialiser would not read.
	//
	// `None` is gyro's own — the floor of a development run with no agent, and the splash and the
	// console when they are authored — and it is drawn on every output rather than on none.
	SessionId Session = SessionId::None;

	// What a `Reference` presents, as an id rather than as an index.
	//
	// **A separate field from `Content` even though the record fuses them**, because the record's
	// spelling is a *node index* and an index does not exist until something has been serialised.
	// Decision 88 also makes the distinction real rather than notational: a reference is not a second
	// identity, so what it names is the referenced subtree's own `EntityId` and never a copy of it.
	EntityId Target{};

	// The published record, less the three fields that are positions in a serialisation.
	//
	// `SubtreeLength` is the walk's, and the channel slots and `Content` are indices into runs the
	// walk is still building — so this is the whole of the one-to-one mapping decision 111 asserts, and
	// `Scene/Serializer.h` supplies exactly what a record cannot know about itself.
	//
	// **Every channel's inline value is the model value, whether or not the channel is moving.** While
	// a spring is active the inline value is redundant and the walk takes the coefficient (decision 86);
	// when it settles the reference is dropped and this is the whole answer (decision 98). Publishing
	// the model unconditionally is what makes those two the same line of code rather than a case.
	[[nodiscard]] constexpr Node Record() const noexcept
	{
		Node node{};

		node.Transform.Translation = Translation.Model();
		node.Transform.Rotation = Orientation;
		node.Transform.Scale = Scale.Model();
		node.Transform.Anchor = Anchor;
		node.Transform.Projection = Projection;

		node.Extent = Extent;
		node.Opacity = Opacity.Model();
		node.TimeScale = TimeScale;
		node.Flags = Flags;
		node.Kind = Kind;
		node.Dress = Dress;
		node.Lift = Lift;

		return node;
	}
};
