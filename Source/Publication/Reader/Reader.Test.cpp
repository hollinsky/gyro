#include "Publication/Reader/Reader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "Core/Time.h"
#include "Core/Wake.h"
#include "Publication/Snapshot.h"
#include "Testing/Test.h"

// The reader is tested against buffers built by hand rather than by the publisher, and the separation
// is deliberate: the publisher lives in the dispatch half, which the frame half may not include, and
// the reader is worth exercising on bytes it did not produce anyway — a truncated mapping, a stale
// version, a run entry that points past the end. What it must never do with any of those is read past
// the span or fault; what it must do is resolve to nothing. The publisher-to-reader round trip over
// well-formed bytes is proven beside the publisher.

namespace
{
// A stand-in coefficient record: an Instant origin and a couple of numbers, shaped like the springs
// that really cross but owing nothing to Animation, which this module does not depend on.
struct Coeff
{
	Instant Origin;
	double Value;
	float Rate;

	friend bool operator==(const Coeff&, const Coeff&) = default;
};

// A stand-in node record. Publication does not depend on World and Nodes<T>() is a template so that
// it need not — decision 91 — so what the reader is exercised against here is a record of the right
// shape and the wrong provenance, which is the case the boundary's type check exists for.
struct Skeleton
{
	std::uint32_t SubtreeLength;
	std::uint32_t Translation;

	friend bool operator==(const Skeleton&, const Skeleton&) = default;
};

// Storage aligned as the reader demands of any snapshot it adopts. std::array over-aligned this way
// gives a base the resolver will accept, which a bare array would not guarantee.
template<std::size_t N>
struct alignas(std::max_align_t) Bytes
{
	std::array<std::byte, N> Data{};

	[[nodiscard]] std::span<const std::byte> View() const noexcept { return { Data.data(), Data.size() }; }
	[[nodiscard]] std::span<const std::byte> View(std::size_t size) const noexcept { return { Data.data(), size }; }
};
} // namespace

GYRO_TEST(SnapshotReader, DefaultIsValidlyEmpty)
{
	const SnapshotReader reader;

	GYRO_CHECK(!reader.IsValid());
	GYRO_CHECK(reader.Sequence() == 0);
	GYRO_CHECK(reader.Wakes().empty());
	GYRO_CHECK(reader.Run<Coeff>(SnapshotRun::Translation).empty());
}

GYRO_TEST(SnapshotReader, TooSmallForAHeaderIsRejected)
{
	Bytes<sizeof(SnapshotHeader)> buffer;

	const SnapshotReader reader{ buffer.View(sizeof(SnapshotHeader) - 1) };

	GYRO_CHECK(!reader.IsValid());
}

GYRO_TEST(SnapshotReader, HeaderOnlyIsValidAndEmpty)
{
	Bytes<sizeof(SnapshotHeader)> buffer;

	SnapshotHeader header{};
	header.Sequence = 42;
	header.ByteSize = sizeof(SnapshotHeader);
	std::memcpy(buffer.Data.data(), &header, sizeof(SnapshotHeader));

	const SnapshotReader reader{ buffer.View() };

	GYRO_CHECK(reader.IsValid());
	GYRO_CHECK_EQ(reader.Sequence(), std::uint64_t{ 42 });
	GYRO_CHECK(reader.Run<Coeff>(SnapshotRun::Translation).empty());
	GYRO_CHECK(reader.Wakes().empty());
}

GYRO_TEST(SnapshotReader, WrongMagicIsRejected)
{
	Bytes<sizeof(SnapshotHeader)> buffer;

	SnapshotHeader header{};
	header.Magic = 0xDEAD'BEEFu;
	header.ByteSize = sizeof(SnapshotHeader);
	std::memcpy(buffer.Data.data(), &header, sizeof(SnapshotHeader));

	GYRO_CHECK(!SnapshotReader{ buffer.View() }.IsValid());
}

