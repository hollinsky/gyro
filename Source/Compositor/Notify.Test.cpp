#include "Compositor/Notify.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

#include "Core/Fd.h"
#include "Testing/Test.h"

// The service manager's datagram, against a real socket — every test here binds one and reads back
// what gyro sent it.
//
// **Both namespaces are exercised, because systemd hands out the one this code is most likely to get
// wrong.** `NOTIFY_SOCKET` is usually abstract — a leading `@` standing for a leading NUL — and an
// abstract name is bytes rather than a string, so a terminator copied into it makes a name nothing is
// bound to and a compositor that never becomes ready. That failure is invisible from inside gyro:
// `sendto` to an unbound abstract name is `ECONNREFUSED`, which looks exactly like a service manager
// that has gone away.

namespace
{
// A directory that removes itself, so a failed test leaves no sockets behind in `/tmp`.
class TemporaryDirectory
{
public:
	TemporaryDirectory()
	{
		std::string pattern = (std::filesystem::temp_directory_path() / "gyro-notify-XXXXXX").string();

		if (const char* made = ::mkdtemp(pattern.data()); made != nullptr)
		{
			m_Path = made;
		}
	}

	~TemporaryDirectory()
	{
		if (!m_Path.empty())
		{
			std::error_code ignored;
			std::filesystem::remove_all(m_Path, ignored);
		}
	}

	TemporaryDirectory(const TemporaryDirectory&) = delete;
	TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

	[[nodiscard]] const std::string& Path() const noexcept { return m_Path; }

private:
	std::string m_Path;
};

// A bound datagram socket standing in for the service manager, at a filesystem path or at an abstract
// name. The address is spelled here independently of `Notify.cpp` on purpose: a bug shared by the
// sender and the receiver would otherwise pass.
[[nodiscard]] Fd Listen(std::string_view name, bool abstracted)
{
	Fd socket{ ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0) };

	if (!socket.IsValid())
	{
		return Fd{};
	}

	sockaddr_un address{};
	address.sun_family = AF_UNIX;

	// A leading NUL and no terminator for an abstract name, a terminator and no leading NUL for a path.
	const std::size_t offset = abstracted ? 1U : 0U;
	const std::size_t terminator = abstracted ? 0U : 1U;

	std::memcpy(address.sun_path + offset, name.data(), name.size());

	const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + offset + name.size() + terminator);

	if (::bind(socket.Get(), reinterpret_cast<const sockaddr*>(&address), length) != 0)
	{
		return Fd{};
	}

	return socket;
}

// What arrived, or empty where nothing did.
[[nodiscard]] std::string Read(const Fd& socket)
{
	std::array<char, 512> buffer{};

	const ssize_t read = ::recv(socket.Get(), buffer.data(), buffer.size(), MSG_DONTWAIT);

	return read > 0 ? std::string{ buffer.data(), static_cast<std::size_t>(read) } : std::string{};
}

// An abstract name nothing else on the machine is using, since the namespace is machine-wide and two
// test binaries may be running at once.
[[nodiscard]] std::string UniqueName()
{
	return std::format("gyro-notify-test-{}", ::getpid());
}
} // namespace

GYRO_TEST(Notify, ReadyArrivesOnAFilesystemSocket)
{
	const TemporaryDirectory directory;

	GYRO_REQUIRE(!directory.Path().empty());

	const std::string path = directory.Path() + "/notify";
	const Fd manager = Listen(path, false);

	GYRO_REQUIRE(manager.IsValid());

	const Result<void> told = NotifyReady(path, "drm, 1 output, SCHED_FIFO at 50");

	GYRO_REQUIRE(told.has_value());
	GYRO_CHECK(Read(manager) == "READY=1\nSTATUS=drm, 1 output, SCHED_FIFO at 50");
}

GYRO_TEST(Notify, ReadyArrivesOnAnAbstractSocket)
{
	const std::string name = UniqueName();
	const Fd manager = Listen(name, true);

	GYRO_REQUIRE(manager.IsValid());

	const Result<void> told = NotifyReady("@" + name, "headless");

	GYRO_REQUIRE(told.has_value());
	GYRO_CHECK(Read(manager) == "READY=1\nSTATUS=headless");
}

// A status is a value in a `KEY=VALUE` protocol, so a newline in one would be a second key — and
// `STATUS=drm` followed by a line the service manager reads as an unknown assignment is a status that
// silently lost half of itself.
GYRO_TEST(Notify, AStatusIsOneLineWhateverItWasGiven)
{
	const std::string name = UniqueName();
	const Fd manager = Listen(name, true);

	GYRO_REQUIRE(manager.IsValid());

	const Result<void> told = NotifyReady("@" + name, "drm\nMAINPID=1");

	GYRO_REQUIRE(told.has_value());
	GYRO_CHECK(Read(manager) == "READY=1\nSTATUS=drm MAINPID=1");
}

// Refused rather than guessed at. `vsock:` is a third address form this does not implement, and
// treating one as a path would produce a compositor that never becomes ready with nothing said.
GYRO_TEST(Notify, AnAddressThatNamesNoUnixSocketIsRefused)
{
	GYRO_CHECK(!NotifyReady("vsock:2:1234", "drm").has_value());
	GYRO_CHECK(!NotifyReady("notify", "drm").has_value());
	GYRO_CHECK(!NotifyReady("", "drm").has_value());
}

// `sun_path` is 108 bytes and a longer address would be copied over the end of it.
GYRO_TEST(Notify, AnAddressLongerThanASocketPathIsRefused)
{
	const std::string path = "/" + std::string(sizeof(sockaddr_un::sun_path), 'x');
	const Result<void> told = NotifyReady(path, "drm");

	GYRO_REQUIRE(!told.has_value());
	GYRO_CHECK(told.error().Code() == ENAMETOOLONG);
}

// The absence of a service manager is the ordinary state of a development run, so it is an answer the
// caller branches on rather than a failure.
GYRO_TEST(Notify, NoServiceManagerIsAnEmptyAddress)
{
	::unsetenv("NOTIFY_SOCKET");

	GYRO_CHECK(ServiceManagerAddress().empty());

	::setenv("NOTIFY_SOCKET", "@gyro", 1);

	GYRO_CHECK(ServiceManagerAddress() == "@gyro");

	::unsetenv("NOTIFY_SOCKET");
}
