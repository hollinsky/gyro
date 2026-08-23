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
#include <vector>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Testing/Test.h"
#include "Wayland/Wayland.h"
#include "Wire/Buffer.h"
#include "Wire/Connection.h"
#include "Wire/Message.h"
#include "Wire/Writer.h"

// The generated bindings driven against a peer the test is playing, over a `socketpair`.
//
// **Compiling is not the claim.** Tools/Bindings/Emit.Test.cpp already reads the emitted text and
// holds it to the properties the generator promises — every event has a case, every request newer
// than version one has a gate. What it cannot show is that the bytes are right: an argument order
// reversed in the emitter produces text that passes every structural check, compiles clean, and puts
// a surface's height where its width belongs. So this file writes a real `wl_registry.global` onto a
// real socket and asks what the handler saw.
//
// It lives in Integration for the reason the others here do: it names two things no module may name
// together. The bindings are generated into the build tree and are outside the module graph
// altogether, and only something above both can put one in front of a `Wire::Connection`.
//
// **A descriptor is carried in one of these cases on purpose.** Wire/Reader.h's queue runs across
// messages rather than per message, so an event whose `fd` argument nobody demarshals leaves every
// later one pointing at the wrong file — the failure mode that makes an unknown opcode the end of the
// connection rather than a skip. The keymap case is that path exercised end to end.

namespace
{
using namespace Wire;
using namespace Wayland;

struct Pair
{
	Connection Client;
	Fd Peer;
};

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

	if (!pair.Client.Open())
	{
		::close(ends[0]);
		return false;
	}

	return true;
}

// What the peer sends, framed with the same codec the client reads with — the codec checking itself
// rather than a second implementation to keep in step, which is Wire/Connection.Test.cpp's argument.
struct Marshaller
{
	OutputBuffer Buffer;

	[[nodiscard]] bool WriteTo(RawFd socket) const
	{
		const std::span<const std::byte> bytes = Buffer.Pending();

		return ::write(socket.Value, bytes.data(), bytes.size()) == static_cast<::ssize_t>(bytes.size());
	}

	// The same, with one descriptor attached. `SCM_RIGHTS` is how a keymap, a dmabuf plane and a
	// syncobj timeline all arrive, so the generated demarshaller has to consume them in order.
	[[nodiscard]] bool WriteTo(RawFd socket, RawFd passed) const
	{
		const std::span<const std::byte> bytes = Buffer.Pending();

		::iovec vector{ .iov_base = const_cast<std::byte*>(bytes.data()), .iov_len = bytes.size() };

		alignas(::cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};

		::msghdr message{};
		message.msg_iov = &vector;
		message.msg_iovlen = 1;
		message.msg_control = control.data();
		message.msg_controllen = control.size();

		::cmsghdr* header = CMSG_FIRSTHDR(&message);
		header->cmsg_level = SOL_SOCKET;
		header->cmsg_type = SCM_RIGHTS;
		header->cmsg_len = CMSG_LEN(sizeof(int));
		std::memcpy(CMSG_DATA(header), &passed.Value, sizeof(int));

		return ::sendmsg(socket.Value, &message, 0) == static_cast<::ssize_t>(bytes.size());
	}
};

// Everything the client has queued, flushed and read back off the peer's end. The bytes are what a
// host would see, so a test can assert on the wire rather than on the proxy that wrote it.
std::vector<std::byte> Sent(Pair& pair)
{
	if (!pair.Client.Flush())
	{
		return {};
	}

	std::array<std::byte, 4096> scratch{};
	const ::ssize_t read = ::recv(pair.Peer.Borrow().Value, scratch.data(), scratch.size(), MSG_DONTWAIT);

	if (read <= 0)
	{
		return {};
	}

	return std::vector<std::byte>{ scratch.begin(), scratch.begin() + read };
}

struct Global
{
	std::uint32_t Name = 0;
	std::string Interface;
	std::uint32_t Version = 0;
};

class Registry final : public WlRegistryListener
{
public:
	std::vector<Global> Globals;
	std::vector<std::uint32_t> Removed;

	void OnGlobal(std::uint32_t name, std::string_view interface, std::uint32_t version) override
	{
		Globals.push_back(Global{ .Name = name, .Interface = std::string{ interface }, .Version = version });
	}

	void OnGlobalRemove(std::uint32_t name) override { Removed.push_back(name); }
};

