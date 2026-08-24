#include "Wire/Buffer.h"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "Core/Fd.h"
#include "Testing/Test.h"
#include "Wire/Message.h"

// The two byte queues, on their own. Every case here is bookkeeping a socket would hide: what an
// offset is relative to after a partial write, what a rollback leaves behind, and where the dead
// prefix of a receive buffer goes.

namespace
{
using namespace Wire;

// A descriptor that costs nothing and is distinguishable from the next one. A pipe rather than
// `/dev/null`, so the test needs no filesystem at all.
[[nodiscard]] Fd Descriptor()
{
	std::array<int, 2> ends{ -1, -1 };

	if (::pipe(ends.data()) != 0)
	{
		return Fd{};
	}

	::close(ends[1]);

	return Fd{ ends[0] };
}

[[nodiscard]] std::span<const std::byte> Bytes(const char* text, std::size_t length)
{
	return std::span<const std::byte>{ reinterpret_cast<const std::byte*>(text), length };
}
} // namespace

GYRO_TEST(OutputBuffer, WordsAreNativeOrder)
{
	OutputBuffer buffer;
	buffer.PutWord(0x01020304);

	GYRO_REQUIRE_EQ(buffer.Size(), std::size_t{ 4 });
	GYRO_CHECK_EQ(LoadWord(buffer.Pending(), 0), std::uint32_t{ 0x01020304 });
}

// Padding is the argument boundary rule, and getting it wrong shifts every subsequent argument. Four
// lengths, because the interesting ones are "already aligned" and "one past aligned".
GYRO_TEST(OutputBuffer, PaddingRoundsToAWord)
{
	// Each payload is a literal of exactly its own length rather than a prefix of one long one: the
	// span a caller really passes is sized to its object, and handing GCC a pointer into a longer
	// literal with a length it cannot narrow makes -Warray-bounds guess the reserve as the bound.
	const auto SizeAfter = [](const char* text, std::size_t length) {
		OutputBuffer buffer;
		buffer.PutPadded(Bytes(text, length));

		return buffer.Size();
	};

	GYRO_CHECK_EQ(SizeAfter("", 0), Padded(0));
	GYRO_CHECK_EQ(SizeAfter("a", 1), Padded(1));
	GYRO_CHECK_EQ(SizeAfter("abcd", 4), Padded(4));
	GYRO_CHECK_EQ(SizeAfter("abcde", 5), Padded(5));

	// A terminated payload pads the terminator with the text rather than after it, so a four-byte
	// string occupies eight bytes and not four.
	OutputBuffer terminated;
	terminated.PutTerminated(Bytes("abcd", 4));
	GYRO_CHECK_EQ(terminated.Size(), std::size_t{ 8 });

	OutputBuffer exact;
	exact.PutTerminated(Bytes("abc", 3));
	GYRO_CHECK_EQ(exact.Size(), std::size_t{ 4 });
}

// A writer that is destroyed without being sent must leave the buffer exactly as it found it,
// descriptors included. The leak is the half nothing else would catch — the bytes are visible, the
// descriptor is a number nobody is counting.
GYRO_TEST(OutputBuffer, RollbackRestoresBytesAndDescriptors)
{
	OutputBuffer buffer;
	buffer.PutWord(1);
	buffer.Commit();

	const OutputBuffer::Mark mark = buffer.Position();

	Fd descriptor = Descriptor();
	GYRO_REQUIRE(descriptor.IsValid());
	const int number = descriptor.Get();

	buffer.PutWord(2);
	buffer.PutFd(std::move(descriptor));
	buffer.Rollback(mark);

	GYRO_CHECK_EQ(buffer.Size(), std::size_t{ 4 });
	GYRO_CHECK(buffer.Fds().empty());

	// The rollback closed it rather than leaking it, which is the point of the buffer owning what
	// passes through it.
	GYRO_CHECK_EQ(::close(number), -1);
	GYRO_CHECK_EQ(errno, EBADF);
}

// A partial write moves what is left without moving the offsets a live writer is holding. Get this
// wrong and the clamp stops at the wrong message, which shows up as a descriptor arriving one message
// late — under load only, and on the machine with the slow host.
GYRO_TEST(OutputBuffer, APartialWriteLeavesTheRestPending)
{
	OutputBuffer buffer;

	for (int message = 0; message < 3; ++message)
	{
		GYRO_REQUIRE(buffer.Begin());
		buffer.PutWord(0);
		buffer.PutWord(0);
		buffer.PutFd(Descriptor());
		buffer.Commit();
		buffer.End();
	}

	GYRO_REQUIRE_EQ(buffer.Fds().size(), std::size_t{ 3 });
	GYRO_CHECK_EQ(buffer.Committed().size(), std::size_t{ 24 });

	// Three descriptors is under the limit, so the batch is everything.
	const OutputBuffer::Batch batch = buffer.NextBatch();
	GYRO_CHECK_EQ(batch.Bytes.size(), std::size_t{ 24 });
	GYRO_CHECK_EQ(batch.Fds.size(), std::size_t{ 3 });

	// The kernel took the first message and four bytes of the second; all three descriptors went with
	// the first byte, which is what `SCM_RIGHTS` does.
	buffer.Sent(12, 3);

	GYRO_CHECK(buffer.Fds().empty());
	GYRO_CHECK_EQ(buffer.Pending().size(), std::size_t{ 12 });
	GYRO_CHECK_EQ(buffer.Committed().size(), std::size_t{ 12 });
}

