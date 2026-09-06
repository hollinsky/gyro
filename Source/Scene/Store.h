#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Animation/Author/Bundle.h"
#include "Animation/Author/Motion.h"
#include "Core/Clock.h"
#include "Core/Handle.h"
#include "Core/Session.h"
#include "Core/SlotAllocator.h"
#include "Core/Time.h"
#include "Scene/Entity.h"
#include "Scene/Focus.h"
#include "Scene/Input.h"
#include "Scene/Output.h"
#include "Scene/Pointer.h"
#include "World/Content.h"
#include "World/Node.h"

// The world, as the dispatch thread holds it.
//
// Docs/Decisions.md decision 111: **the store is a slot map and intrusive links.**
// `Core/SlotAllocator.h` owns identity and mints decision 15's generational handle; the tree is
// parent, first child and next sibling held as handles rather than pointers, so a stale link compares
// unequal instead of naming whatever occupies the slot now. The publisher chases those links, on the
// thread that is allowed to.
//
// **The top level is a list rather than a root**, which is what `Frame/Evaluator.h` already walks: it
// starts at index zero of the node run and treats the span to the end as siblings. A distinguished
// root would be a node every walk pays for, that no reader needs, carrying a transform that is always
// the identity.
//
// **Identity is on every entity, including the structural ones nobody names.** Decision 15 says *for
// every animatable entity whether protocol-backed or compositor-invented*, and a container forced by
// `wl_subsurface.place_below` is compositor-invented. It also pays forward: the per-node damage
// Docs/Open.md wants needs a stable identity on the record, and an identity that already exists here
// is a field to publish rather than a mechanism to invent afterwards.
//
// **Creation is per kind, and that is what keeps a kind from disagreeing with its payload.** A node's
// `Content` is a position in whichever run its `Kind` selects (decision 95), so the two are one fact
// and arriving separately is how they come apart. Four constructors, one per member of decision 95's
// closed vocabulary, each taking exactly what its kind means.
//
// **Mutation is `Scene/Commit.h`'s and reaches an entity through here rather than past it.** Decision
// 89 makes setting a model value *be* a retarget under a commit's shared origin, so a setter that
// staged a value is the shape that decision killed — and an accessor handing back a writable entity is
// the same shape with the staging left to the caller, since what it produces is a motion with no
// transaction around it. So the writable side is private and the scope is its only friend, and what
// this class owns of a commit is the two things a scope cannot: the clock its origin is clamped
// against, and the flag that makes decision 112's *one at a time* a refusal rather than a convention.
//
// **Destruction is two steps and decision 114 is why**, which is the shape that keeps a closing window
// on screen for as long as it is still closing. Retirement is the author going away — a flag on the
// entity, set over the subtree, leaving the entity exactly where it was in the tree so it goes on being
// drawn. The free is separate and later, and what schedules it is the retiring subtree coming to rest:
// `Scene/Serializer.h` sweeps before it walks, on the pass that has the settle thresholds in hand, and
// destroys what has finished. Building the free at the point somebody stopped wanting a window — which
// is the obvious single-step version — puts the slot back while the exit animation is still running on
// it, and what a person sees is a window vanishing instead of leaving.
//
// **The two are not the same event, and conflating them costs a workspace switch.** A window hidden,
// moved between workspaces, or reparented is a link change or a flag and the entity is untouched; only
// destruction by the entity's author retires. Decision 114 makes that distinction because decision 95's
// overview has the real windows under a hidden container with thumbnails referencing them, and a rule
// that retired on removal from the tree would retire nine workspaces of windows every time the
// arrangement changed.

// SPEC: how many entities one system may hold at once. It bounds the index space rather than
// estimating a working set — decision 27 sizes this kind of limit an order of magnitude above anything
// real, and a refusal is what a caller answers for, by killing the client that caused it. A busy
// desktop is a few thousand nodes and an overview of nine workspaces is a few thousand more; nothing
// is reserved up front, so the number costs nothing until the slots are used.
inline constexpr std::uint32_t MaxEntities = 1u << 20;

class SceneStore
{
public:
	explicit SceneStore(const IClock& clock, std::uint32_t capacity = MaxEntities)
		: m_Clock{ &clock }, m_Ids{ capacity }
	{}

	// A container: subnodes and no content. The commonest node there is — every toplevel is one,
	// because `wl_subsurface.place_below` names the parent surface itself as a legal reference, and so
	// is every opacity group, workspace, and overview grid.
	[[nodiscard]] std::optional<EntityId> CreateContainer(EntityId parent, const NodeProperties& properties)
	{
		return Create(parent, properties, NodeKind::Container);
	}

	// An image: a live client surface or a compositor-owned snapshot, which `World/Content.h` argues at
	// length are one kind — exit pixels swap one for the other while the closing spring is running, and
	// a kind that changed mid-transition would restart the collapse a person is watching.
	[[nodiscard]] std::optional<EntityId>
	CreateImage(EntityId parent, const NodeProperties& properties, const ImageContent& content)
	{
		return Create(parent, properties, NodeKind::Image, m_Images, m_ImageOwners, content);
	}

	// A colour fill: the background before a wallpaper, the firmware colour decision 37's handoff
	// continues into, a letterbox fill.
	[[nodiscard]] std::optional<EntityId>
	CreateSolid(EntityId parent, const NodeProperties& properties, const SolidContent& content)
	{
		return Create(parent, properties, NodeKind::Solid, m_Solids, m_SolidOwners, content);
	}

	// A presentation of another subtree, per decision 88 — an overview thumbnail or a switcher tile.
	//
	// **It is not a second identity.** The referenced subtree keeps its own ids and this entity has its
	// own for its own transform, which is what makes a tile and the window it shows one object rather
	// than two the system keeps in agreement. `target` is therefore an id and never a copy.
	//
	// A reference has no children of its own: decision 95 makes `Content` the whole payload of the
	// kind, so children beside the expansion are not a shape the vocabulary has, and `Frame`'s walk
	// would not visit them.
	[[nodiscard]] std::optional<EntityId>
	CreateReference(EntityId parent, const NodeProperties& properties, EntityId target)
	{
		const std::optional<EntityId> created = Create(parent, properties, NodeKind::Reference);

		if (created)
		{
			m_Entities[created->Index].Target = target;
		}

		return created;
	}

