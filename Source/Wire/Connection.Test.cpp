#include "Wire/Connection.h"

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Testing/Test.h"
#include "Wire/Buffer.h"
#include "Wire/Message.h"
#include "Wire/Reader.h"
#include "Wire/Writer.h"

// The connection against a peer the test is playing, over a `socketpair`.
//
// **No host compositor is involved and none is wanted here.** Every case below is one a real host
// either cannot be asked to produce — an event for an id nothing is bound to, a `delete_id` for an id
// that was never allocated, a truncated event — or produces only under a load nobody can arrange, like
// a send that has to be split because it carries more descriptors than one `sendmsg` may. That is
// decision 119's argument for the module, and this file is where it is spent.
//
// The way in is `WAYLAND_SOCKET`, which is not a testing hatch: it is how a sandboxed client reaches a
// compositor whose socket it cannot open, and using it here means the inherited-descriptor path is
// exercised by every test in the file rather than by one.

namespace
{
using namespace Wire;

// A connection whose far end is the test. Both halves are held so the peer's descriptor closes with
// the fixture, which is what makes "the host went away" a one-line case.
struct Pair
{
	Connection Client;
	Fd Peer;
};

// Hands the client one end through the environment and keeps the other.
[[nodiscard]] bool Connect(Pair& pair)
{
	std::array<int, 2> ends{ -1, -1 };

	if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends.data()) != 0)
	{
		return false;
	}

	pair.Peer = Fd{ ends[1] };

	const std::string number = std::to_string(ends[0]);
	::setenv("WAYLAND_SOCKET", number.c_str(), 1);

	const Result<void> opened = pair.Client.Open();

	if (!opened)
	{
		::close(ends[0]);
		return false;
	}

	return true;
}

// A message written by the peer, framed with the same codec the client reads with. The framing is
// symmetric — an event and a request differ only in which direction they travel — so this is the
// codec checking itself rather than a second implementation to keep in step.
struct Marshaller
{
	OutputBuffer Buffer;

	[[nodiscard]] bool WriteTo(RawFd socket)
	{
		const std::span<const std::byte> bytes = Buffer.Pending();
		const ::ssize_t written = ::write(socket.Value, bytes.data(), bytes.size());

		if (written < 0 || static_cast<std::size_t>(written) != bytes.size())
		{
			return false;
		}

		Buffer.Sent(bytes.size(), 0);

		return true;
	}
};

// What a bound proxy is, for a runtime that has no idea what a proxy is: a `void*` and a function
// pointer. The generated bindings are this with a `switch` in the middle.
//
// **Every case reads first and records second**, which is Wire/Connection.h's `Dispatcher` contract
// written out: `self` is null once the proxy has been unbound, and the arguments still have to come
// off the wire or the descriptor queue is left out of step for every message after this one. This is
// the shape the generator emits.
struct Recorder
{
	std::vector<std::uint32_t> Values;
	std::vector<std::string> Text;
	std::vector<Fd> Descriptors;
	std::uint16_t LastOpcode = 0;
	bool ReadPastTheEnd = false;

	static void Dispatch(void* self, std::uint16_t opcode, MessageReader& reader)
	{
		Recorder* const recorder = static_cast<Recorder*>(self);

		if (recorder != nullptr)
		{
			recorder->LastOpcode = opcode;
		}

		switch (opcode)
		{
			case 0:
			{
				const std::uint32_t value = reader.GetUint();

				if (recorder != nullptr)
				{
					recorder->Values.push_back(value);
				}

				break;
			}

			case 1:
			{
				const std::string_view text = reader.GetString();

				if (recorder != nullptr)
				{
					recorder->Text.emplace_back(text);
				}

				break;
			}

			case 2:
			{
				Fd descriptor = reader.GetFd();

				if (recorder != nullptr)
				{
					recorder->Descriptors.push_back(std::move(descriptor));
				}

				break;
			}

			default:
			{
				// Reads two words whatever the message carries, which is what a generated dispatcher
				// does: the signature comes from the protocol file, not from the bytes.
				const std::uint32_t first = reader.GetUint();
				const std::uint32_t second = reader.GetUint();

				if (recorder != nullptr)
				{
					recorder->Values.push_back(first);
					recorder->Values.push_back(second);
					recorder->ReadPastTheEnd = reader.Failed();
				}

				break;
			}
		}
	}
};

