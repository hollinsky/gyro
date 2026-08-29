#pragma once

#include <cstdint>
#include <optional>

#include "Animation/Author/Bundle.h"
#include "Animation/Author/Motion.h"
#include "Animation/Solve/Spring.h"
#include "Core/Handle.h"
#include "Core/Texture.h"
#include "Core/Time.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Space.h"
#include "Scene/Entity.h"
#include "Scene/Store.h"
#include "World/Content.h"

// The transaction a mutation happens inside, and the only door onto an entity's channels.
//
// Docs/Decisions.md decision 112: **a commit is a scope on the dispatch thread rather than an object
// with a lifetime.** One is open at a time, it carries an author and an origin, and it closes when the
// wire request that opened it completes — which is what a stack-scoped guard already is, so this is a
// guard and not something a caller can hold, queue, or hand on. Decision 112 rejects the object form by
// name, and for the reason that it invites deferral back in: something with a lifetime is something
// that can be held, and decision 89's phase one is eager.
//
// **Phase one is the whole of what is here: a property write retargets at the write, under the commit's
// shared `t₀`.** Decision 89's case is two commits inside one frame with different origins touching one
// property — ordinary against 1000 Hz input and a 60 Hz panel. A window opens; eight milliseconds later
// a second event focuses and moves it. Resolved once per frame the second target wins and the opening
// never happened; resolved here the second is a retarget from the true position and velocity of an open
// already eight milliseconds in progress. Render what happened, not a summary of it.
//
// **Eager costs nothing to be eager**, which is what makes that affordable at event rate: a write is a
// spring evaluated at its own origin and four floats stored, with no allocation, no lookup, and no
// queue. And two writes to one channel at one `t₀` are *exactly* idempotent — the second samples the
// first's spring at the instant that spring started and reads back the same position and velocity — so
// repeated writes inside one commit are arithmetic rather than motion and the last target wins with no
// trace of the others. `Source/Integration/SceneCommit.Test.cpp` asserts both, on published bytes.
//
// **What is deliberately not here is phase two**, which decision 89 states as availability rather than
// as a list: matching, lifetime, the atlas reservation, and every derived geometry resolve at close,
// because their inputs are the rest of the commit. Closing is therefore the empty half of this file for
// now, and when it fills, the work lists it walks belong to `SceneStore` — reused across transactions
// rather than allocated per one, which is what decision 112 means by the commit being a member of the
// scene. The guard itself is two words and a pointer either way.
//
// **A bundle is not applied here either, and that is the same boundary.** `Animation/Author/Bundle.h`
// resolves a `Transition` to a per-channel `ChannelMotion` — including the reduced-motion overlay, via
// `Channels(bundle, policy)` — and every write below takes one of those. What a bundle *also* carries
// is an anchor policy, which decision 89 puts squarely in phase two: a transition resolved at the write
// captures the node's extent as it stood then, so a window whose extent changes later in the same
// commit grows out of a corner it never had.

// Who opened it. Decision 112's three, and the split that matters is whether an input event is behind
// the commit: the shell over the protocol and gyro handling input it routes both carry that event's
// timestamp, which is what lets two shell processes reacting to one event compose into a single gesture
// with no coordination between them (decision 51). A client at `wl_surface.commit` carries no timestamp
// at all, and that is not an omission — nothing a client authors is a sprung channel.
enum class CommitAuthor : std::uint8_t
{
	Shell,      // a shell, over the protocol, responding to an input event
	Compositor, // gyro itself, handling input it routes
	Client,     // a client, at wl_surface.commit
};

class SceneCommit
{
public:
	// A commit with an origin: everything an input event is behind.
	//
	// **The origin is never later than the dispatch thread's own now.** A `t₀` in the future is not a
	// late start, it is a motion that stands still until the clock catches up — a window that was
	// dragged and does not move for as long as the stamp was wrong. Clamping makes the elapsed time
	// non-negative by construction, since dispatch's now is at or before the frame thread's read of the
	// clock, which is at or before the presentation instant it evaluates for. The backward direction is
	// self-limiting and needs nothing: a stale origin reads as a motion that has already finished, and a
	// decaying exponential evaluated far along is settled rather than wrong.
	SceneCommit(SceneStore& scene, CommitAuthor author, Instant origin) noexcept
		: m_Scene{ &scene }, m_Origin{ Earlier(origin, scene.Now()) }, m_Author{ author }, m_Open{ scene.OpenCommit() }
	{}