	// The entity behind an id, or nothing where the id is stale — freed, or held past what it named.
	// Reaching for `handle.Index` directly is how the generation check gets skipped, so there is no
	// accessor that takes one.
	[[nodiscard]] const Entity* Find(EntityId id) const noexcept
	{
		const std::optional<std::uint32_t> index = m_Ids.IndexOf(id);

		return index ? &m_Entities[*index] : nullptr;
	}

	// Dispatch's own now, which is the instant a commit's origin is never later than. The one reader of
	// the timebase is `Core/Clock.cpp` (decision 57), and this is a scene-wide clock rather than one
	// handed in per transaction, so two callers cannot open two commits against two answers to one
	// question.
	[[nodiscard]] Instant Now() const noexcept { return m_Clock->Now(); }

	// The pacing every `Motion` in the catalog resolves through, and the modifiers laid over it.
	//
	// Scene-wide because it is a preference rather than a property of anything authored: a person who
	// has asked for slower motion has asked for all of it, and `Animation/Author/Motion.h`'s `Overlay`
	// exists to fold a configured pair onto the authored table without a call site knowing either.
	void SetMotions(const MotionTable& table, MotionModifiers modifiers = {}) noexcept
	{
		m_Motions = table;
		m_Modifiers = modifiers;
	}

	[[nodiscard]] const MotionTable& Motions() const noexcept { return m_Motions; }
	[[nodiscard]] MotionModifiers Modifiers() const noexcept { return m_Modifiers; }

	// Whether motion is being watched or is being made accessible, which `Channels(transition, policy)`
	// resolves a commit's transition through.
	//
	// **Beside the motion table rather than folded into it**, for the reason `Animation/Author/Bundle.h`
	// gives: a modifier rescales the vocabulary and this substitutes one channel table for another, so
	// the two are different operations and would read as one slider if they sat in one struct. What they
	// share is this scope — a person who has asked for reduced motion has asked for all of it, and a
	// policy handed in per commit would be a preference two call sites could answer differently.
	//
	// **Read once per commit rather than once per write**, which is `Scene/Commit.h`'s doing: a commit
	// resolves its table in its constructor, so a policy that changed mid-transaction cannot land half a
	// window on each side of it.
	void SetMotionPolicy(MotionPolicy policy) noexcept { m_MotionPolicy = policy; }

	[[nodiscard]] MotionPolicy Policy() const noexcept { return m_MotionPolicy; }

	// Who the keyboard is on, per [Focus.h](Focus.h). Held here rather than beside the store because
	// `ISceneAuthor::Advance` hands an author the world as one argument, and focus is part of the world
	// it authors: a client host reads it to decide who its `wl_keyboard.enter` names, and a shell will
	// write it to declare a model.
	[[nodiscard]] SceneFocus& Focus() noexcept { return m_Focus; }
	[[nodiscard]] const SceneFocus& Focus() const noexcept { return m_Focus; }

	// Where the pointer is, per [Pointer.h](Pointer.h). Beside focus and *not* for focus's reason: focus
	// is one person's and this is the desk's, so a store that splits per session leaves this behind with
	// the seat. They are together here because there is one of each, and the header says which way each
	// goes when there is not.
	[[nodiscard]] ScenePointer& Pointer() noexcept { return m_Pointer; }
	[[nodiscard]] const ScenePointer& Pointer() const noexcept { return m_Pointer; }

	// What this entity accepts of the pointer, or null where it accepts nothing — which is every node
	// nobody has said otherwise about, and is the whole of what most of the world is. [Input.h](Input.h)
	// carries why the default runs that way; `Scene/Hit.h` is the one reader.
	[[nodiscard]] const NodeInput* InputFor(EntityId id) const noexcept
	{
		const std::optional<std::uint32_t> index = m_Ids.IndexOf(id);

		if (!index || *index >= m_Input.size() || !m_Input[*index].Accepts)
		{
			return nullptr;
		}

		return &m_Input[*index];
	}

	// Put this entity in front of its siblings, which is decision 55's z order written to rather than read:
	// the sibling list is the paint order and the last child is the frontmost, so raising is moving it to
	// the end of the chain it is already in.
	//
	// **It is mechanism, and that is why it is a verb here rather than something `SceneFocus` does on the
	// way past.** Decision 51 gives gyro the stacking and a shell the model, and a shell that focuses a
	// window without bringing it forward is an ordinary arrangement rather than a mistake to prevent — a
	// tiled layout where focus moves by keyboard and nothing overlaps, a video pinned above everything
	// that must not fall behind the window a person clicks. Folding the raise into the focus would make
	// both of those unwritable; folding them together in the policy that stands in for an absent shell
	// (162) is a choice that can be taken back.
	//
	// **A link change and never a lifetime one**, which is decision 114's distinction: the entity keeps
	// its id, its children, its channels and its coefficients, and nothing about it retires. It is the
	// same operation on one node that a workspace switch performs on a subtree.
	//
	// True and untouched where the entity is already frontmost, which is the common answer — a person
	// clicking about inside the window they are already using. False for an id that names nothing live.
	bool Raise(EntityId id) noexcept
	{
		Entity* const entity = Mutable(id);

		if (entity == nullptr)
		{
			return false;
		}

		const EntityId last = entity->Parent.IsNull() ? m_LastRoot : m_Entities[entity->Parent.Index].LastChild;

		if (last == id)
		{
			return true;
		}

		// Out of the chain and onto the end of it, through the two halves `Destroy` and `Create` already
		// use. The `NextSibling` is cleared in between because `Append` links onto a node it takes to be
		// fresh, and a stale link here would be a cycle in the walk that draws the world.
		Unlink(id, *entity);

		entity->NextSibling = {};

		Append(entity->Parent, id);

		return true;
	}

