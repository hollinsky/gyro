#include "Render/GpuClock.h"

#include <dirent.h>
#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace
{
// The DRM character major, the same for a primary node and a render one. A minor is what tells them
// apart, and `/sys/dev/char/226:<minor>` is the kobject the driver hangs its attributes off.
constexpr int DrmMajor = 226;

// The last path component of a sysfs symlink — `/sys/.../drivers/msm_dpu` becomes `msm_dpu` — or
// empty where the link is absent or unreadable. The `device` component is itself a symlink, which
// `readlink` resolves before reading the final target.
[[nodiscard]] std::string SymlinkLeaf(const std::string& path)
{
	std::array<char, 256> target{};
	const ssize_t length = readlink(path.c_str(), target.data(), target.size() - 1);

	if (length <= 0)
	{
		return {};
	}

	const std::string_view resolved{ target.data(), static_cast<std::size_t>(length) };
	const std::size_t slash = resolved.find_last_of('/');

	return std::string{ slash == std::string_view::npos ? resolved : resolved.substr(slash + 1) };
}
} // namespace

std::string GpuClock::NodeBase(std::int64_t primaryMinor)
{
	return std::format("/sys/dev/char/{}:{}", DrmMajor, primaryMinor);
}

// The kernel driver bound to this DRM node, read from the `device/driver` symlink and returned as its
// last path component — `i915`, `xe`, `msm_dpu` on the split kernel, `msm` before it.
std::string GpuClock::BoundDriver(std::int64_t primaryMinor)
{
	return SymlinkLeaf(NodeBase(primaryMinor) + "/device/driver");
}

bool GpuClock::IsMsmDriver(std::string_view driver) noexcept
{
	// The msm driver split made the DRM device the display controller's — `msm_dpu`, `msm_mdp`,
	// `msm_mdp4` — where the monolithic `msm` bound it on older kernels. The GPU is a separate
	// platform device either way, and the family test is what sends both the clock and the floor to
	// the GPU's devfreq.
	return driver == "msm" || driver.starts_with("msm_");
}

std::string GpuClock::MsmDevfreqDirectory()
{
	// The GPU's devfreq is a child of the GPU's own platform device (`5000000.gpu`), and the DRM
	// device is the display controller's — a sibling of the GPU under the SoC, so the DRM node's
	// subtree cannot reach the node. `/sys/class/devfreq` is the common parent, and the entry whose
	// backing device's driver is the GPU's (`adreno` on the kernels gyro targets) is the one.
	constexpr std::string_view GpuDriver = "adreno";
	const std::string directory = "/sys/class/devfreq";
	DIR* const handle = opendir(directory.c_str());

	if (handle == nullptr)
	{
		return {};
	}

	std::string found;

	while (const dirent* const entry = readdir(handle))
	{
		const std::string_view name{ entry->d_name };

		if (name == "." || name == "..")
		{
			continue;
		}

		const std::string base = directory + "/" + std::string{ name };

		if (SymlinkLeaf(base + "/device/driver") == GpuDriver)
		{
			found = base;
			break;
		}
	}

	closedir(handle);

	return found;
}

GpuClock::Source GpuClock::Resolve(std::string_view driver, std::int64_t primaryMinor)
{
	const std::string base = NodeBase(primaryMinor);
	Source source;

	if (driver == "i915")
	{
		// The legacy single-tile attributes first, then the per-`gt` ones newer multi-tile kernels moved
		// them to. All four are MHz. A part with more than one tile is not one gyro composites across, so
		// the first tile is the one that matters. `act` is what the part is running at and `cur` is the
		// point RPS has been commanded to — the two i915 fields the parked case is told apart by.
		source.Actual[0] = base + "/gt_act_freq_mhz";
		source.Requested[0] = base + "/gt_cur_freq_mhz";
		source.Actual[1] = base + "/gt/gt0/rps_act_freq_mhz";
		source.Requested[1] = base + "/gt/gt0/rps_cur_freq_mhz";
		source.Count = 2;
		source.Divisor = 1;
	}
	else if (driver == "xe")
	{
		// xe names the same pair `act_freq` and `cur_freq` under the tile's `freq0` directory.
		source.Actual[0] = base + "/device/tile0/gt0/freq0/act_freq";
		source.Requested[0] = base + "/device/tile0/gt0/freq0/cur_freq";
		source.Count = 1;
		source.Divisor = 1;
	}
	else if (IsMsmDriver(driver))
	{
		// devfreq reports Hz, and the node name is dynamic — resolved at `Open`, not here, so this
		// leaves the candidates empty and only carries the unit.
		source.Count = 0;
		source.Divisor = 1'000'000;
	}

	return source;
}