// The peer's `sendmsg`, since `Marshaller::WriteTo` is a plain write and cannot carry descriptors.
// The client's own `Flush` is the thing under test elsewhere, so this end does it by hand.
[[nodiscard]] bool SendWithDescriptors(RawFd socket, const OutputBuffer::Batch& batch)
{
	alignas(::cmsghdr) std::array<std::byte, CMSG_SPACE(MaxFdsPerMessage * sizeof(int))> control{};

	::iovec vector{ .iov_base = const_cast<std::byte*>(batch.Bytes.data()), .iov_len = batch.Bytes.size() };

	::msghdr message{};
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control.data();
	message.msg_controllen = CMSG_SPACE(batch.Fds.size() * sizeof(int));

	::cmsghdr* header = CMSG_FIRSTHDR(&message);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(batch.Fds.size() * sizeof(int));

	for (std::size_t index = 0; index < batch.Fds.size(); ++index)
	{
		const int descriptor = batch.Fds[index].Descriptor.Get();
		std::memcpy(CMSG_DATA(header) + index * sizeof(int), &descriptor, sizeof descriptor);
	}

	return ::sendmsg(socket.Value, &message, MSG_NOSIGNAL) == static_cast<::ssize_t>(batch.Bytes.size());
}

[[nodiscard]] Fd Descriptor(std::byte mark)
{
	std::array<int, 2> ends{ -1, -1 };

	if (::pipe(ends.data()) != 0)
	{
		return Fd{};
	}

	// A byte the far end can read back, so a test can tell which descriptor arrived where rather than
	// only that one did.
	const ssize_t written = ::write(ends[1], &mark, 1);

	::close(ends[1]);

	if (written != 1)
	{
		::close(ends[0]);

		return Fd{};
	}

	return Fd{ ends[0] };
}
} // namespace

// The inherited descriptor is adopted and the variable is unset, which is not tidiness: a descriptor
// number is not a thing to inherit twice, and a child spawned later would find the variable naming
// something that has since been closed and reused.
GYRO_TEST(Connection, InheritsAndUnsetsWaylandSocket)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	GYRO_CHECK(pair.Client.Descriptor().IsValid());
	GYRO_CHECK_EQ(std::getenv("WAYLAND_SOCKET"), nullptr);
}

// Unset even when it was nonsense, and for the same reason. libwayland leaves it in place on this
// path; leaving a bad descriptor number in the environment of everything gyro spawns is worse than
// the failure that caused it.
GYRO_TEST(Connection, RefusesAndClearsAMalformedWaylandSocket)
{
	::setenv("WAYLAND_SOCKET", "not-a-descriptor", 1);

	Connection connection;
	const Result<void> opened = connection.Open();

	GYRO_REQUIRE(!opened.has_value());
	GYRO_CHECK_EQ(opened.error().Code(), EINVAL);
	GYRO_CHECK_EQ(std::getenv("WAYLAND_SOCKET"), nullptr);
}

// Ids start at two, because `wl_display` is one and the runtime answers it.
GYRO_TEST(Connection, AllocatesFromTwo)
{
	Connection connection;

	GYRO_CHECK_EQ(connection.Allocate(), ObjectId{ 2 });
	GYRO_CHECK_EQ(connection.Allocate(), ObjectId{ 3 });
	GYRO_CHECK_EQ(connection.Allocate(), ObjectId{ 4 });
}

