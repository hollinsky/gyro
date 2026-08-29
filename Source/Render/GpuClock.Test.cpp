#include "Render/GpuClock.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "Testing/Test.h"

// The clock reader's logic, without a `/sys` to read it from.
//
// **What is testable here is the two halves that run without hardware**: the per-driver mapping from a
// driver name and a DRM minor to a pair of sysfs paths, and the read path itself — parse, divisor, the
// two halves against each other, and what a bad read does — driven against plain files the test wrote.
// The parts that genuinely need a machine — reading the bound driver off the DRM node's symlink, and
// scanning msm's devfreq — are the composition `Open` does over these, and are covered by decision
// 142's hardware bring-up rather than here, where there is no `i915` to bind.

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
	GYRO_CHECK(source.Actual[0] == "/sys/dev/char/226:0/gt_act_freq_mhz");
	GYRO_CHECK(source.Actual[1] == "/sys/dev/char/226:0/gt/gt0/rps_act_freq_mhz");
}

// The commanded point comes from the same generation of node as the actual one it sits at the index of
// — a kernel that moved `gt_act_freq_mhz` under `gt/gt0` moved `gt_cur_freq_mhz` with it, and reading a
// legacy actual against a per-`gt` requested would be two parts' numbers presented as one operating
// point.
GYRO_TEST(GpuClock, I915PairsEachActualNodeWithItsRequestedOne)
{
	const GpuClock::Source source = GpuClock::Resolve("i915", 0);

	GYRO_REQUIRE(source.Count == 2);
	GYRO_CHECK(source.Requested[0] == "/sys/dev/char/226:0/gt_cur_freq_mhz");
	GYRO_CHECK(source.Requested[1] == "/sys/dev/char/226:0/gt/gt0/rps_cur_freq_mhz");
}

// xe reports MHz too, at its own node, and the minor is threaded into the path so the right GPU's
// clock is read on a machine with more than one.
GYRO_TEST(GpuClock, XeResolvesOneNodeUnderItsMinor)
{
	const GpuClock::Source source = GpuClock::Resolve("xe", 1);

	GYRO_REQUIRE(source.Count == 1);
	GYRO_CHECK(source.Divisor == 1);
	GYRO_CHECK(source.Actual[0] == "/sys/dev/char/226:1/device/tile0/gt0/freq0/act_freq");
	GYRO_CHECK(source.Requested[0] == "/sys/dev/char/226:1/device/tile0/gt0/freq0/cur_freq");
}

// msm reports Hz through devfreq, so it carries the divisor but no fixed candidate — the node name is
// dynamic and resolved at `Open`. The split kernel names the DRM device after the display driver
// (`msm_dpu`), so the family test covers both it and the monolithic `msm`.
GYRO_TEST(GpuClock, MsmCarriesTheHzDivisorAndNoFixedNode)
{
	GYRO_CHECK(GpuClock::Resolve("msm", 0).Count == 0);
	GYRO_CHECK(GpuClock::Resolve("msm", 0).Divisor == 1'000'000);
	GYRO_CHECK(GpuClock::Resolve("msm_dpu", 0).Count == 0);
	GYRO_CHECK(GpuClock::Resolve("msm_dpu", 0).Divisor == 1'000'000);
}

// The msm driver split: the device behind a DRM minor is the display controller's, bound as
// `msm_dpu`/`msm_mdp`, and the GPU is a separate platform device — the family test is what sends both
// the clock and the floor to the GPU's devfreq rather than to the DRM device's own subtree.
GYRO_TEST(GpuClock, TheSplitDisplayDriversAreStillMsm)
{
	GYRO_CHECK(GpuClock::IsMsmDriver("msm"));
	GYRO_CHECK(GpuClock::IsMsmDriver("msm_dpu"));
	GYRO_CHECK(GpuClock::IsMsmDriver("msm_mdp"));
	GYRO_CHECK(GpuClock::IsMsmDriver("msm_mdp4"));
	GYRO_CHECK(!GpuClock::IsMsmDriver("i915"));
	GYRO_CHECK(!GpuClock::IsMsmDriver("xe"));
	GYRO_CHECK(!GpuClock::IsMsmDriver("amdgpu"));
	GYRO_CHECK(!GpuClock::IsMsmDriver(""));
}

// A driver the reader does not cover resolves to nothing, which is what makes `Open` warn and file a
// zero clock rather than guess a path.
GYRO_TEST(GpuClock, AnUnknownDriverResolvesToNothing)
{
	GYRO_CHECK(GpuClock::Resolve("amdgpu", 0).Count == 0);
	GYRO_CHECK(GpuClock::Resolve("", 0).Count == 0);
}

// An invalid clock — every device gyro cannot read one off — answers zeros forever.
GYRO_TEST(GpuClock, AnInvalidClockReadsZero)
{
	GpuClock clock;

	GYRO_CHECK(!clock.IsValid());
	GYRO_CHECK(clock.Read() == GpuClock::Reading{});
}

// A node that does not open leaves the clock invalid rather than throwing.
GYRO_TEST(GpuClock, AMissingNodeIsInvalid)
{
	GpuClock clock =
		GpuClock::OpenPath("/sys/dev/char/226:0/this-node-does-not-exist", "/sys/dev/char/226:0/nor-this", 1);

	GYRO_CHECK(!clock.IsValid());
	GYRO_CHECK(clock.Read() == GpuClock::Reading{});
}

