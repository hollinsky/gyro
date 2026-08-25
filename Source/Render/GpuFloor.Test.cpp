#include "Render/GpuFloor.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include "Testing/Test.h"

// The floor writer's logic, against files a test wrote rather than against a GPU.
//
// **A sysfs attribute is a small file, which is what makes this testable at all** — the read, the
// clamp, the never-downward rule, the write and the restore are all exercised against a scratch path,
// and what is left needing a machine is only whether the kernel's own node accepts the number. That
// half is decision 142's hardware bring-up and is the same half the probe beside this measures.
//
// **The one behaviour worth naming here is `Release`**, because it is what stops a development run of
// gyro from permanently changing the machine it was launched from: a floor written into sysfs outlives
// the process, so the destructor putting the old value back is the difference between an experiment
// and a config change.

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

[[nodiscard]] std::string Get(const std::filesystem::path& path)
{
	std::ifstream in{ path };
	std::string content;
	in >> content;

	return content;
}
} // namespace

// i915 carries a legacy pair and a per-`gt` pair, and the floor and the ceiling share an index because
// a kernel that moved one moved the other.
GYRO_TEST(GpuFloor, I915ResolvesBothPairsWithTheirCeilings)
{
	const GpuFloor::Nodes nodes = GpuFloor::Resolve("i915", 0);

	GYRO_REQUIRE(nodes.Count == 2);
	GYRO_CHECK(nodes.Floor[0] == "/sys/dev/char/226:0/gt_min_freq_mhz");
	GYRO_CHECK(nodes.Ceiling[0] == "/sys/dev/char/226:0/gt_RP0_freq_mhz");
	GYRO_CHECK(nodes.Floor[1] == "/sys/dev/char/226:0/gt/gt0/rps_min_freq_mhz");
	GYRO_CHECK(nodes.Ceiling[1] == "/sys/dev/char/226:0/gt/gt0/rps_RP0_freq_mhz");
}

GYRO_TEST(GpuFloor, XeResolvesOnePairUnderItsMinor)
{
	const GpuFloor::Nodes nodes = GpuFloor::Resolve("xe", 1);

	GYRO_REQUIRE(nodes.Count == 1);
	GYRO_CHECK(nodes.Floor[0] == "/sys/dev/char/226:1/device/tile0/gt0/freq0/min_freq");
	GYRO_CHECK(nodes.Ceiling[0] == "/sys/dev/char/226:1/device/tile0/gt0/freq0/rp0_freq");
}

// amdgpu's minimum is a word in `power_dpm_force_performance_level` rather than a number in a file,
// and msm's pair is dynamic — devfreq's `min_freq`/`max_freq`, resolved by scanning at `Open` rather
// than named as a constant here. Both come back empty from the pure mapping, and `Open` says which by
// name.
GYRO_TEST(GpuFloor, ADriverWithNoNumericMinimumResolvesToNothing)
{
	GYRO_CHECK(GpuFloor::Resolve("amdgpu", 0).Count == 0);
	GYRO_CHECK(GpuFloor::Resolve("msm", 0).Count == 0);
	GYRO_CHECK(GpuFloor::Resolve("", 0).Count == 0);
}

// msm's floor and ceiling are devfreq's `min_freq` and `max_freq`, which devfreq reports in Hz — the
// divisor scales the reading down and the commanded write back up, so the same pair of nodes serves
// the MHz-based caller.
GYRO_TEST(GpuFloor, ADivisorScalesTheReadAndTheWrite)
{
	const std::filesystem::path floor = Scratch("GyroFloorHzMin");
	const std::filesystem::path ceiling = Scratch("GyroFloorHzMax");
	Put(floor, "180000000\n");
	Put(ceiling, "800000000\n");

	{
		GpuFloor commanded = GpuFloor::OpenPaths(floor.string(), ceiling.string(), 1'000'000);
		GYRO_REQUIRE(commanded.IsValid());
		GYRO_CHECK_EQ(commanded.Original(), 180U);
		GYRO_CHECK_EQ(commanded.Ceiling(), 800U);

		GYRO_CHECK(commanded.Command(commanded.Ceiling()));
		GYRO_CHECK_EQ(commanded.Commanded(), 800U);
		GYRO_CHECK_EQ(Get(floor), std::string{ "800000000" });
	}

	GYRO_CHECK_EQ(Get(floor), std::string{ "180000000" });

	std::filesystem::remove(floor);
	std::filesystem::remove(ceiling);
}

GYRO_TEST(GpuFloor, TheFloorIsRaisedToTheCeilingAndPutBackOnRelease)
{
	const std::filesystem::path floor = Scratch("GyroFloorMin");
	const std::filesystem::path ceiling = Scratch("GyroFloorRp0");
	Put(floor, "100\n");
	Put(ceiling, "1250\n");

	{
		GpuFloor commanded = GpuFloor::OpenPaths(floor.string(), ceiling.string());
		GYRO_REQUIRE(commanded.IsValid());
		GYRO_CHECK_EQ(commanded.Original(), 100U);
		GYRO_CHECK_EQ(commanded.Ceiling(), 1250U);

		GYRO_CHECK(commanded.Command(commanded.Ceiling()));
		GYRO_CHECK_EQ(commanded.Commanded(), 1250U);
		GYRO_CHECK_EQ(Get(floor), std::string{ "1250" });
	}

	// The destructor, and the whole reason this object owns one: running gyro must not be a permanent
	// change to the machine's power policy.
	GYRO_CHECK_EQ(Get(floor), std::string{ "100" });

	std::filesystem::remove(floor);
	std::filesystem::remove(ceiling);
}

