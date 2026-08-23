#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "Core/Result.h"
#include "Core/Time.h"
#include "Frame/Admission.h"

// What the composition root was asked to construct, parsed from the command line and nothing else.
//
// It is a plain aggregate with a total parse in front of it, and both halves are deliberate. gyro is
// a boot service — Docs/Architecture.md#boot-and-the-display-lifetime has it starting before nearly
// everything, on a machine with no VT to read a stack trace from — so an unparseable argument has to
// produce a sentence naming the argument rather than a default silently taken. And the parse is pure,
// so the one part of startup a person types into is the part that can be tested without a GPU, a
// seat, or a ring.

enum class BackendKind : std::uint8_t
{
	// WAYLAND_DISPLAY set picks nested, DRM otherwise. Docs/Architecture.md#selection.
	Auto = 0,
	Headless = 1,
	Nested = 2,
	Drm = 3,

	// A real presenter whose consumer is a file. Virtual/Dump.h writes one PAM per presented frame,
	// which is what gives gyro a picture of itself before there is a panel or a protocol — and what
	// paces frames while doing it, since a virtual output has a period and a phase exactly as a panel
	// does. The presenter is `Virtual`, the renderer is `Blit`, and nothing on the path needs a GPU.
	Dump = 4,
};

[[nodiscard]] constexpr std::string_view Name(BackendKind backend) noexcept
{
	switch (backend)
	{
		case BackendKind::Auto:
			return "auto";
		case BackendKind::Headless:
			return "headless";
		case BackendKind::Nested:
			return "nested";
		case BackendKind::Drm:
			return "drm";
		case BackendKind::Dump:
			return "dump";
	}

	return "?";
}

// One output as the command line asked for it, before anything has been asked whether it can be had.
struct OutputRequest
{
	std::int64_t Width = 1920;
	std::int64_t Height = 1080;

	// Hertz rather than a period, because that is the unit a panel and a person both quote. The
	// conversion is Core/Time.h's and happens once, where the configuration is built.
	double Refresh = 60.0;
};

// Where `--backend=dump` writes, when the command line does not say. Relative, because a boot
// service that scattered images across an absolute path nobody named would be worse than one that
// filled the directory somebody ran it from.
inline constexpr std::string_view DefaultDumpDirectory = "gyro-frames";

struct Options
{
	BackendKind Backend = BackendKind::Auto;

	// Where the dump backend writes its frames. Empty under every other backend, and never read from
	// the environment: this header is the command line and nothing else, which is what lets the one
	// part of startup a person types into be tested without a seat. Virtual/Pam.h's `GYRO_FRAME_DUMP`
	// is a separate knob for a separate audience — a test that has already failed — and keeping the
	// two apart is cheaper than a precedence rule nobody remembers.
	std::string DumpDirectory;

	std::array<OutputRequest, MaxOutputs> Outputs{};
	std::size_t OutputCount = 0;

	// SCHED_FIFO, mlockall, and RLIMIT_RTTIME. Default on, and Docs/Architecture.md#backends makes
	// headless and nested force it off anyway — a real-time thread inside a normal-priority host is an
	// effective way to hard-lock the desktop somebody is developing on. `--realtime` is the explicit
	// override that rule names, which is why the two are separate fields: one is what was asked for and
	// the other is whether the asking was deliberate enough to beat a safety rule.
	bool RealTime = true;
	bool RealTimeForced = false;

	// SCHED_FIFO priority for the frame thread. Below the kernel's own threaded IRQ handlers by
	// default, which is where a userspace real-time thread belongs.
	int Priority = 50;

