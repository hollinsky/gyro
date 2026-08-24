#include "Trace/Perfetto.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Time.h"
#include "Core/Trace.h"
#include "Testing/Test.h"

namespace
{

// Just enough protobuf to read back what Trace/Protobuf.h wrote, because the alternative is asserting
// on a hex string that says nothing about which field moved when it changes. It is a walker rather
// than a decoder: it knows wire types and nothing about the schema, and every question below is
// phrased as *find this field number inside that message*.
struct Field
{
	std::uint32_t Number = 0;
	std::uint32_t Wire = 0;
	std::uint64_t Value = 0;
	std::span<const std::byte> Bytes;
};

class Reader
{
public:
	explicit Reader(std::span<const std::byte> bytes) noexcept : m_Bytes{ bytes } {}

	[[nodiscard]] bool Done() const noexcept { return m_At >= m_Bytes.size(); }

	[[nodiscard]] std::optional<Field> Next() noexcept
	{
		const std::optional<std::uint64_t> tag = Varint();

		if (!tag)
		{
			return std::nullopt;
		}

		Field field{ .Number = static_cast<std::uint32_t>(*tag >> 3),
			         .Wire = static_cast<std::uint32_t>(*tag & 7U),
			         .Value = 0,
			         .Bytes = {} };

		if (field.Wire == 0)
		{
			const std::optional<std::uint64_t> value = Varint();

			if (!value)
			{
				return std::nullopt;
			}

			field.Value = *value;

			return field;
		}

		if (field.Wire == 1)
		{
			if (m_At + 8 > m_Bytes.size())
			{
				return std::nullopt;
			}

			for (int shift = 0; shift < 64; shift += 8)
			{
				field.Value |= static_cast<std::uint64_t>(m_Bytes[m_At++]) << shift;
			}

			return field;
		}

		if (field.Wire != 2)
		{
			return std::nullopt;
		}

		const std::optional<std::uint64_t> length = Varint();

		if (!length || m_At + *length > m_Bytes.size())
		{
			return std::nullopt;
		}

		field.Bytes = m_Bytes.subspan(m_At, static_cast<std::size_t>(*length));
		m_At += static_cast<std::size_t>(*length);

		return field;
	}

private:
	[[nodiscard]] std::optional<std::uint64_t> Varint() noexcept
	{
		std::uint64_t value = 0;

		for (int shift = 0; shift < 64; shift += 7)
		{
			if (m_At >= m_Bytes.size())
			{
				return std::nullopt;
			}

			const std::uint64_t byte = static_cast<std::uint64_t>(m_Bytes[m_At++]);

			value |= (byte & 0x7FU) << shift;

			if ((byte & 0x80U) == 0)
			{
				return value;
			}
		}

		return std::nullopt;
	}

	std::span<const std::byte> m_Bytes;
	std::size_t m_At = 0;
};

[[nodiscard]] std::optional<Field> Find(std::span<const std::byte> message, std::uint32_t number)
{
	Reader reader{ message };

	while (!reader.Done())
	{
		const std::optional<Field> field = reader.Next();

		if (!field)
		{
			return std::nullopt;
		}

		if (field->Number == number)
		{
			return field;
		}
	}

	return std::nullopt;
}

// Every top-level `Trace.packet`, which is the only thing a trace file contains.
[[nodiscard]] std::vector<Field> Packets(std::span<const std::byte> trace)
{
	std::vector<Field> packets;
	Reader reader{ trace };

	while (!reader.Done())
	{
		const std::optional<Field> field = reader.Next();

		if (!field)
		{
			break;
		}

		if (field->Number == 1)
		{
			packets.push_back(*field);
		}
	}

	return packets;
}

constexpr std::uint32_t TimestampField = 8;
constexpr std::uint32_t TrackEventField = 11;
constexpr std::uint32_t TrackDescriptorField = 60;
constexpr std::uint32_t TypeField = 9;
constexpr std::uint32_t TrackUuidField = 11;
constexpr std::uint32_t NameIdField = 10;
constexpr std::uint32_t FlowField = 47;
constexpr std::uint32_t CounterValueField = 30;
constexpr std::uint32_t DescriptorNameField = 2;

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
   std::uint16_t scope = TraceThread)
{
	return TraceEvent{
		.Stamp = Monotonic::FromNanoseconds(nanoseconds), .Name = name, .Payload = payload, .Scope = scope, .Kind = kind
	};
}

constexpr std::uint64_t SliceBegin = 1;
constexpr std::uint64_t SliceEnd = 2;
constexpr std::uint64_t InstantEvent = 3;
constexpr std::uint64_t CounterEvent = 4;

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

	GYRO_CHECK(Names(names, "output 1"));
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
	const std::vector<TraceEvent> published{ At(1'000, TraceKind::Begin, "publish", 77),
		                                     At(1'500, TraceKind::End, nullptr) };
	const std::vector<TraceEvent> acquired{ At(2'000, TraceKind::Begin, "acquire", 77),
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