	// A commit with no origin, which is the client shape. Every immediate write works; an animating one
	// is refused rather than stamped with `now`, because stamping is a frame of lag added invisibly to
	// whatever it started, on the one axis a person judges most harshly. If a client-driven change ever
	// does want motion it is given an origin deliberately, through the constructor above.
	SceneCommit(SceneStore& scene, CommitAuthor author) noexcept
		: m_Scene{ &scene }, m_Author{ author }, m_Open{ scene.OpenCommit() }
	{}

	// Close is the destructor because the scope is the transaction: `wl_surface.commit` is atomic for
	// one surface by Wayland's own definition and closes one commit, and a shell's commit request closes
	// another. Batching an iteration's traffic into one would mix origins and put two unrelated clients
	// into one phase two, where a match key declared by one could pair with the other's.
	~SceneCommit() noexcept
	{
		if (m_Open)
		{
			m_Scene->CloseCommit();
		}
	}

	SceneCommit(const SceneCommit&) = delete;
	SceneCommit& operator=(const SceneCommit&) = delete;
	SceneCommit(SceneCommit&&) = delete;
	SceneCommit& operator=(SceneCommit&&) = delete;

	// Whether this scope is the open one. Commits do not nest — the double buffering Wayland requires
	// is `Protocol`'s, since a synchronized subsurface's state and an `xdg_surface.ack_configure` both
	// resolve before anything reaches the store — so a commit opened inside another is a bug at a call
	// site, and it refuses every write rather than borrowing the outer scope's origin.
	[[nodiscard]] bool IsOpen() const noexcept { return m_Open; }

	[[nodiscard]] CommitAuthor Author() const noexcept { return m_Author; }

	// The shared `t₀` every write in this scope retargets under, clamped, or nothing. Shared rather than
	// read per write so that opacity and scale start together whatever order a commit body wrote them
	// in, which is what makes write order inside a commit unobservable.
	[[nodiscard]] std::optional<Instant> Origin() const noexcept { return m_Origin; }

	// The four sprung channels, and the one field that is not a channel.
	//
	// Each takes the disposition the transition gave this channel, which is `Animation/Author/Bundle.h`'s
	// vocabulary rather than a second one: `Animate(motion)` springs it, `Immediate()` lands it with no
	// movement, and a default-constructed `ChannelMotion` is `Absent` — not part of this transition, so
	// whatever the channel was doing continues. Absent is a write that says nothing and not a refusal.
	//
	// False is the refusal, and there are three of them: a scope that is not the open one, an id that
	// names nothing live, and an animating write in a commit with no origin.
	bool Move(EntityId id, Vector3<double> position, ChannelMotion how) noexcept
	{
		Entity* entity = Mutable(id);

		return entity != nullptr && Write(entity->Translation, position, how);
	}

	bool Scale(EntityId id, Vector3<float> scale, ChannelMotion how) noexcept
	{
		Entity* entity = Mutable(id);

		return entity != nullptr && Write(entity->Scale, scale, how);
	}

	bool Fade(EntityId id, float opacity, ChannelMotion how) noexcept
	{
		Entity* entity = Mutable(id);

		return entity != nullptr && Write(entity->Opacity, opacity, how);
	}

