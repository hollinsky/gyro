#include <poll.h>

#include <cstdint>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.h"
#include "Testing/Test.h"
#include "Wire/Connection.h"
#include "Wire/Message.h"
#include "Wire/Reader.h"
#include "Wire/Writer.h"

// The runtime against a real Wayland host, hand-marshalled.
//
// **There is no generated code here and that is the point of the file.** Every other test in this
// module proves the codec agrees with itself; this one proves it agrees with somebody else's — that
// the header layout, the string padding, and the id allocation are what mutter, KWin, or sway
// actually expect, rather than what this module believes they expect. Two requests are enough to
// establish it: `get_registry` makes the host talk, and `sync` makes it answer, which is the round
// trip every Wayland client is built on.
//
// **A machine with no host compositor skips this rather than failing it.** Docs/Decisions.md decision
// 6 requires the tree's tests to run where there is no GPU, no seat, and no compositor, and this is
// the one file that cannot honour that — so it says so by name and errno rather than passing quietly,
// which is the convention Virtual/Udmabuf.Test.cpp already set. A green run that skipped everything is
// exactly the rot the build-time checks exist to prevent.
//
// The opcodes below are `wl_display`, `wl_registry`, and `wl_callback` as the protocol has had them
// since 1.0. They are written out rather than named by a generated header because there is no
// generator yet; when there is, this file is what its output has to keep agreeing with.

namespace
{
using namespace Wire;

constexpr std::uint16_t DisplaySync = 0;
constexpr std::uint16_t DisplayGetRegistry = 1;

constexpr std::uint16_t RegistryGlobal = 0;
constexpr std::uint16_t RegistryGlobalRemove = 1;

constexpr std::uint16_t CallbackDone = 0;

struct Registry
{
	std::vector<std::string> Interfaces;

	static void Dispatch(void* self, std::uint16_t opcode, MessageReader& reader)
	{
		Registry& registry = *static_cast<Registry*>(self);

		switch (opcode)
		{
			case RegistryGlobal:
			{
				(void)reader.GetUint();
				const std::string_view interface = reader.GetString();
				(void)reader.GetUint();

				registry.Interfaces.emplace_back(interface);
				break;
			}

			case RegistryGlobalRemove:
				(void)reader.GetUint();
				break;

			default:
				break;
		}
	}
};

struct Callback
{
	bool Done = false;

	static void Dispatch(void* self, std::uint16_t opcode, MessageReader& reader)
	{
		if (opcode == CallbackDone)
		{
			(void)reader.GetUint();
			static_cast<Callback*>(self)->Done = true;
		}
	}
};

// Waits for the socket rather than spinning on it, so a slow host costs no CPU and a dead one costs
// no more than the timeout.
[[nodiscard]] bool Wait(const Connection& connection, int milliseconds)
{
	::pollfd watched{ .fd = connection.Descriptor().Value, .events = POLLIN, .revents = 0 };

	return ::poll(&watched, 1, milliseconds) > 0;
}
} // namespace

GYRO_TEST(Live, RegistryAndSyncRoundTrip)
{
	Connection connection;

	if (const Result<void> opened = connection.Open(); !opened)
	{
		std::println("  skipped Live.RegistryAndSyncRoundTrip: {}", opened.error());

		return;
	}

	Registry registry;
	const ObjectId registryId = connection.Allocate();
	GYRO_REQUIRE(registryId != ObjectId::None);
	GYRO_REQUIRE(connection.Bind(registryId, &registry, &Registry::Dispatch).has_value());

	{
		MessageWriter request{ connection, ObjectId::Display, DisplayGetRegistry };
		request.PutNewId(registryId);
		request.Send();
	}

	// **The sync is what makes the assertion below deterministic.** A host advertises its globals when
	// it feels like it, so a test that read once and counted would be a race; `wl_display.sync` is
	// answered only after everything queued before it has been sent, which is the protocol's one
	// ordering guarantee and the reason every client's startup is built on it.
	Callback callback;
	const ObjectId callbackId = connection.Allocate();
	GYRO_REQUIRE(callbackId != ObjectId::None);
	GYRO_REQUIRE(connection.Bind(callbackId, &callback, &Callback::Dispatch).has_value());

	{
		MessageWriter request{ connection, ObjectId::Display, DisplaySync };
		request.PutNewId(callbackId);
		request.Send();
	}

	GYRO_REQUIRE(connection.Flush().has_value());

	// Five seconds is generous for a local socket and is a bound rather than an expectation: what it
	// buys is that a host which never answers fails the test instead of hanging the suite.
	for (int round = 0; round < 50 && !callback.Done; ++round)
	{
		(void)Wait(connection, 100);

		const Result<void> drained = connection.Drain();

		if (!drained)
		{
			if (connection.Fault())
			{
				GYRO_FAIL(connection.Fault()->Message.c_str());
			}

			GYRO_REQUIRE(drained.has_value());

			return;
		}
	}

	GYRO_REQUIRE(callback.Done);

	// Every host has a compositor and a shm pool; asserting on one of them rather than merely on a
	// non-empty list is what makes a string that decoded one byte short a failure here.
	GYRO_CHECK(!registry.Interfaces.empty());
	GYRO_CHECK(std::ranges::find(registry.Interfaces, "wl_compositor") != registry.Interfaces.end());
	GYRO_CHECK(std::ranges::find(registry.Interfaces, "wl_shm") != registry.Interfaces.end());
}
