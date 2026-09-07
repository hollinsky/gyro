#include "Trace/Perfetto.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/Clock.h"
#include "Core/Time.h"
#include "Core/Trace.h"
#include "Testing/Test.h"
#include "Trace/Protobuf.h"
#include "Trace/Schema.h"

namespace
{

// The reader is Trace/Protobuf.h's and the field numbers are Trace/Schema.h's, because a test that
// spelled either out again would be asserting that the encoder agrees with the test rather than that
// it agrees with Perfetto. What is left here is the shorthand the assertions are phrased in.
using Field = ProtoField;

[[nodiscard]] std::optional<Field> Find(std::span<const std::byte> message, std::uint32_t number)
{
	return ProtoFind(message, number);
}

// Every top-level `Trace.packet`, which is the only thing a trace file contains.
[[nodiscard]] std::vector<Field> Packets(std::span<const std::byte> trace)
{
	return ProtoFindAll(trace, Perfetto::TracePacket);
}

constexpr std::uint32_t TimestampField = Perfetto::Packet::Timestamp;
constexpr std::uint32_t TrackEventField = Perfetto::Packet::TrackEvent;
constexpr std::uint32_t TrackDescriptorField = Perfetto::Packet::TrackDescriptor;
constexpr std::uint32_t TypeField = Perfetto::Event::Type;
constexpr std::uint32_t TrackUuidField = Perfetto::Event::TrackUuid;
constexpr std::uint32_t NameIdField = Perfetto::Event::NameId;
constexpr std::uint32_t FlowField = Perfetto::Event::FlowIds;
constexpr std::uint32_t InlineNameField = Perfetto::Event::Name;
constexpr std::uint32_t CounterValueField = Perfetto::Event::CounterValue;
constexpr std::uint32_t DescriptorNameField = Perfetto::Descriptor::Name;
constexpr std::uint32_t AnnotationField = Perfetto::Event::DebugAnnotations;
constexpr std::uint32_t AnnotationValueField = Perfetto::Annotation::UintValue;
constexpr std::uint32_t AnnotationNameField = Perfetto::Annotation::Name;

[[nodiscard]] std::size_t CountEvents(std::span<const std::byte> trace, std::uint64_t type)
{
	std::size_t count = 0;

	for (const Field& packet : Packets(trace))
	{
		const std::optional<Field> event = Find(packet.Bytes, TrackEventField);

		if (!event)
		{
			continue;
		}

		const std::optional<Field> kind = Find(event->Bytes, TypeField);

		if (kind && kind->Value == type)
		{
			++count;
		}
	}

	return count;
}

[[nodiscard]] std::vector<std::string> DescriptorNames(std::span<const std::byte> trace)
{
	std::vector<std::string> names;

	for (const Field& packet : Packets(trace))
	{
		const std::optional<Field> descriptor = Find(packet.Bytes, TrackDescriptorField);

		if (!descriptor)
		{
			continue;
		}

		if (const std::optional<Field> name = Find(descriptor->Bytes, DescriptorNameField); name)
		{
			names.emplace_back(reinterpret_cast<const char*>(name->Bytes.data()), name->Bytes.size());
		}
	}

	return names;
}

[[nodiscard]] bool Names(std::span<const std::string> names, std::string_view wanted)
{
	for (const std::string& name : names)
	{
		if (name == wanted)
		{
			return true;
		}
	}

	return false;
}

[[nodiscard]] TraceEvent
At(std::int64_t nanoseconds,
   TraceKind kind,
   const char* name,
   std::uint64_t payload = 0,
   std::uint16_t scope = TraceThread,
   bool arrow = false)
{
	return TraceEvent{ .Stamp = Monotonic::FromNanoseconds(nanoseconds),
		               .Name = name,
		               .Payload = payload,
		               .Scope = scope,
		               .Kind = kind,
		               .Arrow = arrow };
}

constexpr std::uint64_t SliceBegin = Perfetto::SliceBegin;
constexpr std::uint64_t SliceEnd = Perfetto::SliceEnd;
constexpr std::uint64_t InstantEvent = Perfetto::InstantEvent;
constexpr std::uint64_t CounterEvent = Perfetto::CounterEvent;

} // namespace