// The read path against files the test wrote: plain MHz values come back as themselves, and the two
// halves come back as the halves they were opened as rather than as one number twice.
GYRO_TEST(GpuClock, ReadsBothValuesInMhz)
{
	const std::filesystem::path actual = Scratch("gyro-gpuclock-act");
	const std::filesystem::path requested = Scratch("gyro-gpuclock-cur");
	Put(actual, "350\n");
	Put(requested, "1150\n");

	GpuClock clock = GpuClock::OpenPath(actual.c_str(), requested.c_str(), 1);
	GYRO_REQUIRE(clock.IsValid());

	GYRO_CHECK(clock.Read() == GpuClock::Reading{ .ActualMhz = 350, .RequestedMhz = 1150 });

	std::filesystem::remove(actual);
	std::filesystem::remove(requested);
}

// **The case the pair exists for**: a parked part reports an actual of zero against a commanded point
// the governor is still holding. One number would make that the cheapest frame the cost window ever
// saw; two say it was asleep.
GYRO_TEST(GpuClock, AParkedPartReadsZeroAgainstItsCommandedPoint)
{
	const std::filesystem::path actual = Scratch("gyro-gpuclock-parked-act");
	const std::filesystem::path requested = Scratch("gyro-gpuclock-parked-cur");
	Put(actual, "0\n");
	Put(requested, "1150\n");

	GpuClock clock = GpuClock::OpenPath(actual.c_str(), requested.c_str(), 1);
	GYRO_REQUIRE(clock.IsValid());

	GYRO_CHECK(clock.Read() == GpuClock::Reading{ .ActualMhz = 0, .RequestedMhz = 1150 });

	std::filesystem::remove(actual);
	std::filesystem::remove(requested);
}

// There is no rate limit: a value that changed underneath is seen by the very next read. This is what
// makes the pair one instant's reading rather than a fresh half beside a cached one.
GYRO_TEST(GpuClock, EveryReadIsFresh)
{
	const std::filesystem::path actual = Scratch("gyro-gpuclock-fresh-act");
	const std::filesystem::path requested = Scratch("gyro-gpuclock-fresh-cur");
	Put(actual, "350\n");
	Put(requested, "350\n");

	GpuClock clock = GpuClock::OpenPath(actual.c_str(), requested.c_str(), 1);
	GYRO_REQUIRE(clock.IsValid());

	GYRO_CHECK(clock.Read() == GpuClock::Reading{ .ActualMhz = 350, .RequestedMhz = 350 });

	Put(actual, "700\n");
	Put(requested, "1150\n");
	GYRO_CHECK(clock.Read() == GpuClock::Reading{ .ActualMhz = 700, .RequestedMhz = 1150 });

	std::filesystem::remove(actual);
	std::filesystem::remove(requested);
}

// An absent commanded node does not take the clock down with it — the actual half is what a cost span
// is filed against, and the other one is context.
GYRO_TEST(GpuClock, AMissingRequestedNodeLeavesTheClockValid)
{
	const std::filesystem::path actual = Scratch("gyro-gpuclock-lonely");
	Put(actual, "350\n");

	GpuClock clock = GpuClock::OpenPath(actual.c_str(), "/sys/dev/char/226:0/this-node-does-not-exist", 1);
	GYRO_REQUIRE(clock.IsValid());

	GYRO_CHECK(clock.Read() == GpuClock::Reading{ .ActualMhz = 350, .RequestedMhz = 0 });

	std::filesystem::remove(actual);
}

// devfreq's Hz is divided down to MHz on both halves, which is the whole of what the divisor is for.
GYRO_TEST(GpuClock, DividesHzToMhz)
{
	const std::filesystem::path actual = Scratch("gyro-gpuclock-hz-act");
	const std::filesystem::path requested = Scratch("gyro-gpuclock-hz-cur");
	Put(actual, "500000000\n");
	Put(requested, "800000000\n");

	GpuClock clock = GpuClock::OpenPath(actual.c_str(), requested.c_str(), 1'000'000);
	GYRO_REQUIRE(clock.IsValid());

	GYRO_CHECK(clock.Read() == GpuClock::Reading{ .ActualMhz = 500, .RequestedMhz = 800 });

	std::filesystem::remove(actual);
	std::filesystem::remove(requested);
}

// A read that fails to parse keeps the last good value rather than reporting a spurious zero the cost
// window would have to tell apart from a device that genuinely measures none — and it keeps it per
// half, so one unreadable attribute does not discard the other's fresh value.
GYRO_TEST(GpuClock, AnUnparseableReadKeepsTheLastValue)
{
	const std::filesystem::path actual = Scratch("gyro-gpuclock-garbage-act");
	const std::filesystem::path requested = Scratch("gyro-gpuclock-garbage-cur");
	Put(actual, "350\n");
	Put(requested, "1150\n");

	GpuClock clock = GpuClock::OpenPath(actual.c_str(), requested.c_str(), 1);
	GYRO_REQUIRE(clock.IsValid());

	GYRO_CHECK(clock.Read() == GpuClock::Reading{ .ActualMhz = 350, .RequestedMhz = 1150 });

	Put(actual, "not-a-number\n");
	Put(requested, "700\n");
	GYRO_CHECK(clock.Read() == GpuClock::Reading{ .ActualMhz = 350, .RequestedMhz = 700 });

	std::filesystem::remove(actual);
	std::filesystem::remove(requested);
}