GpuClock GpuClock::OpenPath(const char* actual, const char* requested, std::uint32_t divisor)
{
	GpuClock clock;
	clock.m_Divisor = divisor == 0 ? 1 : divisor;
	clock.m_Fd = Fd{ open(actual, O_RDONLY | O_CLOEXEC) };

	// The requested half is allowed to be absent without taking the clock down with it. A kernel that
	// carries the actual attribute and not the commanded one is unlikely, but the failure it would
	// otherwise produce is losing every cost figure's operating point over the half that is only
	// context.
	if (requested != nullptr)
	{
		clock.m_Requested = Fd{ open(requested, O_RDONLY | O_CLOEXEC) };
	}

	return clock;
}

GpuClock GpuClock::Open(std::int64_t primaryMinor)
{
	if (primaryMinor < 0)
	{
		spdlog::warn("no GPU clock: VK_EXT_physical_device_drm gave no primary DRM node");

		return {};
	}

	const std::string driver = BoundDriver(primaryMinor);

	// msm's path is not a fixed candidate — the GPU's devfreq node is scanned for, and the DRM
	// device's driver is the display's (`msm_dpu` on the split kernel) rather than the GPU's.
	if (IsMsmDriver(driver))
	{
		if (const std::string directory = MsmDevfreqDirectory(); !directory.empty())
		{
			// devfreq's `cur_freq` goes through the driver's own `get_cur_freq`, so it is the actual one;
			// `target_freq` is what the governor last asked for. Opposite names to i915's, same pair.
			if (GpuClock clock =
			        OpenPath((directory + "/cur_freq").c_str(), (directory + "/target_freq").c_str(), 1'000'000);
			    clock.IsValid())
			{
				return clock;
			}
		}

		spdlog::warn(
			"no GPU clock: msm has no readable devfreq node (driver '{}' behind DRM {})", driver, primaryMinor
		);

		return {};
	}

	const Source source = Resolve(driver, primaryMinor);

	if (source.Count == 0)
	{
		// The line decision 142 asks for by name, so that a machine gyro cannot read a clock off — an
		// amdgpu the reader does not yet cover, above all — says so once at startup rather than quietly
		// filing zeros a cost window then has to make sense of.
		spdlog::warn(
			"no GPU clock: driver '{}' behind DRM {} has no frequency reader; GpuCost's clocks will be 0",
			driver.empty() ? "unknown" : driver,
			primaryMinor
		);

		return {};
	}

	for (std::uint32_t index = 0; index < source.Count; ++index)
	{
		if (GpuClock clock = OpenPath(source.Actual[index].c_str(), source.Requested[index].c_str(), source.Divisor);
		    clock.IsValid())
		{
			return clock;
		}
	}

	spdlog::warn("no GPU clock: driver '{}' has no readable frequency node under DRM {}", driver, primaryMinor);

	return {};
}

void GpuClock::ReadInto(const Fd& fd, std::uint32_t divisor, std::uint32_t& last) noexcept
{
	if (fd.Get() < 0)
	{
		return;
	}

	// sysfs regenerates the attribute on a read from offset zero, so a `pread` at zero is a fresh value
	// every time without an `lseek`. A small stack buffer — the value is a handful of digits and a
	// newline — and no allocation, because this runs on the frame thread.
	std::array<char, 32> buffer{};
	const ssize_t length = pread(fd.Get(), buffer.data(), buffer.size() - 1, 0);

	if (length <= 0)
	{
		// A momentary failure keeps the last good reading rather than reporting a zero the cost window
		// would have to recognise as "unread" separately from a device that genuinely reports none.
		return;
	}

	std::uint32_t raw = 0;

	if (const std::from_chars_result parsed = std::from_chars(buffer.data(), buffer.data() + length, raw);
	    parsed.ec == std::errc{})
	{
		last = raw / divisor;
	}
}

GpuClock::Reading GpuClock::Read() noexcept
{
	if (!IsValid())
	{
		return {};
	}

	ReadInto(m_Fd, m_Divisor, m_LastActualMhz);
	ReadInto(m_Requested, m_Divisor, m_LastRequestedMhz);

	return { .ActualMhz = m_LastActualMhz, .RequestedMhz = m_LastRequestedMhz };
}
