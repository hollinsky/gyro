#pragma once

#include <cstdint>
#include <optional>

#include "Animation/Author/Bundle.h"
#include "Animation/Author/Catalog.h"
#include "Animation/Author/Motion.h"
#include "Animation/Solve/Spring.h"
#include "Core/Handle.h"
#include "Core/Texture.h"
#include "Core/Time.h"
#include "Core/Trace.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Shape.h"
#include "Geometry/Space.h"
#include "Scene/Entity.h"
#include "Scene/Input.h"
#include "Scene/Reach.h"
#include "Scene/Store.h"
#include "World/Content.h"
#include "World/Node.h"

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
// **The transition is a property of the scope and not of the write**, which is
// Docs/Animation.md#declarative-commits: a caller says what kind of event this is, once, and mutates
// world state — it never animates a property, so it cannot forget to. What that buys is retroactive:
// a channel added to the catalog later animates at every site already written, because no site named a
// channel. The shape this replaced took a `ChannelMotion` per write, which is per-property animation
// with a nicer spelling and would have made a fifth channel a fifth line everywhere.
//
// **`Channels(transition, policy)` is resolved in the constructor**, so the table and the origin are
// fixed together for the whole scope. That is what makes write order unobservable in the second axis
// as well as the first: two channels of one transition cannot resolve against two answers to what the
// reduced-motion preference is.
//
// **What a bundle *also* carries is an anchor policy, and that is still not here.** Decision 89 puts it
// squarely in phase two: an anchor resolved at the write captures the node's extent as it stood then,
// so a window whose extent changes later in the same commit grows out of a corner it never had.

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
	SceneCommit(SceneStore& scene, CommitAuthor author, Instant origin, Transition transition) noexcept
		: m_Scene{ &scene }, m_Origin{ Earlier(origin, scene.Now()) },
		  m_Channels{ Channels(transition, scene.Policy()) }, m_Author{ author }, m_Open{ scene.OpenCommit() }
	{}

	// **The escape hatch, and it is a constructor rather than an argument on every write.**
	// Docs/Animation.md#escape-hatch asks for exactly this shape: something greppable in a single
	// command and obvious in review, reachable from gyro's own code and never from a shell — which a
	// per-write parameter is not, because a parameter present on every call site is not an escape from
	// anything. `Uncatalogued` is that command.
	//
	// **What it is for is the instrument rather than the desktop.** `Gym` exists to put one channel in
	// front of a renderer under a motion somebody chose, which is precisely what naming a transition
	// forbids and precisely what the gym is: a transition is four channels designed together, and a
	// tool for looking at one of them cannot be expressed as one. Nothing that draws a person's desktop
	// belongs here — if a real change wants a table the catalog does not have, the answer is a catalog
	// entry, and `Transition::BackgroundChange` is one that was found this way.
	struct Uncatalogued
	{
		ChannelTable Channels;
	};

	SceneCommit(SceneStore& scene, CommitAuthor author, Instant origin, Uncatalogued channels) noexcept
		: m_Scene{ &scene }, m_Origin{ Earlier(origin, scene.Now()) }, m_Channels{ channels.Channels },
		  m_Author{ author }, m_Open{ scene.OpenCommit() }
	{}

	// A commit with no origin, which is the client shape. Every immediate write works; an animating one
	// is refused rather than stamped with `now`, because stamping is a frame of lag added invisibly to
	// whatever it started, on the one axis a person judges most harshly. If a client-driven change ever
	// does want motion it is given an origin deliberately, through the constructor above.
	SceneCommit(SceneStore& scene, CommitAuthor author) noexcept
		: m_Scene{ &scene }, m_Channels{ Channels(Transition::None, scene.Policy()) }, m_Author{ author },
		  m_Open{ scene.OpenCommit() }
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
	// **None of them takes a motion**, because the scope already said what kind of change this is. Each
	// resolves against the disposition the commit's transition gave that channel — `Animate(motion)`
	// springs it, `Immediate()` lands it with no movement, and `Absent` means this transition has no
	// opinion about the channel, so whatever it was doing continues.
	//
	// **Absent is a write that says nothing rather than a refusal**, and a caller writing a channel its
	// transition is silent about gets exactly that: nothing. That is the sharp edge of naming the
	// transition once — `Transition::WindowOpen` is silent about position, so placing a window inside a
	// WindowOpen commit does not place it. Placement belongs to a `Transition::None` scope, or to
	// `Scene/Entity.h`'s `NodeProperties` at creation, which is the state the author would have set had
	// it been asked.
	//
	// False is the refusal, and there are three of them: a scope that is not the open one, an id that
	// names nothing live, and an animating write in a commit with no origin.
	bool Move(EntityId id, Vector3<double> position) noexcept
	{
		Entity* entity = Mutable(id);

		return entity != nullptr && Write(entity->Translation, position, m_Channels.Translation);
	}

	bool Scale(EntityId id, Vector3<float> scale) noexcept
	{
		Entity* entity = Mutable(id);

		return entity != nullptr && Write(entity->Scale, scale, m_Channels.Scale);
	}

	bool Fade(EntityId id, float opacity) noexcept
	{
		Entity* entity = Mutable(id);

		return entity != nullptr && Write(entity->Opacity, opacity, m_Channels.Opacity);
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
	bool Turn(EntityId id, Quaternion orientation) noexcept
	{
		Entity* entity = Mutable(id);

		if (entity == nullptr)
		{
			return false;
		}

		const ChannelMotion how = m_Channels.Rotation;

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
	bool Retire(EntityId id) noexcept
	{
		if (!m_Open || !m_Scene->Retire(id))
		{
			return false;
		}

		// **The exit storage is taken here, which is where the retirement is observed** (46). A
		// rectangle per output the subtree is on (190), out of a packer, with no allocation and no
		// device call — the cost of it landing on the frame a person closed something is the whole
		// reason it is a reservation rather than an image.
		//
		// **Reserved eagerly, and decision 89 already names what that costs.** Phase two cancels a
		// retirement that turns out to be a move — remove-then-add inside one commit — so an eager
		// reservation is taken for entities that were never leaving, and under decision 46 that
		// pressure settles *other* exits early. It is eager for the same reason the flag above is:
		// phase two is the empty half of this file, and a reservation that waited for it would be one
		// nothing takes. When close arrives, this moves into it.
		//
		// **A refusal changes nothing here, which it did not used to.** There is no snapshot, so the exit
		// is drawn from the window's own pixels for its whole length — and the pin below is what makes
		// that a real fallback rather than a hope, because those pixels can no longer be taken away
		// mid-exit. Decision 46's *cut instead of fading* is still the answer where a window genuinely
		// has nothing to draw; it is `FinishRetirement` below, reached by whoever finds that out, and it
		// is no longer reached by a client destroying a surface, since that no longer takes anything.

		// **The pixels this exit will be drawn from, taken here because here is where they are known.**
		// Decision 20 draws a leaving window from a compositor-owned copy of its last frame, and the copy
		// is made on the frame thread — so the client's pixels have to outlive the client by at least the
		// frame that copies them. Pinning them at the retirement is what makes that unconditional: the
		// world is holding exactly the buffers the window is drawing right now, and nothing about a
		// destroy request arriving later can change which those are.
		//
		// **It also outranks whatever the dying surface does**, which is the reversal. A surface used to
		// offer the id it was holding and the store used to go looking for a subtree that drew from it;
		// the two were never the same commit, so nothing was ever kept and every client window vanished
		// instead of fading. `Protocol/Surface.h` now simply gives its ids up and asks whether an exit
		// still wants them.
		static_cast<void>(m_Scene->HoldExitPixels(id));

		const Coverage cover = Cover(*m_Scene, id);

		// **The first of the four marks an exit that never appeared is diagnosed by, and the only one
		// that can say *nothing was ever going to be drawn*.** A refusal is silent by design — the line
		// above discards the result and the paragraph above that says why it is allowed to — so a window
		// that cuts instead of fading looks identical here to one that faded correctly. The mark is what
		// separates them, and it is on the dispatch thread's own row because a retirement is one event
		// about one window rather than something that happened to a screen.
		if (!cover.Reachable)
		{
			TraceMark("exit unreachable", TraceThread, TraceTag(id.Index));

			return true;
		}

		const bool reserved = m_Scene->ReserveExit(id, ReachOf(cover, m_Scene->Outputs()), cover.Bounds);

		TraceMark(reserved ? "exit reserved" : "exit unreserved", TraceThread, TraceTag(id.Index));

		return true;
	}

	// End a retirement that is already running: every channel under `id` stops where its target is, so
	// the next serialisation pass finds the subtree at rest and frees it.
	//
	// **The exit that cannot be drawn is the exit that should not be attempted.** Decision 20 draws a
	// leaving window from a compositor-owned snapshot of its last committed frame, and until that
	// exists a client that destroyed its surface has taken the pixels with it — the texture id is given
	// up, and the frame thread reclaims it partway through the exit. A window fading out as an empty
	// rectangle is worse than one that goes at once. `Protocol/Surface.h` is the caller, on destruction
	// and nowhere else.
	//
	// **Decision 46 reaches for the same operation twice more**, which is why it is a verb rather than
	// a branch inside the retire: atlas exhaustion hard-settles older exits so their rectangles free,
	// and device loss hard-settles the whole retiring set because freeing a snapshot must not need the
	// GPU that has just been unplugged.
	//
	// A commit verb for `Retire`'s reason and with `Retire`'s shape — no motion and no origin, since
	// what it does is take motion away — and refused on anything whose author has not gone away, since
	// hard-settling a live node would stop a transition somebody is watching.
	bool FinishRetirement(EntityId id) noexcept
	{
		if (!m_Open || !m_Scene->FinishRetirement(id))
		{
			return false;
		}

		// **The mark that says a window went instead of leaving.** A cut and a fade that finished are
		// the same shape from every other row — the subtree settles, the sweep frees it, the rectangle
		// goes back — so without this the one visible difference between a window sliding away and a
		// window blinking out of existence is invisible in a trace. On the dispatch thread's own row
		// beside the reservation it is undoing.
		TraceMark("exit cut", TraceThread, TraceTag(id.Index));

		return true;
	}

	// What this node accepts of the pointer: the whole of its extent where `shape` is nothing, and the
	// shape's interior otherwise. See [Scene/Input.h](Input.h) for why the two are not the same absence.
	//
	// **It is a commit verb rather than a setter for `Resize`'s reason, and it is the same fact.** A
	// client that resizes and reshapes in one `wl_surface.commit` has stated one arrangement, and a hit
	// test that saw the new extent against the old shape would put a dead strip down the side of a
	// window for exactly one frame — which is a click that does nothing, on the frame a person is most
	// likely to be clicking. Both land inside the same scope, so no reader ever sees half of it.
	//
	// **No motion argument, because a shape does not animate.** Nothing here is a channel: the interior
	// of a window is a fact about where its buttons are, and springing it would mean the pointer landing
	// somewhere the picture had already left.
	bool AcceptInput(EntityId id, std::optional<SurfaceShape> shape)
	{
		return m_Open && m_Scene->SetInput(id, NodeInput{ .Accepts = true, .Shape = std::move(shape) });
	}

	// Back to inert: this node takes no pointer at all. A surface that has lost its role, and the state
	// every node in the world is in until something says otherwise.
	bool RefuseInput(EntityId id) { return m_Open && m_Scene->SetInput(id, NodeInput{}); }

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

	// The material drawn behind this node's own pixels, which is decision 33's *named surface* rather
	// than a filter with parameters on it.
	//
	// **A commit verb for `Resize`'s reason, because it is the same fact as the pixels it sits behind.**
	// A shell that changes its material and redraws in one `wl_surface.commit` has stated one
	// arrangement, and applying the two in different frames would show a frame of the new blur under the
	// old artwork — which on a launcher opening is the first frame a person sees of it.
	//
	// **No motion argument, because a material does not animate.** There is nothing between two names to
	// interpolate, and a cross-fade from glass to smoke would be a third material nobody declared.
	bool Dress(EntityId id, Material material) noexcept
	{
		Entity* entity = Mutable(id);

		if (entity == nullptr)
		{
			return false;
		}

		entity->Dress = material;

		return true;
	}

	// Move a node, and everything under it, into another parent. A shell putting a window on a
	// workspace, and a container being removed with windows still in it handing them back to the floor.
	//
	// **A commit verb rather than a setter, because a reparent alone is never the whole sentence.** A
	// node's position is stated relative to its parent, so a window moved between two containers is
	// somewhere else on screen the instant the link changes and stays there until something writes a
	// position — and the two have to be one arrangement rather than two, or a person sees the window
	// arrive in the new workspace at the coordinates it had in the old one. `SceneStore::Reparent` is
	// deliberately silent about position for that reason: the pairing is stated here, at the call site,
	// where both halves are in the same scope.
	//
	// **No motion argument, because a link is not a quantity.** There is nothing between one parent and
	// another to interpolate. What animates across a reparent is the position written beside it, under
	// whatever transition this scope named, and the spring that was already running on that channel is
	// retargeted rather than restarted — so a window dragged onto a workspace mid-settle finishes
	// settling there.
	//
	// False for a scope that is not the open one, and for everything `SceneStore::Reparent` refuses: an
	// id or a parent that names nothing live, a null parent, a retiring node, and a parent inside the
	// subtree being moved.
	bool Reparent(EntityId id, EntityId parent) noexcept { return m_Open && m_Scene->Reparent(id, parent); }

	// Draw this node and everything under it. The state every node is in unless something says
	// otherwise, and the one a hidden workspace comes back from.
	//
	// **A commit verb for `Dress`'s reason and with `Dress`'s shape**: it is a fact about the same
	// arrangement the rest of the scope states, so a shell that reveals a workspace and moves it in one
	// commit has them arrive together rather than showing one frame of the workspace at the position it
	// is about to leave.
	//
	// **No motion argument, because there is nothing between shown and hidden.** A fade is an opacity,
	// which is a channel and has one; this is the flag the frame thread's walk skips a subtree on, and
	// a half-skipped subtree is not a picture of anything. A shell wanting a workspace to fade out
	// fades it under a transition and hides it when it has gone.
	bool Show(EntityId id) noexcept { return SetHidden(id, false); }

	// Stop drawing this node and everything under it. The subtree keeps its position, its children and
	// its channels — a hidden workspace is a workspace that is still arranged, which is what makes
	// coming back to it free.
	bool Hide(EntityId id) noexcept { return SetHidden(id, true); }

private:
	bool SetHidden(EntityId id, bool hidden) noexcept
	{
		Entity* const entity = Mutable(id);

		if (entity == nullptr)
		{
			return false;
		}

		entity->Flags = hidden ? entity->Flags | Node::Hidden : entity->Flags & ~Node::Hidden;

		return true;
	}

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

	// Resolved once, in the constructor, from the transition this scope was opened with and the policy
	// in force when it opened. Held rather than looked up per write for the reason the origin is: the
	// two together are what makes write order inside a commit unobservable, and a table re-resolved per
	// write would let a reduced-motion preference change land on half a window.
	ChannelTable m_Channels;

	CommitAuthor m_Author;
	bool m_Open;
};
