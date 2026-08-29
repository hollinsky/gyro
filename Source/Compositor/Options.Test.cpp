#include "Compositor/Options.h"

#include <array>
#include <chrono>
#include <format>
#include <span>
#include <string>
#include <string_view>

#include "Core/Time.h"
#include "Testing/Test.h"

// What is worth testing here is not that a flag sets a field. It is the three ways a command line can
// be wrong on a machine with no console to complain to: an argument that is quietly ignored, a value
// that fails to parse and is replaced by a default, and a pair of figures that parse individually and
// contradict each other. All three produce a compositor running a configuration nobody asked for, and
// the third produces one whose recovery path is more expensive than the thing it recovers from.

namespace
{
using namespace std::chrono_literals;

Result<Options> Parse(std::initializer_list<std::string_view> arguments)
{
	return ParseOptions(std::span<const std::string_view>{ arguments.begin(), arguments.size() });
}
} // namespace

GYRO_TEST(Options, DefaultsToOneOrdinaryPanel)
{
	const Result<Options> options = Parse({});

	GYRO_REQUIRE(options.has_value());
	GYRO_CHECK_EQ(options->OutputCount, std::size_t{ 1 });
	GYRO_CHECK_EQ(options->Requested()[0].Width, std::int64_t{ 1920 });
	GYRO_CHECK_EQ(options->Requested()[0].Height, std::int64_t{ 1080 });
	GYRO_CHECK(options->Requested()[0].Refresh == 60.0);
	GYRO_CHECK(options->Backend == BackendKind::Auto);
}

// The short form is the one somebody sweeping rate combinations types, and it is the reason a
// resolution is optional rather than the reason a refresh is.
GYRO_TEST(Options, ARefreshRateAloneIsAnOutput)
{
	const Result<Options> options = Parse({ "--output=144", "--output=60" });

	GYRO_REQUIRE(options.has_value());
	GYRO_CHECK_EQ(options->OutputCount, std::size_t{ 2 });
	GYRO_CHECK(options->Requested()[0].Refresh == 144.0);
	GYRO_CHECK(options->Requested()[1].Refresh == 60.0);
	GYRO_CHECK_EQ(options->Requested()[0].Width, std::int64_t{ 1920 });
}

GYRO_TEST(Options, AFullSpecNamesBothHalves)
{
	const Result<Options> options = Parse({ "--output=2560x1440@144" });

	GYRO_REQUIRE(options.has_value());
	GYRO_CHECK_EQ(options->Requested()[0].Width, std::int64_t{ 2560 });
	GYRO_CHECK_EQ(options->Requested()[0].Height, std::int64_t{ 1440 });
	GYRO_CHECK(options->Requested()[0].Refresh == 144.0);
}

GYRO_TEST(Options, BareOutputIsTheDefaultPanel)
{
	const Result<Options> options = Parse({ "--output", "--output" });

	GYRO_REQUIRE(options.has_value());
	GYRO_CHECK_EQ(options->OutputCount, std::size_t{ 2 });
	GYRO_CHECK(options->Requested()[1].Refresh == 60.0);
}

// The case this file exists for. An unknown argument is a configuration somebody believes is in
// effect, and on a boot service there is nowhere for them to find out otherwise.
GYRO_TEST(Options, AnUnknownArgumentIsRefused)
{
	GYRO_CHECK(!Parse({ "--monitors=3" }).has_value());
	GYRO_CHECK(!Parse({ "--backend" }).has_value());
	GYRO_CHECK(!Parse({ "-x" }).has_value());
	GYRO_CHECK(!Parse({ "1080p" }).has_value());
}

// A partial parse is the same defect wearing a plausible value: `--output=144x` reads as a width with
// the height missing, and taking 144 as a refresh rate there would be a guess rather than an answer.
GYRO_TEST(Options, APartialValueIsRefusedRatherThanGuessedAt)
{
	GYRO_CHECK(!Parse({ "--output=144x" }).has_value());
	GYRO_CHECK(!Parse({ "--output=x900" }).has_value());
	GYRO_CHECK(!Parse({ "--output=1920x1080@" }).has_value());
	GYRO_CHECK(!Parse({ "--output=1920x1080@0" }).has_value());
	GYRO_CHECK(!Parse({ "--output=-1920x1080" }).has_value());
	GYRO_CHECK(!Parse({ "--cost=1.5ms" }).has_value());
	GYRO_CHECK(!Parse({ "--priority=0" }).has_value());
	GYRO_CHECK(!Parse({ "--priority=100" }).has_value());
}

