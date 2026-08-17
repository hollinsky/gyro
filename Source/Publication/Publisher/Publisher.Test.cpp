#include "Publication/Publisher/Publisher.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "Core/Time.h"
#include "Core/Wake.h"
#include "Publication/Reader/Reader.h"
#include "Publication/Snapshot.h"
#include "Testing/Test.h"

// The round trip, on the dispatch side because only it may name both halves. What the publisher lays
// down the reader must read back, byte for byte, across heterogeneous runs at their own alignments —
// which is the whole claim of an offset-addressed snapshot. The reader's behaviour on bytes it did
// not produce is proven beside the reader; this is the agreement between the two on bytes that were.

namespace
{
// Two unrelated record shapes, to prove the directory carries runs of different strides and
// alignments at once. Neither is a spring: Publication does not depend on Animation, and the format
// is agnostic to what a run holds beyond its size and alignment.
struct Wide
{
	Instant Origin;
	double A;
	double B;

	friend bool operator==(const Wide&, const Wide&) = default;
};

struct Narrow
{
	float X;
	float Y;
	float Z;

	friend bool operator==(const Narrow&, const Narrow&) = default;
};
} // namespace

GYRO_TEST(SnapshotPublisher, EmptyPublishIsAValidEmptySnapshot)
{
	const SnapshotBuffer buffer = SnapshotPublisher{}.Build();
	const SnapshotReader reader{ buffer.Bytes() };

	GYRO_CHECK(reader.IsValid());
	GYRO_CHECK_EQ(buffer.Size(), sizeof(SnapshotHeader));
	GYRO_CHECK(reader.Run<Wide>(SnapshotRun::Positions).empty());
	GYRO_CHECK(reader.Run<Narrow>(SnapshotRun::Channels).empty());
	GYRO_CHECK(reader.Wakes().empty());
}

GYRO_TEST(SnapshotPublisher, TheSequenceCrosses)
{
	const SnapshotBuffer buffer = SnapshotPublisher{}.Sequence(1234).Build();

	GYRO_CHECK_EQ(SnapshotReader{ buffer.Bytes() }.Sequence(), std::uint64_t{ 1234 });
}

GYRO_TEST(SnapshotPublisher, HeterogeneousRunsRoundTrip)
{
	const std::array<Wide, 2> positions{ Wide{ Monotonic::FromNanoseconds(0), 1.0, 2.0 },
		                                 Wide{ Monotonic::FromNanoseconds(1'000), 3.0, 4.0 } };
	const std::array<Narrow, 3> channels{ Narrow{ 0.1f, 0.2f, 0.3f },
		                                  Narrow{ 0.4f, 0.5f, 0.6f },
		                                  Narrow{ 0.7f, 0.8f, 0.9f } };
	const std::array<Wake, 2> wakes{ Wake::EveryFrame(Monotonic::FromNanoseconds(500)),
		                             Wake::At(Monotonic::FromNanoseconds(700)) };

	const SnapshotBuffer buffer = SnapshotPublisher{}
	                                  .Sequence(9)
	                                  .Put<Wide>(SnapshotRun::Positions, positions)
	                                  .Put<Narrow>(SnapshotRun::Channels, channels)
	                                  .PutWakes(wakes)
	                                  .Build();

	const SnapshotReader reader{ buffer.Bytes() };
	GYRO_REQUIRE(reader.IsValid());
	GYRO_CHECK_EQ(reader.Sequence(), std::uint64_t{ 9 });

	const std::span<const Wide> readPositions = reader.Run<Wide>(SnapshotRun::Positions);
	GYRO_REQUIRE_EQ(readPositions.size(), std::size_t{ 2 });
	GYRO_CHECK(readPositions[0] == positions[0]);
	GYRO_CHECK(readPositions[1] == positions[1]);

	const std::span<const Narrow> readChannels = reader.Run<Narrow>(SnapshotRun::Channels);
	GYRO_REQUIRE_EQ(readChannels.size(), std::size_t{ 3 });
	GYRO_CHECK(readChannels[0] == channels[0]);
	GYRO_CHECK(readChannels[2] == channels[2]);

	const std::span<const Wake> readWakes = reader.Wakes();
	GYRO_REQUIRE_EQ(readWakes.size(), std::size_t{ 2 });
	GYRO_CHECK(readWakes[0] == wakes[0]);
	GYRO_CHECK(readWakes[1] == wakes[1]);

	// The driven-progress slot was never written, so it is an absent run rather than a present empty
	// one — the pinned shape reserves it, and this cut leaves it unfilled.
	GYRO_CHECK(reader.Run<Narrow>(SnapshotRun::DrivenProgress).empty());
}

GYRO_TEST(SnapshotPublisher, EachRunLandsAtAnAlignedAddress)
{
	const std::array<Wide, 1> positions{ Wide{ Monotonic::FromNanoseconds(0), 1.0, 2.0 } };
	const std::array<Narrow, 1> channels{ Narrow{ 1.0f, 2.0f, 3.0f } };

	const SnapshotBuffer buffer = SnapshotPublisher{}
	                                  .Put<Wide>(SnapshotRun::Positions, positions)
	                                  .Put<Narrow>(SnapshotRun::Channels, channels)
	                                  .Build();

	const SnapshotReader reader{ buffer.Bytes() };
	GYRO_REQUIRE(reader.IsValid());

	const std::span<const Wide> wide = reader.Run<Wide>(SnapshotRun::Positions);
	const std::span<const Narrow> narrow = reader.Run<Narrow>(SnapshotRun::Channels);
	GYRO_REQUIRE_EQ(wide.size(), std::size_t{ 1 });
	GYRO_REQUIRE_EQ(narrow.size(), std::size_t{ 1 });

	GYRO_CHECK(reinterpret_cast<std::uintptr_t>(wide.data()) % alignof(Wide) == 0);
	GYRO_CHECK(reinterpret_cast<std::uintptr_t>(narrow.data()) % alignof(Narrow) == 0);
}