// The opposite declaration, and the point of it: `Ignoring` is one word at the class that says the
// event list was read and none of it is wanted. Without it every handler here would be a `{}` body,
// which is the same silence with more typing and nothing to grep for.
class SilentRegistry final : public WlRegistry::Ignoring
{};

class Callback final : public WlCallbackListener
{
public:
	std::vector<std::uint32_t> Done;

	void OnDone(std::uint32_t callbackData) override { Done.push_back(callbackData); }
};

// wl_keyboard has six events and this file wants one of them, which is exactly the case `Ignoring`
// exists for.
class Keyboard final : public WlKeyboard::Ignoring
{
public:
	WlKeyboardKeymapFormat Format = WlKeyboardKeymapFormat::NoKeymap;
	std::uint32_t Size = 0;
	std::string Contents;

	void OnKeymap(WlKeyboardKeymapFormat format, Fd fd, std::uint32_t size) override
	{
		Format = format;
		Size = size;

		if (!fd.IsValid())
		{
			return;
		}

		std::array<char, 64> scratch{};
		const ::ssize_t read = ::read(fd.Borrow().Value, scratch.data(), scratch.size());

		if (read > 0)
		{
			Contents.assign(scratch.data(), static_cast<std::size_t>(read));
		}
	}
};

// A `wl_keyboard.keymap` for `target`, carrying a pipe holding `contents`. The descriptor is what the
// test is really about: it travels out of band, and which message consumes it is decided by the order
// dispatchers read rather than by anything in the bytes.
[[nodiscard]] bool SendKeymap(Pair& pair, ObjectId target, std::string_view contents)
{
	std::array<int, 2> ends{ -1, -1 };

	if (::pipe(ends.data()) != 0)
	{
		return false;
	}

	const Fd readEnd{ ends[0] };
	const Fd writeEnd{ ends[1] };

	if (::write(writeEnd.Borrow().Value, contents.data(), contents.size()) != static_cast<::ssize_t>(contents.size()))
	{
		return false;
	}

	Marshaller peer;
	{
		// `format`, then the descriptor — which occupies no bytes — then `size`.
		MessageWriter event{ peer.Buffer, target, 0 };
		event.PutUint(static_cast<std::uint32_t>(WlKeyboardKeymapFormat::XkbV1));
		event.PutUint(static_cast<std::uint32_t>(contents.size()));
		event.Send();
	}

	return peer.WriteTo(pair.Peer.Borrow(), readEnd.Borrow());
}
} // namespace

GYRO_TEST(WaylandBindings, ARegistryIsReachedAndItsGlobalsArrive)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	Registry registry;
	const WlDisplay display{ pair.Client, ObjectId::Display, WlDisplay::WireVersion };
	const WlRegistry bound = display.GetRegistry(registry);

	GYRO_REQUIRE(bound.IsValid());

	// `wl_display.get_registry` is opcode 1 over one `new_id`: twelve bytes, and the last word is the
	// id the proxy allocated for itself.
	const std::vector<std::byte> request = Sent(pair);
	GYRO_REQUIRE_EQ(request.size(), std::size_t{ 12 });

	const MessageHeader header = ReadHeader(request);
	GYRO_CHECK_EQ(header.Target, ObjectId::Display);
	GYRO_CHECK_EQ(header.Opcode, std::uint16_t{ 1 });
	GYRO_CHECK_EQ(header.Size, std::uint16_t{ 12 });
	GYRO_CHECK_EQ(LoadWord(request, 8), static_cast<std::uint32_t>(bound.Id()));

	// And the other direction: a global the host announces, through the generated dispatcher.
	Marshaller peer;
	{
		MessageWriter global{ peer.Buffer, bound.Id(), 0 };
		global.PutUint(7);
		global.PutString("wl_compositor");
		global.PutUint(6);
		global.Send();
	}
	GYRO_REQUIRE(peer.WriteTo(pair.Peer.Borrow()));

	GYRO_REQUIRE(pair.Client.Drain().has_value());

	GYRO_REQUIRE_EQ(registry.Globals.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(registry.Globals[0].Name, std::uint32_t{ 7 });
	GYRO_CHECK_EQ(registry.Globals[0].Interface, std::string{ "wl_compositor" });
	GYRO_CHECK_EQ(registry.Globals[0].Version, std::uint32_t{ 6 });

	// The listener knows which object it answers for, which is what makes a handler that takes no
	// proxy still able to say where it came from.
	GYRO_CHECK_EQ(registry.Object().Id(), bound.Id());
}