GYRO_TEST(Perfetto, ASliceIsABeginAndAnEnd)
{
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Begin, "iteration"),
		                                  At(2'000, TraceKind::End, nullptr) };
	const std::array sources{ TraceSource{ .Name = "frame", .Tid = 42, .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);

	GYRO_CHECK_EQ(CountEvents(trace, SliceBegin), std::size_t{ 1 });
	GYRO_CHECK_EQ(CountEvents(trace, SliceEnd), std::size_t{ 1 });
}

GYRO_TEST(Perfetto, TimestampsAreTheRecordsOwn)
{
	const std::vector<TraceEvent> events{ At(1'234'567, TraceKind::Mark, "presented") };
	const std::array sources{ TraceSource{ .Name = "frame", .Tid = 42, .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);

	bool found = false;

	for (const Field& packet : Packets(trace))
	{
		if (!Find(packet.Bytes, TrackEventField))
		{
			continue;
		}

		const std::optional<Field> stamp = Find(packet.Bytes, TimestampField);

		GYRO_REQUIRE(stamp.has_value());
		GYRO_CHECK_EQ(stamp->Value, std::uint64_t{ 1'234'567 });

		found = true;
	}

	GYRO_CHECK(found);
}

GYRO_TEST(Perfetto, AnEndWithNoBeginningIsDropped)
{
	// The oldest records in a ring that has lapped are the tail of whatever was running before the
	// window opened. A reader given those would draw a slice that started at the beginning of time.
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::End, nullptr),
		                                  At(2'000, TraceKind::End, nullptr),
		                                  At(3'000, TraceKind::Begin, "iteration"),
		                                  At(4'000, TraceKind::End, nullptr) };
	const std::array sources{ TraceSource{ .Name = "frame", .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);

	GYRO_CHECK_EQ(CountEvents(trace, SliceBegin), std::size_t{ 1 });
	GYRO_CHECK_EQ(CountEvents(trace, SliceEnd), std::size_t{ 1 });
}

GYRO_TEST(Perfetto, ABeginningWithNoEndIsClosedAtTheWindow)
{
	// The other end of the same window: the iteration the snapshot interrupted. It is real work that
	// was really running, so it is drawn reaching the right edge rather than discarded.
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Begin, "iteration"),
		                                  At(2'000, TraceKind::Begin, "evaluate"),
		                                  At(3'000, TraceKind::Mark, "acquired") };
	const std::array sources{ TraceSource{ .Name = "frame", .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);

	GYRO_CHECK_EQ(CountEvents(trace, SliceBegin), std::size_t{ 2 });
	GYRO_CHECK_EQ(CountEvents(trace, SliceEnd), std::size_t{ 2 });
	GYRO_CHECK_EQ(CountEvents(trace, InstantEvent), std::size_t{ 1 });
}

GYRO_TEST(Perfetto, AnOutputGetsARowOfItsOwn)
{
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Begin, "record", 0, TraceOutput(1)),
		                                  At(2'000, TraceKind::End, nullptr, 0, TraceOutput(1)),
		                                  At(3'000, TraceKind::Elapsed, "gpu", 4'000'000, TraceGpu(1)) };
	const std::array sources{ TraceSource{ .Name = "frame", .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);
	const std::vector<std::string> names = DescriptorNames(trace);

	GYRO_CHECK(Names(names, "output 1 frame"));
	GYRO_CHECK(Names(names, "output 1 gpu"));

	// The counter is a track of its own, parented to the row it is about.
	GYRO_CHECK(Names(names, "gpu"));
	GYRO_CHECK_EQ(CountEvents(trace, CounterEvent), std::size_t{ 1 });
}

GYRO_TEST(Perfetto, ACounterCarriesItsValue)
{
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Count, "held", 19) };
	const std::array sources{ TraceSource{ .Name = "dispatch", .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);

	bool found = false;

	for (const Field& packet : Packets(trace))
	{
		const std::optional<Field> event = Find(packet.Bytes, TrackEventField);

		if (!event)
		{
			continue;
		}

		const std::optional<Field> value = Find(event->Bytes, CounterValueField);

		GYRO_REQUIRE(value.has_value());
		GYRO_CHECK_EQ(value->Value, std::uint64_t{ 19 });

		found = true;
	}

	GYRO_CHECK(found);
}

GYRO_TEST(Perfetto, AFlowIdRidesOnTheSliceThatCarriesIt)
{
	// The arrow from the publish that produced a snapshot to the acquire that took it, which is the one
	// thing a two-thread compositor is asked about most and the one thing two unrelated rows cannot say.
	const std::vector<TraceEvent> published{ At(1'000, TraceKind::Begin, "publish", 77, TraceThread, true),
		                                     At(1'500, TraceKind::End, nullptr) };
	const std::vector<TraceEvent> acquired{ At(2'000, TraceKind::Begin, "acquire", 77, TraceThread, true),
		                                    At(2'500, TraceKind::End, nullptr) };

	const std::array sources{ TraceSource{ .Name = "dispatch", .Tid = 2, .Events = published },
		                      TraceSource{ .Name = "frame", .Tid = 3, .Events = acquired } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);

	std::size_t flows = 0;

	for (const Field& packet : Packets(trace))
	{
		const std::optional<Field> event = Find(packet.Bytes, TrackEventField);

		if (event && Find(event->Bytes, FlowField))
		{
			++flows;
		}
	}

	GYRO_CHECK_EQ(flows, std::size_t{ 2 });
}

GYRO_TEST(Perfetto, EveryEventNamesAnInternedString)
{
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Begin, "iteration"),
		                                  At(1'100, TraceKind::Mark, "skipped"),
		                                  At(1'200, TraceKind::Begin, "iteration"),
		                                  At(1'300, TraceKind::End, nullptr) };
	const std::array sources{ TraceSource{ .Name = "frame", .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);

	for (const Field& packet : Packets(trace))
	{
		const std::optional<Field> event = Find(packet.Bytes, TrackEventField);

		if (!event)
		{
			continue;
		}

		const std::optional<Field> kind = Find(event->Bytes, TypeField);

		GYRO_REQUIRE(kind.has_value());

		if (kind->Value == SliceEnd)
		{
			continue;
		}

		// A zero interning id is the writer having lost a name between the two passes, which would open
		// as a trace full of anonymous slices rather than as a failure.
		const std::optional<Field> name = Find(event->Bytes, NameIdField);

		GYRO_REQUIRE(name.has_value());
		GYRO_CHECK(name->Value != 0);
	}
}

GYRO_TEST(Perfetto, EveryEventLandsOnATrack)
{
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Begin, "iteration"),
		                                  At(1'100, TraceKind::Count, "held", 3),
		                                  At(1'200, TraceKind::End, nullptr) };
	const std::array sources{ TraceSource{ .Name = "frame", .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);

	for (const Field& packet : Packets(trace))
	{
		const std::optional<Field> event = Find(packet.Bytes, TrackEventField);

		if (!event)
		{
			continue;
		}

		const std::optional<Field> track = Find(event->Bytes, TrackUuidField);

		GYRO_REQUIRE(track.has_value());
		GYRO_CHECK(track->Value != 0);
	}
}

GYRO_TEST(Perfetto, NothingRecordedIsStillAValidTrace)
{
	const std::array sources{ TraceSource{ .Name = "frame", .Tid = 3, .Events = {} } };
	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);

	// The process and the thread, which is what a reader needs to say *gyro was here and did nothing*
	// rather than refusing the file.
	GYRO_CHECK(!trace.empty());
	GYRO_CHECK_EQ(CountEvents(trace, SliceBegin), std::size_t{ 0 });
}

// A labelled record spells its number into its name, which is what lets one frame be found on five
// rows by reading them rather than by following an arrow between each pair. The name is written out
// rather than interned because `frame 142` is used once and a table of four hundred single-use strings
// is a table nobody reads twice.
GYRO_TEST(Perfetto, ALabelIsSpelledIntoTheName)
{
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Begin, "frame", 142),
		                                  At(2'000, TraceKind::End, nullptr) };
	const std::array sources{ TraceSource{ .Name = "frame", .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);

	bool named = false;

	for (const Field& packet : Packets(trace))
	{
		const std::optional<Field> event = Find(packet.Bytes, TrackEventField);

		if (!event)
		{
			continue;
		}

		if (const std::optional<Field> name = Find(event->Bytes, InlineNameField))
		{
			named = named || std::string_view{ reinterpret_cast<const char*>(name->Bytes.data()),
				                               name->Bytes.size() } == "frame 142";
		}
	}

	GYRO_CHECK(named);
}

// A label draws no arrow unless it was asked to. Every flow this trace used to carry said only *these
// slices are the same frame* — eight of them per frame, three thousand in a seven-second capture — and
// the name above says it without drawing anything.
GYRO_TEST(Perfetto, ATagCarriesNoFlow)
{
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Begin, "frame", 142),
		                                  At(2'000, TraceKind::End, nullptr) };
	const std::array sources{ TraceSource{ .Name = "frame", .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);

	std::size_t flows = 0;

	for (const Field& packet : Packets(trace))
	{
		const std::optional<Field> event = Find(packet.Bytes, TrackEventField);

		if (event && Find(event->Bytes, FlowField))
		{
			++flows;
		}
	}

	GYRO_CHECK_EQ(flows, std::size_t{ 0 });
}

// One screen's rows are declared consecutively and in the order a frame happens, so that reading down
// a column is reading the life of a frame. Grouped the other way — every output's GPU row together —
// two monitors put four rows between a frame and its own pixels.
GYRO_TEST(Perfetto, AnOutputsRowsAreDeclaredInTheOrderAFrameHappens)
{
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Mark, "glass", 0, TraceGlass(0)),
		                                  At(1'100, TraceKind::Mark, "gpu", 0, TraceGpu(0)),
		                                  At(1'200, TraceKind::Mark, "grid", 0, TraceGrid(0)),
		                                  At(1'300, TraceKind::Mark, "frame", 0, TraceOutput(0)) };
	const std::array sources{ TraceSource{ .Name = "frame", .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);
	const std::vector<std::string> names = DescriptorNames(trace);

	std::vector<std::string> rows;

	for (const std::string& name : names)
	{
		if (name.starts_with("output "))
		{
			rows.push_back(name);
		}
	}

	// Declared in lane order rather than in the order a lane first spoke, which above is deliberately
	// backwards.
	GYRO_REQUIRE_EQ(rows.size(), std::size_t{ 4 });
	GYRO_CHECK_EQ(rows[0], std::string{ "output 0 refresh" });
	GYRO_CHECK_EQ(rows[1], std::string{ "output 0 frame" });
	GYRO_CHECK_EQ(rows[2], std::string{ "output 0 gpu" });
	GYRO_CHECK_EQ(rows[3], std::string{ "output 0 glass" });
}

namespace
{

// Every `debug_annotation` hanging off every track event, in the order the file has them, as the pairs
// a person reads in Perfetto's argument panel.
[[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> Annotations(std::span<const std::byte> trace)
{
	std::vector<std::pair<std::string, std::uint64_t>> arguments;

	for (const Field& packet : Packets(trace))
	{
		const std::optional<Field> event = Find(packet.Bytes, TrackEventField);

		if (!event)
		{
			continue;
		}

		ProtoReader reader{ event->Bytes };

		while (!reader.Done())
		{
			const std::optional<Field> field = reader.Next();

			if (!field)
			{
				break;
			}

			if (field->Number != AnnotationField)
			{
				continue;
			}

			const std::optional<Field> name = Find(field->Bytes, AnnotationNameField);
			const std::optional<Field> value = Find(field->Bytes, AnnotationValueField);

			if (!name || !value)
			{
				continue;
			}

			arguments.emplace_back(
				std::string{ reinterpret_cast<const char*>(name->Bytes.data()), name->Bytes.size() }, value->Value
			);
		}
	}

	return arguments;
}

} // namespace

// An attribute is a field of the slice it decorates rather than an event of its own, which is what the
// format requires: annotations are read off the `Begin` packet, so a record arriving afterwards has to
// be folded back into it.
GYRO_TEST(Perfetto, AnAttributeIsWrittenOntoTheSliceItFollows)
{
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Begin, "frame", 142, TraceGlass(0)),
		                                  At(1'000, TraceKind::Attribute, "scene", 6, TraceGlass(0)),
		                                  At(1'000, TraceKind::Attribute, "refresh", 99, TraceGlass(0)),
		                                  At(2'000, TraceKind::End, nullptr, 0, TraceGlass(0)) };
	const std::array sources{ TraceSource{ .Name = "frame", .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);

	// Two records that are not two events: the slice is one begin and one end, exactly as it would be
	// without them.
	GYRO_CHECK_EQ(CountEvents(trace, SliceBegin), std::size_t{ 1 });
	GYRO_CHECK_EQ(CountEvents(trace, SliceEnd), std::size_t{ 1 });

	const std::vector<std::pair<std::string, std::uint64_t>> arguments = Annotations(trace);

	GYRO_REQUIRE_EQ(arguments.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(arguments[0].first, std::string{ "scene" });
	GYRO_CHECK_EQ(arguments[0].second, std::uint64_t{ 6 });
	GYRO_CHECK_EQ(arguments[1].first, std::string{ "refresh" });
	GYRO_CHECK_EQ(arguments[1].second, std::uint64_t{ 99 });
}

// The ring holds a window, so its oldest records are the middle of whatever was running — an attribute
// whose slice was lapped has nothing to hang on. Attached to the first slice of the window instead, it
// would put one frame's scene onto a different frame, which is worse than saying nothing.
GYRO_TEST(Perfetto, AnAttributeWithNoSliceOpenOnItsRowIsDropped)
{
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Attribute, "scene", 6, TraceGlass(0)),
		                                  At(1'100, TraceKind::Begin, "frame", 142, TraceGlass(0)),
		                                  At(2'000, TraceKind::End, nullptr, 0, TraceGlass(0)) };
	const std::array sources{ TraceSource{ .Name = "frame", .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);

	GYRO_CHECK_EQ(CountEvents(trace, SliceBegin), std::size_t{ 1 });
	GYRO_CHECK(Annotations(trace).empty());
}

// It binds to the slice it follows and not to whichever one is open, which is the difference between
// an argument and a guess: the row below has a closed slice and an open one, and the attribute belongs
// to neither.
GYRO_TEST(Perfetto, AnAttributeBindsToTheSliceItImmediatelyFollows)
{
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Begin, "outer", 0, TraceGlass(0)),
		                                  At(1'100, TraceKind::Begin, "inner", 0, TraceGlass(0)),
		                                  At(1'100, TraceKind::Attribute, "scene", 6, TraceGlass(0)),
		                                  At(1'200, TraceKind::End, nullptr, 0, TraceGlass(0)),
		                                  At(1'300, TraceKind::Attribute, "scene", 7, TraceGlass(0)),
		                                  At(2'000, TraceKind::End, nullptr, 0, TraceGlass(0)) };
	const std::array sources{ TraceSource{ .Name = "frame", .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);
	const std::vector<std::pair<std::string, std::uint64_t>> arguments = Annotations(trace);

	// The inner slice takes the one that follows it; the one after the end takes nothing, rather than
	// reattaching to the outer slice that is still open.
	GYRO_REQUIRE_EQ(arguments.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(arguments[0].second, std::uint64_t{ 6 });
}

// A row whose name is not knowable until it exists — a client is `firefox`, a session is a user — is
// named through Core/Trace.h's table, and the descriptor is where that name has to arrive. The ring
// still carries only a row number, so this is the one place the two halves meet.
GYRO_TEST(Perfetto, ARuntimeNameReachesTheTrackDescriptor)
{
	const std::uint16_t client = ClaimTraceClient();

	GYRO_REQUIRE(client != TraceThread);

	NameTraceScope(client, "firefox");
	NameTraceScope(TraceSession(), "paul");

	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Mark, "attach", 0, client),
		                                  At(1'100, TraceKind::Mark, "locked", 0, TraceSession()) };
	const std::array sources{ TraceSource{ .Name = "dispatch", .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);
	const std::vector<std::string> names = DescriptorNames(trace);

	GYRO_CHECK(Names(names, "firefox"));
	GYRO_CHECK(Names(names, "paul"));

	ForgetTraceScope(TraceSession());
	ReleaseTraceClient(client);
}

// A row nobody named still has to come out as something. A track with no descriptor is precisely the
// defect Tools/TraceDump.cpp --check exists to report, and a client that connected and said nothing
// about itself is a real row with real slices on it.
GYRO_TEST(Perfetto, AnUnnamedClientRowStillGetsADescriptor)
{
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Mark, "attach", 0, TraceClient(3)),
		                                  At(1'100, TraceKind::Mark, "motion", 0, TraceInput()),
		                                  At(1'200, TraceKind::Mark, "started", 0, TraceLog()) };
	const std::array sources{ TraceSource{ .Name = "dispatch", .Events = events } };

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources);
	const std::vector<std::string> names = DescriptorNames(trace);

	GYRO_CHECK(Names(names, "client 3"));
	GYRO_CHECK(Names(names, "input"));
	GYRO_CHECK(Names(names, "log"));
}

namespace
{

// Every clock the snapshot packet relates, as the pairs a reader uses to place this file's timestamps
// against somebody else's.
[[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>> Clocks(std::span<const std::byte> trace)
{
	std::vector<std::pair<std::uint64_t, std::uint64_t>> readings;

	for (const Field& packet : Packets(trace))
	{
		const std::optional<Field> snapshot = Find(packet.Bytes, Perfetto::Packet::ClockSnapshot);

		if (!snapshot)
		{
			continue;
		}

		for (const Field& clock : ProtoFindAll(snapshot->Bytes, Perfetto::Snapshot::Clocks))
		{
			const std::optional<Field> id = Find(clock.Bytes, Perfetto::Snapshot::ClockId);
			const std::optional<Field> stamp = Find(clock.Bytes, Perfetto::Snapshot::Timestamp);

			if (id && stamp)
			{
				readings.emplace_back(id->Value, stamp->Value);
			}
		}
	}

	return readings;
}

} // namespace

// Monotonic against boot-time is what lets this file be concatenated with a system trace; monotonic
// against the wall clock is what lets a slice be lined up with a journal line, a screen recording, or
// a person saying it stuttered at about quarter past. Both relations are one packet at the front.
GYRO_TEST(Perfetto, TheClockSnapshotRelatesAllThreeDomains)
{
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Mark, "tick") };
	const std::array sources{ TraceSource{ .Name = "frame", .Events = events } };

	const TraceIdentity identity{ .Pid = 7,
		                          .Anchor = ClockAnchor{ .Monotonic = 41'203'000'000'000,
		                                                 .Boottime = 41'900'000'000'000,
		                                                 .Realtime = 1'757'000'000'000'000'000 } };

	const std::vector<std::pair<std::uint64_t, std::uint64_t>> readings = Clocks(EncodeTrace(identity, sources));

	GYRO_REQUIRE_EQ(readings.size(), std::size_t{ 3 });

	GYRO_CHECK_EQ(readings[0].first, Perfetto::ClockMonotonic);
	GYRO_CHECK_EQ(readings[0].second, std::uint64_t{ 41'203'000'000'000 });

	GYRO_CHECK_EQ(readings[1].first, Perfetto::ClockBoottime);
	GYRO_CHECK_EQ(readings[1].second, std::uint64_t{ 41'900'000'000'000 });

	GYRO_CHECK_EQ(readings[2].first, Perfetto::ClockRealtime);
	GYRO_CHECK_EQ(readings[2].second, std::uint64_t{ 1'757'000'000'000'000'000 });
}

namespace
{

// Every `process_labels` on the process descriptor, which is the half of the identity a person reads
// without clicking anything.
[[nodiscard]] std::vector<std::string> Labels(std::span<const std::byte> trace)
{
	std::vector<std::string> labels;

	for (const Field& packet : Packets(trace))
	{
		const std::optional<Field> descriptor = Find(packet.Bytes, TrackDescriptorField);

		if (!descriptor)
		{
			continue;
		}

		const std::optional<Field> process = Find(descriptor->Bytes, Perfetto::Descriptor::Process);

		if (!process)
		{
			continue;
		}

		for (const Field& label : ProtoFindAll(process->Bytes, Perfetto::ProcessDescriptor::Labels))
		{
			labels.emplace_back(reinterpret_cast<const char*>(label.Bytes.data()), label.Bytes.size());
		}
	}

	return labels;
}

// Every instant in the file, as its name and the string annotations hanging off it — which is the
// other half, and the one a tool reads a key at a time.
[[nodiscard]] std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>
Instants(std::span<const std::byte> trace)
{
	std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> found;

	for (const Field& packet : Packets(trace))
	{
		const std::optional<Field> event = Find(packet.Bytes, TrackEventField);

		if (!event)
		{
			continue;
		}

		const std::optional<Field> kind = Find(event->Bytes, TypeField);
		const std::optional<Field> name = Find(event->Bytes, InlineNameField);

		if (!kind || kind->Value != InstantEvent || !name)
		{
			continue;
		}

		std::vector<std::pair<std::string, std::string>> notes;

		for (const Field& annotation : ProtoFindAll(event->Bytes, AnnotationField))
		{
			const std::optional<Field> key = Find(annotation.Bytes, AnnotationNameField);
			const std::optional<Field> value = Find(annotation.Bytes, Perfetto::Annotation::StringValue);

			if (key && value)
			{
				notes.emplace_back(
					std::string{ reinterpret_cast<const char*>(key->Bytes.data()), key->Bytes.size() },
					std::string{ reinterpret_cast<const char*>(value->Bytes.data()), value->Bytes.size() }
				);
			}
		}

		found.emplace_back(
			std::string{ reinterpret_cast<const char*>(name->Bytes.data()), name->Bytes.size() }, std::move(notes)
		);
	}

	return found;
}

} // namespace

// A capture three weeks old and off somebody else's machine has to say what produced it, and none of
// it is in a slice: the build, the kernel, the backend and whether the frame thread actually got
// real-time priority are facts about the run rather than about anything that happened during it.
GYRO_TEST(Perfetto, TheIdentitySaysWhatProducedTheTrace)
{
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Mark, "tick") };
	const std::array sources{ TraceSource{ .Name = "frame", .Events = events } };

	const std::array<std::string_view, 2> invocation{ "gyro", "--backend=headless" };
	const std::array<TraceFact, 2> facts{ TraceFact{ .Name = "backend", .Value = "headless" },
		                                  TraceFact{ .Name = "clocksource", .Value = "tsc" } };

	const TraceIdentity identity{ .Pid = 7,
		                          .Anchor = ClockAnchor{ .Monotonic = 500, .Boottime = 900, .Realtime = 1'000 },
		                          .CommandLine = invocation,
		                          .Machine = TraceMachine{ .Sysname = "Linux",
		                                                   .Release = "7.1.9-200.fc44.x86_64",
		                                                   .Version = "#1 SMP",
		                                                   .Machine = "x86_64" },
		                          .Facts = facts };

	const std::vector<std::byte> trace = EncodeTrace(identity, sources);

	// The invocation, one field per argument.
	std::vector<std::string> arguments;

	for (const Field& packet : Packets(trace))
	{
		const std::optional<Field> descriptor = Find(packet.Bytes, TrackDescriptorField);
		const std::optional<Field> process =
			descriptor ? Find(descriptor->Bytes, Perfetto::Descriptor::Process) : std::nullopt;

		if (!process)
		{
			continue;
		}

		for (const Field& argument : ProtoFindAll(process->Bytes, Perfetto::ProcessDescriptor::Cmdline))
		{
			arguments.emplace_back(reinterpret_cast<const char*>(argument.Bytes.data()), argument.Bytes.size());
		}
	}

	GYRO_REQUIRE_EQ(arguments.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(arguments[1], std::string{ "--backend=headless" });

	const std::vector<std::string> labels = Labels(trace);

	GYRO_REQUIRE_EQ(labels.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(labels[0], std::string{ "backend=headless" });
	GYRO_CHECK_EQ(labels[1], std::string{ "clocksource=tsc" });

	// The kernel, in the message Perfetto already knows by that name.
	std::string release;

	for (const Field& packet : Packets(trace))
	{
		const std::optional<Field> system = Find(packet.Bytes, Perfetto::Packet::SystemInfo);
		const std::optional<Field> utsname = system ? Find(system->Bytes, Perfetto::SystemInfo::Utsname) : std::nullopt;

		if (const std::optional<Field> found =
		        utsname ? Find(utsname->Bytes, Perfetto::Utsname::Release) : std::nullopt;
		    found)
		{
			release.assign(reinterpret_cast<const char*>(found->Bytes.data()), found->Bytes.size());
		}
	}

	GYRO_CHECK_EQ(release, std::string{ "7.1.9-200.fc44.x86_64" });

	// And the same facts again as annotations, on one instant rather than one each.
	// One instant, not one per fact: `tick` above is interned rather than named inline, so what this
	// finds is the identity and nothing else.
	const auto instants = Instants(trace);

	GYRO_REQUIRE_EQ(instants.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(instants[0].first, std::string{ "run" });
	GYRO_REQUIRE_EQ(instants[0].second.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(instants[0].second[1].first, std::string{ "clocksource" });
	GYRO_CHECK_EQ(instants[0].second[1].second, std::string{ "tsc" });
}

// A log line and the frame that was on the screen when it was written are the same moment described
// twice, and until now they were in two files with no shared clock between them.
GYRO_TEST(Perfetto, ALogLineIsAnInstantOnTheLogRow)
{
	const std::vector<TraceEvent> events{ At(1'000, TraceKind::Mark, "tick") };
	const std::array sources{ TraceSource{ .Name = "frame", .Events = events } };

	const std::array<TraceLine, 2> logs{
		TraceLine{
			.Stamp = Monotonic::FromNanoseconds(1'200), .Level = "info", .Message = "hosting clients on wayland-1" },
		TraceLine{ .Stamp = Monotonic::FromNanoseconds(1'800), .Level = "warn", .Message = "pages are not locked" }
	};

	const std::vector<std::byte> trace = EncodeTrace(TraceIdentity{ .Pid = 7 }, sources, logs);

	// The row exists and is named, which is what stops `Tools/TraceDump.cpp --check` reporting a track
	// nothing described.
	GYRO_CHECK(Names(DescriptorNames(trace), "log"));

	const auto instants = Instants(trace);

	GYRO_REQUIRE_EQ(instants.size(), std::size_t{ 2 });

	// The message is the name, written out in full: interning a table of log lines would be a table
	// with one entry per row.
	GYRO_CHECK_EQ(instants[0].first, std::string{ "hosting clients on wayland-1" });
	GYRO_REQUIRE_EQ(instants[0].second.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(instants[0].second[0].first, std::string{ "level" });
	GYRO_CHECK_EQ(instants[0].second[0].second, std::string{ "info" });

	GYRO_CHECK_EQ(instants[1].first, std::string{ "pages are not locked" });
	GYRO_CHECK_EQ(instants[1].second[0].second, std::string{ "warn" });
}