GYRO_TEST(Options, CostsArriveInMilliseconds)
{
	const Result<Options> options = Parse({ "--cost=4", "--floor=0.5" });

	GYRO_REQUIRE(options.has_value());
	GYRO_CHECK_EQ(options->PlannedCost, 4ms);
	GYRO_CHECK_EQ(options->FloorCost, 500us);
}

// Unset rather than defaulted here, because the figure it overrides lives in the composition root and
// a default in two places is a default that drifts.
GYRO_TEST(Options, TheArmingLeadIsAnOverrideRatherThanAValue)
{
	const Result<Options> bare = Parse({});

	GYRO_REQUIRE(bare.has_value());
	GYRO_CHECK(!bare->Lead.has_value());

	const Result<Options> led = Parse({ "--lead=1.5" });

	GYRO_REQUIRE(led.has_value());
	GYRO_REQUIRE(led->Lead.has_value());
	GYRO_CHECK_EQ(*led->Lead, 1500us);

	// Zero is the permissive reading Timing.h names and has to survive the parse, since it is the arm
	// of a sweep that says what the lead was worth at all.
	const Result<Options> none = Parse({ "--lead=0" });

	GYRO_REQUIRE(none.has_value());
	GYRO_REQUIRE(none->Lead.has_value());
	GYRO_CHECK_EQ(*none->Lead, Duration::zero());

	GYRO_CHECK(!Parse({ "--lead=-1" }).has_value());
	GYRO_CHECK(!Parse({ "--lead=2ms" }).has_value());
}

// Each of these parses; together they describe a machine whose floor composite costs more than the
// frame it is the recovery for, which decision 35's second branch cannot mean.
GYRO_TEST(Options, AFloorAboveTheCostIsRefused)
{
	GYRO_CHECK(!Parse({ "--cost=1", "--floor=2" }).has_value());
	GYRO_CHECK(Parse({ "--cost=2", "--floor=2" }).has_value());
}

// Two fields rather than one, because the safety rule in Docs/Architecture.md#backends has to be able
// to tell *on by default* from *asked for on purpose*: only the second beats a hosted backend forcing
// it off.
GYRO_TEST(Options, RealTimeRecordsWhetherItWasAskedForDeliberately)
{
	const Result<Options> silent = Parse({});
	const Result<Options> asked = Parse({ "--realtime" });
	const Result<Options> declined = Parse({ "--realtime", "--no-realtime" });

	GYRO_REQUIRE(silent.has_value() && asked.has_value() && declined.has_value());

	GYRO_CHECK(silent->RealTime && !silent->RealTimeForced);
	GYRO_CHECK(asked->RealTime && asked->RealTimeForced);
	GYRO_CHECK(!declined->RealTime && !declined->RealTimeForced);
}

// The ordering between the two resolution rules, and the regression that made it a function: the
// safety rule reads the backend, so it has to run after `Auto` has been settled. Read the other way
// round, `Auto` is not `Drm`, and gyro booting on a panel with nothing typed — which is how it boots
// — ran its frame thread at normal priority and dropped frames nobody could account for.
GYRO_TEST(Options, AutoOnAPanelKeepsRealTime)
{
	const Result<Options> silent = Parse({});

	GYRO_REQUIRE(silent.has_value());

	const Options panel = ResolveOptions(*silent, false);
	const Options nested = ResolveOptions(*silent, true);

	GYRO_CHECK(panel.Backend == BackendKind::Drm && panel.RealTime);
	GYRO_CHECK(nested.Backend == BackendKind::Nested && !nested.RealTime);
}