GYRO_TEST(WaylandBindings, BindPutsTheInterfaceNameAndVersionAheadOfTheId)
{
	// The shape Tools/Bindings/Protocol.h warns about: `wl_registry.bind` is not one id on the wire
	// but three values, and an emitter that missed it would write a message two words short that a
	// host answers by killing the connection.
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	SilentRegistry registry;
	const WlDisplay display{ pair.Client, ObjectId::Display, WlDisplay::WireVersion };
	const WlRegistry bound = display.GetRegistry(registry);
	GYRO_REQUIRE(!Sent(pair).empty());

	// `wl_compositor` has no events, so it is the overload that takes no listener — the distinction
	// the two `requires` clauses draw, exercised rather than read.
	const WlCompositor compositor = bound.Bind<WlCompositor>(7, 6);
	GYRO_REQUIRE(compositor.IsValid());
	GYRO_CHECK_EQ(compositor.Version(), std::uint32_t{ 6 });

	const std::vector<std::byte> request = Sent(pair);

	// Header, name, a length-prefixed NUL-terminated "wl_compositor" padded to a word, version, id.
	GYRO_REQUIRE_EQ(request.size(), std::size_t{ 8 + 4 + 4 + 16 + 4 + 4 });

	const MessageHeader header = ReadHeader(request);
	GYRO_CHECK_EQ(header.Target, bound.Id());
	GYRO_CHECK_EQ(header.Opcode, std::uint16_t{ 0 });

	GYRO_CHECK_EQ(LoadWord(request, 8), std::uint32_t{ 7 });
	GYRO_CHECK_EQ(LoadWord(request, 12), std::uint32_t{ 14 });
	GYRO_CHECK_EQ(std::string{ reinterpret_cast<const char*>(request.data()) + 16 }, std::string{ "wl_compositor" });
	GYRO_CHECK_EQ(LoadWord(request, 32), std::uint32_t{ 6 });
	GYRO_CHECK_EQ(LoadWord(request, 36), static_cast<std::uint32_t>(compositor.Id()));
}

