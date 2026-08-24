// `setenv`, `mkdtemp`, and the AF_UNIX client below are POSIX rather than ISO C, and glibc gates them
// behind a feature-test macro. Named here for Server.Test.cpp's reason: what is wanted from the
// platform is stated rather than inherited.
#define _POSIX_C_SOURCE 200809L

#include "Protocol/Host.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>

#include "Core/Clock.h"
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Geometry/Space.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"
#include "Testing/Test.h"

// What the host promises the dispatch loop, which is the same contract a gym answers and one extra
// thing on the side.
//
// The claim worth a test is the one the whole wiring rests on: the descriptor the composition root
// sleeps on goes readable when somebody connects, and `Advance` is what makes it quiet again. If the
// second half were false the dispatch thread would spin at full tilt on a level-triggered `ppoll` the
// moment a client appeared, on a machine where the frame thread is `SCHED_FIFO` and the spin is what
// it preempts. Nothing else here can catch that: `Server` alone has no `Advance`, and the loop above
// has no socket.
//
// The rest is the author contract. `Never()` from a host with no clients is not a stub answer — it is
// what makes the dispatch thread park on the socket instead of waking on a cadence — and a host that
// touches neither the store nor the texture space is what says the empty registry is honest rather
// than half-wired.

namespace
{
// A private XDG_RUNTIME_DIR, for Server.Test.cpp's reason: the developer's live session has a running
// compositor on `wayland-0` and colliding with it would be this test's fault rather than a finding.
//
// **Where a socket landed is read back from the environment rather than from this object**, because
// Server.Test.cpp installs one of these too and the two are in the same binary: which of them wins is
// static initialisation order, which is unspecified across translation units. Both are private
// directories and either is fine to bind under — what is not fine is asking one of them where a socket
// the other one owns is.
struct PrivateRuntimeDir
{
	PrivateRuntimeDir()
	{
		char pattern[] = "/tmp/gyro-host-XXXXXX";
		const char* const made = ::mkdtemp(pattern);

		if (made != nullptr)
		{
			Path = made;
			::setenv("XDG_RUNTIME_DIR", Path.c_str(), 1);
		}
	}

	std::string Path;
};

const PrivateRuntimeDir g_RuntimeDir;

// The texture space as the nothing a host touches today. It counts, so that "never called" is asserted
// rather than assumed — the day a `wl_shm` pool arrives this is the fake that will start counting.
class CountingTextures final : public ITextures
{
public:
	[[nodiscard]] Result<TextureId>
	Adopt(PixelSize<BufferSpace>, std::uint32_t, std::span<const std::byte>, TextureAlpha) override
	{
		++Adopted;

		return Failure(ENOSYS, "no texture space in this test");
	}

	void Retire(TextureId) noexcept override { ++Retired; }

	std::uint32_t Adopted = 0;
	std::uint32_t Retired = 0;
};

// A client that is a bare socket rather than libwayland: `Protocol` links the server library and not
// the client one, and what is being tested is the descriptor going readable, which a `connect` with no
// bytes behind it is enough to cause. It also keeps the test honest about what it proves — the
// handshake is not being exercised, the wakeup is.
class RawClient
{
public:
	[[nodiscard]] bool Connect(std::string_view socket)
	{
		m_Fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

		if (m_Fd < 0)
		{
			return false;
		}

		const char* const directory = ::getenv("XDG_RUNTIME_DIR");

		if (directory == nullptr)
		{
			return false;
		}

		const std::string path = std::string{ directory } + "/" + std::string{ socket };

		struct sockaddr_un address = {};
		address.sun_family = AF_UNIX;

		if (path.size() >= sizeof(address.sun_path))
		{
			return false;
		}

		std::memcpy(address.sun_path, path.c_str(), path.size());

		return ::connect(m_Fd, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) == 0;
	}

	~RawClient()
	{
		if (m_Fd >= 0)
		{
			::close(m_Fd);
		}
	}

