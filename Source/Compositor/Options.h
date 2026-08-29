#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "Core/Result.h"
#include "Core/Time.h"
#include "Frame/Admission.h"
#include "Gym/Gym.h"
#include "Seam/Renderer.h"

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

// Where a trace snapshot lands when the command line does not say. Relative for the dump directory's
// reason, and named for what opens it rather than for gyro — a person who has one of these in a
// directory a month from now needs the extension to tell them what to do with it.
inline constexpr std::string_view DefaultTracePath = "gyro.pftrace";

struct Options
{
	BackendKind Backend = BackendKind::Auto;

	// Where the dump backend writes its frames. Empty under every other backend, and never read from
	// the environment: this header is the command line and nothing else, which is what lets the one
	// part of startup a person types into be tested without a seat. Virtual/Pam.h's `GYRO_FRAME_DUMP`
	// is a separate knob for a separate audience — a test that has already failed — and keeping the
	// two apart is cheaper than a precedence rule nobody remembers.
	std::string DumpDirectory;

	// Which card node the DRM backend drives. Empty is *the first one with something connected*, which
	// is what a boot service with no configuration has to do — a laptop with a discrete GPU has two
	// card nodes and the panel is on one of them. Named for the case where that guess is wrong and for
	// the machine with two monitors on two cards, which gyro cannot yet drive at once.
	std::string Device;

	std::array<OutputRequest, MaxOutputs> Outputs{};
	std::size_t OutputCount = 0;

	// How many outputs there are, where saying it once is what somebody means.
	//
	// **`--outputs=3` is one host window each under nested**, which is the feature
	// Docs/Architecture.md#nested-wayland names first: multi-monitor layout, cross-output window drags
	// and per-output scale become testable without owning three monitors. It is a *count* rather than
	// a fourth field on `--output` because the thing being asked for is usually three of the same
	// panel, and typing the same geometry three times is how a sweep ends up with two of them at 60
	// and one at 59.94 by accident.
	//
	// Zero is *however many `--output` said*. Past that it pads with the last `--output`, so
	// `--output=1280x720 --outputs=2` is two 720p windows and `--outputs=3` alone is three of the
	// default panel. Fewer than the `--output`s given is a contradiction rather than a truncation, and
	// is refused.
	std::size_t WantedOutputs = 0;

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

	// Whether gyro probes the GPU's response to a stated deadline and takes the frequency floor where
	// there is none — decision 142, and Render/Governor.h is the whole of it.
	//
	// **On by default and worth a flag anyway, because it is the one thing gyro does that outlives
	// gyro.** The floor is a sysfs value that stays where it was put if the process is killed rather
	// than shut down, so a person debugging a crash loop wants a way to run without it. It also costs a
	// tenth of a second of startup on a machine that turns out to need the floor, which is a thing to be
	// able to take back out when measuring something else.
	//
	// It says nothing about the *deadline*: that is one ioctl per submission, it is always right to
	// send, and there is no configuration under which gyro declines to tell the driver what it knows.
	bool Governor = true;

	// Which composite every frame is drawn with, or nothing for the per-frame check that is what gyro
	// actually does.
	//
	// **`--composite=planned` makes an overrun cost a frame rather than a rung**, which is the only way
	// to watch a drop and the recovery after it: with the floor tier available the loop takes it, the
	// picture gets simpler for one frame, and nothing is ever late. **`--composite=floor` pins the cheap
	// composite whatever the deadline allowed**, which is the steady low load the frequency governor is
	// measured against.
	//
	// Neither is a configuration to run a desktop under, and both are honest about it: the pin changes
	// what is drawn and never what is admitted, so a frame that will not fit is still refused and the
	// schedule the sweep measures is the same one.
	std::optional<RenderMode> Composite{};