	// The one channel that composes rather than replaces, and the reason it is not three lines like the
	// others. A rotation is sprung over the geodesic deviation from the orientation the record carries,
	// so a new orientation is a *new chart* and the interrupted velocity is stated in the old one.
	// `Geometry/NodeTransform.h` owns all three conversions and says why copying the velocity across is
	// wrong at exactly the moment interruption is the point: the chart change costs a first-order term,
	// so a retarget that moves the deviation by a right angle sends the angular velocity forty degrees
	// off course — a window that changes direction when it is interrupted.
	//
	// **A retarget that does not move the chart takes the flat path**, which is not a shortcut for
	// speed. The transport is exact arithmetic on paper and a quaternion round trip in floating point,
	// so routing an unchanged base point through it would cost the idempotence the rest of this file
	// has: writing one orientation twice at one `t₀` would drift, where here it is bit-identical.
	bool Turn(EntityId id, Quaternion orientation, ChannelMotion how) noexcept
	{
		Entity* entity = Mutable(id);

		if (entity == nullptr)
		{
			return false;
		}

		if (how.How == Disposition::Absent)
		{
			return true;
		}

		if (how.How == Disposition::Immediate)
		{
			entity->Orientation = orientation;
			entity->Turn.SetImmediate(RotationVector{});

			return true;
		}

		if (!m_Origin)
		{
			return false;
		}

		const SpringState<RotationVector> from = entity->Turn.PresentationState(*m_Origin);
		const SpringParameters<float> motion = Resolve<float>(how.Using, m_Scene->Motions(), m_Scene->Modifiers());

		if (orientation == entity->Orientation)
		{
			entity->Turn.AnimateFrom(from.Position, from.Velocity, RotationVector{}, motion, *m_Origin);

			return true;
		}

		const Quaternion current = Quaternion::FromDeviation(from.Position, entity->Orientation);
		const RotationVector deviation = Quaternion::Deviation(current, orientation);
		const RotationVector velocity = Quaternion::TransportVelocity(from.Position, deviation, from.Velocity);

		entity->Orientation = orientation;
		entity->Turn.AnimateFrom(deviation, velocity, RotationVector{}, motion, *m_Origin);

		return true;
	}

	// The pixels a node draws, replaced — `wl_surface.attach` arriving in the store, and gyro's own
	// authored images swapping a buffer for the same reason a client does.
	//
	// **Immediate, and there is no version of this that is sprung.** A texture id is an identity rather
	// than a quantity: there is no value between the buffer a surface had and the one it has, so a
	// channel here would have nothing to interpolate. What *does* animate across a buffer swap is the
	// node — World/Content.h's exit pixels are a snapshot replacing a live surface underneath a spring
	// that never notices, which is the whole reason an image and a snapshot are one kind — and that
	// motion is already running on the transform when this lands.
	//
	// **It carries no origin and therefore works in a commit that has none**, which is the client shape:
	// `wl_surface.commit` has no timestamp, and this is the write that shape exists for.
	//
	// False for a scope that is not the open one and for an id that is not a live image, which is
	// `SceneStore::MutableImage`'s refusal reaching the call site unchanged.
	//
	// **`source` is the texels this node samples, and it is stated on every attach rather than being
	// sticky.** A buffer swap is where a window's pixel count changes — a resize is a new buffer, and
	// nothing else is — so the rect belongs with the id it describes. Empty is World/Content.h's *the
	// whole image* and is the honest answer for a caller that does not know its own texel count; what
	// it costs is `Frame/Projection.h`'s classification, which cannot promote a node whose texels it
	// cannot count. The dangerous spelling is the one this signature refuses: a stale rect from the
	// last buffer, which claims a sharpness the new one does not have.
	bool Attach(EntityId id, TextureId texture, Rect<BufferSpace> source) noexcept
	{
		ImageContent* const content = m_Open ? m_Scene->MutableImage(id) : nullptr;

		if (content == nullptr)
		{
			return false;
		}

		content->Texture = texture;
		content->Source = source;

		// **The attach is what puts this entity in decision 115's ledger**, and it is the whole of what
		// `Protocol` needs to answer a `wl_surface.frame`: the client wants to know when these pixels
		// reached the glass, and until that answer arrives a toolkit does not draw the next frame.
		//
		// **A commit that asks for a callback and attaches nothing is covered by this and not missed.**
		// It is legal and ordinary — a client asking to be paced while showing what it already showed —
		// and `Protocol/Shell.cpp` reaches this verb on every commit a mapped window makes, because a
		// surface's content is sticky and the attach it performs is of whatever the surface currently
		// holds. A commit against a surface with no window at all reaches nothing here, correctly: there
		// is no node, so there is nothing for a panel to show.
		m_Scene->Await(id);

		return true;
	}