GYRO_TEST(WaylandBindings, AFrameCallbackIsBoundBeforeTheRequestLeaves)
{
	// Decision 2's property, in the only form that proves it: the host answers before the client has
	// flushed. If the binding happened after the request rather than before it, the `done` below
	// would arrive at an id nothing is bound to and end the connection.
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	const ObjectId id = pair.Client.Allocate();
	GYRO_REQUIRE(id != ObjectId::None);

	SilentRegistry unused;
	WlSurface surface{ pair.Client, id, WlSurface::WireVersion };
	GYRO_REQUIRE(pair.Client.Bind(id, &unused, &WlRegistry::Dispatch).has_value());

	Callback callback;
	const WlCallback frame = surface.Frame(callback);
	GYRO_REQUIRE(frame.IsValid());

	// Nothing has been flushed. The id is live anyway.
	Marshaller peer;
	{
		MessageWriter done{ peer.Buffer, frame.Id(), 0 };
		done.PutUint(1234);
		done.Send();
	}
	GYRO_REQUIRE(peer.WriteTo(pair.Peer.Borrow()));

	GYRO_REQUIRE(pair.Client.Drain().has_value());

	GYRO_REQUIRE_EQ(callback.Done.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(callback.Done[0], std::uint32_t{ 1234 });

	// A `wl_callback` is version 1 however new the surface that made it is, which is what the version
	// cap on a child proxy is for.
	GYRO_CHECK_EQ(frame.Version(), std::uint32_t{ 1 });
}

GYRO_TEST(WaylandBindings, AKeymapDescriptorReachesTheHandler)
{
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	const ObjectId id = pair.Client.Allocate();
	GYRO_REQUIRE(id != ObjectId::None);

	Keyboard keyboard;
	const WlKeyboard proxy{ pair.Client, id, WlKeyboard::WireVersion };
	proxy.Listen(keyboard);

	GYRO_REQUIRE(SendKeymap(pair, id, "xkb_keymap {}"));

	GYRO_REQUIRE(pair.Client.Drain().has_value());

	GYRO_CHECK(keyboard.Format == WlKeyboardKeymapFormat::XkbV1);
	GYRO_CHECK_EQ(keyboard.Size, std::uint32_t{ 13 });
	GYRO_CHECK_EQ(keyboard.Contents, std::string{ "xkb_keymap {}" });
}

GYRO_TEST(WaylandBindings, AnEventForARetiredProxyStillConsumesItsDescriptor)
{
	// Both ends talk at once, so a host routinely has an event on the wire for an object before it has
	// read the request destroying it — a stale event is the ordinary case rather than a host
	// misbehaving. Wire/Connection.h answers it by keeping the dispatcher, dropping the object, and
	// calling in with a null `self`; the generated code's obligation is to demarshal anyway.
	//
	// **What is at stake is not the dead object.** Descriptors are taken from a queue in the order
	// messages consume them, so a stale event whose `fd` argument nobody reads leaves that queue one
	// entry out of step and the *next* event takes a descriptor belonging to the message that was
	// thrown away. Nothing detects that: it is a valid descriptor for the wrong buffer, which on a real
	// session is a frame composited out of somebody else's memory.
	//
	// So: two keyboards, one of them retired, a keymap for each, and the question is which descriptor
	// the live one ends up holding.
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	const ObjectId retired = pair.Client.Allocate();
	const ObjectId live = pair.Client.Allocate();
	GYRO_REQUIRE(retired != ObjectId::None && live != ObjectId::None);

	Keyboard gone;
	Keyboard present;
	const WlKeyboard retiredProxy{ pair.Client, retired, WlKeyboard::WireVersion };
	const WlKeyboard liveProxy{ pair.Client, live, WlKeyboard::WireVersion };
	retiredProxy.Listen(gone);
	liveProxy.Listen(present);

	// The client destroys one of them. The id stays out of circulation and the dispatcher stays with
	// it, which is the window this test is about.
	pair.Client.Unbind(retired);

	GYRO_REQUIRE(SendKeymap(pair, retired, "first"));
	GYRO_REQUIRE(SendKeymap(pair, live, "second"));

	GYRO_REQUIRE(pair.Client.Drain().has_value());

	// The retired listener heard nothing, because there was nothing left to hear it.
	GYRO_CHECK(gone.Contents.empty());

	// And the live one got its own descriptor rather than the dead object's, which is only true if the
	// stale event was read through rather than skipped.
	GYRO_CHECK_EQ(present.Contents, std::string{ "second" });
}

GYRO_TEST(WaylandBindings, AnOpcodeWithNoCaseEndsTheConnection)
{
	// Skipping the bytes would be free and skipping the descriptors is not, so an event these bindings
	// have no signature for is the end of the connection rather than a shrug — Wire/Connection.h makes
	// the identical call for an id nothing is bound to.
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	const ObjectId id = pair.Client.Allocate();
	GYRO_REQUIRE(id != ObjectId::None);

	Callback callback;
	WlCallback proxy{ pair.Client, id, WlCallback::WireVersion };
	proxy.Listen(callback);

	Marshaller peer;
	{
		// `wl_callback` has exactly one event. Opcode 4 is a host that has lost track of the object.
		MessageWriter nonsense{ peer.Buffer, id, 4 };
		nonsense.PutUint(0);
		nonsense.Send();
	}
	GYRO_REQUIRE(peer.WriteTo(pair.Peer.Borrow()));

	const Result<void> drained = pair.Client.Drain();
	GYRO_REQUIRE(!drained.has_value());
	GYRO_CHECK_EQ(drained.error().Code(), EPROTO);
	GYRO_CHECK(callback.Done.empty());
}

GYRO_TEST(WaylandBindings, ARequestNewerThanTheBoundVersionNeverReachesTheWire)
{
	// The host's answer to a request it has not heard of is to kill the connection, which for a nested
	// output is every window on the screen going away at once. So it stops here, as a fault the next
	// flush reports — Wire/Writer.h's contract, which is that a call site has nothing to check.
	Pair pair;
	GYRO_REQUIRE(Connect(pair));

	const ObjectId id = pair.Client.Allocate();
	GYRO_REQUIRE(id != ObjectId::None);

	// A host that only speaks version 1 of `wl_surface`. `set_buffer_scale` arrived in version 3.
	WlSurface surface{ pair.Client, id, 1 };
	surface.SetBufferScale(2);

	const Result<void> flushed = pair.Client.Flush();
	GYRO_REQUIRE(!flushed.has_value());
	GYRO_CHECK_EQ(flushed.error().Code(), EPROTO);

	std::array<std::byte, 64> scratch{};
	GYRO_CHECK(::recv(pair.Peer.Borrow().Value, scratch.data(), scratch.size(), MSG_DONTWAIT) < 0);
}
