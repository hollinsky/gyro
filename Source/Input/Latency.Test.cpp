#include "Input/Latency.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "Core/Clock.h"
#include "Core/Trace.h"
#include "Testing/Test.h"

// The left edge of input latency, and the one thing that must never be on it.
//
// Three properties are worth a test rather than a reading: the arrival mark lands where the *device*
// said the event happened rather than where the clock is, a burst is one mark and a count, and no
// name anywhere here can vary with what was typed.

namespace
{
using namespace Input;

// A ring and the storage it holds a span over, as Core/Trace.Test.cpp shapes it. Enrolls this thread
// on construction and lets it go on destruction, so a test that fails an assertion still leaves the
// next one running against its own buffer.
class Recording
{
public:
	explicit Recording(const IClock& clock, std::size_t capacity = 64) : m_Records{ new TraceRecord[capacity] }
	{
		m_Buffer.Arm({ m_Records.get(), capacity }, clock);

		EnrollTracing(&m_Buffer);
	}

	~Recording() { EnrollTracing(nullptr); }

	Recording(const Recording&) = delete;
	Recording& operator=(const Recording&) = delete;

	[[nodiscard]] std::vector<TraceEvent> Events() const
	{
		std::array<TraceEvent, 256> into{};
		const std::size_t count = m_Buffer.Copy(into);

		return { into.begin(), into.begin() + static_cast<std::ptrdiff_t>(count) };
	}

private:
	std::unique_ptr<TraceRecord[]> m_Records;
	TraceBuffer m_Buffer;
};

// A stamp far enough in the past that a mark landing on the clock's now cannot be mistaken for one
// landing on the device's.
constexpr Instant Now = Monotonic::FromMicroseconds(5'000'000);
constexpr Instant Typed = Monotonic::FromMicroseconds(4'990'000);

[[nodiscard]] std::string_view NameOf(const TraceEvent& event)
{
	return event.Name != nullptr ? std::string_view{ event.Name } : std::string_view{};
}
} // namespace

GYRO_TEST(InputLatency, TheArrivalIsStampedByTheDeviceAndTheDrainByTheClock)
{
	const ManualClock clock{ Now };
	const Recording recording{ clock };

	InputLatency latency;
	latency.Saw(InputKey, Typed);
	latency.Drained();

	const std::vector<TraceEvent> events = recording.Events();

	GYRO_REQUIRE_EQ(events.size(), std::size_t{ 2 });

	// The whole reason the row exists: ten milliseconds of kernel, libinput and a dispatch thread that
	// had not got to the descriptor yet, which nothing in the trace could see before.
	GYRO_CHECK_EQ(NameOf(events[0]), std::string_view{ "key" });
	GYRO_CHECK_EQ(events[0].Stamp, Typed);

	GYRO_CHECK_EQ(NameOf(events[1]), std::string_view{ "key drained" });
	GYRO_CHECK_EQ(events[1].Stamp, Now);

	// Both on the input row, so end-to-end latency is read by scrolling down one column.
	GYRO_CHECK_EQ(events[0].Scope, TraceInput());
	GYRO_CHECK_EQ(events[1].Scope, TraceInput());
}

GYRO_TEST(InputLatency, APressAndAReleaseAreDifferentMarks)
{
	const ManualClock clock{ Now };
	const Recording recording{ clock };

	InputLatency latency;
	latency.Saw(InputKey, Typed);
	latency.Saw(InputKeyUp, Typed);
	latency.Drained();

	const std::vector<TraceEvent> events = recording.Events();

	GYRO_REQUIRE_EQ(events.size(), std::size_t{ 4 });

	// A key that repeats on its own is a release that never arrived, and the pair beside each other is
	// the whole diagnosis. One literal for both would hide it.
	GYRO_CHECK_EQ(NameOf(events[0]), std::string_view{ "key" });
	GYRO_CHECK_EQ(NameOf(events[2]), std::string_view{ "key up" });
	GYRO_CHECK(NameOf(events[0]) != NameOf(events[2]));
}

