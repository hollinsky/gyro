#include "Session/Options.h"

#include <array>
#include <cerrno>
#include <string>
#include <string_view>
#include <vector>

#include "Testing/Test.h"

namespace
{
using namespace Session;

template<std::size_t Count>
[[nodiscard]] Result<AgentOptions> Parse(const std::array<std::string_view, Count>& arguments)
{
	return ParseAgentOptions(arguments);
}
} // namespace

GYRO_TEST(AgentOptions, DefaultsToTheRendezvousAndNoCommand)
{
	const Result<AgentOptions> options = ParseAgentOptions({});
	GYRO_REQUIRE(options.has_value());

	GYRO_CHECK(options->ControlPath == DefaultControlPath);
	GYRO_CHECK(options->RuntimeDirectory.empty());
	GYRO_CHECK(options->Display.empty());
	GYRO_CHECK(!options->Respawn);
	GYRO_CHECK(options->Command.empty());
}

GYRO_TEST(AgentOptions, TakesEveryOption)
{
	const Result<AgentOptions> options = Parse(
		std::array<std::string_view, 4>{
			"--control=/tmp/control", "--runtime-dir=/tmp/run", "--display=wayland-4", "--respawn" }
	);
	GYRO_REQUIRE(options.has_value());

	GYRO_CHECK(options->ControlPath == "/tmp/control");
	GYRO_CHECK(options->RuntimeDirectory == "/tmp/run");
	GYRO_CHECK(options->Display == "wayland-4");
	GYRO_CHECK(options->Respawn);
}

GYRO_TEST(AgentOptions, TheFirstWordThatIsNotAnOptionBeginsTheCommand)
{
	const Result<AgentOptions> options = Parse(std::array<std::string_view, 3>{ "--respawn", "foot", "--login" });
	GYRO_REQUIRE(options.has_value());

	GYRO_CHECK(options->Respawn);
	GYRO_REQUIRE(options->Command.size() == 2);
	GYRO_CHECK(options->Command[0] == "foot");

	// The program's own option rather than the agent's, which is the whole reason the command is a
	// tail rather than a scan.
	GYRO_CHECK(options->Command[1] == "--login");
}

GYRO_TEST(AgentOptions, ADoubleDashBeginsACommandThatLooksLikeAnOption)
{
	const Result<AgentOptions> options = Parse(std::array<std::string_view, 3>{ "--", "--respawn", "x" });
	GYRO_REQUIRE(options.has_value());

	GYRO_CHECK(!options->Respawn);
	GYRO_REQUIRE(options->Command.size() == 2);
	GYRO_CHECK(options->Command[0] == "--respawn");
}

GYRO_TEST(AgentOptions, NamesWhatItWillNotTake)
{
	GYRO_CHECK(!Parse(std::array<std::string_view, 1>{ "--nonsense" }).has_value());
	GYRO_CHECK(!Parse(std::array<std::string_view, 1>{ "--control" }).has_value());
	GYRO_CHECK(!Parse(std::array<std::string_view, 1>{ "--display=" }).has_value());
	GYRO_CHECK(!Parse(std::array<std::string_view, 1>{ "--respawn=yes" }).has_value());

	// A display with a separator in it would be bound outside the runtime directory, which gyro
	// refuses — caught here, where the message can say what the argument was for.
	const Result<AgentOptions> path = Parse(std::array<std::string_view, 1>{ "--display=/tmp/wayland-0" });
	GYRO_REQUIRE(!path.has_value());
	GYRO_CHECK(path.error().Code() == EINVAL);
}

GYRO_TEST(AgentOptions, ShellSaysTheCommandIsTheSessionsShell)
{
	const std::array arguments{ std::string_view{ "--shell" }, std::string_view{ "gyro-shell" } };
	const Result<AgentOptions> options = Parse(arguments);

	GYRO_REQUIRE(options.has_value());
	GYRO_CHECK(options->Shell);
	GYRO_CHECK(options->Command == std::vector<std::string>{ "gyro-shell" });
}

// The default, and the one every session has had: no second listener, and nothing in the session can
// reach the System tier.
GYRO_TEST(AgentOptions, ASessionHasNoShellUnlessAskedFor)
{
	const std::array arguments{ std::string_view{ "foot" } };
	const Result<AgentOptions> options = Parse(arguments);

	GYRO_REQUIRE(options.has_value());
	GYRO_CHECK(!options->Shell);
}

// The two readings of a bare `--shell` are *bind a socket nobody will use* and *the command went
// missing*, and refusing says which one this build thinks a person meant.
GYRO_TEST(AgentOptions, ShellWithNoCommandIsRefused)
{
	const std::array arguments{ std::string_view{ "--shell" } };
	const Result<AgentOptions> options = Parse(arguments);

	GYRO_REQUIRE(!options.has_value());
	GYRO_CHECK_EQ(options.error().Code(), EINVAL);
}

GYRO_TEST(AgentOptions, ShellTakesNoValue)
{
	const std::array arguments{ std::string_view{ "--shell=yes" }, std::string_view{ "gyro-shell" } };

	GYRO_CHECK(!Parse(arguments).has_value());
}
