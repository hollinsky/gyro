#include "Trace/Perfetto.h"

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/Time.h"
#include "Trace/Protobuf.h"

namespace
{

// Perfetto's field numbers, from `protos/perfetto/trace/`. Named here rather than spelled at the call
// site because a wrong number does not fail: it writes a field the reader skips, and the trace opens
// with the events silently missing. The message each one belongs to is the enclosing struct.
struct Packet
{
	static constexpr std::uint32_t ClockSnapshot = 6;
	static constexpr std::uint32_t Timestamp = 8;
	static constexpr std::uint32_t TrustedSequence = 10;
	static constexpr std::uint32_t TrackEvent = 11;
	static constexpr std::uint32_t InternedData = 12;
	static constexpr std::uint32_t SequenceFlags = 13;
	static constexpr std::uint32_t Defaults = 59;
	static constexpr std::uint32_t TrackDescriptor = 60;
};

struct Defaults
{
	static constexpr std::uint32_t TimestampClock = 58;
};

struct Descriptor
{
	static constexpr std::uint32_t Uuid = 1;
	static constexpr std::uint32_t Name = 2;
	static constexpr std::uint32_t Process = 3;
	static constexpr std::uint32_t Thread = 4;
	static constexpr std::uint32_t ParentUuid = 5;
	static constexpr std::uint32_t Counter = 8;
};

struct ProcessDescriptor
{
	static constexpr std::uint32_t Pid = 1;
	static constexpr std::uint32_t Name = 6;
};

struct ThreadDescriptor
{
	static constexpr std::uint32_t Pid = 1;
	static constexpr std::uint32_t Tid = 2;
	static constexpr std::uint32_t Name = 5;
};

struct CounterDescriptor
{
	static constexpr std::uint32_t Unit = 3;
};

struct Event
{
	static constexpr std::uint32_t Type = 9;
	static constexpr std::uint32_t NameId = 10;
	static constexpr std::uint32_t TrackUuid = 11;
	static constexpr std::uint32_t CounterValue = 30;
	static constexpr std::uint32_t FlowIds = 47;
};

struct Snapshot
{
	static constexpr std::uint32_t Clocks = 1;
	static constexpr std::uint32_t PrimaryClock = 2;
	static constexpr std::uint32_t ClockId = 1;
	static constexpr std::uint32_t Timestamp = 2;
};

struct Interned
{
	static constexpr std::uint32_t EventNames = 2;
	static constexpr std::uint32_t Iid = 1;
	static constexpr std::uint32_t Name = 2;
};

// TrackEvent.Type
constexpr std::uint64_t SliceBegin = 1;
constexpr std::uint64_t SliceEnd = 2;
constexpr std::uint64_t InstantEvent = 3;
constexpr std::uint64_t CounterEvent = 4;

// CounterDescriptor.Unit
constexpr std::uint64_t UnitTimeNs = 1;
constexpr std::uint64_t UnitCount = 2;

// TracePacket.SequenceFlags
constexpr std::uint64_t StateCleared = 1;
constexpr std::uint64_t NeedsState = 2;

// BuiltinClock
constexpr std::uint64_t ClockMonotonic = 3;
constexpr std::uint64_t ClockBoottime = 6;

// Track identities. Fixed rather than allocated so that two snapshots of the same run describe the
// same rows, which is what lets a person compare them.
constexpr std::uint64_t ProcessTrack = 1;
constexpr std::uint64_t ThreadTrackBase = 0x100;
constexpr std::uint64_t ScopeTrackBase = 0x200;
constexpr std::uint64_t CounterTrackBase = 0x1000;

// Descriptors go out on their own packet sequence, so that a source's sequence contains only its own
// events in timestamp order and its interning state is cleared exactly once at the front of it.
constexpr std::uint64_t DescriptorSequence = 1;
constexpr std::uint64_t SourceSequenceBase = 2;

[[nodiscard]] std::uint64_t ThreadTrack(std::size_t source) noexcept
{
	return ThreadTrackBase + source;
}

[[nodiscard]] std::uint64_t ScopeTrack(std::uint16_t scope) noexcept
{
	return ScopeTrackBase + scope;
}

// What an output's rows are called. The GPU row is a row rather than a slice depth on the output's own
// because the work on it is not on any thread — it is the queue, and a person reading a dropped frame
// needs to see it beside the record that submitted it rather than inside it.
[[nodiscard]] std::string ScopeName(std::uint16_t scope)
{
	if (scope == TraceThread)
	{
		return "compositor";
	}

	if (scope <= TracedOutputs)
	{
		return std::format("output {}", scope - 1);
	}

	if (scope <= 2 * TracedOutputs)
	{
		return std::format("output {} gpu", scope - 1 - TracedOutputs);
	}

	if (scope <= 3 * TracedOutputs)
	{
		return std::format("output {} deadline", scope - 1 - 2 * TracedOutputs);
	}

	return std::format("output {} blocked", scope - 1 - 3 * TracedOutputs);
}

// A counter is a track of its own, named for the figure and parented to whatever the figure is about.
struct CounterTrack
{
	std::uint16_t Scope = TraceThread;
	const char* Name = nullptr;
	bool Nanoseconds = false;
	std::uint64_t Uuid = 0;
};

// One source, read once before anything is written: which names it uses, which tracks it touches, and
// which counters it samples. A second pass could discover them as it went, but a descriptor that
// arrives after the event referring to it is a row the reader has to backfill, and Perfetto's own
// tooling is happier when it does not have to.
struct Inventory
{
	std::vector<const char*> Names;
	std::unordered_map<const void*, std::uint64_t> NameIds;
	std::vector<CounterTrack> Counters;
	std::vector<std::uint16_t> Scopes;
};

void InternName(Inventory& inventory, const char* name)
{
	const char* const text = name != nullptr ? name : "?";

	if (inventory.NameIds.contains(text))
	{
		return;
	}

	inventory.Names.push_back(text);
	inventory.NameIds.emplace(text, inventory.Names.size());
}

// The same lookup once the inventory is closed. Zero cannot be a valid interning id, so a name the
// first pass did not see writes an event with no name rather than one with somebody else's.
[[nodiscard]] std::uint64_t NameId(const Inventory& inventory, const char* name)
{
	const auto found = inventory.NameIds.find(name != nullptr ? name : "?");

	return found != inventory.NameIds.end() ? found->second : 0;
}

void NoteScope(Inventory& inventory, std::uint16_t scope)
{
	if (scope == TraceThread || std::ranges::find(inventory.Scopes, scope) != inventory.Scopes.end())
	{
		return;
	}

	inventory.Scopes.push_back(scope);
}

[[nodiscard]] std::uint64_t CounterUuid(const Inventory& inventory, const TraceEvent& event)
{
	for (const CounterTrack& counter : inventory.Counters)
	{
		if (counter.Scope == event.Scope && counter.Name == event.Name)
		{
			return counter.Uuid;
		}
	}

	return 0;
}

// The uuid runs across every source rather than within one, which is not a detail: two threads that
// each sampled their first counter would otherwise both claim the same track, and a reader told twice
// that one uuid is two different things keeps the first and silently drops the second.
void NoteCounter(Inventory& inventory, const TraceEvent& event, std::uint64_t& next)
{
	if (CounterUuid(inventory, event) != 0)
	{
		return;
	}

	inventory.Counters.push_back(
		CounterTrack{ .Scope = event.Scope,
	                  .Name = event.Name != nullptr ? event.Name : "?",
	                  .Nanoseconds = event.Kind == TraceKind::Elapsed,
	                  .Uuid = next }
	);

	++next;
}

[[nodiscard]] Inventory TakeInventory(const TraceSource& source, std::uint64_t& nextCounter)
{
	Inventory inventory;

	for (const TraceEvent& event : source.Events)
	{
		NoteScope(inventory, event.Scope);

		switch (event.Kind)
		{
			case TraceKind::Begin:
			case TraceKind::Mark:
				InternName(inventory, event.Name);
				break;

			case TraceKind::Count:
			case TraceKind::Elapsed:
				NoteCounter(inventory, event, nextCounter);
				break;

			case TraceKind::End:
				break;
		}
	}

	return inventory;
}

// A trace file is a sequence of length-delimited `Trace.packet` fields and nothing else, which is what
// makes concatenating two of them a valid third.
void AppendPacket(std::vector<std::byte>& into, const ProtoWriter& packet)
{
	ProtoWriter framed;
	framed.Nested(1, packet);

	const std::span<const std::byte> bytes = framed.View();
	into.insert(into.end(), bytes.begin(), bytes.end());
}

void WriteDescriptor(std::vector<std::byte>& into, const ProtoWriter& descriptor)
{
	ProtoWriter packet;
	packet.Nested(Packet::TrackDescriptor, descriptor);
	packet.Varint(Packet::TrustedSequence, DescriptorSequence);

	AppendPacket(into, packet);
}

} // namespace

