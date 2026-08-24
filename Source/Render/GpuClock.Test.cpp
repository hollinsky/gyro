#include "Render/GpuClock.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "Core/Time.h"
#include "Testing/Test.h"

// The clock reader's logic, without a `/sys` to read it from.
//
// **What is testable here is the two halves that run without hardware**: the per-driver mapping from a
// driver name and a DRM minor to a sysfs path, and the read path itself — parse, rate-limit, divisor,
// and what a bad read does — driven against a plain file the test wrote. The parts that genuinely need
// a machine — reading the bound driver off the DRM node's symlink, and scanning msm's devfreq — are
// the composition `Open` does over these, and are covered by decision 142's hardware bring-up rather
// than here, where there is no `i915` to bind.

namespace
{
[[nodiscard]] std::filesystem::path Scratch(std::string_view name)
{
	return std::filesystem::temp_directory_path() / std::filesystem::path{ name };
}

void Put(const std::filesystem::path& path, std::string_view content)
{
	std::ofstream out{ path, std::ios::trunc };
	out << content;
}
} // namespace

// i915 carries a legacy node and the newer per-`gt` one, both in MHz, and the legacy one is preferred
// so the reader does not depend on a kernel new enough to have moved it.
GYRO_TEST(GpuClock, I915ResolvesBothNodesInMhz)
{
	const GpuClock::Source source = GpuClock::Resolve("i915", 0);

	GYRO_REQUIRE(source.Count == 2);
	GYRO_CHECK(source.Divisor == 1);
	GYRO_CHECK(source.Candidates[0] == "/sys/dev/char/226:0/gt_act_freq_mhz");
	GYRO_CHECK(source.Candidates[1] == "/sys/dev/char/226:0/gt/gt0/rps_act_freq_mhz");
}

// xe reports MHz too, at its own node, and the minor is threaded into the path so the right GPU's
// clock is read on a machine with more than one.
GYRO_TEST(GpuClock, XeResolvesOneNodeUnderItsMinor)
{
	const GpuClock::Source source = GpuClock::Resolve("xe", 1);

	GYRO_REQUIRE(source.Count == 1);
	GYRO_CHECK(source.Divisor == 1);
	GYRO_CHECK(source.Candidates[0] == "/sys/dev/char/226:1/device/tile0/gt0/freq0/act_freq");
}

// msm reports Hz through devfreq, so it carries the divisor but no fixed candidate — the node name is
// dynamic and resolved at `Open`.
GYRO_TEST(GpuClock, MsmCarriesTheHzDivisorAndNoFixedNode)
{
	const GpuClock::Source source = GpuClock::Resolve("msm", 0);

	GYRO_CHECK(source.Count == 0);
	GYRO_CHECK(source.Divisor == 1'000'000);
}

// A driver the reader does not cover resolves to nothing, which is what makes `Open` warn and file a
// zero clock rather than guess a path.
GYRO_TEST(GpuClock, AnUnknownDriverResolvesToNothing)
{
	GYRO_CHECK(GpuClock::Resolve("amdgpu", 0).Count == 0);
	GYRO_CHECK(GpuClock::Resolve("", 0).Count == 0);
}

// An invalid clock — every device gyro cannot read one off — answers zero forever.
GYRO_TEST(GpuClock, AnInvalidClockSamplesZero)
{
	GpuClock clock;

	GYRO_CHECK(!clock.IsValid());
	GYRO_CHECK(clock.Sample(Monotonic::FromMicroseconds(100'000)) == 0);
}

// A node that does not open leaves the clock invalid rather than throwing.
GYRO_TEST(GpuClock, AMissingNodeIsInvalid)
{
	GpuClock clock = GpuClock::OpenPath("/sys/dev/char/226:0/this-node-does-not-exist", 1);

	GYRO_CHECK(!clock.IsValid());
	GYRO_CHECK(clock.Sample(Monotonic::FromMicroseconds(100'000)) == 0);
}

// The read path against a file the test wrote: a plain MHz value comes back as itself.
GYRO_TEST(GpuClock, ReadsAValueInMhz)
{
	const std::filesystem::path path = Scratch("gyro-gpuclock-mhz");
	Put(path, "350\n");

	GpuClock clock = GpuClock::OpenPath(path.c_str(), 1);
	GYRO_REQUIRE(clock.IsValid());

	GYRO_CHECK(clock.Sample(Monotonic::FromMicroseconds(100'000)) == 350);

	std::filesystem::remove(path);
}

// devfreq's Hz is divided down to MHz, which is the whole of what the divisor is for.
GYRO_TEST(GpuClock, DividesHzToMhz)
{
	const std::filesystem::path path = Scratch("gyro-gpuclock-hz");
	Put(path, "500000000\n");

	GpuClock clock = GpuClock::OpenPath(path.c_str(), 1'000'000);
	GYRO_REQUIRE(clock.IsValid());

	GYRO_CHECK(clock.Sample(Monotonic::FromMicroseconds(100'000)) == 500);

	std::filesystem::remove(path);
}

// The rate limit: a second sample inside the refresh interval answers from the cache without re-reading
// — so a value that changed underneath is not seen until the interval has passed. This is the property
// that lets the frame thread call `Sample` every frame.
GYRO_TEST(GpuClock, RateLimitsReadsToTheRefreshInterval)
{
	const std::filesystem::path path = Scratch("gyro-gpuclock-ratelimit");
	Put(path, "350\n");

	GpuClock clock = GpuClock::OpenPath(path.c_str(), 1);
	GYRO_REQUIRE(clock.IsValid());

	const Instant base = Monotonic::FromMicroseconds(100'000);
	GYRO_CHECK(clock.Sample(base) == 350);

	// Changed underneath, but a sample one millisecond later is inside the ten-millisecond interval and
	// still reports the cached value.
	Put(path, "700\n");
	GYRO_CHECK(clock.Sample(Advanced(base, std::chrono::milliseconds{ 1 })) == 350);

	// Past the interval, the new value is read.
	GYRO_CHECK(clock.Sample(Advanced(base, std::chrono::milliseconds{ 11 })) == 700);

	std::filesystem::remove(path);
}

// A read that fails to parse keeps the last good value rather than reporting a spurious zero the cost
// window would have to tell apart from a device that genuinely measures none.
GYRO_TEST(GpuClock, AnUnparseableReadKeepsTheLastValue)
{
	const std::filesystem::path path = Scratch("gyro-gpuclock-garbage");
	Put(path, "350\n");

	GpuClock clock = GpuClock::OpenPath(path.c_str(), 1);
	GYRO_REQUIRE(clock.IsValid());

	const Instant base = Monotonic::FromMicroseconds(100'000);
	GYRO_CHECK(clock.Sample(base) == 350);

	Put(path, "not-a-number\n");
	GYRO_CHECK(clock.Sample(Advanced(base, std::chrono::milliseconds{ 11 })) == 350);

	std::filesystem::remove(path);
}
