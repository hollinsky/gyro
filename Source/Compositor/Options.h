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
#include "Scene/Density.h"
#include "Seam/Renderer.h"
#include "Session/Handover.h"

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
	// Which panel this is about, as the kernel names the connector — `eDP-1`, `DP-7`, `HDMI-A-1`.
	// Empty is *the next one that nothing has claimed*, which is what every request was before this
	// field existed and what an ordinary run still means.
	//
	// **A name because the position is the kernel's rather than a person's.** Requests bind to
	// connectors in the order the card enumerates them, so on a laptop the internal panel is always
	// first and there was no way to say *drive the dock and not the lid* — the one arrangement
	// somebody with a docked laptop asks for by name. Positional order is kept for the requests that
	// do not name anything, because a person with one monitor should not have to learn what it is
	// called.
	std::string Connector;

	std::int64_t Width = 1920;
	std::int64_t Height = 1080;

	// Hertz rather than a period, because that is the unit a panel and a person both quote. The
	// conversion is Core/Time.h's and happens once, where the configuration is built.
	double Refresh = 60.0;

	// How far this panel's viewer is from it, in millimetres, or nothing for the form-factor prior.
	//
	// **The one term in decision 164's derivation that is on nobody's connector**, and the reason the
	// entry wants a setup: the same laptop panel is at 350 mm on a lap and 700 mm shoved aside as a
	// third screen, and EDID cannot tell those apart. `Scene/Density.h`'s `SeededDistance` guesses from
	// form factor and is right often enough to boot into; this is a person overriding that guess for
	// one panel, and it *overrides* rather than replaces — an output nobody names still takes the prior.
	//
	// **A setup fact riding a mode request, which is what it is until there is a setup.** Everything
	// else on this struct is what was asked of the panel; a viewing distance is a fact about the room.
	// It lives here anyway because the flag is already keyed by connector and a second one would
	// duplicate the whole binding rule, positional fallback included — the same expedient decision 141
	// takes when the Floorplanner stands in for an absent shell.
	std::optional<std::int32_t> DistanceMm{};
};

// Where `--backend=dump` writes, when the command line does not say. Relative, because a boot
// service that scattered images across an absolute path nobody named would be worse than one that
// filled the directory somebody ran it from.
inline constexpr std::string_view DefaultDumpDirectory = "gyro-frames";

// Where a trace snapshot lands when the command line does not say. Relative for the dump directory's
// reason, and named for what opens it rather than for gyro — a person who has one of these in a
// directory a month from now needs the extension to tell them what to do with it.
inline constexpr std::string_view DefaultTracePath = "gyro.pftrace";