	// Move one entity to sit directly after `after` among its siblings, or to the front of the chain
	// where `after` is null. `Raise` is this with the last sibling named, and it is kept separate
	// because the two are asked by different parties for different reasons.
	//
	// **What this is for is `wl_subsurface.place_above` and `place_below`**, which is the one thing in
	// the protocol that names a z order directly. Decision 55 makes the sibling list the z order, so a
	// client restacking its own parts is a relink here and nothing else — no lifetime, no channel, and
	// nothing published that was not published before.
	//
	// **A reference in another chain is refused rather than adopted.** `place_above` names *a sibling
	// surface or the parent*, so a reference that is neither is a client error the caller posts; moving
	// the node anyway would reparent a subsurface into somebody else's window, which is a rectangle
	// drawn over an application that never asked for it.
	//
	// True and untouched where the entity is already there, which is the common answer: a toolkit
	// restates its stacking on every commit and almost never changes it.
	bool Order(EntityId id, EntityId after) noexcept
	{
		Entity* const entity = Mutable(id);

		if (entity == nullptr || id == after)
		{
			return false;
		}

		if (!after.IsNull())
		{
			const Entity* const sibling = Find(after);

			if (sibling == nullptr || sibling->Parent != entity->Parent)
			{
				return false;
			}
		}

		const bool root = entity->Parent.IsNull();
		const EntityId first = root ? m_FirstRoot : m_Entities[entity->Parent.Index].FirstChild;

		if (after.IsNull() ? first == id : m_Entities[after.Index].NextSibling == id)
		{
			return true;
		}

		// Out of the chain and back into it, through the same two halves `Raise` uses. The `NextSibling`
		// is cleared in between because `Insert` links onto a node it takes to be loose, and a stale link
		// here would be a cycle in the walk that draws the world.
		Unlink(id, *entity);

		entity->NextSibling = {};

		Insert(entity->Parent, id, after);

		return true;
	}

	[[nodiscard]] bool IsLive(EntityId id) const noexcept { return m_Ids.IsValid(id); }

	// The top of the tree, as the first of a sibling chain. Decision 55 makes the list order the z
	// order, so the last root is the frontmost.
	[[nodiscard]] EntityId FirstRoot() const noexcept { return m_FirstRoot; }

	[[nodiscard]] std::uint32_t Count() const noexcept { return m_Ids.LiveCount(); }

	// The high-water mark of the index space, which is what anything keyed by an entity index has to be
	// able to address. Not the live count: a freed slot keeps its index reserved.
	[[nodiscard]] std::uint32_t SlotCount() const noexcept { return m_Ids.SlotCount(); }

	// What the leaves draw, indexed by an entity's `Content`. The serializer copies out of these rather
	// than building a record, which is decision 111's out-of-line payload arriving as a memcpy.
	[[nodiscard]] std::span<const ImageContent> Images() const noexcept { return m_Images; }
	[[nodiscard]] std::span<const SolidContent> Solids() const noexcept { return m_Solids; }

	// The output set, replaced whole rather than edited. Hotplug is what changes it, decision 84
	// renumbers the set when it does, and a set that arrived one output at a time would have a moment
	// in which it was neither the old set nor the new one — which is precisely the state decision 84's
	// generation exists to make unreadable rather than to make brief.
	void SetOutputs(std::span<const SceneOutput> outputs)
	{
		m_Outputs.assign(outputs.begin(), outputs.end());
		++m_OutputGeneration;

		// Done here rather than left to the caller for `Retire`'s reason: a pointer stranded where a
		// display used to be is one no motion can rescue, since every displacement from out there
		// slides along a union it is not touching. Whoever unplugs a monitor should not have to
		// remember that.
		m_Pointer.Reconfine(m_Outputs);

		// And for the same reason one step further on: a monitor unplugged is the session it was showing
		// no longer reachable, and the keyboard has to leave with it rather than stay on a window nobody
		// can see (`Scene/Focus.h`).
		m_Focus.Present(m_Outputs);
	}

	[[nodiscard]] std::span<const SceneOutput> Outputs() const noexcept { return m_Outputs; }

	// Assign one output to a session, or back to none.
	//
	// **Deliberately not `SetOutputs` above, and the difference is what the two facts mean.** That verb
	// replaces the set because a hotplug changed which monitors exist, and decision 84's generation is
	// what makes the moment between the old set and the new one unreadable. A session arriving changes
	// no monitor, so renumbering the set here would answer a question nobody asked — *do these runs mean
	// my outputs at all* — and would reconfine the pointer, which a person sees as their cursor jumping
	// because somebody else logged in on another screen.
	//
	// Does nothing for an output that is not here, which is one a hotplug removed between an agent
	// offering its listener and the root getting to this call.
	void SetOutputSession(OutputId output, SessionId session) noexcept
	{
		for (SceneOutput& held : m_Outputs)
		{
			if (held.Id == output)
			{
				held.Session = session;

				// **The cut, which is the absence of a coefficient rather than a mode** (188). Whatever
				// was leaving stops leaving in the same step: a suspend needs the new assignment on the
				// glass at the next flip (59), so there is nothing half-finished to carry over.
				held.Fading = SessionId::None;
				held.Fade.SetImmediate(0.0F);

				// **And the lock goes with it**, because this is the assignment only the composition
				// root can reach and the refusal that hangs off `Locked` is the root's own (188). The
				// caller that needs it is a locked session *ending*: the screen is showing gyro rather
				// than that session, so nothing else in the world would ever give the output back.
				held.Locked = SessionId::None;

				m_Focus.Present(m_Outputs);

				return;
			}
		}
	}

