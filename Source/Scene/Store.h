#pragma once

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
// **What is still absent is destruction** — decision 114 makes retirement *the author going away*, a
// flag on the entity and a term in decision 69's wake fold rather than a `Free` at the point somebody
// stopped wanting a window, and building the free before the flag would put the slot back while an exit
// animation is still running on it.

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
		return Create(parent, properties, NodeKind::Image, m_Images, content);
	}

	// A colour fill: the background before a wallpaper, the firmware colour decision 37's handoff
	// continues into, a letterbox fill.
	[[nodiscard]] std::optional<EntityId>
	CreateSolid(EntityId parent, const NodeProperties& properties, const SolidContent& content)
	{
		return Create(parent, properties, NodeKind::Solid, m_Solids, content);
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

private:
	friend class SceneCommit;

	// The writable side of `Find`, which only a commit reaches. Reaching for `handle.Index` directly is
	// how the generation check gets skipped, so there is no accessor that takes one.
	[[nodiscard]] Entity* Mutable(EntityId id) noexcept
	{
		const std::optional<std::uint32_t> index = m_Ids.IndexOf(id);

		return index ? &m_Entities[*index] : nullptr;
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
	[[nodiscard]] std::optional<EntityId>
	Create(EntityId parent, const NodeProperties& properties, NodeKind kind, std::vector<T>& run, const T& content)
	{
		const std::optional<EntityId> created = Create(parent, properties, kind);

		if (created)
		{
			m_Entities[created->Index].Content = static_cast<std::uint32_t>(run.size());
			run.push_back(content);
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

	const IClock* m_Clock;

	SlotAllocator<EntityTag> m_Ids;

	// Indexed by an id's slot, and sized to the high-water mark rather than to the live count — which
	// is the arrangement `Core/SlotAllocator.h` exists to allow: it hands out indices and declines to
	// own a payload, so that the data an entity has can be walked as an array rather than chased.
	std::vector<Entity> m_Entities;

	std::vector<ImageContent> m_Images;
	std::vector<SolidContent> m_Solids;

	EntityId m_FirstRoot{};
	EntityId m_LastRoot{};

	std::vector<SceneOutput> m_Outputs;
	std::uint64_t m_OutputGeneration = 0;

	MotionTable m_Motions{};
	MotionModifiers m_Modifiers{};

	bool m_Committing = false;
};