std::vector<std::byte> EncodeTrace(const TraceIdentity& identity, std::span<const TraceSource> sources)
{
	std::vector<std::byte> bytes;
	std::vector<Inventory> inventories;

	inventories.reserve(sources.size());

	std::uint64_t nextCounter = CounterTrackBase;

	for (const TraceSource& source : sources)
	{
		inventories.push_back(TakeInventory(source, nextCounter));
	}

	// **First, because everything after it is unreadable without it.** A packet whose clock the reader
	// cannot place is not misplaced, it is dropped — so a trace missing this is not a trace with a
	// shifted timeline, it is an empty one.
	if (identity.Anchor.Monotonic != 0)
	{
		ProtoWriter monotonic;
		monotonic.Varint(Snapshot::ClockId, ClockMonotonic);
		monotonic.Varint(Snapshot::Timestamp, static_cast<std::uint64_t>(identity.Anchor.Monotonic));

		ProtoWriter boottime;
		boottime.Varint(Snapshot::ClockId, ClockBoottime);
		boottime.Varint(Snapshot::Timestamp, static_cast<std::uint64_t>(identity.Anchor.Boottime));

		// No `primary_trace_clock`. Declaring one would be this trace telling a reader what the whole
		// timeline is stamped in, which is a claim it has no standing to make about the system trace it
		// may have been concatenated with — the pair above is a relation, and a relation composes.
		ProtoWriter snapshot;
		snapshot.Nested(Snapshot::Clocks, monotonic);
		snapshot.Nested(Snapshot::Clocks, boottime);

		ProtoWriter packet;
		packet.Nested(Packet::ClockSnapshot, snapshot);
		packet.Varint(Packet::TrustedSequence, DescriptorSequence);

		AppendPacket(bytes, packet);
	}

	{
		ProtoWriter process;
		process.Signed(ProcessDescriptor::Pid, identity.Pid);
		process.Text(ProcessDescriptor::Name, identity.Name);

		ProtoWriter descriptor;
		descriptor.Varint(Descriptor::Uuid, ProcessTrack);
		descriptor.Nested(Descriptor::Process, process);

		WriteDescriptor(bytes, descriptor);
	}

	// Scope tracks are the process's rather than a thread's: an output outlives whichever thread
	// happened to say something about it, and a row that moved when the speaker changed would be a row
	// nobody could read down.
	std::vector<std::uint16_t> declared;

	for (std::size_t index = 0; index < sources.size(); ++index)
	{
		ProtoWriter thread;
		thread.Signed(ThreadDescriptor::Pid, identity.Pid);
		thread.Signed(ThreadDescriptor::Tid, sources[index].Tid != 0 ? sources[index].Tid : identity.Pid);
		thread.Text(ThreadDescriptor::Name, sources[index].Name);

		ProtoWriter descriptor;
		descriptor.Varint(Descriptor::Uuid, ThreadTrack(index));
		descriptor.Nested(Descriptor::Thread, thread);

		WriteDescriptor(bytes, descriptor);

		for (const std::uint16_t scope : inventories[index].Scopes)
		{
			if (std::ranges::find(declared, scope) != declared.end())
			{
				continue;
			}

			declared.push_back(scope);

			const std::string name = ScopeName(scope);

			ProtoWriter track;
			track.Varint(Descriptor::Uuid, ScopeTrack(scope));
			track.Varint(Descriptor::ParentUuid, ProcessTrack);
			track.Text(Descriptor::Name, name);

			WriteDescriptor(bytes, track);
		}

		for (const CounterTrack& counter : inventories[index].Counters)
		{
			ProtoWriter unit;
			unit.Varint(CounterDescriptor::Unit, counter.Nanoseconds ? UnitTimeNs : UnitCount);

			ProtoWriter track;
			track.Varint(Descriptor::Uuid, counter.Uuid);
			track.Varint(
				Descriptor::ParentUuid, counter.Scope == TraceThread ? ThreadTrack(index) : ScopeTrack(counter.Scope)
			);
			track.Text(Descriptor::Name, counter.Name);
			track.Nested(Descriptor::Counter, unit);

			WriteDescriptor(bytes, track);
		}
	}

	for (std::size_t index = 0; index < sources.size(); ++index)
	{
		const TraceSource& source = sources[index];
		const Inventory& inventory = inventories[index];
		const std::uint64_t sequence = SourceSequenceBase + index;

		{
			ProtoWriter interned;

			for (std::size_t name = 0; name < inventory.Names.size(); ++name)
			{
				ProtoWriter entry;
				entry.Varint(Interned::Iid, name + 1);
				entry.Text(Interned::Name, inventory.Names[name]);

				interned.Nested(Interned::EventNames, entry);
			}

			ProtoWriter defaults;
			defaults.Varint(Defaults::TimestampClock, ClockMonotonic);

			// The front of the sequence, and it says two things the rest of it depends on: every
			// timestamp below is monotonic nanoseconds, and the interning table starts here rather than
			// continuing one from a trace this was concatenated with.
			ProtoWriter packet;
			packet.Varint(Packet::TrustedSequence, sequence);
			packet.Varint(Packet::SequenceFlags, StateCleared);
			packet.Nested(Packet::Defaults, defaults);
			packet.Nested(Packet::InternedData, interned);

			AppendPacket(bytes, packet);
		}

		// Slice depth per track, which is what turns a window into balanced slices. See the header. An
		// array rather than a map because the track space is fixed and small, and because the order the
		// leftovers are closed in should not depend on a hash.
		std::array<std::uint32_t, TraceScopes> depth{};

		const auto track = [&](std::uint16_t scope) {
			return scope == TraceThread ? ThreadTrack(index) : ScopeTrack(scope);
		};

		const auto emit = [&](std::uint64_t stamp, const ProtoWriter& event) {
			ProtoWriter packet;
			packet.Varint(Packet::Timestamp, stamp);
			packet.Varint(Packet::TrustedSequence, sequence);
			packet.Varint(Packet::SequenceFlags, NeedsState);
			packet.Nested(Packet::TrackEvent, event);

			AppendPacket(bytes, packet);
		};

		std::uint64_t last = 0;

		for (const TraceEvent& record : source.Events)
		{
			// A track the vocabulary does not have is a record whose scope was never a scope, which can
			// only be a caller bug. Dropped rather than folded onto track zero, where it would read as
			// the compositor having done something it did not.
			if (record.Scope >= TraceScopes)
			{
				continue;
			}

			const std::uint64_t stamp = static_cast<std::uint64_t>(Monotonic::ToNanoseconds(record.Stamp));

			last = stamp;

			ProtoWriter event;

			switch (record.Kind)
			{
				case TraceKind::Begin:
				{
					event.Varint(Event::Type, SliceBegin);
					event.Varint(Event::TrackUuid, track(record.Scope));
					event.Varint(Event::NameId, NameId(inventory, record.Name));

					if (record.Payload != 0)
					{
						event.Fixed64(Event::FlowIds, record.Payload);
					}

					++depth[record.Scope];
					break;
				}

				case TraceKind::End:
				{
					if (depth[record.Scope] == 0)
					{
						continue;
					}

					--depth[record.Scope];

					event.Varint(Event::Type, SliceEnd);
					event.Varint(Event::TrackUuid, track(record.Scope));
					break;
				}

				case TraceKind::Mark:
				{
					event.Varint(Event::Type, InstantEvent);
					event.Varint(Event::TrackUuid, track(record.Scope));
					event.Varint(Event::NameId, NameId(inventory, record.Name));

					if (record.Payload != 0)
					{
						event.Fixed64(Event::FlowIds, record.Payload);
					}

					break;
				}

				case TraceKind::Count:
				case TraceKind::Elapsed:
				{
					event.Varint(Event::Type, CounterEvent);
					event.Varint(Event::TrackUuid, CounterUuid(inventory, record));
					event.Signed(Event::CounterValue, static_cast<std::int64_t>(record.Payload));
					break;
				}
			}

			emit(stamp, event);
		}

		// The iteration the snapshot interrupted, drawn as what it was rather than dropped.
		for (std::uint16_t scope = 0; scope < TraceScopes; ++scope)
		{
			for (std::uint32_t remaining = 0; remaining < depth[scope]; ++remaining)
			{
				ProtoWriter event;
				event.Varint(Event::Type, SliceEnd);
				event.Varint(Event::TrackUuid, track(scope));

				emit(last, event);
			}
		}
	}

	return bytes;
}