	// The same reassignment, faded rather than cut: the output composites both sessions, live, for the
	// length of the transition, and the one in front moves against the one behind it.
	//
	// **Immediate against animated is not a new axis and this is the whole of the difference** (188).
	// The store already separates an immediate write from a sprung one and already uses it twice —
	// `Scene/Cursor.h` writes the pointer glyph immediately every iteration and `Protocol/Drag.h`
	// writes a dragged window's position immediately rather than sprung — so suspend calls the verb
	// above and a person pressing the lock chord calls this one. Nothing anywhere records which kind of
	// transition happened, because the coefficient's absence is the record.
	//
	// **Not through `Scene/Commit.h`**, which is the one place a channel is otherwise reached: a commit
	// is a scope over the entities one author changed, carrying that author and an origin (112), and
	// there is no entity here. This channel is the compositor's own and no protocol reaches it, which
	// is what decision 43's anti-spoofing argument rests on across a transition — a client may not
	// start one, extend one, re-enter one, or push the opacity back up.
	//
	// **A transition onto the session already being shown does nothing**, rather than fading a screen
	// into itself: it is what a second press of the lock chord is, and what a session arriving twice is.
	// A transition entered while one is running takes the newer pair and drops the older faded session
	// outright — one output moves between two sessions at a time, and the alternative is a queue of
	// screens nobody asked to see.
	//
	// Does nothing for an output that is not here, which `SetOutputSession` above has the reason for.
	void FadeOutputSession(OutputId output, SessionId session, Motion motion, Instant t0)
	{
		for (SceneOutput& held : m_Outputs)
		{
			if (held.Id != output)
			{
				continue;
			}

			if (held.Session == session)
			{
				return;
			}

			// **Which of the two carries the coefficient is decided here, and it is the one in front**
			// (`World/Root.h`). gyro's own scene is never it: `None` is the background at the back and the
			// pointer glyph at the front, so it is not a layer that could be faded as one and a coefficient
			// on it would take the cursor off the screen for the length of the transition. So a transition
			// with gyro on one end fades the *session* end — down when it is leaving, up when it is arriving
			// — and locking and unlocking are one verb read in the two directions rather than two verbs.
			//
			// **Between two real sessions it is still the one being left**, which is right exactly while
			// that session's roots are in front of the arriving one's. Root order is authoring order (55)
			// and nothing orders roots across sessions, so a session that connected later would be in front
			// and the switch would read as a cut. Nothing switches between two sessions yet and this is
			// Docs/Open.md's to settle with the verb that does — the answer is either a third id in the
			// published record or an ordering rule that keeps gyro's own roots at the two ends.
			const bool arriving = session != SessionId::None && held.Session == SessionId::None;

			held.Fading = arriving ? session : held.Session;
			held.Session = session;

			// From the end it is starting at rather than from wherever a previous transition had got to,
			// because what is moving is a whole screen a person is looking at and not the tail of one they
			// already lost.
			held.Fade.SetImmediate(arriving ? 0.0F : 1.0F);
			held.Fade.AnimateTo(arriving ? 1.0F : 0.0F, Resolve<float>(motion, {}), t0);

			// **The keyboard moves now rather than when this settles** (188), which `Scene/Focus.h`
			// gives for nothing: the outgoing session is no longer any output's `Session`, so every
			// window of it stops being somewhere a keystroke can land at the instant the fade is
			// authored. Otherwise the first characters of a password go to the terminal being faded out.
			m_Focus.Present(m_Outputs);

			return;
		}
	}

	// Lock this output: fade to whoever holds a locked screen, and hold the session that was on it.
	//
	// **Locking is a reassignment and nothing else, which is decision 43 taken literally** — the lock
	// screen and the greeter are one UI, so *locked* is the state of an output showing that UI rather
	// than a mode every other part of the compositor has to ask about. What one press of the chord
	// does is move a screen from a person's session to somebody else's, which is the same verb a fast
	// user switch spends and the reason there is no lock state machine (188).
	//
	// **What the lock actually is, is `Locked` being set**, and it is set from what the output was
	// showing rather than from an argument: the party entitled to unlock is the person whose screen it
	// was, and asking the caller to name them again is asking it to get that wrong. `to` is who takes
	// the screen — `SessionId::None`, gyro's own, until there is a greeter — and it is a parameter
	// because the greeter is a session like any other and this verb must not have to change when one
	// arrives.
	//
	// **A locked output is not locked again.** A second press would otherwise overwrite `Locked` with
	// the session currently on screen, which is the lock screen, and the person's session would be
	// held by nothing and unreachable for the rest of the run.
	//
	// The fade is decision 188's: the person's windows dissolve, live, over the length of the
	// transition, and what is underneath them is already on screen at full strength from the first
	// frame. A film playing when a laptop is locked goes on playing as it leaves the screen.
	void LockOutput(OutputId output, SessionId to, Motion motion, Instant t0)
	{
		for (SceneOutput& held : m_Outputs)
		{
			if (held.Id != output || held.Locked != SessionId::None || held.Session == to)
			{
				continue;
			}

			const SessionId locked = held.Session;

			FadeOutputSession(output, to, motion, t0);

			held.Locked = locked;

			return;
		}
	}

	// Unlock it: fade back to the session that was held, and stop holding one.
	//
	// **This exists for the fade to be seen from both ends and leaves when the login agent arrives.**
	// Nothing about a lock a keystroke can undo is a lock, and saying so here is cheaper than a
	// comment somebody has to find later: what unlocks a screen is the party decision 43's greeter
	// authenticates a person to, and until there is one the only way to see the second half of a
	// transition is a development verb that skips the authentication entirely.
	//
	// Does nothing for an output that is not locked, which is a second press of the chord on a screen
	// the first press did not take.
	void UnlockOutput(OutputId output, Motion motion, Instant t0)
	{
		for (SceneOutput& held : m_Outputs)
		{
			if (held.Id != output || held.Locked == SessionId::None)
			{
				continue;
			}

			const SessionId locked = held.Locked;

			held.Locked = SessionId::None;

			FadeOutputSession(output, locked, motion, t0);

			return;
		}
	}

	// Decision 84's set generation: what a per-output run in the snapshot means by *output 2*. It is
	// carried here and not yet on the wire — the header reserves four bytes for it and nothing spends
	// them, so a run whose length matches is trusted for now and the guard lands with the publisher's
	// half of decision 84.
	[[nodiscard]] std::uint64_t OutputGeneration() const noexcept { return m_OutputGeneration; }

	// The entities whose committed state is owed a presentation report, since the last time somebody
	// took them.
	//
	// **This is the authoring half of decision 115's derivation and the reason it is a list rather than
	// a flag on the entity.** The return leg says *sequence S reached the glass on output N*, and what
	// `Protocol` needs from that is *this surface's frame callback is due*. Nothing on the frame side
	// carries a surface, so the join has to be made here, and it is made out of what a commit already
	// knows: the entity it wrote. Keeping it as a list is what makes the cost proportional to what a
	// client did rather than to what the world holds — decision 115 rejects a scan for exactly that,
	// and a flag on the entity would be one.
	//
	// It is taken rather than read: `Dispatch/Loop.h` seals it against the sequence about to be
	// published, which is the one number that turns *this entity committed* into *this entity is owed a
	// frame*.
	[[nodiscard]] std::span<const EntityId> Awaiting() const noexcept { return m_Awaiting; }