GYRO_TEST(Connection, RefusesToBindTheDisplay)
{
	Connection connection;
	Recorder recorder;

	const Result<void> bound = connection.Bind(ObjectId::Display, &recorder, &Recorder::Dispatch);

	GYRO_REQUIRE(!bound.has_value());
	GYRO_CHECK_EQ(bound.error().Code(), EINVAL);
}

// An id has to have been reserved. Binding one that was not is how two proxies end up sharing an id,
// and the symptom is one of them receiving the other's events.
GYRO_TEST(Connection, RefusesToBindAnUnreservedId)
{
	Connection connection;
	Recorder recorder;

	GYRO_CHECK(!connection.Bind(ObjectId{ 5 }, &recorder, &Recorder::Dispatch).has_value());

	const ObjectId id = connection.Allocate();
	GYRO_REQUIRE(connection.Bind(id, &recorder, &Recorder::Dispatch).has_value());
	GYRO_CHECK(!connection.Bind(id, &recorder, &Recorder::Dispatch).has_value());
}

// **An unbound id is not a free id.** The protocol forbids reuse until the host has said the id is
// gone, because the host may still have events in flight naming the old object — and reusing it early
// is how those events reach the new one.
GYRO_TEST(Connection, ARetiredIdStaysOutOfCirculation)
{
	Connection connection;
	Recorder recorder;

	const ObjectId first = connection.Allocate();
	GYRO_REQUIRE(connection.Bind(first, &recorder, &Recorder::Dispatch).has_value());

	connection.Unbind(first);

	GYRO_CHECK(connection.Allocate() != first);
}

// **An event for a proxy the client has already destroyed is read and dropped, not a disconnect.**
// Both ends talk at once, so the host has an event on the wire before it has read the request
// destroying its object — every time, for one round trip. Failing there would end the connection on
// the ordinary case rather than on a host misbehaving.
//
// The descriptor is the point. It arrives out of band and comes off a queue in the order messages
// consume it, so the stale event has to be *read* rather than skipped; the assertion is that the next
// event's descriptor is the next one the peer sent, which is only true if the dead object's was taken
// off the queue first.
GYRO_TEST(Connection, AnEventForARetiredProxyIsReadAndDropped)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	Recorder gone;
	Recorder living;

	const ObjectId retired = pair.Client.Allocate();
	GYRO_REQUIRE(pair.Client.Bind(retired, &gone, &Recorder::Dispatch).has_value());

	const ObjectId live = pair.Client.Allocate();
	GYRO_REQUIRE(pair.Client.Bind(live, &living, &Recorder::Dispatch).has_value());

	// The proxy goes. The id does not, because the host has not said `delete_id` yet.
	pair.Client.Unbind(retired);

	// Two events each carrying a descriptor, the first for the object that is already gone.
	Marshaller peer;
	{
		MessageWriter stale{ peer.Buffer, retired, 2 };
		stale.PutFd(Descriptor(std::byte{ 0xa1 }));
		stale.Send();
	}
	{
		MessageWriter fresh{ peer.Buffer, live, 2 };
		fresh.PutFd(Descriptor(std::byte{ 0xb2 }));
		fresh.Send();
	}

	{
		const OutputBuffer::Batch batch = peer.Buffer.NextBatch();
		GYRO_REQUIRE_EQ(batch.Fds.size(), std::size_t{ 2 });
		GYRO_REQUIRE(SendWithDescriptors(pair.Peer.Borrow(), batch));
		peer.Buffer.Sent(batch.Bytes.size(), batch.Fds.size());
	}

	GYRO_REQUIRE(pair.Client.Drain().has_value());

	// Nothing reached the destroyed proxy.
	GYRO_CHECK(gone.Descriptors.empty());

	// And the living one got its own descriptor rather than the dead object's, which is what says the
	// queue never went out of step.
	GYRO_REQUIRE_EQ(living.Descriptors.size(), std::size_t{ 1 });

	std::byte mark{ 0x00 };
	GYRO_REQUIRE_EQ(::read(living.Descriptors[0].Get(), &mark, 1), static_cast<::ssize_t>(1));
	GYRO_CHECK_EQ(mark, std::byte{ 0xb2 });
}

