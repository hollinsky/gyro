#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Animation/Author/Motion.h"
#include "Core/Clock.h"
#include "Core/Handle.h"
#include "Core/SlotAllocator.h"
#include "Core/Time.h"
#include "Scene/Entity.h"
#include "Scene/Output.h"
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
	// exists to fold a configured pair onto the authored table without a call site knowing either. The
	// reduced-motion policy is not here, because it is not a modifier of the same kind — it replaces a
	// bundle's channels rather than rescaling them, which `Channels(bundle, policy)` does upstream of
	// any write.
	void SetMotions(const MotionTable& table, MotionModifiers modifiers = {}) noexcept
	{
		m_Motions = table;
		m_Modifiers = modifiers;
	}

	[[nodiscard]] const MotionTable& Motions() const noexcept { return m_Motions; }
	[[nodiscard]] MotionModifiers Modifiers() const noexcept { return m_Modifiers; }

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
	}

	[[nodiscard]] std::span<const SceneOutput> Outputs() const noexcept { return m_Outputs; }

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

	// The writable side of `Find`, which only a commit reaches. Reaching for `handle.Index` directly is
	// how the generation check gets skipped, so there is no accessor that takes one.
	[[nodiscard]] Entity* Mutable(EntityId id) noexcept
	{
		const std::optional<std::uint32_t> index = m_Ids.IndexOf(id);

		return index ? &m_Entities[*index] : nullptr;
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

			for (EntityId child = entity->FirstChild; !child.IsNull();)
			{
				const Entity* const next = Find(child);

				m_Work.push_back(child);
				child = next != nullptr ? next->NextSibling : EntityId{};
			}
		}

		return true;
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

	MotionTable m_Motions{};
	MotionModifiers m_Modifiers{};

	bool m_Committing = false;
};
