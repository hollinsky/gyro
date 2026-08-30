#include <array>
#include <cstring>
#include <format>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include "Compositor/Compositor.h"
#include "Compositor/Options.h"
#include "Version.h"

// The entry point, and deliberately almost nothing.
//
// `--version` and `--help` answer and exit without constructing anything, because a person asking
// either of them on a machine where gyro cannot start still deserves an answer. Everything else is
// handed to Compositor/Options.h, which is total, and then to the composition root, which is the only
// thing in the process that knows what an implementation is.

static void PrintVersion()
{
	std::cout << AppName << " " << AppVersion << std::endl;
}

static void PrintUsage()
{
	std::cout << "Usage: " << AppName << " [options]\n"
			  << "\n"
			  << "Options:\n"
			  << "  --help              Show this help message\n"
			  << "  --version           Show version information\n"
			  << "\n"
			  << "  --backend=KIND      auto, headless, nested, drm, or dump (headless and dump\n"
			  << "                      are built)\n"
			  << "  --dump=DIR          Where --backend=dump writes its frames, one PAM per\n"
			  << "                      presented frame, named for the frame's own sequence.\n"
			  << "                      The default is ./gyro-frames\n"
			  << "  --output[=SPEC]     Add an output, as [CONNECTOR:]WIDTHxHEIGHT@REFRESH, or a\n"
			  << "                      refresh rate alone, or a connector alone. CONNECTOR is the\n"
			  << "                      kernel's name for the panel, as eDP-1 or DP-7; requests that\n"
			  << "                      name none bind in the order the card enumerates them.\n"
			  << "                      Repeat for several; the default is one 1920x1080@60\n"
			  << "  --gym[=NAME]        Author one of gyro's own scenes instead of hosting clients;\n"
			  << "                      bare is lanes\n"
			  << "  --socket[=NAME]     Host clients on this Wayland socket; bare picks the first\n"
			  << "                      free wayland-N. This is what a bare run already does\n"
			  << "  --control[=PATH]    Take every listener from a session agent over this control\n"
			  << "                      socket instead of binding one; bare is /run/gyro/control\n"
			  << "  --no-socket         Host nothing and author nothing: no dispatch thread, and a\n"
			  << "                      frame loop compositing an empty scene\n"
			  << "  --cost=MS           What a planned composite is charged, in milliseconds\n"
			  << "  --floor=MS          What a floor composite is charged, in milliseconds\n"
			  << "  --lead=MS           What the frame loop holds ahead of the instant it armed\n"
			  << "                      for, in milliseconds. Covers the wakeup and the driver's\n"
			  << "                      commit-to-latch path; the default is 0.5\n"
			  << "  --composite=KIND    Pin every frame to one composite: planned drops a frame\n"
			  << "                      where it would have stepped down, floor draws the cheap one\n"
			  << "                      whatever the deadline allowed. The default is auto\n"
			  << "  --frames=N          Run at most N iterations, stopping early once the loop\n"
			  << "                      reaches idle. The default runs until SIGINT or SIGTERM\n"
			  << "  --trace[=PATH]      Write a Perfetto trace when the run ends. The ring records\n"
			  << "                      either way; SIGUSR1 writes a numbered snapshot at any time.\n"
			  << "                      The default is ./gyro.pftrace\n"
			  << "  --trace-buffer=N    Per-thread trace ring, as bytes or 32M. Zero records nothing\n"
			  << "  --realtime          Ask for SCHED_FIFO even under a hosted backend\n"
			  << "  --no-realtime       Do not ask for SCHED_FIFO\n"
			  << "  --priority=N        SCHED_FIFO priority, 1 to 99\n"
			  << "  --no-governor       Do not probe the GPU's response to a stated frame deadline,\n"
			  << "                      and do not command its minimum clock where there is none\n"
			  << "\n";

	// From the vocabulary rather than from a list written here, which is the whole reason `--gym`'s own
	// error message declines to name the scenes: this is where somebody reads them, and it cannot drift.
	std::cout << "Gyms:\n";

	for (const GymKind gym : AllGyms)
	{
		std::cout << std::format("  {:<18}{}\n", Name(gym), Describe(gym));
	}

	std::cout << "\n"
			  << "Running with no options starts the compositor.\n";
}

int main(int argc, char** argv)
{
	std::vector<std::string_view> arguments;
	arguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));

	for (int index = 1; index < argc; ++index)
	{
		const std::string_view argument = argv[index];

		if (argument == "--version" || argument == "-v")
		{
			PrintVersion();

			return 0;
		}

		if (argument == "--help" || argument == "-h")
		{
			PrintUsage();

			return 0;
		}

		arguments.push_back(argument);
	}

	const Result<Options> options = ParseOptions(arguments);

	if (!options)
	{
		// Which argument, where one argument is at fault. `ParseOptions` reports what is wrong and not
		// where, because an `Error` carries a `string_view` and a literal cannot name a runtime value —
		// so the naming happens here, in the one place that still has the list. Re-parsing each argument
		// alone finds it, since the grammar is per argument; a failure no single argument reproduces is a
		// *combination* — `--floor` above `--cost` is the one that exists — and those messages already say
		// what the combination was.
		for (const std::string_view argument : arguments)
		{
			if (!ParseOptions({ &argument, 1 }))
			{
				std::cerr << std::format("{}: {}\n", argument, options.error());
				PrintUsage();

				return 1;
			}
		}

		std::cerr << std::format("{}\n", options.error());
		PrintUsage();

		return 1;
	}

	if (const Result<void> ran = Run(*options); !ran)
	{
		std::cerr << std::format("{}\n", ran.error());

		return 1;
	}

	return 0;
}