// And `delete_id` still puts a retired id back, with the dispatcher it was holding for those stale
// events let go with it.
GYRO_TEST(Connection, DeleteIdClearsTheRetiredDispatcher)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	Recorder recorder;
	const ObjectId id = pair.Client.Allocate();
	GYRO_REQUIRE(pair.Client.Bind(id, &recorder, &Recorder::Dispatch).has_value());

	pair.Client.Unbind(id);

	Marshaller peer;
	{
		MessageWriter deleted{ peer.Buffer, ObjectId::Display, 1 };
		deleted.PutUint(static_cast<std::uint32_t>(id));
		deleted.Send();
	}
	GYRO_REQUIRE(peer.WriteTo(pair.Peer.Borrow()));
	GYRO_REQUIRE(pair.Client.Drain().has_value());

	GYRO_CHECK_EQ(pair.Client.Allocate(), id);

	// Free again rather than merely reusable: an event for it now is a host talking about an object
	// this end has never bound.
	Marshaller stale;
	{
		MessageWriter event{ stale.Buffer, id, 0 };
		event.PutUint(1);
		event.Send();
	}
	GYRO_REQUIRE(stale.WriteTo(pair.Peer.Borrow()));

	const Result<void> drained = pair.Client.Drain();
	GYRO_REQUIRE(!drained.has_value());
	GYRO_CHECK_EQ(drained.error().Code(), EPROTO);
}

// An id that was allocated and never bound comes straight back, because nothing on the wire has ever
// named it: there is no `delete_id` coming for it and nothing in flight to answer. Retiring it would
// strand it for the life of the connection.
GYRO_TEST(Connection, AnIdThatWasNeverBoundComesBackImmediately)
{
	Connection connection;

	const ObjectId first = connection.Allocate();
	connection.Unbind(first);

	GYRO_CHECK_EQ(connection.Allocate(), first);
}

// And `delete_id` is what puts it back. The host's word rather than the client's.
GYRO_TEST(Connection, DeleteIdRecyclesTheId)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	Recorder recorder;
	const ObjectId first = pair.Client.Allocate();
	GYRO_REQUIRE(pair.Client.Bind(first, &recorder, &Recorder::Dispatch).has_value());

	pair.Client.Unbind(first);

	Marshaller peer;
	{
		MessageWriter event{ peer.Buffer, ObjectId::Display, 1 };
		event.PutUint(static_cast<std::uint32_t>(first));
		event.Send();
	}
	GYRO_REQUIRE(peer.WriteTo(pair.Peer.Borrow()));

	GYRO_REQUIRE(pair.Client.Drain().has_value());
	GYRO_CHECK_EQ(pair.Client.Allocate(), first);
}

// The ordinary order, not the odd one: a host frees an id the moment it destroys the object, which
// for a `wl_callback` is immediately after it sends `done` — so `delete_id` routinely arrives while
// the proxy is still bound and the client destroys it a moment later.
GYRO_TEST(Connection, DeleteIdMayArriveBeforeTheProxyIsGone)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	Recorder recorder;
	const ObjectId callback = pair.Client.Allocate();
	GYRO_REQUIRE(pair.Client.Bind(callback, &recorder, &Recorder::Dispatch).has_value());

	Marshaller peer;
	{
		MessageWriter done{ peer.Buffer, callback, 0 };
		done.PutUint(42);
		done.Send();
	}
	{
		MessageWriter deleted{ peer.Buffer, ObjectId::Display, 1 };
		deleted.PutUint(static_cast<std::uint32_t>(callback));
		deleted.Send();
	}
	GYRO_REQUIRE(peer.WriteTo(pair.Peer.Borrow()));

	GYRO_REQUIRE(pair.Client.Drain().has_value());
	GYRO_REQUIRE_EQ(recorder.Values.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(recorder.Values[0], std::uint32_t{ 42 });

	// Still out of circulation, because the proxy has not gone yet.
	GYRO_CHECK(pair.Client.Allocate() != callback);

	pair.Client.Unbind(callback);
	GYRO_CHECK_EQ(pair.Client.Allocate(), callback);
}