// **A flush in the middle of a message must not move the ground under it.** A writer holds an offset
// into this buffer until it sends, so reclaiming the sent prefix while one is open would leave the
// size word patched into the middle of the next message. The offset survives; the reclaim waits.
GYRO_TEST(OutputBuffer, ReclaimingWaitsForAnOpenMessage)
{
	OutputBuffer buffer;

	GYRO_REQUIRE(buffer.Begin());
	buffer.PutWord(1);
	buffer.Commit();
	buffer.End();

	GYRO_REQUIRE(buffer.Begin());
	const OutputBuffer::Mark mark = buffer.Position();
	buffer.PutWord(0);

	// Only the finished message may go, and the open one's mark still names where it started.
	GYRO_CHECK_EQ(buffer.Committed().size(), std::size_t{ 4 });
	buffer.Sent(4, 0);

	GYRO_CHECK_EQ(mark.Bytes, std::size_t{ 4 });
	GYRO_CHECK_EQ(buffer.Size() - mark.Bytes, std::size_t{ 4 });
	GYRO_CHECK(buffer.Committed().empty());

	buffer.PatchWord(mark.Bytes, 2);
	buffer.Commit();
	buffer.End();

	// Closed, so the sent prefix is finally reclaimed and what is left is the second message alone.
	GYRO_REQUIRE_EQ(buffer.Committed().size(), std::size_t{ 4 });
	GYRO_CHECK_EQ(LoadWord(buffer.Committed(), 0), std::uint32_t{ 2 });
}

// Two live writers would eat each other's messages, because the second one's rollback truncates past
// the first one's committed bytes. The refusal is latched rather than reported at the call site, for
// Wire/Writer.h's reason.
GYRO_TEST(OutputBuffer, RefusesASecondOpenMessage)
{
	OutputBuffer buffer;

	GYRO_REQUIRE(buffer.Begin());
	GYRO_CHECK(!buffer.Begin());
	GYRO_REQUIRE(buffer.Fault().has_value());
	GYRO_CHECK_EQ(buffer.Fault()->Code(), EBUSY);

	buffer.End();
	GYRO_CHECK(buffer.Begin());
}

GYRO_TEST(InputBuffer, ConsumingKeepsViewsAlive)
{
	InputBuffer buffer;
	buffer.Append(Bytes("abcdefgh", 8));

	const std::byte* start = buffer.Available().data();
	buffer.Consume(4);

	// The prefix is dead but the storage has not moved, which is what lets a dispatcher hold a
	// string_view into a message the connection retires after the call returns.
	GYRO_REQUIRE_EQ(buffer.Available().size(), std::size_t{ 4 });
	GYRO_CHECK_EQ(buffer.Available().data(), start + 4);

	buffer.Compact();
	GYRO_CHECK_EQ(buffer.Available().size(), std::size_t{ 4 });
	GYRO_CHECK_EQ(buffer.Available()[0], std::byte{ 'e' });
}

GYRO_TEST(InputBuffer, DescriptorsComeOutInOrder)
{
	InputBuffer buffer;

	Fd first = Descriptor();
	Fd second = Descriptor();
	GYRO_REQUIRE(first.IsValid() && second.IsValid());

	const int numbers[2] = { first.Get(), second.Get() };

	buffer.PutFd(std::move(first));
	buffer.PutFd(std::move(second));

	GYRO_CHECK_EQ(buffer.FdCount(), std::size_t{ 2 });
	GYRO_CHECK_EQ(buffer.TakeFd().Get(), numbers[0]);
	GYRO_CHECK_EQ(buffer.TakeFd().Get(), numbers[1]);
	GYRO_CHECK_EQ(buffer.FdCount(), std::size_t{ 0 });

	// An empty queue is an invalid descriptor rather than a wrapped index, which is what Wire/Reader.h
	// turns into a connection error instead of handing -1 to a caller about to call mmap on it.
	GYRO_CHECK(!buffer.TakeFd().IsValid());
}