// The safety rule itself, and the one thing that beats it. A hosted backend gives up `SCHED_FIFO`
// whether it was chosen by name or by `Auto`, because a real-time thread inside a normal-priority
// host hard-locks the desktop somebody is developing on.
GYRO_TEST(Options, AHostedBackendGivesUpRealTimeUnlessAskedByName)
{
	const Result<Options> silent = Parse({ "--backend=nested" });
	const Result<Options> asked = Parse({ "--backend=nested", "--realtime" });

	GYRO_REQUIRE(silent.has_value() && asked.has_value());

	GYRO_CHECK(!ResolveOptions(*silent, true).RealTime);
	GYRO_CHECK(ResolveOptions(*asked, true).RealTime);
}

GYRO_TEST(Options, MoreOutputsThanTheLoopAdmitsIsRefused)
{
	std::array<std::string_view, MaxOutputs + 1> arguments{};
	arguments.fill("--output=60");

	GYRO_CHECK(!ParseOptions(std::span<const std::string_view>{ arguments }).has_value());
	GYRO_CHECK(ParseOptions(std::span<const std::string_view>{ arguments.data(), MaxOutputs }).has_value());
}

GYRO_TEST(Options, TheDumpBackendIsSelectedByName)
{
	const Result<Options> options = Parse({ "--backend=dump" });

	GYRO_REQUIRE(options.has_value());
	GYRO_CHECK(options->Backend == BackendKind::Dump);
	GYRO_CHECK_EQ(Name(options->Backend), std::string_view{ "dump" });

	// Somewhere rather than nowhere, because a dump that wrote nothing until a second argument was
	// supplied would be a backend that looks like it ran.
	GYRO_CHECK_EQ(options->DumpDirectory, std::string{ DefaultDumpDirectory });
}

GYRO_TEST(Options, ADumpDirectoryReplacesTheDefault)
{
	const Result<Options> options = Parse({ "--backend=dump", "--dump=/tmp/gyro-run" });

	GYRO_REQUIRE(options.has_value());
	GYRO_CHECK_EQ(options->DumpDirectory, std::string{ "/tmp/gyro-run" });
}

// Order-independent, because the check that pairs them runs after the loop rather than inside it.
GYRO_TEST(Options, ADumpDirectoryWithoutTheDumpBackendIsRefused)
{
	GYRO_CHECK(!Parse({ "--dump=/tmp/gyro-run" }).has_value());
	GYRO_CHECK(!Parse({ "--backend=headless", "--dump=/tmp/gyro-run" }).has_value());
	GYRO_CHECK(!Parse({ "--dump=/tmp/gyro-run", "--backend=headless" }).has_value());
	GYRO_CHECK(Parse({ "--dump=/tmp/gyro-run", "--backend=dump" }).has_value());
}

// An empty destination is a typo — `--dump=` with the path left off — rather than a request for the
// default, which `--backend=dump` alone already is.
GYRO_TEST(Options, AnEmptyDumpDirectoryIsRefused)
{
	GYRO_CHECK(!Parse({ "--backend=dump", "--dump=" }).has_value());
	GYRO_CHECK(!Parse({ "--backend=dump", "--dump" }).has_value());
}

// Every backend the parse accepts has a name, and a name nothing accepts is not one of them.
GYRO_TEST(Options, EveryBackendNameRoundTrips)
{
	for (const std::string_view name : { "auto", "headless", "nested", "drm", "dump" })
	{
		const std::string argument = std::format("--backend={}", name);
		const Result<Options> options = Parse({ argument });

		GYRO_REQUIRE(options.has_value());
		GYRO_CHECK_EQ(Name(options->Backend), name);
	}

	GYRO_CHECK(!Parse({ "--backend=virtual" }).has_value());
}

