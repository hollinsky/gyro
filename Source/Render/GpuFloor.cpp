#include "Render/GpuFloor.h"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <format>
#include <utility>

#include "Core/Fd.h"
#include "Render/GpuClock.h"

namespace
{
// One sysfs attribute as a number, or zero where it is absent, unreadable or not a number. Zero is
// also a legitimate reading on a parked part, which is why nothing here treats it as an error — the
// caller decides, and for a ceiling it means "no ceiling known".
[[nodiscard]] std::uint32_t Read(const std::string& path)
{
	const Fd file{ open(path.c_str(), O_RDONLY | O_CLOEXEC) };

	if (!file.IsValid())
	{
		return 0;
	}

	std::array<char, 32> buffer{};
	const ssize_t length = pread(file.Get(), buffer.data(), buffer.size() - 1, 0);

	if (length <= 0)
	{
		return 0;
	}

	std::uint32_t value = 0;

	return std::from_chars(buffer.data(), buffer.data() + length, value).ec == std::errc{} ? value : 0;
}
} // namespace

GpuFloor::GpuFloor(GpuFloor&& other) noexcept
	: m_Floor{ std::move(other.m_Floor) }, m_Ceiling{ std::exchange(other.m_Ceiling, 0) },
	  m_Original{ std::exchange(other.m_Original, 0) }, m_Commanded{ std::exchange(other.m_Commanded, 0) }
{
	// The path is what `IsValid` and `Release` both read, so emptying it is what makes the husk inert
	// rather than a second owner that will restore the same node twice.
	other.m_Floor.clear();
}

GpuFloor& GpuFloor::operator=(GpuFloor&& other) noexcept
{
	if (this != &other)
	{
		Release();
		m_Floor = std::move(other.m_Floor);
		m_Ceiling = std::exchange(other.m_Ceiling, 0);
		m_Original = std::exchange(other.m_Original, 0);
		m_Commanded = std::exchange(other.m_Commanded, 0);
		other.m_Floor.clear();
	}

	return *this;
}

GpuFloor::Nodes GpuFloor::Resolve(std::string_view driver, std::int64_t primaryMinor)
{
	const std::string base = GpuClock::NodeBase(primaryMinor);
	Nodes nodes;

	if (driver == "i915")
	{
		// The legacy pair first and the per-`gt` pair second, in the order Render/GpuClock.h reads the
		// actual frequency in — a kernel that moved one moved the other, so the index is shared and a
		// floor is never written to a node whose ceiling came from somewhere else.
		nodes.Floor[0] = base + "/gt_min_freq_mhz";
		nodes.Ceiling[0] = base + "/gt_RP0_freq_mhz";
		nodes.Floor[1] = base + "/gt/gt0/rps_min_freq_mhz";
		nodes.Ceiling[1] = base + "/gt/gt0/rps_RP0_freq_mhz";
		nodes.Count = 2;
	}
	else if (driver == "xe")
	{
		nodes.Floor[0] = base + "/device/tile0/gt0/freq0/min_freq";
		nodes.Ceiling[0] = base + "/device/tile0/gt0/freq0/rp0_freq";
		nodes.Count = 1;
	}

	return nodes;
}

GpuFloor GpuFloor::OpenPaths(const std::string& floor, const std::string& ceiling)
{
	GpuFloor commanded;

	// Readable is the bar, not writable. What a machine that will not take the write should do is run
	// and say so, and the probe beside this still wants the ceiling to put in its log line.
	const std::uint32_t original = Read(floor);

	if (original == 0)
	{
		return commanded;
	}

	commanded.m_Floor = floor;
	commanded.m_Original = original;
	commanded.m_Ceiling = Read(ceiling);

	return commanded;
}

GpuFloor GpuFloor::Open(std::int64_t primaryMinor)
{
	if (primaryMinor < 0)
	{
		return {};
	}

	const std::string driver = GpuClock::BoundDriver(primaryMinor);
	const Nodes nodes = Resolve(driver, primaryMinor);

	if (nodes.Count == 0)
	{
		// Named rather than silent, because the two omissions are different things: amdgpu's equivalent
		// is a *word* in `power_dpm_force_performance_level` rather than a number, and msm is the one
		// driver that honours the deadline and would never be asked for a floor.
		spdlog::info(
			"no GPU frequency floor: driver '{}' behind DRM {} exposes no minimum-clock node gyro writes",
			driver.empty() ? "unknown" : driver,
			primaryMinor
		);

		return {};
	}

	for (std::uint32_t index = 0; index < nodes.Count; ++index)
	{
		if (GpuFloor floor = OpenPaths(nodes.Floor[index], nodes.Ceiling[index]); floor.IsValid())
		{
			return floor;
		}
	}

	spdlog::info(
		"no GPU frequency floor: driver '{}' has no readable minimum-clock node under DRM {}", driver, primaryMinor
	);

	return {};
}

bool GpuFloor::Write(std::uint32_t mhz) const
{
	// A fresh `open` per write rather than a held descriptor: a sysfs store replaces the attribute's
	// value from offset zero, and holding this open for a session would move the permission failure to
	// startup — where it would be indistinguishable from a driver with no such node.
	//
	// **`O_TRUNC` is for the test rather than for sysfs**, and it is honest either way. A sysfs store
	// replaces the whole value whatever the length, but an ordinary file does not — writing `100` over
	// `1250` leaves `1004` — and a test that drives this against a scratch path is the only way the
	// restore is checked at all. Truncating makes the two behave the same.
	const Fd file{ open(m_Floor.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC) };

	if (!file.IsValid())
	{
		return false;
	}

	const std::string value = std::format("{}", mhz);

	return pwrite(file.Get(), value.data(), value.size(), 0) == static_cast<ssize_t>(value.size());
}

bool GpuFloor::Command(std::uint32_t mhz)
{
	if (!IsValid())
	{
		return false;
	}

	const std::uint32_t bounded = m_Ceiling != 0 && mhz > m_Ceiling ? m_Ceiling : mhz;

	// Never downward. A request under what was already there would be gyro relaxing a floor somebody
	// else set on purpose — a thermal daemon, a tuned profile, a person with a reason — and the only
	// number this file is entitled to move is the one it raised.
	if (bounded <= m_Original)
	{
		return true;
	}

	if (!Write(bounded))
	{
		// Once, by path, and at warning rather than error: the session runs, at whatever clock the
		// governor gives it, which is the state gyro was in before this file existed. The path is in the
		// line because the fix is a permission on exactly it.
		spdlog::warn(
			"GPU frequency floor refused: cannot write {} MHz to {} — frames will be composited at whatever "
			"clock the kernel's governor grants, which decision 142 measured at a quarter of the part's range",
			bounded,
			m_Floor
		);

		return false;
	}

	m_Commanded = bounded;

	spdlog::info("GPU frequency floor at {} MHz (was {}, ceiling {})", bounded, m_Original, m_Ceiling);

	return true;
}

void GpuFloor::Release() noexcept
{
	if (m_Commanded == 0)
	{
		return;
	}

	// What was found, not what looks sensible. A machine gyro raised the floor on is one whose owner
	// chose the old number, and putting anything else back would make running the compositor once a
	// permanent change to the machine.
	const bool restored = Write(m_Original);

	m_Commanded = 0;

	if (!restored)
	{
		spdlog::warn("GPU frequency floor left at raised value: {} would not take {} MHz back", m_Floor, m_Original);
	}
}