	// Decision 114's retirement: this entity's author has gone away, so it and its subtree stop being
	// authorable and begin dying. A client destroying a `wl_surface`, a shell disconnecting and taking
	// the arrangement it built with it.
	//
	// **It retires rather than removes, and the whole point is that the window stays on screen.** The
	// subtree keeps its links and its position, so it goes on being published and drawn for as long as
	// anything on it is still moving — which is what makes an exit animation possible at all — and
	// `Scene/Serializer.h` frees it on the pass its last channel settles. Before there is an exit catalog
	// that is the very next pass, so a closed window disappears; the rule does not change when the
	// catalog lands, only the number of passes does.
	//
	// **It is a write and therefore inside the transaction**, which is not ceremony: decision 89 puts
	// lifetime at close so that a retire and a re-create in one commit annihilate before either spends an
	// atlas rectangle or launches an exit — a declarative shell rebuilding its arrangement is
	// remove-then-add on every node, every commit. That cancellation is phase two and is not here; what
	// is here sets the flag eagerly, the way every other write in this file resolves eagerly.
	//
	// **No motion argument and no origin.** What retirement starts is an exit the *catalog* names, not one
	// a caller passes, and it works in a commit with no origin because a client destroying a surface is
	// exactly the shape that has none.
	//
	// False for a scope that is not the open one and for an id that names nothing live — the second being
	// a double retire arriving through a handle that has already gone stale.
	bool Retire(EntityId id) noexcept { return m_Open && m_Scene->Retire(id); }

	// A node's own quad, which has no coefficient slot and never had one. It is here because it is what
	// a client commit mostly writes: a client resizing itself changes this, and decision 68 has a
	// subsurface's position snap for the same reason — a spring between a video player's controls and
	// the screen would make them lag the video underneath.
	bool Resize(EntityId id, Size<SurfaceSpace, float> extent) noexcept
	{
		Entity* entity = Mutable(id);

		if (entity == nullptr)
		{
			return false;
		}

		entity->Extent = extent;

		return true;
	}

private:
	[[nodiscard]] static constexpr Instant Earlier(Instant left, Instant right) noexcept
	{
		return left < right ? left : right;
	}

	[[nodiscard]] Entity* Mutable(EntityId id) noexcept { return m_Open ? m_Scene->Mutable(id) : nullptr; }

	// One channel's write, which is the whole of phase one for everything whose chart does not move.
	//
	// The retarget is `Animatable`'s own: it samples the spring at this commit's origin and starts a new
	// one from that position and that velocity. Nothing here decides *whether* it is a retarget, because
	// there is no other kind of write — decision 89 kills the staged model value, and what it kills it
	// for is the invariant a second field would need across resurrection, hard settle on resume, atlas
	// eviction, and device migration. Drift there is not a hitch that passes: it is a window whose
	// logical position and animated destination permanently disagree, so clicks land where it is not.
	template<typename Channel, typename Value>
	bool Write(Channel& channel, const Value& target, ChannelMotion how) noexcept
	{
		switch (how.How)
		{
			case Disposition::Absent:
				return true;

			case Disposition::Immediate:
				channel.SetImmediate(target);
				return true;

			case Disposition::Animate:
				break;
		}

		if (!m_Origin)
		{
			return false;
		}

		channel.AnimateTo(
			target, Resolve<typename Channel::Scalar>(how.Using, m_Scene->Motions(), m_Scene->Modifiers()), *m_Origin
		);

		return true;
	}

	SceneStore* m_Scene;
	std::optional<Instant> m_Origin;
	CommitAuthor m_Author;
	bool m_Open;
};