	// What the simulated renderer charges per frame under the headless backend. This is decision 29's
	// `C` supplied by hand, which is the only way it can be supplied before there is a renderer that
	// measures one — and it is what makes the binary useful for pointing at a rate combination to see
	// what admission control does with it.
	Duration PlannedCost = std::chrono::microseconds{ 2'000 };
	Duration FloorCost = std::chrono::microseconds{ 500 };

	// Stop after this many iterations rather than running until signalled. Zero is *until signalled*,
	// which is the ordinary case; anything else is a smoke test that terminates on its own.
	std::uint64_t Iterations = 0;

	// The outputs actually requested, which is the default single 1080p60 when the command line named
	// none. Returning a span keeps the "none means one" rule in one place rather than at each reader.
	[[nodiscard]] std::span<const OutputRequest> Requested() const noexcept { return { Outputs.data(), OutputCount }; }
};

namespace Detail
{

[[nodiscard]] inline bool ParseInteger(std::string_view text, std::int64_t& into) noexcept
{
	if (text.empty())
	{
		return false;
	}

	const char* const last = text.data() + text.size();
	const std::from_chars_result result = std::from_chars(text.data(), last, into);

	return result.ec == std::errc{} && result.ptr == last;
}

[[nodiscard]] inline bool ParseReal(std::string_view text, double& into) noexcept
{
	if (text.empty())
	{
		return false;
	}

	const char* const last = text.data() + text.size();
	const std::from_chars_result result = std::from_chars(text.data(), last, into);

	return result.ec == std::errc{} && result.ptr == last;
}

// `WIDTHxHEIGHT@REFRESH`, with every part optional and the whole of it optional too. `--output` alone
// is 1920x1080 at 60, `--output=144` is that resolution at 144 Hz, and `--output=2560x1440@144` is
// what somebody with the panel in front of them writes.
[[nodiscard]] inline Result<OutputRequest> ParseOutput(std::string_view text)
{
	OutputRequest request{};

	if (text.empty())
	{
		return request;
	}

	std::string_view geometry = text;

	if (const std::size_t at = text.find('@'); at != std::string_view::npos)
	{
		geometry = text.substr(0, at);

		if (!ParseReal(text.substr(at + 1), request.Refresh) || !(request.Refresh > 0.0))
		{
			return Failure(EINVAL, "--output refresh must be a positive number of hertz");
		}
	}

	if (geometry.empty())
	{
		return request;
	}

	const std::size_t by = geometry.find('x');

	if (by == std::string_view::npos)
	{
		// No separator, so the whole of it is a refresh rate — the short form somebody sweeping rate
		// combinations types, and the reason `--output=144` does not have to name a resolution.
		if (!ParseReal(geometry, request.Refresh) || !(request.Refresh > 0.0))
		{
			return Failure(EINVAL, "--output wants WIDTHxHEIGHT@REFRESH, or a refresh rate alone");
		}

		return request;
	}

	if (!ParseInteger(geometry.substr(0, by), request.Width) ||
	    !ParseInteger(geometry.substr(by + 1), request.Height) || request.Width <= 0 || request.Height <= 0)
	{
		return Failure(EINVAL, "--output resolution must be positive integers as WIDTHxHEIGHT");
	}

	return request;
}

[[nodiscard]] inline bool Matches(std::string_view argument, std::string_view name, std::string_view& value) noexcept
{
	if (!argument.starts_with(name))
	{
		return false;
	}

	const std::string_view rest = argument.substr(name.size());

	if (rest.empty())
	{
		value = {};

		return true;
	}

	if (rest.front() != '=')
	{
		return false;
	}

	value = rest.substr(1);

	return true;
}

[[nodiscard]] inline Result<Duration> ParseMilliseconds(std::string_view text)
{
	double milliseconds = 0.0;

	if (!ParseReal(text, milliseconds) || !(milliseconds >= 0.0))
	{
		return Failure(EINVAL, "a cost is a non-negative number of milliseconds");
	}

	return DurationFromSeconds(milliseconds / 1'000.0);
}

} // namespace Detail

// Total: every argument either sets a field or produces an error naming itself. There is no argument
// this accepts and ignores, because an ignored argument on a boot service is a configuration somebody
// believes is in effect.
[[nodiscard]] inline Result<Options> ParseOptions(std::span<const std::string_view> arguments)
{
	Options options{};

	for (const std::string_view argument : arguments)
	{
		std::string_view value;

		if (Detail::Matches(argument, "--backend", value))
		{
			if (value == "auto")
			{
				options.Backend = BackendKind::Auto;
			}
			else if (value == "headless")
			{
				options.Backend = BackendKind::Headless;
			}
			else if (value == "nested")
			{
				options.Backend = BackendKind::Nested;
			}
			else if (value == "drm")
			{
				options.Backend = BackendKind::Drm;
			}
			else if (value == "dump")
			{
				options.Backend = BackendKind::Dump;
			}
			else
			{
				return Failure(EINVAL, "--backend is one of auto, headless, nested, drm, dump");
			}

			continue;
		}

		if (Detail::Matches(argument, "--dump", value))
		{
			if (value.empty())
			{
				return Failure(EINVAL, "--dump wants a directory to write frames into");
			}

			options.DumpDirectory = value;

			continue;
		}

		if (Detail::Matches(argument, "--output", value))
		{
			if (options.OutputCount >= options.Outputs.size())
			{
				return Failure(EINVAL, "more outputs than the frame loop admits");
			}

			const Result<OutputRequest> request = Detail::ParseOutput(value);

			if (!request)
			{
				return std::unexpected{ request.error() };
			}

			options.Outputs[options.OutputCount] = *request;
			++options.OutputCount;

			continue;
		}

		if (Detail::Matches(argument, "--cost", value))
		{
			const Result<Duration> cost = Detail::ParseMilliseconds(value);

			if (!cost)
			{
				return std::unexpected{ cost.error() };
			}

			options.PlannedCost = *cost;

			continue;
		}

		if (Detail::Matches(argument, "--floor", value))
		{
			const Result<Duration> cost = Detail::ParseMilliseconds(value);

			if (!cost)
			{
				return std::unexpected{ cost.error() };
			}

			options.FloorCost = *cost;

			continue;
		}

		if (Detail::Matches(argument, "--priority", value))
		{
			std::int64_t priority = 0;

			if (!Detail::ParseInteger(value, priority) || priority < 1 || priority > 99)
			{
				return Failure(EINVAL, "--priority is a SCHED_FIFO priority in [1, 99]");
			}

			options.Priority = static_cast<int>(priority);

			continue;
		}

		if (Detail::Matches(argument, "--frames", value))
		{
			std::int64_t frames = 0;

			if (!Detail::ParseInteger(value, frames) || frames < 0)
			{
				return Failure(EINVAL, "--frames is a non-negative iteration count, or zero to run until signalled");
			}

			options.Iterations = static_cast<std::uint64_t>(frames);

			continue;
		}

		if (argument == "--realtime")
		{
			options.RealTime = true;
			options.RealTimeForced = true;

			continue;
		}

		if (argument == "--no-realtime")
		{
			options.RealTime = false;
			options.RealTimeForced = false;

			continue;
		}

		return Failure(EINVAL, "unknown option");
	}

	// The default is one ordinary panel. Spelled here rather than in the aggregate's initializer so that
	// `OutputCount` means *what was asked for* everywhere it is read, and the default is one place.
	if (options.OutputCount == 0)
	{
		options.Outputs[0] = OutputRequest{};
		options.OutputCount = 1;
	}

	// The floor is what decision 35's second branch renders, so a floor above the planned cost is a
	// configuration where the recovery is more expensive than the thing it recovers from.
	if (options.FloorCost > options.PlannedCost)
	{
		return Failure(EINVAL, "--floor cannot exceed --cost");
	}

	// A destination under a backend that writes nothing is the case this header refuses to accept
	// quietly: somebody who typed `--dump` believes frames are being written, and a run that says
	// nothing leaves them looking for a directory that will never appear.
	if (!options.DumpDirectory.empty() && options.Backend != BackendKind::Dump)
	{
		return Failure(EINVAL, "--dump names where the dump backend writes, so it wants --backend=dump");
	}

	if (options.Backend == BackendKind::Dump && options.DumpDirectory.empty())
	{
		options.DumpDirectory = DefaultDumpDirectory;
	}

	return options;
}