	void ClearAwaiting() noexcept { m_Awaiting.clear(); }

	// Say which session's scene a root's subtree is. Decision 21's partition, writer's side.
	//
	// **A root only, and the refusal is what keeps the fact single.** A session belongs to the top of a
	// subtree — `World/Root.h` publishes it per root and the frame thread reads it at depth one — so an
	// entity with a parent is already in whatever session its root is in, and accepting one here would
	// store a number nothing reads and let two answers disagree. A window is therefore never asked: it
	// is parented into its session's floor and the floor answers for it (141).
	//
	// False for an id that names nothing live, and for one that is not a root.
	bool SetSession(EntityId id, SessionId session) noexcept
	{
		Entity* const root = Mutable(id);

		if (root == nullptr || !root->Parent.IsNull())
		{
			return false;
		}

		root->Session = session;

		return true;
	}

private:
	friend class SceneCommit;

	// **The second friend, and it writes for the opposite reason.** `Scene/Serializer.h` retires a spring
	// that has settled — `Animatable::Settle`, and nothing else — which is not a motion an author asked
	// for but the coefficients catching up with arithmetic that was already true. Decision 89's objection
	// to a writable entity is that a caller could *start* a motion outside a transaction; what the
	// serializer does is end one that ended by itself, and it has no way to say anything else, because
	// `Animatable` exposes settling and retargeting as different verbs. Why it is the serializer at all
	// rather than a pass of its own is that file's argument, and it is about touching every entity once.
	friend class SceneSerializer;

	// The writable side of the output set, which only the serializer reaches, and for the one thing it
	// reaches every other channel for: retiring a fade that has finished. Decision 122 puts that on the
	// walk that has the thresholds in hand rather than in a pass of its own, and this is that rule for
	// the one channel that hangs off an output instead of off an entity.
	[[nodiscard]] std::span<SceneOutput> MutableOutputs() noexcept { return m_Outputs; }

	// The writable side of `Find`, which only a commit reaches. Reaching for `handle.Index` directly is
	// how the generation check gets skipped, so there is no accessor that takes one.
	[[nodiscard]] Entity* Mutable(EntityId id) noexcept
	{
		const std::optional<std::uint32_t> index = m_Ids.IndexOf(id);

		return index ? &m_Entities[*index] : nullptr;
	}

	// What a node accepts of the pointer, written through the same door as everything else — see
	// `Scene/Commit.h`. Kept beside the store rather than in a table `Protocol` holds because the slot
	// is the store's: an acceptance has to die with the entity that owned the index, and nothing
	// outside here is told when that happens.
	//
	// False for an id that names nothing live. The row is grown on demand rather than with the entity
	// array, since the nodes that take input are the client surfaces and they are a small part of a
	// world full of containers.
	bool SetInput(EntityId id, NodeInput input)
	{
		const std::optional<std::uint32_t> index = m_Ids.IndexOf(id);

		if (!index)
		{
			return false;
		}

		if (m_Input.size() <= *index)
		{
			m_Input.resize(*index + 1);
		}

		m_Input[*index] = std::move(input);

		return true;
	}

	// The writable side of an image's payload, and it is the one payload with a standing reason to
	// change: a client's next buffer is a new `TextureId` on a node that is otherwise exactly what it
	// was. Behind the same friendship as `Mutable` and reached through an id for the same reason.
	//
	// **Null for anything that is not a live image**, which folds three different mistakes into one
	// refusal a caller can act on: a stale id, a container or a solid handed to an image verb, and a
	// content index that does not address the run. The last cannot happen by construction today —
	// `Create` writes the index and the payload together — and is checked anyway, because what it would
	// otherwise be is a write past the end of a vector on the thread that authors every window.
	[[nodiscard]] ImageContent* MutableImage(EntityId id) noexcept
	{
		Entity* const entity = Mutable(id);

		if (entity == nullptr || entity->Kind != NodeKind::Image || entity->Content >= m_Images.size())
		{
			return nullptr;
		}

		return &m_Images[entity->Content];
	}

	// Decision 114's retirement: the author of this entity has gone away, so it and everything beneath it
	// stop being authorable and start dying. The flag goes on the whole subtree because that is what the
	// author authored — a client destroying its `wl_surface` takes the subsurfaces under it — and the
	// links are untouched, so every one of them is published from exactly where it was on the next pass
	// and for as many passes as its exit takes.
	//
	// **Idempotent, and the guard is the flag rather than a search.** Retiring a node inside an already
	// retiring subtree adds nothing, because the ancestor's retirement already set this flag on the way
	// down. That makes a client tearing down a window surface by surface cost one subtree walk rather
	// than one per surface, and it keeps `m_Retiring` a list of independent deaths.
	//
	// **What it deliberately is not is removal from the tree.** See the header: only the author's
	// disappearance retires, and a workspace switch must not.
	//
	// False for an id that names nothing live, which is a double retire arriving through a stale handle.
	bool Retire(EntityId id) noexcept
	{
		Entity* const root = Mutable(id);

		if (root == nullptr)
		{
			return false;
		}

		if (root->Retiring)
		{
			return true;
		}

		m_Retiring.push_back(id);

		// Iteratively, for `Scene/Serializer.h`'s reason: the depth is the author's, and a shell that
		// nests a thousand containers must not be a stack overflow in the one process on the machine that
		// cannot have one.
		m_Work.clear();
		m_Work.push_back(id);

		while (!m_Work.empty())
		{
			const EntityId at = m_Work.back();
			m_Work.pop_back();

			Entity* const entity = Mutable(at);

			if (entity == nullptr)
			{
				continue;
			}

			entity->Retiring = true;

			// Focus leaves with the author, on the retirement rather than on the free. See Focus.h: the
			// subtree goes on being drawn for as long as its exit runs, and a keystroke reaching a window
			// that is collapsing is the bug the two-step lifetime would otherwise introduce.
			m_Focus.Withdraw(at);

			for (EntityId child = entity->FirstChild; !child.IsNull();)
			{
				const Entity* const next = Find(child);

				m_Work.push_back(child);
				child = next != nullptr ? next->NextSibling : EntityId{};
			}
		}

		return true;
	}