// A host freeing an id it never allocated is a host that has lost track of the connection, and there
// is nothing to resynchronise against.
GYRO_TEST(Connection, RefusesDeleteIdForAnIdNobodyHolds)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	Marshaller peer;
	{
		MessageWriter deleted{ peer.Buffer, ObjectId::Display, 1 };
		deleted.PutUint(9);
		deleted.Send();
	}
	GYRO_REQUIRE(peer.WriteTo(pair.Peer.Borrow()));

	const Result<void> drained = pair.Client.Drain();
	GYRO_REQUIRE(!drained.has_value());
	GYRO_CHECK_EQ(drained.error().Code(), EPROTO);
}

// An event reaches the object bound to its id, with its opcode and its arguments.
GYRO_TEST(Connection, DispatchesToTheBoundObject)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	Recorder recorder;
	const ObjectId registry = pair.Client.Allocate();
	GYRO_REQUIRE(pair.Client.Bind(registry, &recorder, &Recorder::Dispatch).has_value());

	Marshaller peer;
	{
		MessageWriter global{ peer.Buffer, registry, 1 };
		global.PutString("wl_compositor");
		global.Send();
	}
	{
		MessageWriter global{ peer.Buffer, registry, 1 };
		global.PutString("wl_shm");
		global.Send();
	}
	GYRO_REQUIRE(peer.WriteTo(pair.Peer.Borrow()));

	GYRO_REQUIRE(pair.Client.Drain().has_value());

	GYRO_REQUIRE_EQ(recorder.Text.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(recorder.Text[0], "wl_compositor");
	GYRO_CHECK_EQ(recorder.Text[1], "wl_shm");
}

// **An event for an id nothing is bound to ends the connection**, which is the runtime's half of
// decision 2's argument that a dispatch table cannot have a hole. libwayland discards such a message
// instead, and it can only do that because it knows the event's signature and can close exactly the
// descriptors the discarded message carried. This module has no signatures by construction, so
// discarding would leave the descriptor queue one entry out of step and the next message would take a
// buffer belonging to a dead object.
GYRO_TEST(Connection, RefusesAnEventForAnUnboundId)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	Marshaller peer;
	{
		MessageWriter event{ peer.Buffer, ObjectId{ 12 }, 0 };
		event.PutUint(1);
		event.Send();
	}
	GYRO_REQUIRE(peer.WriteTo(pair.Peer.Borrow()));

	const Result<void> drained = pair.Client.Drain();
	GYRO_REQUIRE(!drained.has_value());
	GYRO_CHECK_EQ(drained.error().Code(), EPROTO);

	// Latched. There is no resynchronising a stream whose framing has been lost, so every verb keeps
	// reporting the first failure rather than the last consequence of it.
	GYRO_CHECK(pair.Client.Failed().has_value());
	GYRO_CHECK_EQ(pair.Client.Drain().error().Code(), EPROTO);
}

// A dispatcher that read further than the message went. Checked once after it returns rather than at
// every accessor, so the generated code stays a straight run of `Get` calls.
GYRO_TEST(Connection, RefusesAnEventShorterThanItsSignature)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	Recorder recorder;
	const ObjectId id = pair.Client.Allocate();
	GYRO_REQUIRE(pair.Client.Bind(id, &recorder, &Recorder::Dispatch).has_value());

	Marshaller peer;
	{
		// Opcode 3 reads two words; this carries one.
		MessageWriter event{ peer.Buffer, id, 3 };
		event.PutUint(1);
		event.Send();
	}
	GYRO_REQUIRE(peer.WriteTo(pair.Peer.Borrow()));

	const Result<void> drained = pair.Client.Drain();
	GYRO_REQUIRE(!drained.has_value());
	GYRO_CHECK_EQ(drained.error().Code(), EPROTO);
	GYRO_CHECK(recorder.ReadPastTheEnd);
}