GYRO_TEST(InputLatency, NoNameVariesWithWhatWasTyped)
{
	const ManualClock clock{ Now };
	const Recording recording{ clock };

	InputLatency latency;

	// Every key on a keyboard, pressed and released. A trace that leaked a keycode would have to spell
	// it into a name, and the names here are two.
	for (std::uint32_t code = 0; code < 256; ++code)
	{
		latency.Saw(code % 2 == 0 ? InputKey : InputKeyUp, Typed);
	}

	latency.Drained();

	for (const TraceEvent& event : recording.Events())
	{
		const std::string_view name = NameOf(event);

		GYRO_CHECK(name == "key" || name == "key drained" || name == "key up" || name == "key up drained");

		// And nothing rides in the payload either: a label is printed into the name by the writer, so a
		// keycode there would be a keycode in the picture.
		GYRO_CHECK_EQ(event.Payload, std::uint64_t{ 0 });
	}
}

GYRO_TEST(InputLatency, ABurstOfMotionIsOneMarkAndACount)
{
	const ManualClock clock{ Now };
	const Recording recording{ clock };

	InputLatency latency;

	// A thousand-hertz mouse against a dispatch thread that fell eight events behind. Marked
	// individually this is the highest-rate thing in the system writing into a ring that has to hold
	// the frame it caused.
	for (int step = 0; step < 8; ++step)
	{
		latency.Saw(InputMotion, Advanced(Typed, std::chrono::milliseconds{ step }));
	}

	// Nothing is on the row until the queue is empty, because a folded kind's drain edge is the end of
	// the drain — which is also the only edge anything downstream can act on.
	GYRO_CHECK(recording.Events().empty());

	latency.Drained();

	const std::vector<TraceEvent> events = recording.Events();

	GYRO_REQUIRE_EQ(events.size(), std::size_t{ 2 });

	// The oldest stamp, because the batch's latency is the latency of the event that waited longest.
	GYRO_CHECK_EQ(NameOf(events[0]), std::string_view{ "motion" });
	GYRO_CHECK_EQ(events[0].Stamp, Typed);
	GYRO_CHECK_EQ(events[0].Payload, std::uint64_t{ 8 });

	GYRO_CHECK_EQ(NameOf(events[1]), std::string_view{ "motion drained" });
	GYRO_CHECK_EQ(events[1].Stamp, Now);
	GYRO_CHECK_EQ(events[1].Payload, std::uint64_t{ 8 });

	// A count is not a flow id: nothing should be drawing a line from one drain's motion to another's.
	GYRO_CHECK(!events[0].Arrow);
}

GYRO_TEST(InputLatency, ContinuousKindsFoldApartFromEachOther)
{
	const ManualClock clock{ Now };
	const Recording recording{ clock };

	InputLatency latency;
	latency.Saw(InputMotion, Typed);
	latency.Saw(InputTouchMotion, Advanced(Typed, std::chrono::milliseconds{ 1 }));
	latency.Saw(InputMotion, Advanced(Typed, std::chrono::milliseconds{ 2 }));
	latency.Drained();

	const std::vector<TraceEvent> events = recording.Events();

	GYRO_REQUIRE_EQ(events.size(), std::size_t{ 4 });

	// A hand on the touchpad and a finger on the screen are two latencies, and averaging them into one
	// mark would say nothing about either.
	GYRO_CHECK_EQ(NameOf(events[0]), std::string_view{ "motion" });
	GYRO_CHECK_EQ(events[0].Payload, std::uint64_t{ 2 });
	GYRO_CHECK_EQ(NameOf(events[2]), std::string_view{ "touch motion" });
	GYRO_CHECK_EQ(events[2].Payload, std::uint64_t{ 1 });
}

GYRO_TEST(InputLatency, ADrainThatSawNothingWritesNothing)
{
	const ManualClock clock{ Now };
	const Recording recording{ clock };

	InputLatency latency;

	// The common case on a compositor that wakes for a presentation event: the fold costs a walk of
	// three empty slots and leaves the ring alone.
	latency.Drained();
	latency.Drained();

	GYRO_CHECK(recording.Events().empty());
}