	// Finish a retirement now: every channel under the root stops where its target is, so the next
	// serialisation pass finds the subtree at rest and frees it.
	//
	// **The exit that cannot be drawn is the exit that should not be attempted.** Decision 20 draws a
	// leaving window from a compositor-owned snapshot of its last frame, and until that snapshot exists
	// the pixels under a retiring subtree belong to a client that has destroyed its surface — the
	// texture registry has been told the id is given up, and the frame thread's watermark will reclaim
	// it partway through the exit. A window that fades out as an empty rectangle is worse than one that
	// goes at once, so this is how a retirement says it has no pixels to leave with.
	//
	// **Cutting rather than degrading, which is decision 46's answer to the same shortfall.** That
	// entry resolves atlas exhaustion by hard-settling older exits so their rectangles free, and
	// device loss by hard-settling the whole retiring set — both of them this operation, reached for a
	// different reason. It is CPU-side by construction, which is what makes it available on the path
	// where a GPU has just been unplugged.
	//
	// **Only a retiring subtree, and the guard is the point rather than defensive.** Hard-settling a
	// live node would stop a transition somebody is watching, at whatever value it had reached; there
	// is no caller that wants that, and a verb that could do it is one that eventually does.
	//
	// False for an id that names nothing live, and for one whose author has not gone away.
	// Whether anything in this subtree is drawn from the given texture. Iteratively, for the reason every
	// other walk in this file is: the depth is the author's.
	[[nodiscard]] bool DrawsFrom(EntityId root, TextureId texture) noexcept
	{
		m_Work.clear();
		m_Work.push_back(root);

		while (!m_Work.empty())
		{
			const EntityId at = m_Work.back();
			m_Work.pop_back();

			const Entity* const node = Find(at);

			if (node == nullptr)
			{
				continue;
			}

			if (const ImageContent* const image = MutableImage(at); image != nullptr && image->Texture == texture)
			{
				return true;
			}

			for (EntityId child = node->FirstChild; !child.IsNull();)
			{
				const Entity* const next = Find(child);

				m_Work.push_back(child);
				child = next != nullptr ? next->NextSibling : EntityId{};
			}
		}

		return false;
	}

	bool FinishRetirement(EntityId root) noexcept
	{
		const Entity* const entity = Find(root);

		if (entity == nullptr || !entity->Retiring)
		{
			return false;
		}

		m_Work.clear();
		m_Work.push_back(root);

		while (!m_Work.empty())
		{
			const EntityId at = m_Work.back();
			m_Work.pop_back();

			Entity* const node = Mutable(at);

			if (node == nullptr)
			{
				continue;
			}

			node->Translation.Settle();
			node->Turn.Settle();
			node->Scale.Settle();
			node->Opacity.Settle();

			for (EntityId child = node->FirstChild; !child.IsNull();)
			{
				const Entity* const next = Find(child);

				m_Work.push_back(child);
				child = next != nullptr ? next->NextSibling : EntityId{};
			}
		}

		return true;
	}

	// The pixels behind an id have been given up, so any exit still being drawn from them ends now.
	//
	// **The texture is the link and the entity is not**, which is what this looked for first and did not
	// find. A client tears a window down in the order xdg-shell requires — the role objects, then the
	// `wl_surface` they were given to — so by the time the surface takes its texture away, the role that
	// knew which entity it drew into is already gone. What survives the whole sequence is the id the
	// pixels are named by, which the retiring subtree is still holding because that is exactly what
	// decision 114 keeps it around to draw.
	//
	// A walk of the retirements rather than an index, because the retiring set is the deaths in flight —
	// a handful at the very worst — and this runs when a surface is destroyed rather than per frame. An
	// index would be a second structure to keep true for a scan that is already shorter than it.
	//
	// The whole root is finished rather than the image that named the texture, because the exit is on the
	// window: decision 111's toplevel animates the container and the pixels hang under it, so settling
	// the child alone would leave the frame around it fading with nothing inside.
	bool Abandon(TextureId texture) noexcept
	{
		if (texture.IsNull())
		{
			return false;
		}

		bool finished = false;

		// A copy, because finishing a retirement is a write and the list is the thing being walked.
		m_Abandoning.assign(m_Retiring.begin(), m_Retiring.end());

		for (const EntityId root : m_Abandoning)
		{
			if (DrawsFrom(root, texture))
			{
				finished = FinishRetirement(root) || finished;
			}
		}

		return finished;
	}

	// The retirement roots, for the sweep that decides which of them have finished. `Scene/Serializer.h`
	// is the caller and the header says why it rather than something here: the question is whether every
	// channel in the subtree has settled, and settling is a question about thresholds that only the
	// serializer holds.
	[[nodiscard]] std::span<const EntityId> RetiringRoots() const noexcept { return m_Retiring; }

	// Destroy a subtree: unlink its root from the tree, then give back every slot and every payload
	// underneath it. This is the free decision 114 separates from retirement, and the sweep is what
	// decides when to call it.
	//
	// **Unlinking is what keeps the walk's sibling chains honest.** A destroyed node whose predecessor
	// still pointed at it would leave a link naming a dead slot, and the serializer's walk reads a stale
	// link as the end of a chain — so one window closing would take every window behind it off the screen
	// until something else republished. Finding the predecessor is a scan of the sibling list, which is
	// the one place decision 111's singly-linked children costs something; it is paid when a window
	// closes rather than when one opens, and the append that runs at open stays constant time.
	//
	// False for an id that names nothing live.
	bool Destroy(EntityId id) noexcept
	{
		const Entity* const root = Find(id);

		if (root == nullptr)
		{
			return false;
		}

		Unlink(id, *root);

		m_Work.clear();
		m_Work.push_back(id);

		while (!m_Work.empty())
		{
			const EntityId at = m_Work.back();
			m_Work.pop_back();

			Entity* const entity = Mutable(at);

			if (entity == nullptr)
			{
				continue;
			}

			for (EntityId child = entity->FirstChild; !child.IsNull();)
			{
				const Entity* const next = Find(child);

				m_Work.push_back(child);
				child = next != nullptr ? next->NextSibling : EntityId{};
			}

			// The payload first and the slot second, which is the order the swap below depends on: the
			// entity whose payload moves into this hole has to still be reachable through its handle to
			// have its index repaired, and an entity already freed is one whose payload was already
			// popped and so cannot be the element that moves.
			Release(*entity);

			// The slot's acceptance goes with it, because the slot comes back. Left behind, it is a
			// region belonging to a program that has exited deciding where the clicks land on whatever
			// window is minted into that index next — which reads as one application swallowing another's
			// input, on a machine where nothing connects the two.
			if (at.Index < m_Input.size())
			{
				m_Input[at.Index] = NodeInput{};
			}

			[[maybe_unused]] const bool freed = m_Ids.Free(at);
		}

		std::erase(m_Retiring, id);

		return true;
	}

