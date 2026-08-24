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

// msm reports through devfreq under a node whose name is assigned at boot rather than fixed, so the
// path cannot be a constant the way i915's and xe's are — the one entry under `device/devfreq` is
// scanned for here. Empty where the directory is absent or holds nothing.
[[nodiscard]] std::string MsmDevfreqNode(std::int64_t primaryMinor)
{
	const std::string directory = GpuClock::NodeBase(primaryMinor) + "/device/devfreq";
	DIR* const handle = opendir(directory.c_str());

	if (handle == nullptr)
	{
		return {};
	}

	std::string node;

	while (const dirent* const entry = readdir(handle))
	{
		const std::string_view name{ entry->d_name };

		if (name != "." && name != "..")
		{
			node = directory + "/" + std::string{ name } + "/cur_freq";

			break;
		}
	}

	closedir(handle);

	return node;
}
} // namespace

std::string GpuClock::NodeBase(std::int64_t primaryMinor)
{
	return std::format("/sys/dev/char/{}:{}", DrmMajor, primaryMinor);
}

// The kernel driver bound to this DRM node, read from the `device/driver` symlink and returned as its
// last path component — `i915`, `xe`, `msm`, `amdgpu`.
std::string GpuClock::BoundDriver(std::int64_t primaryMinor)
{
	const std::string link = NodeBase(primaryMinor) + "/device/driver";
	std::array<char, 256> target{};
	const ssize_t length = readlink(link.c_str(), target.data(), target.size() - 1);

	if (length <= 0)
	{
		return {};
	}

	const std::string_view resolved{ target.data(), static_cast<std::size_t>(length) };
	const std::size_t slash = resolved.find_last_of('/');

	return std::string{ slash == std::string_view::npos ? resolved : resolved.substr(slash + 1) };
}

GpuClock::Source GpuClock::Resolve(std::string_view driver, std::int64_t primaryMinor)
{
	const std::string base = NodeBase(primaryMinor);
	Source source;

	if (driver == "i915")
	{
		// The legacy single-tile attribute first, then the per-`gt` one newer multi-tile kernels moved
		// it to. Both are MHz. A part with more than one tile is not one gyro composites across, so the
		// first tile is the one that matters.
		source.Candidates[0] = base + "/gt_act_freq_mhz";
		source.Candidates[1] = base + "/gt/gt0/rps_act_freq_mhz";
		source.Count = 2;
		source.Divisor = 1;
	}
	else if (driver == "xe")
	{
		source.Candidates[0] = base + "/device/tile0/gt0/freq0/act_freq";
		source.Count = 1;
		source.Divisor = 1;
	}
	else if (driver == "msm")
	{
		// devfreq reports Hz, and the node name is dynamic — resolved at `Open`, not here, so this
		// leaves the candidate empty and only carries the unit.
		source.Count = 0;
		source.Divisor = 1'000'000;
	}

	return source;
}

GpuClock GpuClock::OpenPath(const char* path, std::uint32_t divisor)
{
	GpuClock clock;
	clock.m_Divisor = divisor == 0 ? 1 : divisor;
	clock.m_Fd = Fd{ open(path, O_RDONLY | O_CLOEXEC) };

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

	// msm's path is not a fixed candidate — the devfreq node is scanned for.
	if (driver == "msm")
	{
		if (const std::string node = MsmDevfreqNode(primaryMinor); !node.empty())
		{
			if (GpuClock clock = OpenPath(node.c_str(), 1'000'000); clock.IsValid())
			{
				return clock;
			}
		}

		spdlog::warn("no GPU clock: msm has no readable devfreq node under DRM {}", primaryMinor);

		return {};
	}

	const Source source = Resolve(driver, primaryMinor);

	if (source.Count == 0)
	{
		// The line decision 142 asks for by name, so that a machine gyro cannot read a clock off — an
		// amdgpu the reader does not yet cover, above all — says so once at startup rather than quietly
		// filing zeros a cost window then has to make sense of.
		spdlog::warn(
			"no GPU clock: driver '{}' behind DRM {} has no frequency reader; GpuCost.ClockMhz will be 0",
			driver.empty() ? "unknown" : driver,
			primaryMinor
		);

		return {};
	}

	for (std::uint32_t index = 0; index < source.Count; ++index)
	{
		if (GpuClock clock = OpenPath(source.Candidates[index].c_str(), source.Divisor); clock.IsValid())
		{
			return clock;
		}
	}

	spdlog::warn("no GPU clock: driver '{}' has no readable frequency node under DRM {}", driver, primaryMinor);

	return {};
}

std::uint32_t GpuClock::Sample(Instant now)
{
	if (!IsValid())
	{
		return 0;
	}

	// The rate limit, and the reason the frame thread can call this every frame: a read that is younger
	// than a governor evaluation interval cannot have moved, so it is answered from the last value with
	// no syscall. The epoch sentinel forces the first call to read.
	if (m_Sampled != Instant{} && Elapsed(m_Sampled, now) < RefreshInterval)
	{
		return m_LastMhz;
	}

	m_Sampled = now;

	// sysfs regenerates the attribute on a read from offset zero, so a `pread` at zero is a fresh value
	// every time without an `lseek`. A small stack buffer — the value is a handful of digits and a
	// newline — and no allocation, because this runs on the frame thread.
	std::array<char, 32> buffer{};
	const ssize_t length = pread(m_Fd.Get(), buffer.data(), buffer.size() - 1, 0);

	if (length <= 0)
	{
		// A momentary failure keeps the last good reading rather than reporting a zero the cost window
		// would have to recognise as "unread" separately from a device that genuinely reports none.
		return m_LastMhz;
	}

	std::uint32_t raw = 0;
	const std::from_chars_result parsed = std::from_chars(buffer.data(), buffer.data() + length, raw);

	if (parsed.ec == std::errc{})
	{
		m_LastMhz = raw / m_Divisor;
	}

	return m_LastMhz;
}
