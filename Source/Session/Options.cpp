#include "Session/Options.h"

#include <cerrno>
#include <cstddef>

namespace Session
{
namespace
{
// Whether `argument` is `--name` or `--name=value`, and what the value was.
[[nodiscard]] bool Matches(std::string_view argument, std::string_view name, std::string_view& value) noexcept
{
	if (!argument.starts_with("--"))
	{
		return false;
	}

	const std::string_view rest = argument.substr(2);

	if (rest == name)
	{
		value = {};

		return true;
	}

	if (rest.size() > name.size() && rest.starts_with(name) && rest[name.size()] == '=')
	{
		value = rest.substr(name.size() + 1);

		return true;
	}

	return false;
}
} // namespace

Result<AgentOptions> ParseAgentOptions(std::span<const std::string_view> arguments)
{
	AgentOptions options;

	std::size_t index = 0;

	for (; index < arguments.size(); ++index)
	{
		const std::string_view argument = arguments[index];

		if (argument == "--")
		{
			++index;

			break;
		}

		if (!argument.starts_with("--"))
		{
			break;
		}

		std::string_view value;

		if (Matches(argument, "control", value))
		{
			if (value.empty())
			{
				return Failure(EINVAL, "--control wants a path");
			}

			options.ControlPath = value;

			continue;
		}

		if (Matches(argument, "runtime-dir", value))
		{
			if (value.empty())
			{
				return Failure(EINVAL, "--runtime-dir wants a path");
			}

			options.RuntimeDirectory = value;

			continue;
		}

		if (Matches(argument, "display", value))
		{
			// A name with a separator in it would be bound somewhere other than the runtime directory,
			// which gyro's `InspectOffer` refuses — so it is caught here, where the message can say what
			// the argument was for rather than arriving as a rejected offer three steps later.
			if (value.empty() || value.contains('/'))
			{
				return Failure(EINVAL, "--display wants a name, not a path");
			}

			options.Display = value;

			continue;
		}

		if (Matches(argument, "respawn", value))
		{
			if (!value.empty())
			{
				return Failure(EINVAL, "--respawn takes no value");
			}

			options.Respawn = true;

			continue;
		}

		return Failure(EINVAL, "unrecognised option");
	}

	for (; index < arguments.size(); ++index)
	{
		options.Command.emplace_back(arguments[index]);
	}

	return options;
}
} // namespace Session