	// This entity's committed state is owed a presentation report. `Scene/Commit.h` is the only caller,
	// because a commit is the only thing that changes what the world shows.
	//
	// **One entry per entity, and the newest commit is the one that resolves it.** A client that commits
	// twice before either frame reaches the glass gets one report rather than two, which is exactly what
	// `Protocol` does with the callbacks themselves — `ClientSurface::Apply` folds the pending list into
	// the due list on every commit, so a second entry here would be a second answer to a question that
	// has one. The scan is linear over a list whose length is the windows that committed since the last
	// publication, which is one on almost every iteration.
	void Await(EntityId id)
	{
		if (id.IsNull() || std::find(m_Awaiting.begin(), m_Awaiting.end(), id) != m_Awaiting.end())
		{
			return;
		}

		m_Awaiting.push_back(id);
	}

	// Decision 112's *one is open at a time*, as a refusal. A commit opened inside another is a bug at a
	// call site rather than a state to support, and answering it with `false` costs the nested scope
	// every write instead of letting it borrow an origin that belongs to a different event.
	[[nodiscard]] bool OpenCommit() noexcept
	{
		if (m_Committing)
		{
			return false;
		}

		m_Committing = true;

		return true;
	}

	void CloseCommit() noexcept { m_Committing = false; }

	// Nothing is returned when the index space is exhausted, which is a refusal the caller answers for
	// rather than a null id that flows onward and fails somewhere less attributable. A parent that is
	// not live is the same refusal for the same reason: attaching to a window that has gone is a bug
	// with a call site, and silently promoting the node to the top level would put it on screen.
	[[nodiscard]] std::optional<EntityId> Create(EntityId parent, const NodeProperties& properties, NodeKind kind)
	{
		if (!parent.IsNull() && !m_Ids.IsValid(parent))
		{
			return std::nullopt;
		}

		// A reference has no children, and refusing here is what makes that unwritable rather than
		// something the frame walk has to be careful about. Decision 95 makes `Content` the whole
		// payload of the kind — a reference's content *is* a node — so `Frame`'s walk descends into the
		// expansion and never into the run beside it, and a child attached here would be authored,
		// serialised, paid for, and never drawn.
		if (!parent.IsNull() && m_Entities[parent.Index].Kind == NodeKind::Reference)
		{
			return std::nullopt;
		}

		// The array is grown before the id is minted, so the one step here that can fail does so with
		// the store exactly as it was. The other order leaks a slot on an allocation failure — an index
		// permanently occupied by an entity that was never constructed, which the allocator will never
		// hand out again and nothing will ever free. It grows by at most one element ahead of the
		// high-water mark, because a slot coming back off the free list is already inside it.
		if (m_Entities.size() <= m_Ids.SlotCount())
		{
			m_Entities.resize(m_Ids.SlotCount() + 1);
		}

		const std::optional<EntityId> id = m_Ids.Allocate();

		if (!id)
		{
			return std::nullopt;
		}

		Entity& entity = m_Entities[id->Index];

		entity = Entity{ properties };
		entity.Kind = kind;
		entity.Parent = parent;

		Append(parent, *id);

		return id;
	}

	// The same, for a kind that arrives with a payload. The entity is created first and the payload
	// adopted second, so a payload the store cannot store leaves a leaf that draws nothing rather than
	// an id that was minted and thrown away — `World/Node.h`'s sentinel meaning exactly that, and the
	// direction it was chosen for.
	template<typename T>
	[[nodiscard]] std::optional<EntityId> Create(
		EntityId parent,
		const NodeProperties& properties,
		NodeKind kind,
		std::vector<T>& run,
		std::vector<EntityId>& owners,
		const T& content
	)
	{
		const std::optional<EntityId> created = Create(parent, properties, kind);

		if (created)
		{
			m_Entities[created->Index].Content = static_cast<std::uint32_t>(run.size());
			run.push_back(content);
			owners.push_back(*created);
		}

		return created;
	}

	// Link the new entity onto the end of its parent's child list, or onto the end of the top level.
	// The end rather than the front because decision 55 makes the list order the z order, and a window
	// that opened is in front of the ones that were already there.
	void Append(EntityId parent, EntityId id)
	{
		EntityId& first = parent.IsNull() ? m_FirstRoot : m_Entities[parent.Index].FirstChild;
		EntityId& last = parent.IsNull() ? m_LastRoot : m_Entities[parent.Index].LastChild;

		if (last.IsNull())
		{
			first = id;
		}
		else
		{
			m_Entities[last.Index].NextSibling = id;
		}

		last = id;
	}

	// The same link, at a stated position rather than at the end: after `after`, or at the front of the
	// chain where it is null. The caller has already checked that the two share a parent.
	void Insert(EntityId parent, EntityId id, EntityId after)
	{
		EntityId& first = parent.IsNull() ? m_FirstRoot : m_Entities[parent.Index].FirstChild;
		EntityId& last = parent.IsNull() ? m_LastRoot : m_Entities[parent.Index].LastChild;

		if (after.IsNull())
		{
			m_Entities[id.Index].NextSibling = first;
			first = id;

			if (last.IsNull())
			{
				last = id;
			}

			return;
		}

		m_Entities[id.Index].NextSibling = m_Entities[after.Index].NextSibling;
		m_Entities[after.Index].NextSibling = id;

		if (last == after)
		{
			last = id;
		}
	}