GYRO_TEST(GpuFloor, ARequestAboveTheCeilingIsClamped)
{
	const std::filesystem::path floor = Scratch("GyroFloorClampMin");
	const std::filesystem::path ceiling = Scratch("GyroFloorClampRp0");
	Put(floor, "350\n");
	Put(ceiling, "1250\n");

	GpuFloor commanded = GpuFloor::OpenPaths(floor.string(), ceiling.string());
	GYRO_REQUIRE(commanded.IsValid());
	GYRO_CHECK(commanded.Command(9'000));
	GYRO_CHECK_EQ(Get(floor), std::string{ "1250" });

	commanded.Release();
	GYRO_CHECK_EQ(Get(floor), std::string{ "350" });

	std::filesystem::remove(floor);
	std::filesystem::remove(ceiling);
}

// Never downward. A floor already above what gyro would ask for belongs to whoever set it — a thermal
// daemon, a tuned profile, a person with a reason — and gyro moving it down would be this file
// overruling a decision it knows nothing about.
GYRO_TEST(GpuFloor, ARequestBelowWhatIsAlreadyThereChangesNothing)
{
	const std::filesystem::path floor = Scratch("GyroFloorLowerMin");
	const std::filesystem::path ceiling = Scratch("GyroFloorLowerRp0");
	Put(floor, "800\n");
	Put(ceiling, "1250\n");

	{
		GpuFloor commanded = GpuFloor::OpenPaths(floor.string(), ceiling.string());
		GYRO_REQUIRE(commanded.IsValid());

		// True, because nothing failed — the request was already satisfied by the value there.
		GYRO_CHECK(commanded.Command(400));
		GYRO_CHECK_EQ(commanded.Commanded(), 0U);
		GYRO_CHECK_EQ(Get(floor), std::string{ "800" });
	}

	GYRO_CHECK_EQ(Get(floor), std::string{ "800" });

	std::filesystem::remove(floor);
	std::filesystem::remove(ceiling);
}

// A node gyro cannot write is the ordinary case on an unconfigured machine, and it is a warning and a
// session that runs rather than a startup that fails.
GYRO_TEST(GpuFloor, ANodeThatWillNotTakeTheWriteReportsFalseAndCommandsNothing)
{
	const std::filesystem::path floor = Scratch("GyroFloorReadOnlyMin");
	const std::filesystem::path ceiling = Scratch("GyroFloorReadOnlyRp0");
	Put(floor, "100\n");
	Put(ceiling, "1250\n");
	std::filesystem::permissions(floor, std::filesystem::perms::owner_read);

	GpuFloor commanded = GpuFloor::OpenPaths(floor.string(), ceiling.string());
	GYRO_REQUIRE(commanded.IsValid());
	GYRO_CHECK(!commanded.Command(1'250));
	GYRO_CHECK_EQ(commanded.Commanded(), 0U);

	std::filesystem::permissions(floor, std::filesystem::perms::owner_all);
	std::filesystem::remove(floor);
	std::filesystem::remove(ceiling);
}

GYRO_TEST(GpuFloor, AnAbsentNodeComesUpInvalidAndCommandsNothing)
{
	GpuFloor commanded = GpuFloor::OpenPaths("/nonexistent/gyro/min_freq", "/nonexistent/gyro/rp0_freq");

	GYRO_CHECK(!commanded.IsValid());
	GYRO_CHECK(!commanded.Command(1'250));
	GYRO_CHECK_EQ(commanded.Ceiling(), 0U);
}

// Move-only, and the husk must not restore the node the live one still owns.
GYRO_TEST(GpuFloor, AMovedFromFloorRestoresNothing)
{
	const std::filesystem::path floor = Scratch("GyroFloorMoveMin");
	const std::filesystem::path ceiling = Scratch("GyroFloorMoveRp0");
	Put(floor, "100\n");
	Put(ceiling, "1250\n");

	{
		GpuFloor made = GpuFloor::OpenPaths(floor.string(), ceiling.string());
		GYRO_REQUIRE(made.Command(1'250));

		{
			const GpuFloor moved = std::move(made);
			GYRO_CHECK(moved.IsValid());
			GYRO_CHECK_EQ(moved.Commanded(), 1'250U);
		}

		// The husk's destructor ran at the brace above and wrote nothing; the moved-to object's ran too
		// and restored. What this checks is that there was exactly one restore rather than two.
		GYRO_CHECK_EQ(Get(floor), std::string{ "100" });
		GYRO_CHECK(!made.IsValid());
	}

	std::filesystem::remove(floor);
	std::filesystem::remove(ceiling);
}