// Where `--capture` puts a screenshot when the command line does not say. Relative for the dump
// directory's reason, and a different directory from that one because the two are different things:
// `--backend=dump` writes every frame an output presents, and this writes the frames a person asked
// for by name.
inline constexpr std::string_view DefaultCaptureDirectory = "gyro-captures";

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

	// How big a logical pixel should be at the eye — `--ui-size` — which decision 164 makes the only
	// density figure on the machine: one number for every output, from which each derives its own scale out of its own
	// pitch and how far away it is. A *person's* number rather than a panel's, which is why it is not
	// per-output the way the distance beside it is.
	//
	// **It is a size and deliberately not a scale.** What every other system exposes here is a scale,
	// which couples the artefact-free point to the size choice and pins the bottom of the ladder to
	// whatever panel somebody happens to own — decision 164 is the argument, and `--ui-size` is the
	// whole of what it buys a person on the command line.
	// **Named for what the angle is of rather than for the flag that sets it.** A logical pixel is the
	// unit a window's size, a margin, an icon and a requested font size are all in, and the two things
	// a reader here will otherwise assume — that this is a font size, or that it is a scale — are both
	// wrong. The type carries whose preference it is; the name carries what it is a preference about.
	AngularPreference LogicalPixelAngle{};

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

	// What the frame loop holds ahead of the instant it arms for, overriding the composition root's
	// figure. Nothing is *use the compiled-in one*, which is the answer for every run that is not
	// measuring this.
	//
	// **A flag because the figure it overrides is not one gyro can be confident of.** The arming lead
	// covers the wakeup, and it also has to cover everything between the atomic commit and the point
	// the display engine latches it — which is a property of somebody else's driver, measured at
	// between one and a quarter and two milliseconds on one Tiger Lake panel and unknown everywhere
	// else. A constant chosen against one machine is a constant that drops frames on the next, and the
	// only way to learn the shape of the curve is to be able to move the figure without a rebuild.
	//
	// Bounded above by nothing, here or anywhere else. The period is the real ceiling and only the
	// frame clock knows it, so this header cannot name one without naming a panel — but nothing
	// downstream refuses an overlong lead either: `Timing::WakeFor`'s third floor arms for the next
	// frame gyro can still be early for, so a lead longer than a refresh is absorbed as a refresh of
	// latency nobody is told about rather than rejected at startup.
	std::optional<Duration> Lead{};

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

	// Where `Ctrl+Alt+Esc S` writes, and whether the verb works at all.
	//
	// **A flag rather than always on, and Seam/Capture.h is where that argument lives.** Reading a
	// composite back needs the target created for it, target usage is fixed at allocation, and asking
	// for it unconditionally puts an extra bit into the modifier negotiation between the Vulkan device
	// and the panel on every run — where a modifier dropped from that list is a window that stops being
	// scanned out directly. That is a cost on every frame of every session, paid for a key that fires
	// when somebody is hunting a bug. So a run says up front that it wants captures, exactly as it says
	// up front that it wants a trace written at exit, and the chord says which flag is missing where it
	// is off rather than doing nothing visible.
	std::string CaptureDirectory;
	bool Capture = false;

	// Snapshot on the first frame that lands a refresh after the one it was aimed at. The miss being
	// hunted recurs about as often as the ring is long, so waiting for it with a finger on `SIGUSR1`
	// means catching the right half-minute by hand; this is the loop making the request the moment it
	// sees the landing. Off by default, because on a loaded machine late landings are ordinary — the
	// latch-lead ratchet's business rather than a file each.
	bool TraceOnMiss = false;

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
	//
	// Unread where `ControlPath` is set, and the two are refused together below.
	std::string Socket{};

	// The control socket session agents offer their listeners on, or empty for a run that binds its own.
	//
	// **Set, gyro binds no Wayland socket at all.** Docs/Architecture.md#listener-handover inverts
	// socket creation — the user's agent creates the listener in the user's own runtime directory,
	// because gyro cannot `chown` one into place — so what this run offers instead is a rendezvous, and
	// a machine whose agent never connects is a compositor with a screen and no clients rather than a
	// failure. That is why it is a path rather than a boolean: `/run/gyro/control` is where a boot
	// service puts one, and a development run puts it under its own runtime directory beside everything
	// else it owns.
	std::string ControlPath{};

	// A PAM to show behind everything, or empty for a machine with none.
	//
	// **A path on a command line is the stand-in and the descriptor is the destination.** A background
	// belongs to a shell — it is the one piece of what a person sees that is a *setting* rather than a
	// mechanism — and what a shell will do is hand a descriptor over the session handover, for
	// `Session/Handover.h`'s reason: the party that opened the file is the party entitled to it. Until
	// there is a shell, gyro opens the path itself and hands the descriptor to the same verb, so the
	// arrival of one changes what calls `PamImage::Read` and nothing below it.
	//
	// **One image and not a list**, because gyro holds one background: an image is shown on the outputs
	// whose device extent it matches exactly and on no others (`Scene/Background.h`), and a machine
	// whose panels differ is one where the shell hands over a new image as the person's attention moves
	// rather than one where gyro picks from a set it was given at startup.
	std::string BackgroundPath{};

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

// `CONNECTOR:WIDTHxHEIGHT@REFRESH`, with every part optional and the whole of it optional too.
// `--output` alone is 1920x1080 at 60, `--output=144` is that resolution at 144 Hz,
// `--output=2560x1440@144` is what somebody with the panel in front of them writes, and
// `--output=DP-7` is what somebody with two of them writes when only one is the subject.
//
// A connector as the kernel spells one: a type and an index joined by a hyphen. **The shape is the
// test rather than the leading letter**, because the colon is optional — `--output=DP-7` names a
// panel and says nothing else, so there is no separator to find, and a rule that took any leading
// letter would read the malformed `x900` as a monitor nobody has instead of refusing it. This is
// `Drm/Device.cpp`'s `ConnectorName` read backwards, which is the only definition either side has.
[[nodiscard]] inline constexpr bool IsConnectorName(std::string_view text) noexcept
{
	const bool letter =
		!text.empty() && ((text.front() >= 'a' && text.front() <= 'z') || (text.front() >= 'A' && text.front() <= 'Z'));

	return letter && text.find('-') != std::string_view::npos;
}

// How far a viewer can be said to be, in millimetres. Wide enough to hold a phone at arm's length
// and a projector across a hall, and narrow enough that a decimal point in the wrong place is
// refused rather than derived from.
inline constexpr std::int64_t MinimumDistanceMm = 100;
inline constexpr std::int64_t MaximumDistanceMm = 10'000;