// A message whose size field is not a whole number of words never becomes complete, so treating it as
// a short read would be a hang rather than a failure — the connection would stop dispatching and
// never say why.
GYRO_TEST(Connection, RefusesAnImpossibleSize)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	std::array<std::byte, 12> bytes{};
	StoreWord(bytes, 0, 2);
	StoreWord(bytes, 4, PackSizeAndOpcode(11, 0));
	GYRO_REQUIRE_EQ(::write(pair.Peer.Get(), bytes.data(), bytes.size()), static_cast<::ssize_t>(bytes.size()));

	const Result<void> drained = pair.Client.Drain();
	GYRO_REQUIRE(!drained.has_value());
	GYRO_CHECK_EQ(drained.error().Code(), EPROTO);
}

// **The host's protocol error is the only diagnostic there will ever be**, so it survives the trip
// even though `Core/Result.h`'s `Error` cannot carry it: an `Error` holds a `string_view` over static
// storage by design, and the failing object, code, and text are parked where whatever logs it can
// reach them.
GYRO_TEST(Connection, SurfacesTheHostsProtocolError)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	Marshaller peer;
	{
		MessageWriter error{ peer.Buffer, ObjectId::Display, 0 };
		error.PutObject(ObjectId{ 7 });
		error.PutUint(3);
		error.PutString("invalid method 4 on wl_surface@7");
		error.Send();
	}
	GYRO_REQUIRE(peer.WriteTo(pair.Peer.Borrow()));

	const Result<void> drained = pair.Client.Drain();
	GYRO_REQUIRE(!drained.has_value());
	GYRO_CHECK_EQ(drained.error().Code(), EPROTO);

	GYRO_REQUIRE(pair.Client.Fault().has_value());
	GYRO_CHECK_EQ(pair.Client.Fault()->Object, ObjectId{ 7 });
	GYRO_CHECK_EQ(pair.Client.Fault()->Code, std::uint32_t{ 3 });
	GYRO_CHECK_EQ(pair.Client.Fault()->Message, "invalid method 4 on wl_surface@7");
}

// The host going away is a fact gyro reports and recovers from, not a crash. Distinct from finding
// nothing, which is the ordinary result of a wakeup some other source caused.
GYRO_TEST(Connection, ReportsTheHostClosing)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	// Nothing to read is success, per Seam/EventSource.h.
	GYRO_CHECK(pair.Client.Drain().has_value());

	pair.Peer.Reset();

	const Result<void> drained = pair.Client.Drain();
	GYRO_REQUIRE(!drained.has_value());
	GYRO_CHECK_EQ(drained.error().Code(), EPIPE);
}

// Requests are buffered and one flush is one `sendmsg`, so a burst of them costs one syscall rather
// than one each.
GYRO_TEST(Connection, FlushSendsWhatWasMarshalled)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	{
		MessageWriter request{ pair.Client, ObjectId::Display, 1 };
		request.PutNewId(pair.Client.Allocate());
		request.Send();
	}

	GYRO_REQUIRE(pair.Client.Flush().has_value());

	std::array<std::byte, 64> bytes{};
	const ::ssize_t read = ::read(pair.Peer.Get(), bytes.data(), bytes.size());

	GYRO_REQUIRE_EQ(read, static_cast<::ssize_t>(HeaderBytes + 4));

	const MessageHeader header = ReadHeader(bytes);
	GYRO_CHECK_EQ(header.Target, ObjectId::Display);
	GYRO_CHECK_EQ(header.Opcode, std::uint16_t{ 1 });
	GYRO_CHECK_EQ(header.Size, std::uint16_t{ 12 });
	GYRO_CHECK_EQ(LoadWord(bytes, 8), std::uint32_t{ 2 });
}