	// What the simulated renderer charges per frame under the headless backend. This is decision 29's
	// `C` supplied by hand, which is the only way it can be supplied before there is a renderer that
	// measures one — and it is what makes the binary useful for pointing at a rate combination to see
	// what admission control does with it.
	Duration PlannedCost = std::chrono::microseconds{ 2'000 };
	Duration FloorCost = std::chrono::microseconds{ 500 };

	// The always-armed trace ring, per recorded thread, in bytes. Zero records nothing at all.
	//
	// **Sized rather than timed, because how many seconds it buys is what the compositor is doing.** A
	// still desktop writes a handful of records a frame and a four-panel machine under animation writes
	// a few thousand a second; the report line at the end of a run prints what the ring actually held,
	// which is the figure to size this against. The default is a minute or so of an ordinary two-panel
	// machine — chosen because a person who noticed a stutter and reached for the keyboard takes a good
	// part of that to do it.
	std::size_t TraceBytes = 16U * 1024U * 1024U;

	// Where a snapshot goes. `--trace` writes one when the run ends; `SIGUSR1` writes one whenever it
	// arrives, numbered, and needs no flag — the ring is armed either way, which is the whole point of
	// it being armed at all.
	std::string TracePath{ DefaultTracePath };
	bool TraceAtExit = false;

	// Stop after this many iterations rather than running until signalled. Zero is *until signalled*,
	// which is the ordinary case; anything else is a smoke test that terminates on its own.
	std::uint64_t Iterations = 0;

	// Which scene gyro authors for itself, or nothing at all.
	//
	// **Nothing is the default and stays the default**, because a compositor whose only picture is an
	// instrument is one somebody eventually ships. Absent, the author is the client host below and the
	// run hosts windows; the floor case that used to live here — no author, no dispatch thread, an
	// empty scene — is `--no-socket`, and `Clients` carries why it still has a flag.
	//
	// **The kind rather than the name**, so that this header stays total: an unknown gym is an error
	// naming itself here rather than a failure in the composition root, which is where an option that
	// parses and then cannot be honoured always ends up. Gym/Gym.h owns the vocabulary and
	// `GymNamed` is the parse, so there is no second spelling of the list to drift.
	std::optional<GymKind> Gym{};

	// Whether this run hosts clients, and on which socket.
	//
	// **Hosting is the default, and it is the default because that is what a compositor is for.** With
	// no `--gym`, the author the dispatch loop steps is `ClientHost` — gyro's Wayland server standing
	// where a gym stands — so a bare `gyro` binds a socket and waits for somebody to connect to it. A
	// gym replaces that author rather than joining it: the loop steps one author, and a run cannot be
	// both an instrument and a compositor at the same time.
	//
	// **`--no-socket` is what keeps the floor case reachable in one command.** No gym, no clients, no
	// dispatch thread at all, and a frame loop compositing an empty scene — which is the case
	// Docs/Architecture.md#doing-nothing-must-cost-nothing is about, and the thing every idle
	// measurement is read against. It was the default before there was a server to make it worth
	// giving up, and losing it silently would have retired the measurement rather than the flag.
	bool Clients = true;

	// The name to bind, or empty for the first free `wayland-N` under `XDG_RUNTIME_DIR` — which is what
	// a client with nothing set finds. A name is for a second gyro on one machine, or for a test that
	// wants to know where to connect without reading a log line.
	std::string Socket{};

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

// `32M`, `512K`, or a plain count. A suffix because the number is a memory budget and nobody types
// sixteen million by hand — and binary multiples because what is being sized is an allocation rather
// than a disk.
[[nodiscard]] inline Result<std::size_t> ParseBytes(std::string_view text)
{
	if (text.empty())
	{
		return Failure(EINVAL, "a size is a byte count, optionally suffixed K or M");
	}

	std::size_t scale = 1;
	std::string_view digits = text;

	if (const char suffix = text.back(); suffix == 'K' || suffix == 'k')
	{
		scale = 1024;
		digits = text.substr(0, text.size() - 1);
	}
	else if (suffix == 'M' || suffix == 'm')
	{
		scale = 1024 * 1024;
		digits = text.substr(0, text.size() - 1);
	}

	std::int64_t count = 0;

	if (!ParseInteger(digits, count) || count < 0)
	{
		return Failure(EINVAL, "a size is a non-negative byte count, optionally suffixed K or M");
	}

	// A figure that cannot be allocated is a configuration error rather than a runtime one, and saying
	// so here is cheaper than a `bad_alloc` on a boot service.
	if (static_cast<std::uint64_t>(count) > (std::uint64_t{ 1 } << 40) / scale)
	{
		return Failure(EINVAL, "a size that large is not a buffer, it is a typo");
	}

	return static_cast<std::size_t>(count) * scale;
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
	// Whether a socket name was asked for, as opposed to inherited from the default. Only the asking
	// conflicts with a gym.
	bool socketNamed = false;

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

		if (Detail::Matches(argument, "--gym", value))
		{
			// Bare `--gym` is the lanes, which is the one that never settles and is therefore what
			// somebody who typed the flag to *see something move* meant.
			if (value.empty())
			{
				options.Gym = GymKind::Lanes;

				continue;
			}

			const std::optional<GymKind> gym = GymNamed(value);

			if (!gym)
			{
				// The vocabulary is deliberately not restated here. An `Error` carries a `string_view`,
				// so a list in this message would be a literal that drifts the first time a gym is added
				// — and Main.cpp prints the usage immediately after this sentence, from Gym/Gym.h's own
				// names and descriptions.
				return Failure(EINVAL, "--gym is not one of the scenes --help lists");
			}

			options.Gym = *gym;

			continue;
		}

		if (Detail::Matches(argument, "--socket", value))
		{
			options.Clients = true;
			options.Socket = std::string{ value };

			// Recorded so that `--gym --socket=foo` is refused below rather than binding a socket the
			// run then never serves. Bare `--socket` is the default said out loud and conflicts with
			// nothing, so it does not count as naming one.
			socketNamed = !value.empty();

			continue;
		}

		if (argument == "--no-socket")
		{
			options.Clients = false;
			options.Socket.clear();
			socketNamed = false;

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

		if (Detail::Matches(argument, "--device", value))
		{
			if (value.empty())
			{
				return Failure(EINVAL, "--device wants a DRM card node, such as /dev/dri/card0");
			}

			options.Device = value;

			continue;
		}

		if (Detail::Matches(argument, "--trace", value))
		{
			// Bare is the default path, because somebody typing the flag wants the file rather than an
			// argument about where it goes.
			options.TraceAtExit = true;

			if (!value.empty())
			{
				options.TracePath = value;
			}

			continue;
		}

		if (Detail::Matches(argument, "--trace-buffer", value))
		{
			const Result<std::size_t> bytes = Detail::ParseBytes(value);

			if (!bytes)
			{
				return Failure(EINVAL, "--trace-buffer is a per-thread ring size, as bytes or 32M");
			}

			options.TraceBytes = *bytes;

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

		if (Detail::Matches(argument, "--outputs", value))
		{
			std::int64_t count = 0;

			if (!Detail::ParseInteger(value, count) || count < 1 || static_cast<std::size_t>(count) > MaxOutputs)
			{
				return Failure(EINVAL, "--outputs is how many outputs to bring up, within what the frame loop admits");
			}

			options.WantedOutputs = static_cast<std::size_t>(count);

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

		if (Detail::Matches(argument, "--composite", value))
		{
			if (value == "planned")
			{
				options.Composite = RenderMode::Planned;
			}
			else if (value == "floor")
			{
				options.Composite = RenderMode::Floor;
			}
			else if (value == "auto")
			{
				options.Composite.reset();
			}
			else
			{
				return Failure(EINVAL, "--composite is one of auto, planned, floor");
			}

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

		if (argument == "--no-governor")
		{
			options.Governor = false;

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

	// `--outputs` last, because it pads with what `--output` said and the default above is one of those
	// answers. A count below what was spelled out is a contradiction: somebody wrote three geometries
	// and then asked for two, and dropping one silently is the reading nobody wants.
	if (options.WantedOutputs != 0)
	{
		if (options.WantedOutputs < options.OutputCount)
		{
			return Failure(EINVAL, "--outputs is fewer than the number of --output arguments given");
		}

		while (options.OutputCount < options.WantedOutputs)
		{
			options.Outputs[options.OutputCount] = options.Outputs[options.OutputCount - 1];
			++options.OutputCount;
		}
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

	// `--dump`'s refusal, for the same reason: somebody who named a card and got a nested window has
	// been ignored quietly. Auto is exempt, because a machine with no wayland host resolves it to drm.
	if (!options.Device.empty() && options.Backend != BackendKind::Drm && options.Backend != BackendKind::Auto)
	{
		return Failure(EINVAL, "--device names the card the drm backend drives, so it wants --backend=drm");
	}

	if (options.Backend == BackendKind::Dump && options.DumpDirectory.empty())
	{
		options.DumpDirectory = DefaultDumpDirectory;
	}

	// **A gym is an author and so is the client host, and the dispatch loop steps one.** Naming a
	// socket alongside a gym is somebody expecting to connect to a run that will never listen, which is
	// the same failure `--dump` under the wrong backend is refused for: it does not fail, it just never
	// does the thing that was asked for.
	if (options.Gym && socketNamed)
	{
		return Failure(EINVAL, "--gym authors the scene itself, so there is no socket for clients to reach");
	}

	if (options.Gym)
	{
		options.Clients = false;
	}

	return options;
}

// What the command line asked for, turned into what is actually going to be constructed.
//
// Two rules and an ordering between them, which is the whole reason this is a function rather than
// four lines at the top of `Run`. `Auto` picks the panel where there is no host to nest in, and a
// hosted backend gives up `SCHED_FIFO` unless somebody asked for it by name — and the second rule
// has to read the backend the *first* one settled on. Read the other way round, `Auto` is not `Drm`,
// so gyro booting on a panel with no arguments at all — which is how it boots — quietly ran the
// frame thread at normal priority and missed frames nobody could account for.
//
// `host` is passed in rather than read from the environment here so that the ordering is testable on
// any machine; `Run` is where `WAYLAND_DISPLAY` is looked at.
[[nodiscard]] inline Options ResolveOptions(const Options& options, bool host) noexcept
{
	Options resolved = options;

	// Docs/Architecture.md#selection. Auto never picks dump: writing files is something a person asks
	// for by name.
	if (resolved.Backend == BackendKind::Auto)
	{
		resolved.Backend = host ? BackendKind::Nested : BackendKind::Drm;
	}

	// Docs/Architecture.md#backends, enforced by the backend rather than by convention: headless and
	// nested force `SCHED_FIFO` and `mlockall` off unless explicitly overridden, because a real-time
	// thread inside a normal-priority host is an effective way to hard-lock the desktop somebody is
	// developing on. `--realtime` is the override that rule names, and it is the only thing that beats
	// this.
	if (resolved.Backend != BackendKind::Drm && !resolved.RealTimeForced)
	{
		resolved.RealTime = false;
	}

	return resolved;
}