	RawClient() = default;
	RawClient(const RawClient&) = delete;
	RawClient& operator=(const RawClient&) = delete;
	RawClient(RawClient&&) = delete;
	RawClient& operator=(RawClient&&) = delete;

private:
	int m_Fd = -1;
};

// Whether the wait the composition root builds would return at once. Zero timeout, because the
// question is *is there something pending*, which is exactly what the root's `ppoll` asks and never
// blocks to find out.
[[nodiscard]] bool IsReadable(int descriptor)
{
	struct pollfd watched = {};
	watched.fd = descriptor;
	watched.events = POLLIN;

	return ::poll(&watched, 1, 0) == 1;
}

struct Fixture
{
	ManualClock Clock{ Monotonic::FromNanoseconds(1'000'000'000) };
	SceneStore Store{ Clock };
	CountingTextures Textures;
};
} // namespace

GYRO_TEST(ClientHost, OpensASocketAndAnswersAsAnAuthor)
{
	GYRO_REQUIRE(!g_RuntimeDir.Path.empty());

	const Result<std::unique_ptr<ClientHost>> host = MakeClientHost();

	GYRO_REQUIRE(host);
	GYRO_REQUIRE(*host != nullptr);

	// The socket is bound by the factory rather than by `Open` below, because a compositor that cannot
	// offer a socket should stop before it lays out monitors — and because the root needs this
	// descriptor to build the wait, which is before the loop opens anything.
	GYRO_CHECK(!(*host)->SocketName().empty());
	GYRO_CHECK((*host)->PollFd() >= 0);

	// The name the log line prints and the name `--gym` prints are the same kind of answer.
	GYRO_CHECK_EQ((*host)->Name(), "clients");
}

GYRO_TEST(ClientHost, AnEmptyRunAuthorsNothingAndSleepsForever)
{
	Fixture fixture;

	const Result<std::unique_ptr<ClientHost>> host = MakeClientHost();

	GYRO_REQUIRE(host);
	GYRO_REQUIRE((*host)->Open(fixture.Store, fixture.Textures).has_value());

	const Wake wake = (*host)->Advance(fixture.Store, fixture.Textures, fixture.Clock.Now());

	// **`Never()` rather than a cadence.** A host has no schedule of its own — what makes it run again
	// is a client writing to the socket the root is already sleeping on — and answering anything else
	// would be a compositor with no windows waking up on a timer, which is the thing
	// Docs/Architecture.md#doing-nothing-must-cost-nothing forbids.
	GYRO_CHECK(wake.Which == Wake::Kind::Settled);

	// Nothing is advertised, so nothing was authored and nothing was adopted.
	GYRO_CHECK_EQ(fixture.Textures.Adopted, 0u);
	GYRO_CHECK_EQ(fixture.Textures.Retired, 0u);
}

GYRO_TEST(ClientHost, AConnectionWakesTheDescriptorAndAdvanceQuietsIt)
{
	Fixture fixture;

	const Result<std::unique_ptr<ClientHost>> host = MakeClientHost();

	GYRO_REQUIRE(host);
	GYRO_REQUIRE((*host)->Open(fixture.Store, fixture.Textures).has_value());

	// Nothing has happened, so the root's wait would block — which is the idle case, and the baseline
	// the two assertions below only mean something against.
	GYRO_REQUIRE(!IsReadable((*host)->PollFd()));

	RawClient client;

	GYRO_REQUIRE(client.Connect((*host)->SocketName()));

	// The listening socket is inside the event loop's epoll set, so a pending connection is what makes
	// the one descriptor the root sleeps on readable. This is the wakeup path for every client request
	// there will ever be, exercised on the only one that exists yet.
	GYRO_CHECK(IsReadable((*host)->PollFd()));

	const Wake wake = (*host)->Advance(fixture.Store, fixture.Textures, fixture.Clock.Now());

	GYRO_CHECK(wake.Which == Wake::Kind::Settled);

	// **And quiet again, which is the half that matters.** The root's `ppoll` is level-triggered, so a
	// descriptor still readable after the step is a dispatch thread that never sleeps — spinning beside
	// a `SCHED_FIFO` frame thread for as long as a client stays connected.
	GYRO_CHECK(!IsReadable((*host)->PollFd()));

	(*host)->Flush();
}
