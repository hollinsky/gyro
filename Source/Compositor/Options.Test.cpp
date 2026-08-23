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
	GYRO_CHECK(!Parse({ "--outputs=3" }).has_value());
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