// **A partial send is ordinary, not a failure.** A host that has not read yet fills the socket
// buffer, and what did not go stays buffered for the next flush — which is the difference between a
// slow host and a dropped request.
GYRO_TEST(Connection, APartialSendIsNotAFailure)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	// Far more than any socket buffer, so the first flush cannot finish.
	constexpr std::size_t Messages = 40000;

	for (std::uint32_t message = 0; message < Messages; ++message)
	{
		MessageWriter request{ pair.Client, ObjectId::Display, 1 };
		request.PutUint(message);
		request.Send();
	}

	GYRO_REQUIRE(pair.Client.Flush().has_value());
	GYRO_CHECK(!pair.Client.Output().Pending().empty());

	// Drain the peer and flush again until it all goes, checking every word arrives in order.
	std::uint32_t expected = 0;
	std::vector<std::byte> carried;

	for (int round = 0; round < 10000 && expected < Messages; ++round)
	{
		std::array<std::byte, 8192> bytes{};
		const ::ssize_t read = ::read(pair.Peer.Get(), bytes.data(), bytes.size());

		if (read > 0)
		{
			carried.insert(carried.end(), bytes.begin(), bytes.begin() + read);

			std::size_t offset = 0;

			while (carried.size() - offset >= HeaderBytes + 4)
			{
				GYRO_REQUIRE_EQ(LoadWord(carried, offset + 8), expected);
				++expected;
				offset += HeaderBytes + 4;
			}

			carried.erase(carried.begin(), carried.begin() + static_cast<std::ptrdiff_t>(offset));
		}

		GYRO_REQUIRE(pair.Client.Flush().has_value());
	}

	GYRO_CHECK_EQ(expected, Messages);
	GYRO_CHECK(pair.Client.Output().Pending().empty());
}

// **The 28 boundary through a real socket.** Thirty messages each carrying a descriptor cannot go in
// one `sendmsg`, so the write stops at the end of the twenty-eighth message: send one byte more and
// message 29 reaches the far end with nothing behind it, and a host demarshalling it would take a
// descriptor belonging to some other message entirely.
//
// Each descriptor is a pipe holding one distinguishable byte, so the assertion is that descriptor N
// arrived with message N rather than merely that thirty descriptors arrived.
GYRO_TEST(Connection, DescriptorsStayOrderedPastTheSendLimit)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	constexpr std::size_t Messages = 30;

	for (std::uint32_t message = 0; message < Messages; ++message)
	{
		Fd descriptor = Descriptor(static_cast<std::byte>(message));
		GYRO_REQUIRE(descriptor.IsValid());

		MessageWriter request{ pair.Client, ObjectId{ 2 }, 0 };
		request.PutUint(message);
		request.PutFd(std::move(descriptor));
		request.Send();
	}

	// Two rounds are needed and the test does not assume how many: what matters is that everything
	// arrives, in order, however many syscalls it took.
	std::vector<std::byte> carried;
	std::vector<Fd> received;

	for (int round = 0; round < 16 && (carried.size() < Messages * (HeaderBytes + 4) || received.size() < Messages);
	     ++round)
	{
		GYRO_REQUIRE(pair.Client.Flush().has_value());

		std::array<std::byte, 4096> bytes{};
		alignas(::cmsghdr) std::array<std::byte, CMSG_SPACE(MaxFdsPerMessage * sizeof(int))> control{};

		::iovec vector{ .iov_base = bytes.data(), .iov_len = bytes.size() };
		::msghdr message{};
		message.msg_iov = &vector;
		message.msg_iovlen = 1;
		message.msg_control = control.data();
		message.msg_controllen = control.size();

		const ::ssize_t read = ::recvmsg(pair.Peer.Get(), &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);

		if (read <= 0)
		{
			continue;
		}

		for (::cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header))
		{
			if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
			{
				continue;
			}

			const std::size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);

			for (std::size_t index = 0; index < count; ++index)
			{
				int descriptor = -1;
				std::memcpy(&descriptor, CMSG_DATA(header) + index * sizeof(int), sizeof descriptor);
				received.emplace_back(descriptor);
			}
		}

		carried.insert(carried.end(), bytes.begin(), bytes.begin() + read);
	}

	GYRO_REQUIRE_EQ(carried.size(), Messages * (HeaderBytes + 4));
	GYRO_REQUIRE_EQ(received.size(), Messages);

	for (std::size_t message = 0; message < Messages; ++message)
	{
		GYRO_CHECK_EQ(LoadWord(carried, message * (HeaderBytes + 4) + 8), static_cast<std::uint32_t>(message));

		std::byte mark{ 0xff };
		GYRO_REQUIRE_EQ(::read(received[message].Get(), &mark, 1), static_cast<::ssize_t>(1));
		GYRO_CHECK_EQ(mark, static_cast<std::byte>(message));
	}
}