	// Take one entity out of the chain it sits in, repairing the three links that can name it: its
	// predecessor's `NextSibling`, and its parent's `FirstChild` and `LastChild` — or the top level's own
	// pair, which decision 111 makes a sibling list with no distinguished root above it.
	void Unlink(EntityId id, const Entity& entity) noexcept
	{
		const bool root = entity.Parent.IsNull();

		EntityId& first = root ? m_FirstRoot : m_Entities[entity.Parent.Index].FirstChild;
		EntityId& last = root ? m_LastRoot : m_Entities[entity.Parent.Index].LastChild;

		EntityId previous{};

		for (EntityId at = first; !at.IsNull() && at != id; at = m_Entities[at.Index].NextSibling)
		{
			previous = at;
		}

		if (previous.IsNull())
		{
			first = entity.NextSibling;
		}
		else
		{
			m_Entities[previous.Index].NextSibling = entity.NextSibling;
		}

		if (last == id)
		{
			last = previous;
		}
	}

	// Give back whatever run this entity's kind put its payload in. A container and a reference have
	// none, which decision 95 makes a property of the kind rather than a case to remember.
	void Release(const Entity& entity) noexcept
	{
		switch (entity.Kind)
		{
			case NodeKind::Image:
				ReleaseFrom(m_Images, m_ImageOwners, entity.Content);
				break;

			case NodeKind::Solid:
				ReleaseFrom(m_Solids, m_SolidOwners, entity.Content);
				break;

			case NodeKind::Container:
			case NodeKind::Reference:
				break;
		}
	}

	// Fill the hole with the last element and repair the one entity whose index moved.
	//
	// **Swap-and-pop rather than a hole left behind**, because the alternative is a run that only ever
	// grows: a session is windows opening and closing all day, and a per-kind array that never shrinks is
	// a leak with a slow clock on it. It is legal because nothing outside this class sees the order — see
	// the note on the owner arrays — so the only thing a move can break is the moved entity's own
	// `Content`, which is the line below.
	template<typename T>
	void ReleaseFrom(std::vector<T>& run, std::vector<EntityId>& owners, std::uint32_t at) noexcept
	{
		if (at >= run.size())
		{
			return;
		}

		const std::size_t last = run.size() - 1;

		if (at != last)
		{
			run[at] = run[last];
			owners[at] = owners[last];

			if (Entity* const moved = Mutable(owners[at]); moved != nullptr)
			{
				moved->Content = at;
			}
		}

		run.pop_back();
		owners.pop_back();
	}

	const IClock* m_Clock;

	SlotAllocator<EntityTag> m_Ids;

	// Indexed by an id's slot, and sized to the high-water mark rather than to the live count — which
	// is the arrangement `Core/SlotAllocator.h` exists to allow: it hands out indices and declines to
	// own a payload, so that the data an entity has can be walked as an array rather than chased.
	std::vector<Entity> m_Entities;

	std::vector<ImageContent> m_Images;
	std::vector<SolidContent> m_Solids;

	// Which entity owns each element of the run beside it, so that freeing one can fill its hole with the
	// last element and repair the one index that moved. Held here rather than as a field on the payload
	// because the payload is `World/Content.h`'s and crosses the waist: an owning `EntityId` on
	// `ImageContent` would be four bytes per image in every published snapshot that no reader on the
	// frame side has any use for.
	//
	// **Nothing outside this class may see the run order**, which is what makes the swap legal at all.
	// `Scene/Serializer.h` copies a payload out per node during its preorder walk and appends it to a run
	// it is building itself, so the position a payload holds here never reaches the wire — decision 111's
	// out-of-line payload arriving as a memcpy, and the index translation the serializer already performs.
	std::vector<EntityId> m_ImageOwners;
	std::vector<EntityId> m_SolidOwners;

	// Decision 114's retiring set as a worklist of the subtree *roots* that entered it, which is not the
	// second container that decision rejects: the entities are in the tree at the position they had and
	// are published from there, and this is the sweep's list of where to look. Decision 112 puts the work
	// lists a transaction walks on the scene rather than in the scope, and this is the first of them.
	//
	// A root is pushed once. Retiring something already inside a retiring subtree is not a second root
	// because the flag was set over the whole subtree when the ancestor retired, and the flag is what the
	// push is guarded on — so the list length is the number of *independent* things dying, which is one
	// per window a person closed.
	std::vector<EntityId> m_Retiring;

	// The retirement roots being asked about, held apart from `m_Retiring` because `Abandon` writes
	// through the list it is walking. A member rather than a local for `m_Work`'s reason: it is reused
	// across surface destructions rather than allocated per one.
	std::vector<EntityId> m_Abandoning;

	// The subtree walk's stack, a member rather than a local for decision 112's reason: it is reused
	// across transactions instead of allocated per one, so retiring and destroying a window allocate
	// nothing after the first few. Only ever live inside one of the two calls that clear it first.
	std::vector<EntityId> m_Work;

	EntityId m_FirstRoot{};
	EntityId m_LastRoot{};

	// What a commit has said is owed a presentation report, taken by the loop at every publication.
	// Empty on almost every iteration, and never longer than the windows a person touched between two
	// frames.
	std::vector<EntityId> m_Awaiting;

	std::vector<SceneOutput> m_Outputs;
	std::uint64_t m_OutputGeneration = 0;

	// Who the keyboard is on. Withdrawn from by `Retire`, offered to by whoever maps a window.
	SceneFocus m_Focus;

	// One per slot rather than one per live entity, and sparse: an entity that accepts nothing is a
	// default-constructed row. Keyed by index for the reason `Core/SlotAllocator.h` hands indices out at
	// all — whatever wants something per entity keeps its own array — and the hit test reads it once per
	// node it visits, which is what makes an index rather than a lookup the right shape.
	std::vector<NodeInput> m_Input;

	// Where the pointer is. Reconfined by `SetOutputs`, moved by whoever drains a device that has a
	// cursor.
	ScenePointer m_Pointer;

	MotionTable m_Motions{};
	MotionModifiers m_Modifiers{};
	MotionPolicy m_MotionPolicy = MotionPolicy::Ordinary;

	bool m_Committing = false;
};
