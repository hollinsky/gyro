#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/Fd.h"
#include "Testing/Test.h"
#include "Wire/Buffer.h"
#include "Wire/Message.h"
#include "Wire/Reader.h"
#include "Wire/Writer.h"

// The codec against itself, with no socket, no host, and no thread.
//
// This is decision 119's first argument for the module existing, spent. A `MessageWriter` fills an
// output buffer, the bytes and descriptors are carried across by hand rather than by a kernel, and a
// `MessageReader` takes them apart — so every framing, padding, and ordering rule is checked against
// what it produced rather than against what mutter happened to accept. The cases that matter here are
// the ones no host can be asked to produce: a message that claims more arguments than it carries, a
// string with no terminator, and a descriptor queue that has run past what one `sendmsg` can hold.

namespace
{
using namespace Wire;

// The kernel's job, done by the test: everything pending in the output buffer becomes available in
// the input buffer, in order, descriptors included.
void Transfer(OutputBuffer& out, InputBuffer& in)
{
	const std::span<const std::byte> bytes = out.Pending();
	const std::span<PendingFd> fds = out.Fds();

	in.Append(bytes);

	for (PendingFd& pending : fds)
	{
		in.PutFd(std::move(pending.Descriptor));
	}

	out.Sent(bytes.size(), fds.size());
}

// The framing step a connection does before it dispatches, so that a test can read a message back
// without one. Returns nothing where the buffer does not hold a whole message.
[[nodiscard]] std::optional<MessageReader> Next(InputBuffer& in)
{
	const std::span<const std::byte> available = in.Available();

	if (available.size() < HeaderBytes)
	{
		return std::nullopt;
	}

	const MessageHeader header = ReadHeader(available);

	if (!header.IsWellFormed() || available.size() < header.Size)
	{
		return std::nullopt;
	}

	return MessageReader{ header, available.subspan(HeaderBytes, header.Size - HeaderBytes), in };
}

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
} // namespace

// Every argument kind in one message, read back in the order it was written. The string and the array
// are adjacent on purpose: a padding mistake in either shifts the other, and a test that wrote one
// argument at a time would find neither.
GYRO_TEST(Codec, EveryArgumentKindRoundTrips)
{
	OutputBuffer out;
	InputBuffer in;

	Fd sent = Descriptor();
	GYRO_REQUIRE(sent.IsValid());

	constexpr std::array<std::byte, 3> payload{ std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 } };

	{
		MessageWriter writer{ out, ObjectId{ 7 }, 4 };
		writer.PutInt(-5);
		writer.PutUint(0x80000001U);
		writer.PutFixed(Fixed::FromDouble(-2.5));
		writer.PutString("wl_compositor");
		writer.PutArray(payload);
		writer.PutObject(ObjectId{ 9 });
		writer.PutNewId(ObjectId{ 11 });
		writer.PutFd(std::move(sent));
		writer.Send();
	}

	Transfer(out, in);

	std::optional<MessageReader> reader = Next(in);
	GYRO_REQUIRE(reader.has_value());

	GYRO_CHECK_EQ(reader->Target(), ObjectId{ 7 });
	GYRO_CHECK_EQ(reader->Opcode(), std::uint16_t{ 4 });

	GYRO_CHECK_EQ(reader->GetInt(), -5);
	GYRO_CHECK_EQ(reader->GetUint(), std::uint32_t{ 0x80000001U });
	GYRO_CHECK_EQ(reader->GetFixed(), Fixed::FromDouble(-2.5));
	GYRO_CHECK_EQ(reader->GetString(), "wl_compositor");

	const std::span<const std::byte> array = reader->GetArray();
	GYRO_REQUIRE_EQ(array.size(), std::size_t{ 3 });
	GYRO_CHECK(std::equal(array.begin(), array.end(), payload.begin()));

	GYRO_CHECK_EQ(reader->GetObject(), ObjectId{ 9 });
	GYRO_CHECK_EQ(reader->GetNewId(), ObjectId{ 11 });
	GYRO_CHECK(reader->GetFd().IsValid());

	GYRO_CHECK(!reader->Failed());
	GYRO_CHECK_EQ(reader->Remaining(), std::size_t{ 0 });
}