// `--outputs=N` is the nested backend's one host window each, and Docs/Architecture.md#nested-wayland
// names it first: multi-monitor layout without owning three monitors.
GYRO_TEST(Options, OutputsPadsWithWhatOutputSaid)
{
	const Result<Options> three = Parse({ "--outputs=3" });

	GYRO_REQUIRE(three.has_value());
	GYRO_REQUIRE_EQ(three->OutputCount, std::size_t{ 3 });

	// Three of the default panel, because that is what `--output` would have said if it had been
	// asked.
	for (const OutputRequest& request : three->Requested())
	{
		GYRO_CHECK_EQ(request.Width, std::int64_t{ 1920 });
		GYRO_CHECK(request.Refresh == 60.0);
	}

	// And with a geometry given, three of *that* — which is the whole ergonomic point, since typing one
	// geometry twice is how a sweep ends up with two rates it did not mean.
	const Result<Options> sized = Parse({ "--output=1280x720@90", "--outputs=2" });

	GYRO_REQUIRE(sized.has_value());
	GYRO_REQUIRE_EQ(sized->OutputCount, std::size_t{ 2 });
	GYRO_CHECK_EQ(sized->Requested()[1].Width, std::int64_t{ 1280 });
	GYRO_CHECK(sized->Requested()[1].Refresh == 90.0);
}

GYRO_TEST(Options, OutputsBelowWhatWasSpelledOutIsAContradiction)
{
	// Two geometries and then a request for one of them. Truncating would drop an output somebody
	// described, which is the reading this header refuses to take quietly.
	GYRO_CHECK(!Parse({ "--output=1280x720", "--output=1920x1080", "--outputs=1" }).has_value());

	GYRO_CHECK(!Parse({ "--outputs=0" }).has_value());
	GYRO_CHECK(!Parse({ "--outputs" }).has_value());
	GYRO_CHECK(!Parse({ "--outputs=99" }).has_value());
}

// **The default is no world at all**, and the two cases below are the ones that would each ship a
// compositor doing something nobody asked for: a gym running because a name was misread, and a gym
// silently not running because one was.
GYRO_TEST(Options, NoGymIsTheDefaultAndBareGymIsTheLanes)
{
	const Result<Options> none = Parse({});

	GYRO_REQUIRE(none.has_value());
	GYRO_CHECK(!none->Gym.has_value());

	// Both spellings of *say nothing about which*, which is `--output`'s precedent rather than
	// `--dump`'s: a directory has no default to fall back to and a gym does.
	for (const std::string_view argument : { "--gym", "--gym=" })
	{
		const Result<Options> bare = Parse({ argument });

		GYRO_REQUIRE(bare.has_value());
		GYRO_REQUIRE(bare->Gym.has_value());
		GYRO_CHECK(*bare->Gym == GymKind::Lanes);
	}
}

// Over the vocabulary rather than over a list written here, so that a gym added to Gym/Gym.h is
// covered by this the moment it exists — which is the same property `--help` gets by printing from
// `AllGyms`, and the reason the parse's error message names no scenes of its own.
GYRO_TEST(Options, EveryGymNameRoundTrips)
{
	for (const GymKind gym : AllGyms)
	{
		const std::string argument = std::format("--gym={}", Name(gym));
		const Result<Options> options = Parse({ argument });

		GYRO_REQUIRE(options.has_value());
		GYRO_REQUIRE(options->Gym.has_value());
		GYRO_CHECK(*options->Gym == gym);
	}

	GYRO_CHECK(!Parse({ "--gym=splines" }).has_value());
}

// The pin is absent by default, because the per-frame check is what gyro does and a run that quietly
// drew one tier forever would be a compositor nobody asked for — which is this file's whole subject.
GYRO_TEST(Options, TheCompositeIsChosenPerFrameUnlessPinned)
{
	const Result<Options> defaulted = Parse({});
	const Result<Options> planned = Parse({ "--composite=planned" });
	const Result<Options> floor = Parse({ "--composite=floor" });

	GYRO_REQUIRE(defaulted.has_value());
	GYRO_REQUIRE(planned.has_value());
	GYRO_REQUIRE(floor.has_value());
	GYRO_CHECK(!defaulted->Composite.has_value());
	GYRO_CHECK(planned->Composite == RenderMode::Planned);
	GYRO_CHECK(floor->Composite == RenderMode::Floor);
	GYRO_CHECK(!Parse({ "--composite=floor", "--composite=auto" })->Composite.has_value());
}

GYRO_TEST(Options, ACompositeThatNamesNoTierIsRefused)
{
	GYRO_CHECK(!Parse({ "--composite=cheap" }).has_value());
	GYRO_CHECK(!Parse({ "--composite" }).has_value());
}