// One `KEY=VALUE` out of `--output`'s comma-separated tail. Named rather than a fourth positional
// field because the positional grammar is out of separators, and because every further setup fact a
// panel acquires lands the same way rather than growing another one.
[[nodiscard]] inline Result<void> ParseOutputField(std::string_view field, OutputRequest& request)
{
	constexpr std::string_view Distance = "distance=";

	if (!field.starts_with(Distance))
	{
		return Failure(EINVAL, "--output takes distance=NNNmm after a comma, and no other field yet");
	}

	std::string_view text = field.substr(Distance.size());

	// The unit is mandatory for `--ui-size`'s reason: a bare number is a number in whatever unit the
	// reader assumed, and this one is load-bearing for three things at once.
	if (!text.ends_with("mm"))
	{
		return Failure(EINVAL, "--output distance wants millimetres, as distance=700mm");
	}

	text.remove_suffix(2);

	std::int64_t millimetres = 0;

	if (!ParseInteger(text, millimetres) || millimetres < MinimumDistanceMm || millimetres > MaximumDistanceMm)
	{
		return Failure(EINVAL, "--output distance is how far away the viewer is, in millimetres, from 100 to 10000");
	}

	request.DistanceMm = static_cast<std::int32_t>(millimetres);

	return {};
}

[[nodiscard]] inline Result<OutputRequest> ParseOutput(std::string_view text)
{
	OutputRequest request{};

	if (text.empty())
	{
		return request;
	}

	// A comma-separated tail of named fields in front of the positional grammar, and **a piece
	// carrying an `=` is named wherever it sits** — so `--output=distance=700mm` is the whole request
	// for somebody with one panel and nothing to say about its mode, and the positional head stays
	// exactly what it was for everybody who never types a comma.
	{
		std::string_view head{};
		std::string_view rest = text;
		bool first = true;

		while (!rest.empty())
		{
			const std::size_t comma = rest.find(',');
			const std::string_view piece = rest.substr(0, comma);

			rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);

			if (piece.find('=') != std::string_view::npos)
			{
				if (const Result<void> parsed = ParseOutputField(piece, request); !parsed)
				{
					return std::unexpected{ parsed.error() };
				}
			}
			else if (first)
			{
				head = piece;
			}
			else
			{
				return Failure(EINVAL, "--output is one mode and then named fields, as DP-1:2560x1440,distance=700mm");
			}

			first = false;
		}

		text = head;
	}

	if (text.empty())
	{
		return request;
	}

	const std::size_t colon = text.find(':');

	if (IsConnectorName(text.substr(0, colon)))
	{
		request.Connector.assign(text.substr(0, colon));

		// A name on its own is the whole request, and it means this connector at whatever mode the
		// chooser settles on — which is the same answer a resolution the panel cannot offer already gets.
		if (colon == std::string_view::npos)
		{
			return request;
		}

		text = text.substr(colon + 1);

		if (text.empty())
		{
			return request;
		}
	}
	else if (colon != std::string_view::npos)
	{
		return Failure(EINVAL, "--output wants a connector name as TYPE-N before the colon");
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

// The band `--ui-size` accepts, in arcminutes. The useful range is about 0.95 to 1.9 — 20/20 acuity
// at one end and the critical print size at the other, per Scene/Density.h — and this is far wider
// on purpose: guard rails stop a typo, not a taste. What they actually catch is a missing decimal
// point, since `134arcmin` is the way this flag will be got wrong.
inline constexpr double MinimumArcminutes = 0.5;
inline constexpr double MaximumArcminutes = 10.0;

// `1.34arcmin`, with the unit mandatory. The name says the effect and the value says the quantity,
// which is what keeps a flag that sizes the whole interface from reading as a font size — a font size
// is a different lever and lives in the toolkit.
//
// **The angle is one logical pixel's**, which is the unit a window's size, a margin, an icon and a
// requested font size are all in. Text moves with it because a toolkit asks for its fonts in those
// units, not because this flag knows what a font is.
[[nodiscard]] inline Result<AngularPreference> ParseUiSize(std::string_view text)
{
	constexpr std::string_view Unit = "arcmin";

	if (!text.ends_with(Unit))
	{
		return Failure(EINVAL, "--ui-size wants an angle carrying its unit, as --ui-size=1.34arcmin");
	}

	text.remove_suffix(Unit.size());

	double arcminutes = 0.0;

	if (!ParseReal(text, arcminutes) || !(arcminutes >= MinimumArcminutes) || !(arcminutes <= MaximumArcminutes))
	{
		return Failure(EINVAL, "--ui-size is how big a logical pixel is at the eye, from 0.5 to 10 arcminutes");
	}

	return AngularPreference::FromArcminutes(arcminutes);
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

		if (Detail::Matches(argument, "--control", value))
		{
			options.Clients = true;
			options.ControlPath = value.empty() ? std::string{ Session::DefaultControlPath } : std::string{ value };

			continue;
		}

		if (Detail::Matches(argument, "--background", value))
		{
			if (value.empty())
			{
				return Failure(EINVAL, "--background wants a path to a PAM image");
			}

			options.BackgroundPath = value;

			continue;
		}

		if (argument == "--no-socket")
		{
			options.Clients = false;
			options.Socket.clear();
			options.ControlPath.clear();
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

		if (Detail::Matches(argument, "--capture", value))
		{
			// Bare is the default directory, for the reason `--trace` bare is the default path: somebody
			// typing the flag wants the pictures rather than an argument about where they go.
			options.Capture = true;

			if (!value.empty())
			{
				options.CaptureDirectory = value;
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

		if (Detail::Matches(argument, "--trace-on-miss", value))
		{
			// No value to take: which anomaly triggers is the frame loop's to know, and a flag that
			// swallowed one would read `--trace-on-miss=2` as the plain arming somebody thought they
			// refined.
			if (!value.empty())
			{
				return Failure(EINVAL, "--trace-on-miss takes no value");
			}

			options.TraceOnMiss = true;

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

		if (Detail::Matches(argument, "--ui-size", value))
		{
			const Result<AngularPreference> preference = Detail::ParseUiSize(value);

			if (!preference)
			{
				return std::unexpected{ preference.error() };
			}

			options.LogicalPixelAngle = *preference;

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

		if (Detail::Matches(argument, "--lead", value))
		{
			const Result<Duration> lead = Detail::ParseMilliseconds(value);

			if (!lead)
			{
				// `ParseMilliseconds`' own sentence names a cost, which this is not. The figure is the
				// same shape and the word is wrong, so the message is this flag's own.
				return Failure(EINVAL, "--lead is a non-negative number of milliseconds");
			}

			options.Lead = *lead;

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
	if (options.Capture && options.CaptureDirectory.empty())
	{
		options.CaptureDirectory = DefaultCaptureDirectory;
	}

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

	if (options.Gym && !options.ControlPath.empty())
	{
		return Failure(EINVAL, "--gym authors the scene itself, so no session agent has anything to offer it");
	}

	// **Two ways to get a listener, and only one of them has a session behind it.** A socket gyro bound
	// itself admits every connection, because there is no uid to check one against and no agent whose
	// going away ends it — so a run doing both would be tracking sessions carefully on one path while
	// leaving the other open beside it, which is the kind of configuration somebody believes is secure.
	if (socketNamed && !options.ControlPath.empty())
	{
		return Failure(EINVAL, "--control takes every listener from a session agent, so there is no --socket to bind");
	}

	if (options.Gym)
	{
		options.Clients = false;
	}

	// The trigger watches the ring, so arming it while switching the ring off is two figures that parse
	// individually and contradict each other — the third way a command line is wrong, and the one whose
	// recovery is a hunt that silently records nothing.
	if (options.TraceOnMiss && options.TraceBytes == 0)
	{
		return Failure(EINVAL, "--trace-on-miss watches the ring that --trace-buffer=0 switched off");
	}

	return options;
}

// What the command line asked for, turned into what is actually going to be constructed.
//
// Three rules and an ordering between them, which is the whole reason this is a function rather than
// a few lines at the top of `Run`. `Auto` picks the panel where there is no host to nest in; a
// hosted backend gives up `SCHED_FIFO` unless somebody asked for it by name; and a request that
// names a connector is refused under a backend that has none. Both later rules have to read the
// backend the *first* one settled on. Read the other way round, `Auto` is not `Drm`, so gyro booting
// on a panel with no arguments at all — which is how it boots — quietly ran the frame thread at
// normal priority and missed frames nobody could account for.
//
// `host` is passed in rather than read from the environment here so that the ordering is testable on
// any machine; `Run` is where `WAYLAND_DISPLAY` is looked at.
[[nodiscard]] inline Result<Options> ResolveOptions(const Options& options, bool host) noexcept
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

	// **A named connector under a backend with no connectors is refused rather than ignored**, for
	// `--dump`'s reason: somebody who typed `--output=DP-7` believes a panel has been chosen, and a
	// nested or headless run binds the request to nothing of the kind. Here rather than beside
	// `--device`'s parse-time twin because the parse cannot see this one — `--output=DP-7` with no
	// backend typed is drm on a panel and nested inside a desktop session, and only the second is
	// wrong, so the rule has to read the backend `Auto` settled on.
	if (resolved.Backend != BackendKind::Drm)
	{
		for (const OutputRequest& request : resolved.Requested())
		{
			if (!request.Connector.empty())
			{
				return Failure(
					EINVAL, "--output names a connector only the drm backend can drive, so it wants --backend=drm"
				);
			}
		}
	}

	return resolved;
}