// Lengths either side of a word boundary, so that a string whose bytes happen to fill a word is
// distinguished from one that needs a whole word of padding for its terminator alone.
GYRO_TEST(Codec, StringsPadAroundTheirTerminator)
{
	for (const std::string_view text : { "", "a", "abc", "abcd", "abcde" })
	{
		OutputBuffer out;
		InputBuffer in;

		{
			MessageWriter writer{ out, ObjectId{ 2 }, 0 };
			writer.PutString(text);
			writer.PutUint(0xdeadbeefU);
			writer.Send();
		}

		GYRO_CHECK_EQ(out.Size(), HeaderBytes + 4 + Padded(text.size() + 1) + 4);

		Transfer(out, in);

		std::optional<MessageReader> reader = Next(in);
		GYRO_REQUIRE(reader.has_value());

		GYRO_CHECK_EQ(reader->GetString(), text);

		// The word after it is the real assertion: a string that padded wrong takes this one with it.
		GYRO_CHECK_EQ(reader->GetUint(), std::uint32_t{ 0xdeadbeefU });
		GYRO_CHECK(!reader->Failed());
	}
}

// The protocol's null string is a length of zero and is a different thing from the empty string,
// which is a length of one and a lone NUL. Both read back empty; what is checked is that they occupy
// different numbers of bytes, because a nullable argument sent as the empty string is a host being
// told a surface has a title when it has none.
GYRO_TEST(Codec, NullStringIsNotTheEmptyString)
{
	OutputBuffer empty;
	{
		MessageWriter writer{ empty, ObjectId{ 2 }, 0 };
		writer.PutString("");
		writer.Send();
	}

	OutputBuffer absent;
	{
		MessageWriter writer{ absent, ObjectId{ 2 }, 0 };
		writer.PutNullString();
		writer.Send();
	}

	GYRO_CHECK_EQ(empty.Size(), HeaderBytes + 8);
	GYRO_CHECK_EQ(absent.Size(), HeaderBytes + 4);
}

// Arrays carry their own zeros, so the length is the only thing that says where one ends.
GYRO_TEST(Codec, ArraysMayHoldZeros)
{
	constexpr std::array<std::byte, 5> payload{
		std::byte{ 0 }, std::byte{ 0 }, std::byte{ 7 }, std::byte{ 0 }, std::byte{ 0 },
	};

	OutputBuffer out;
	InputBuffer in;

	{
		MessageWriter writer{ out, ObjectId{ 2 }, 0 };
		writer.PutArray(payload);
		writer.PutUint(1);
		writer.Send();
	}

	Transfer(out, in);

	std::optional<MessageReader> reader = Next(in);
	GYRO_REQUIRE(reader.has_value());

	const std::span<const std::byte> array = reader->GetArray();
	GYRO_REQUIRE_EQ(array.size(), std::size_t{ 5 });
	GYRO_CHECK(std::equal(array.begin(), array.end(), payload.begin()));
	GYRO_CHECK_EQ(reader->GetUint(), std::uint32_t{ 1 });
}

// A reader run off the end of its message latches rather than reading memory it does not own. This is
// the runtime half of decision 2's argument for generated bindings: the generated dispatcher is a
// straight run of `Get` calls with no bounds checks in it, so the bounds have to be here.
GYRO_TEST(Codec, ReadingPastTheEndFails)
{
	OutputBuffer out;
	InputBuffer in;

	{
		MessageWriter writer{ out, ObjectId{ 2 }, 0 };
		writer.PutUint(1);
		writer.Send();
	}

	Transfer(out, in);

	std::optional<MessageReader> reader = Next(in);
	GYRO_REQUIRE(reader.has_value());

	GYRO_CHECK_EQ(reader->GetUint(), std::uint32_t{ 1 });
	GYRO_CHECK(!reader->Failed());

	GYRO_CHECK_EQ(reader->GetUint(), std::uint32_t{ 0 });
	GYRO_CHECK(reader->Failed());

	// Latched: everything after the first overrun is a zero of the right type and nothing else
	// happens, so a generated dispatcher runs to the end of its case without a branch in it.
	GYRO_CHECK_EQ(reader->GetString(), "");
	GYRO_CHECK(reader->GetArray().empty());
	GYRO_CHECK(!reader->GetFd().IsValid());
	GYRO_CHECK(reader->Failed());
}