GYRO_TEST(SnapshotReader, WrongVersionIsRejected)
{
	Bytes<sizeof(SnapshotHeader)> buffer;

	SnapshotHeader header{};
	header.Version = SnapshotVersion + 1;
	header.ByteSize = sizeof(SnapshotHeader);
	std::memcpy(buffer.Data.data(), &header, sizeof(SnapshotHeader));

	GYRO_CHECK(!SnapshotReader{ buffer.View() }.IsValid());
}

GYRO_TEST(SnapshotReader, SizeThatDisagreesWithTheSpanIsRejected)
{
	Bytes<sizeof(SnapshotHeader)> buffer;

	SnapshotHeader header{};
	header.ByteSize = sizeof(SnapshotHeader) + 8; // claims more than the span holds
	std::memcpy(buffer.Data.data(), &header, sizeof(SnapshotHeader));

	GYRO_CHECK(!SnapshotReader{ buffer.View() }.IsValid());
}

GYRO_TEST(SnapshotReader, ResolvesATypedRunAndGuardsItsType)
{
	constexpr std::uint32_t offset = sizeof(SnapshotHeader);
	Bytes<offset + sizeof(Coeff)> buffer;

	const Coeff written{ Monotonic::FromNanoseconds(5'000), 1.5, 0.25f };
	std::memcpy(buffer.Data.data() + offset, &written, sizeof(Coeff));

	SnapshotHeader header{};
	header.ByteSize = static_cast<std::uint32_t>(buffer.Data.size());
	header.Runs[RunIndex(SnapshotRun::Translation)] = { offset, 1, sizeof(Coeff), alignof(Coeff) };
	std::memcpy(buffer.Data.data(), &header, sizeof(SnapshotHeader));

	const SnapshotReader reader{ buffer.View() };
	GYRO_REQUIRE(reader.IsValid());

	const std::span<const Coeff> run = reader.Run<Coeff>(SnapshotRun::Translation);
	GYRO_REQUIRE_EQ(run.size(), std::size_t{ 1 });
	GYRO_CHECK(run[0] == written);

	// The same bytes asked for as the wrong-sized type resolve to nothing rather than to a
	// reinterpretation: the boundary's type check is the writer's size and alignment against the
	// reader's.
	GYRO_CHECK(reader.Run<double>(SnapshotRun::Translation).empty());
}

GYRO_TEST(SnapshotReader, ARunReachingPastTheEndResolvesEmpty)
{
	Bytes<sizeof(SnapshotHeader)> buffer;

	SnapshotHeader header{};
	header.ByteSize = sizeof(SnapshotHeader);
	// A run whose bytes would begin at the very end of the snapshot and extend beyond it. The header
	// is well-formed, so the reader is valid; only the run is refused.
	header.Runs[RunIndex(SnapshotRun::Opacity)] = { sizeof(SnapshotHeader), 1, sizeof(Coeff), alignof(Coeff) };
	std::memcpy(buffer.Data.data(), &header, sizeof(SnapshotHeader));

	const SnapshotReader reader{ buffer.View() };

	GYRO_CHECK(reader.IsValid());
	GYRO_CHECK(reader.Run<Coeff>(SnapshotRun::Opacity).empty());
}

GYRO_TEST(SnapshotReader, WakesRoundTripInOutputOrder)
{
	constexpr std::uint32_t offset = sizeof(SnapshotHeader);
	const std::array<Wake, 3> schedule{ Wake::Never(),
		                                Wake::At(Monotonic::FromNanoseconds(900)),
		                                Wake::EveryFrame(Monotonic::FromNanoseconds(500)) };

	Bytes<offset + 3 * sizeof(Wake)> buffer;
	std::memcpy(buffer.Data.data() + offset, schedule.data(), schedule.size() * sizeof(Wake));

	SnapshotHeader header{};
	header.ByteSize = static_cast<std::uint32_t>(buffer.Data.size());
	header.Wakes = { offset, 3, sizeof(Wake), alignof(Wake) };
	std::memcpy(buffer.Data.data(), &header, sizeof(SnapshotHeader));

	const SnapshotReader reader{ buffer.View() };
	GYRO_REQUIRE(reader.IsValid());

	const std::span<const Wake> wakes = reader.Wakes();
	GYRO_REQUIRE_EQ(wakes.size(), std::size_t{ 3 });
	GYRO_CHECK(wakes[0] == schedule[0]);
	GYRO_CHECK(wakes[1] == schedule[1]);
	GYRO_CHECK(wakes[2] == schedule[2]);
}

GYRO_TEST(SnapshotReader, NodesResolveAsATreeOrAsNothing)
{
	constexpr std::uint32_t offset = sizeof(SnapshotHeader);
	const std::array<Skeleton, 2> scene{ Skeleton{ 1, 0xFFFF'FFFFu }, Skeleton{ 0, 7 } };

	Bytes<offset + 2 * sizeof(Skeleton)> buffer;
	std::memcpy(buffer.Data.data() + offset, scene.data(), scene.size() * sizeof(Skeleton));

	SnapshotHeader header{};
	header.ByteSize = static_cast<std::uint32_t>(buffer.Data.size());
	header.Nodes = { offset, 2, sizeof(Skeleton), alignof(Skeleton) };
	std::memcpy(buffer.Data.data(), &header, sizeof(SnapshotHeader));

	const SnapshotReader reader{ buffer.View() };
	GYRO_REQUIRE(reader.IsValid());

	const std::span<const Skeleton> nodes = reader.Nodes<Skeleton>();
	GYRO_REQUIRE_EQ(nodes.size(), std::size_t{ 2 });
	GYRO_CHECK(nodes[0] == scene[0]);
	GYRO_CHECK(nodes[1] == scene[1]);

	GYRO_CHECK(reader.Nodes<Coeff>().empty());
}

GYRO_TEST(SnapshotReader, ANodeRunReachingPastTheEndResolvesEmpty)
{
	Bytes<sizeof(SnapshotHeader)> buffer;

	SnapshotHeader header{};
	header.ByteSize = sizeof(SnapshotHeader);
	// The count the header claims does not fit in the snapshot. The clamp matters more for this run
	// than for any other: a coefficient run read past its end is a wrong number, and a node run read
	// past its end is a walk with no bound on it, inside the frame section, on a SCHED_FIFO thread.
	header.Nodes = { sizeof(SnapshotHeader), 4, sizeof(Skeleton), alignof(Skeleton) };
	std::memcpy(buffer.Data.data(), &header, sizeof(SnapshotHeader));

	const SnapshotReader reader{ buffer.View() };

	GYRO_CHECK(reader.IsValid());
	GYRO_CHECK(reader.Nodes<Skeleton>().empty());
}

GYRO_TEST(SnapshotReader, TheSameBytesResolveAtADifferentAddress)
{
	constexpr std::uint32_t offset = sizeof(SnapshotHeader);

	Bytes<offset + sizeof(Coeff)> original;
	const Coeff written{ Monotonic::FromNanoseconds(7'000), -3.0, 2.0f };
	std::memcpy(original.Data.data() + offset, &written, sizeof(Coeff));

	SnapshotHeader header{};
	header.ByteSize = static_cast<std::uint32_t>(original.Data.size());
	header.Runs[RunIndex(SnapshotRun::Translation)] = { offset, 1, sizeof(Coeff), alignof(Coeff) };
	std::memcpy(original.Data.data(), &header, sizeof(SnapshotHeader));

	// A second, independently allocated buffer at a different address, holding a byte-for-byte copy.
	// If any absolute pointer had leaked into the representation, the copy would resolve to the
	// original's memory or to nothing; offsets make it resolve to its own.
	auto copy = original;
	GYRO_REQUIRE(original.Data.data() != copy.Data.data());

	const SnapshotReader reader{ copy.View() };
	GYRO_REQUIRE(reader.IsValid());

	const std::span<const Coeff> run = reader.Run<Coeff>(SnapshotRun::Translation);
	GYRO_REQUIRE_EQ(run.size(), std::size_t{ 1 });
	GYRO_CHECK(run[0] == written);
	GYRO_CHECK(reinterpret_cast<const std::byte*>(run.data()) >= copy.Data.data());
}
