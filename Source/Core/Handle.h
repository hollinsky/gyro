#pragma once

#include <concepts>
#include <cstdint>
#include <format>
#include <functional>
#include <string_view>
#include <type_traits>

// Runtime identity.
//
// Everything animatable gets one of these — windows, popups, layer surfaces, workspaces, overview
// thumbnails, switcher tiles, drag placeholders, effect nodes — and so does every output. An id is
// a slot index plus the generation that slot carried when the id was minted, so an id that outlives
// what it named compares unequal to whatever occupies the slot now rather than silently naming it.
//
// Three properties are load-bearing and each rules out an alternative:
//
//   It is an index, so evaluation walks arrays instead of chasing pointers, and the published
//   snapshot can address what it refers to by offset from a base that differs between the two sides
//   of the publication boundary. A pointer is neither, and is reused after free with no way to
//   notice — the failure mode being a new entity silently inheriting a dead one's animation state.
//
//   It is independent of tree position, so a window moving between workspaces, going fullscreen, or
//   becoming a tab in a stack keeps its identity. Anything path-derived breaks all three.
//
//   It is independent of protocol lifetime, so an entity outlives the wl_resource that described
//   it. That is what an exit animation requires, and it is what a client destroying a surface while
//   the frame thread still holds it demands.
//
// Runtime only. Session restore across a restart is a different problem needing an app id and a
// persistence key, and it must not contaminate this.
//
// See Docs/Decisions.md decision 15 and Docs/Animation.md#identity.

// The tag is what makes two kinds of id two types: passing an OutputId where an EntityId belongs is
// a compile error rather than a lookup into the wrong array. It carries a name so a handle can
// print as something a log reader can act on, which costs nothing since the tag is otherwise empty.
template<typename T>
concept HandleTag = requires {
	{ T::Name } -> std::convertible_to<std::string_view>;
};

template<HandleTag Tag>
struct Handle
{
	// An aggregate deliberately. This is the shape that crosses the publication boundary, so it has
	// to be trivially copyable and standard layout, and the frame side reconstitutes it from bytes
	// rather than through a constructor. Forgery is not the threat model — a fabricated id is
	// caught by SlotAllocator's generation and parity checks exactly like a stale one.
	std::uint32_t Index = 0;
	std::uint32_t Generation = 0;

	// Generation 0 is never minted, so it is the null state — which is what lets a
	// default-constructed handle be null without reserving an index to mean it.
	[[nodiscard]] constexpr bool IsNull() const noexcept { return Generation == 0; }

	// Deliberately no operator bool. "Is not null" and "still names something live" are different
	// questions, only a SlotAllocator can answer the second, and a conversion appearing to answer
	// either would be read as answering the one that matters.

	friend constexpr bool operator==(Handle, Handle) noexcept = default;

	// Ordering exists so a set of handles can be sorted into index order before a traversal, which
	// is the difference between walking the entity arrays forwards and walking them at random. It
	// is not an invitation to key a node-based container on one.
	friend constexpr auto operator<=>(Handle, Handle) noexcept = default;
};

struct EntityTag
{
	static constexpr std::string_view Name = "Entity";
};

// One id space for everything animatable rather than one per kind. Matched geometry transitions
// between entities of different kinds — a window becomes an overview thumbnail — and a transition
// spanning two id spaces could not be expressed as one entity moving at all.
using EntityId = Handle<EntityTag>;

struct OutputTag
{
	static constexpr std::string_view Name = "Output";
};

// A frame is (output, predicted presentation time), and also (output, device grid), so an OutputId
// is threaded through evaluation, quantization, damage, budgets, and the snapshot atlas. It is
// generational for the entity's reason rather than a weaker one: a monitor is unplugged and the
// next one to arrive takes the slot, and a clock or a budget still keyed to the old one is wrong
// about a display that is physically no longer there.
using OutputId = Handle<OutputTag>;

template<HandleTag Tag>
struct std::hash<Handle<Tag>>
{
	[[nodiscard]] std::size_t operator()(Handle<Tag> handle) const noexcept
	{
		// Mixed rather than concatenated. libstdc++'s prime-modulus buckets would survive the
		// identity hash, and an open-addressed table keyed on the low bits — which is what this
		// codebase would write if it wrote one — would put every id from one slot run in a single
		// probe sequence. splitmix64's finalizer is a handful of instructions and removes the
		// question rather than leaving it to whichever container is reached for later.
		std::uint64_t value = (static_cast<std::uint64_t>(handle.Generation) << 32) | handle.Index;

		value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
		value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;

		// Returned rather than cast: size_t is 64 bits on every target gyro builds for, so a cast
		// would be to the type the value already has. Where it is narrower the low half is taken,
		// which is where the finalizer put the entropy anyway.
		return value ^ (value >> 31);
	}
};

// Prints as Entity#12.4, or Entity#null. Both halves are wanted: the index says which slot to look
// at in a dump, and the generation is what distinguishes this occupant from the one a stale log
// line a few seconds earlier was talking about. No format spec is accepted.
//
// The context is a template parameter rather than std::format_context, and it has to be. The
// std::formattable concept — which is what decides whether a value is printed or elided in a test
// failure — instantiates the formatter against an unspecified context type that is not required to
// be, and in libstdc++ is not, the one std::format uses. Naming the concrete type leaves std::format
// working and the concept false, which is the pair of symptoms that hides this until a report
// somewhere prints <unprintable> at the moment the value was wanted.
template<HandleTag Tag>
struct std::formatter<Handle<Tag>>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(Handle<Tag> handle, Context& context) const
	{
		if (handle.IsNull())
		{
			return std::format_to(context.out(), "{}#null", Tag::Name);
		}

		return std::format_to(context.out(), "{}#{}.{}", Tag::Name, handle.Index, handle.Generation);
	}
};

// The contract everything downstream assumes. The runtime half waits on the test harness.
static_assert(sizeof(EntityId) == 2 * sizeof(std::uint32_t), "The published representation has no padding");
static_assert(std::is_trivially_copyable_v<EntityId> && std::is_standard_layout_v<EntityId>);
static_assert(!std::is_convertible_v<EntityId, OutputId> && !std::is_convertible_v<OutputId, EntityId>);

static_assert(std::formattable<EntityId, char>, "A report prints the id rather than <unprintable>");
static_assert(std::formattable<OutputId, char>);

static_assert(EntityId{}.IsNull(), "A default-constructed id names nothing");
static_assert(!EntityId{ 7, 2 }.IsNull());
static_assert(EntityId{ 7, 0 }.IsNull(), "Nullness is the generation's alone; index 0 is a usable slot");
static_assert(EntityId{ 1, 2 } != EntityId{ 1, 4 }, "A reused slot is not the same id");
static_assert(EntityId{ 1, 4 } < EntityId{ 2, 2 }, "Ordering is by index first, for traversal order");