// A string whose length runs past the message, and a string whose last byte is not a terminator.
// Neither is producible by a conforming host, which is exactly why they are worth a test — the second
// one ends with a view handed to a proxy that may pass `.data()` to a C function.
GYRO_TEST(Codec, MalformedStringsFail)
{
	{
		InputBuffer in;
		std::array<std::byte, 16> bytes{};
		StoreWord(bytes, 0, 2);
		StoreWord(bytes, 4, PackSizeAndOpcode(16, 0));
		StoreWord(bytes, 8, 64);
		StoreWord(bytes, 12, 0);
		in.Append(bytes);

		std::optional<MessageReader> reader = Next(in);
		GYRO_REQUIRE(reader.has_value());
		GYRO_CHECK_EQ(reader->GetString(), "");
		GYRO_CHECK(reader->Failed());
	}

	{
		InputBuffer in;
		std::array<std::byte, 16> bytes{};
		StoreWord(bytes, 0, 2);
		StoreWord(bytes, 4, PackSizeAndOpcode(16, 0));
		StoreWord(bytes, 8, 4);
		StoreWord(bytes, 12, 0x41414141U);
		in.Append(bytes);

		std::optional<MessageReader> reader = Next(in);
		GYRO_REQUIRE(reader.has_value());
		GYRO_CHECK_EQ(reader->GetString(), "");
		GYRO_CHECK(reader->Failed());
	}
}

// A message asking for a descriptor the peer did not send. Failing here rather than returning an
// invalid one is what keeps the queue aligned: a `GetFd` that silently returned nothing would leave
// the *next* message's descriptor sitting in front of the queue, and the object that eventually took
// it would get somebody else's buffer.
GYRO_TEST(Codec, MissingDescriptorFails)
{
	OutputBuffer out;
	InputBuffer in;

	{
		MessageWriter writer{ out, ObjectId{ 2 }, 0 };
		writer.Send();
	}

	Transfer(out, in);

	std::optional<MessageReader> reader = Next(in);
	GYRO_REQUIRE(reader.has_value());
	GYRO_CHECK(!reader->GetFd().IsValid());
	GYRO_CHECK(reader->Failed());
}

// Descriptors are consumed in the order their messages are, across messages rather than within one.
// That is the whole ordering contract: `SCM_RIGHTS` attaches them to a batch of bytes, not to a
// message, so the queue is the connection's and a message takes from the front.
GYRO_TEST(Codec, DescriptorsAreOrderedAgainstTheirMessages)
{
	OutputBuffer out;
	InputBuffer in;
	std::vector<int> numbers;

	for (std::uint32_t message = 0; message < 4; ++message)
	{
		Fd descriptor = Descriptor();
		GYRO_REQUIRE(descriptor.IsValid());
		numbers.push_back(descriptor.Get());

		MessageWriter writer{ out, ObjectId{ 2 }, 0 };
		writer.PutUint(message);
		writer.PutFd(std::move(descriptor));
		writer.Send();
	}

	Transfer(out, in);

	for (std::uint32_t message = 0; message < 4; ++message)
	{
		std::optional<MessageReader> reader = Next(in);
		GYRO_REQUIRE(reader.has_value());

		GYRO_CHECK_EQ(reader->GetUint(), message);

		const Fd received = reader->GetFd();
		GYRO_CHECK_EQ(received.Get(), numbers[message]);
		GYRO_CHECK(!reader->Failed());

		in.Consume(HeaderBytes + 4);
	}
}

// The 28 boundary, as arithmetic rather than as a syscall. Thirty messages each carrying one
// descriptor: the first `sendmsg` may carry 28, so the bytes it writes must stop at the end of the
// twenty-eighth message and not one byte later — otherwise message 29 goes out with nothing behind it
// and the far end demarshals a `new_id` for a buffer it will never receive.
//
// Connection.Test.cpp runs the same case through a real socket. This is the half that says what the
// clamp is *supposed* to be, in numbers a failure can be read off.
GYRO_TEST(Codec, TheDescriptorLimitClampsAtAMessageBoundary)
{
	constexpr std::size_t Messages = 30;
	constexpr std::size_t MessageBytes = HeaderBytes + 4;

	OutputBuffer out;

	for (std::uint32_t message = 0; message < Messages; ++message)
	{
		Fd descriptor = Descriptor();
		GYRO_REQUIRE(descriptor.IsValid());

		MessageWriter writer{ out, ObjectId{ 2 }, 0 };
		writer.PutUint(message);
		writer.PutFd(std::move(descriptor));
		writer.Send();
	}

	GYRO_REQUIRE_EQ(out.Fds().size(), Messages);
	GYRO_CHECK_EQ(out.Size(), Messages * MessageBytes);

	// The first batch stops at the end of the twenty-eighth message rather than at the end of the
	// buffer. One byte more and message 29 reaches the far end with nothing behind it.
	const OutputBuffer::Batch first = out.NextBatch();
	GYRO_CHECK_EQ(first.Fds.size(), MaxFdsPerMessage);
	GYRO_CHECK_EQ(first.Bytes.size(), MaxFdsPerMessage * MessageBytes);

	out.Sent(first.Bytes.size(), first.Fds.size());

	// What is left is two whole messages and the two descriptors they carry.
	const OutputBuffer::Batch second = out.NextBatch();
	GYRO_CHECK_EQ(second.Fds.size(), Messages - MaxFdsPerMessage);
	GYRO_CHECK_EQ(second.Bytes.size(), (Messages - MaxFdsPerMessage) * MessageBytes);

	out.Sent(second.Bytes.size(), second.Fds.size());

	GYRO_CHECK(out.Fds().empty());
	GYRO_CHECK(out.NextBatch().Bytes.empty());
}

