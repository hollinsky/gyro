#pragma once

#include <cstdint>

// Perfetto's field numbers and enumerators, from `protos/perfetto/trace/`.
//
// **Named rather than spelled at the call site because a wrong number does not fail.** It writes a
// field the reader skips, and the trace opens with the events silently missing — which is the same
// symptom as a row nothing recorded on, and the reason an afternoon can go into telling them apart.
//
// **It is a header rather than the encoder's own business because there are two parties now.**
// Trace/Perfetto.cpp writes these numbers and Tools/TraceDump.cpp reads them back, and a second table
// spelled out where the reading happens is a second description of the format: it agrees on the day it
// is written and disagrees the first time a field moves, with no build failure between. One table is
// what makes the dumper evidence about the file rather than a guess that matches.
//
// **Every number below was read out of Perfetto's own `.proto` files rather than remembered.**
// Verified 2026-09-06 against `google/perfetto` at `main`:
// `protos/perfetto/trace/trace_packet.proto`, `protos/perfetto/trace/track_event/track_descriptor.proto`,
// `.../process_descriptor.proto`, `.../thread_descriptor.proto`, `.../debug_annotation.proto`, and
// `protos/perfetto/common/system_info.proto`. There is no copy of those files in this tree and no
// generated code to disagree with them, which is exactly why the provenance is written down: the next
// field somebody adds has to be read the same way, and a number that was recalled rather than read is
// indistinguishable from a correct one until a person opens the trace and finds a gap.
//
// The message each field belongs to is the enclosing struct. Nothing here is a type — these are the
// numbers, and what they mean is Perfetto's schema rather than gyro's.
namespace Perfetto
{

struct Packet
{
	static constexpr std::uint32_t ClockSnapshot = 6;
	static constexpr std::uint32_t Timestamp = 8;
	static constexpr std::uint32_t TrustedSequence = 10;
	static constexpr std::uint32_t TrackEvent = 11;
	static constexpr std::uint32_t InternedData = 12;
	static constexpr std::uint32_t SequenceFlags = 13;
	static constexpr std::uint32_t SystemInfo = 45;
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

	// `cmdline`, repeated: one string per argument rather than one joined string, because Perfetto
	// prints the first as the process's own name and a joined line would name the process after the
	// whole invocation.
	static constexpr std::uint32_t Cmdline = 2;
	static constexpr std::uint32_t Name = 6;

	// `process_labels`, repeated. What a reader sees against the process without clicking anything.
	static constexpr std::uint32_t Labels = 8;
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
	static constexpr std::uint32_t DebugAnnotations = 4;
	static constexpr std::uint32_t Type = 9;
	static constexpr std::uint32_t NameId = 10;
	static constexpr std::uint32_t TrackUuid = 11;
	static constexpr std::uint32_t Name = 23;
	static constexpr std::uint32_t CounterValue = 30;
	static constexpr std::uint32_t FlowIds = 47;
};

// What a slice carries beside its name, shown in Perfetto's argument panel when a person clicks it.
// The name goes out in full rather than through the interning table: the table is `EventName`s, an
// annotation name is a different id space, and there are a handful of these in a trace that has
// thousands of slices.
struct Annotation
{
	static constexpr std::uint32_t UintValue = 3;

	// `string_value`, which is a different field of the same `oneof` the number above belongs to — so a
	// record writing both would be a record whose value is whichever the reader happened to parse last.
	static constexpr std::uint32_t StringValue = 6;
	static constexpr std::uint32_t Name = 10;
};

// What machine this ran on, as `uname` reports it. A message of its own rather than four more labels
// because Perfetto and trace_processor already know these four fields by name, and a system trace
// concatenated alongside carries the same message — so a reader comparing the two is comparing one
// field rather than two spellings of it.
struct SystemInfo
{
	static constexpr std::uint32_t Utsname = 1;
};

struct Utsname
{
	static constexpr std::uint32_t Sysname = 1;
	static constexpr std::uint32_t Version = 2;
	static constexpr std::uint32_t Release = 3;
	static constexpr std::uint32_t Machine = 4;
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
constexpr std::uint64_t ClockRealtime = 1;
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

// What the run said about itself: the identity instant and the log lines, neither of which came out of
// a recorded thread's ring. A sequence of its own rather than the first source's, because a source
// sequence is one ring's window and these two are not — the identity is stamped once at the oldest
// record in the file and a log line may have been written by any thread in the process.
constexpr std::uint64_t RunSequence = 2;
constexpr std::uint64_t SourceSequenceBase = 3;

// A trace file is a sequence of length-delimited `Trace.packet` fields and nothing else, which is what
// makes concatenating two of them a valid third.
constexpr std::uint32_t TracePacket = 1;

} // namespace Perfetto