// **A flush in the middle of a message sends the messages before it and not that one.** A writer
// marshals in place, so until `Send` patches the size word the buffer ends in a header claiming zero
// bytes — and a host handed that closes the connection. Flushing mid-message is a caller mistake, but
// it is one that costs nothing to make harmless, and the alternative is a disconnect whose cause is
// three stack frames away from where it was written.
GYRO_TEST(Connection, FlushLeavesAHalfWrittenMessageAlone)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	{
		MessageWriter finished{ pair.Client, ObjectId::Display, 1 };
		finished.PutUint(1);
		finished.Send();
	}

	{
		MessageWriter open{ pair.Client, ObjectId::Display, 1 };
		open.PutUint(2);

		GYRO_REQUIRE(pair.Client.Flush().has_value());

		std::array<std::byte, 64> bytes{};
		const ::ssize_t read = ::recv(pair.Peer.Get(), bytes.data(), bytes.size(), MSG_DONTWAIT);

		// The first message, whole, and nothing of the second.
		GYRO_REQUIRE_EQ(read, static_cast<::ssize_t>(HeaderBytes + 4));
		GYRO_CHECK_EQ(ReadHeader(bytes).Size, std::uint16_t{ 12 });
		GYRO_CHECK_EQ(LoadWord(bytes, 8), std::uint32_t{ 1 });

		open.Send();
	}

	GYRO_REQUIRE(pair.Client.Flush().has_value());

	std::array<std::byte, 64> bytes{};
	const ::ssize_t read = ::recv(pair.Peer.Get(), bytes.data(), bytes.size(), MSG_DONTWAIT);

	GYRO_REQUIRE_EQ(read, static_cast<::ssize_t>(HeaderBytes + 4));
	GYRO_CHECK_EQ(ReadHeader(bytes).Size, std::uint16_t{ 12 });
	GYRO_CHECK_EQ(LoadWord(bytes, 8), std::uint32_t{ 2 });
}

// Two live writers would eat each other's messages, because the second one's rollback truncates past
// what the first one committed. The refusal is latched on the buffer and reported by the next flush,
// for Wire/Writer.h's reason.
GYRO_TEST(Connection, RefusesTwoMessagesAtOnce)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	{
		MessageWriter first{ pair.Client, ObjectId::Display, 1 };
		MessageWriter second{ pair.Client, ObjectId::Display, 1 };

		first.PutUint(1);
		second.PutUint(2);

		first.Send();
		second.Send();
	}

	const Result<void> flushed = pair.Client.Flush();
	GYRO_REQUIRE(!flushed.has_value());
	GYRO_CHECK_EQ(flushed.error().Code(), EBUSY);
}