// A writer that never sends leaves nothing behind, including in the middle of a stream of messages
// that did. The message either arrives whole or does not exist.
// **A message with more descriptors than one `sendmsg` carries is refused where it is written.**
// Wire/Buffer.h's batch stops at a message boundary so nothing reaches the far end ahead of its
// descriptors; a message with 29 of its own offers no boundary to stop at, so it would go out whole
// with 28 behind it and every descriptor after it would be taken by the wrong message. Latched on the
// buffer like the size cap, because the call site that wrote it is generated code with nowhere to put
// an error.
GYRO_TEST(Codec, RefusesAMessageCarryingMoreDescriptorsThanOneSend)
{
	OutputBuffer out;

	{
		MessageWriter writer{ out, ObjectId{ 2 }, 0 };

		for (std::size_t index = 0; index <= MaxFdsPerMessage; ++index)
		{
			writer.PutFd(Descriptor());
		}

		writer.Send();
	}

	GYRO_REQUIRE(out.Fault().has_value());
	GYRO_CHECK_EQ(out.Fault()->Code(), E2BIG);

	// Rolled back whole: no bytes, and every descriptor it took closed rather than left for the next
	// message to adopt.
	GYRO_CHECK(out.Pending().empty());
	GYRO_CHECK(out.Fds().empty());
}

// And 28 exactly is the message that still goes, so the refusal above is a boundary rather than a
// ceiling somebody guessed at.
GYRO_TEST(Codec, TwentyEightDescriptorsInOneMessageStillGo)
{
	OutputBuffer out;

	{
		MessageWriter writer{ out, ObjectId{ 2 }, 0 };

		for (std::size_t index = 0; index < MaxFdsPerMessage; ++index)
		{
			writer.PutFd(Descriptor());
		}

		writer.Send();
	}

	GYRO_CHECK(!out.Fault().has_value());

	const OutputBuffer::Batch batch = out.NextBatch();
	GYRO_CHECK_EQ(batch.Bytes.size(), HeaderBytes);
	GYRO_CHECK_EQ(batch.Fds.size(), MaxFdsPerMessage);
}

GYRO_TEST(Codec, AnAbandonedMessageLeavesNoTrace)
{
	OutputBuffer out;
	InputBuffer in;

	{
		MessageWriter writer{ out, ObjectId{ 2 }, 0 };
		writer.PutUint(1);
		writer.Send();
	}

	{
		MessageWriter abandoned{ out, ObjectId{ 3 }, 1 };
		abandoned.PutUint(2);
		abandoned.PutString("this never happened");
	}

	{
		MessageWriter writer{ out, ObjectId{ 4 }, 2 };
		writer.PutUint(3);
		writer.Send();
	}

	Transfer(out, in);

	std::optional<MessageReader> first = Next(in);
	GYRO_REQUIRE(first.has_value());
	GYRO_CHECK_EQ(first->Target(), ObjectId{ 2 });
	GYRO_CHECK_EQ(first->GetUint(), std::uint32_t{ 1 });
	in.Consume(HeaderBytes + 4);

	std::optional<MessageReader> second = Next(in);
	GYRO_REQUIRE(second.has_value());
	GYRO_CHECK_EQ(second->Target(), ObjectId{ 4 });
	GYRO_CHECK_EQ(second->GetUint(), std::uint32_t{ 3 });
	in.Consume(HeaderBytes + 4);

	GYRO_CHECK(!Next(in).has_value());
}
