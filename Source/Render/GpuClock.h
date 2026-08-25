#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "Core/Fd.h"
#include "Core/Time.h"

// The GPU's core clock, read off sysfs, so a cost span in seconds knows the operating point it was
// measured at.
//
// **This is decision 142's measurement half and nothing more.** Every GPU cost figure gyro records is
// an elapsed time, and an elapsed time is meaningless without the clock the work ran at — the part
// this was written against sits parked at 350 MHz of a 1250 MHz range because the kernel's frequency
// governor cannot see a frame deadline, so a window of cost samples that mixes clocks is a window of
// nothing. The number belongs on `GpuCost`, and this is what fills it.
//
// **The governing half went next door rather than growing a write verb here**, which is the opposite
// of what this paragraph predicted. *(Revised 2026-08-24.)* Render/Deadline.h states the `dma_fence`
// deadline and Render/GpuFloor.h commands the minimum clock where the deadline does not reach it, and
// what separates them from this object is lifetime rather than subject: a floor has to be *restored*,
// so it owns a destructor, and a destructor on the object the frame thread samples every frame would
// put a sysfs write on a path that only reads and cost the reader its default moves. What the two do
// share is the per-driver node resolution, which is why `NodeBase` and `BoundDriver` below are
// public.
//
// **The read is a `pread` of one small sysfs attribute and it is rate-limited, because the clock it
// reports moves no faster than the governor's evaluation interval — tens of milliseconds — while a
// composite is measured in hundreds of microseconds.** So `Sample` may be called every frame on the
// frame thread and re-enters the kernel at most once every `RefreshInterval`; the rest of the time it
// answers from the last value. A dedicated off-thread sampler was rejected for this: it is a whole
// thread, its own lifetime and a cross-thread hand-off, to read a number that is very nearly constant.
//
// **Which attribute is a per-driver fact, discovered rather than configured.** The kernel driver bound
// to the DRM node names the path — i915 and xe carry it in MHz under different nodes, msm reports Hz
// through devfreq — and a driver gyro does not recognise reads back zero after a warning, which is the
// same honest nothing `Blit` and a device with no timestamp support already report for `Cost` itself.
class GpuClock
{
public:
	GpuClock() = default;

	// Open the clock for the device behind this DRM primary minor, or come up invalid.
	//
	// The minor is `VK_EXT_physical_device_drm`'s answer for the physical device Vulkan chose, which is
	// what makes this the *right* GPU's clock on a machine with more than one. A negative minor — the
	// extension did not answer — an unrecognised driver, or a node that will not open all leave the
	// clock invalid and `Sample` answering zero, each after a warning naming which it was.
	[[nodiscard]] static GpuClock Open(std::int64_t primaryMinor);

	// Open a specific node directly, dividing raw reads by `divisor` to reach MHz. The seam `Open`
	// resolves onto, and the one a test drives against a file it wrote.
	[[nodiscard]] static GpuClock OpenPath(const char* path, std::uint32_t divisor);

	[[nodiscard]] bool IsValid() const noexcept { return m_Fd.Get() >= 0; }

	// The GPU's core clock in MHz as of `now`, rate-limited to one sysfs read per `RefreshInterval`.
	// Zero on an invalid clock, and the last good value where a read momentarily fails rather than a
	// spurious zero the cost window would have to filter back out.
	[[nodiscard]] std::uint32_t Sample(Instant now);

	// The clock now, with no rate limit and therefore a syscall every call.
	//
	// **For a caller that is off the frame thread and is watching the number move.** `Sample` above
	// exists because the frame thread reads a value that changes far more slowly than a frame, and a
	// startup probe wants the opposite: it submits one batch of work and polls until the queue drains,
	// asking what the clock reached while it did. Ten milliseconds of staleness there is most of the
	// measurement. Nothing on the frame path may call this, which is why the rate limit stayed on
	// `Sample` rather than being lifted to a policy the caller states.
	[[nodiscard]] std::uint32_t Read() noexcept;

	// One driver's frequency node, resolved from its name and the DRM minor. Pure and public for the
	// mapping to be tested without a `/sys` to read: `Count` candidates in preference order — i915
	// carries a legacy node and a newer one, everything else a single path — and the divisor that turns
	// a raw read into MHz. `Count` of zero is an unrecognised driver.
	struct Source
	{
		std::array<std::string, 2> Candidates{};
		std::uint32_t Count = 0;
		std::uint32_t Divisor = 1;
	};

	[[nodiscard]] static Source Resolve(std::string_view driver, std::int64_t primaryMinor);

	// The sysfs kobject a DRM minor's driver hangs its attributes off, and the name of the driver bound
	// to it. Public because Render/GpuFloor.h resolves its own pair of nodes off exactly these two, and
	// a second copy of `/sys/dev/char/226:<minor>` in the tree is a second place to get it wrong.
	[[nodiscard]] static std::string NodeBase(std::int64_t primaryMinor);

	// Empty where nothing is bound or the `device/driver` symlink is unreadable, which a caller reports
	// as an unrecognised driver rather than distinguishing.
	[[nodiscard]] static std::string BoundDriver(std::int64_t primaryMinor);

	// Whether a DRM device driver name is an msm-family one. The msm driver split made the device
	// behind a DRM minor the display controller's — `msm_dpu`, `msm_mdp`, `msm_mdp4` — where the
	// monolithic `msm` used to bind it; the GPU is a separate platform device either way, and the
	// family test is what sends both the clock and the floor to the GPU's devfreq.
	[[nodiscard]] static bool IsMsmDriver(std::string_view driver) noexcept;

	// The sysfs directory of the GPU's devfreq — `/sys/class/devfreq/<name>` — or empty where none is
	// found. The GPU's devfreq is hung off the GPU's own platform device rather than the DRM device's
	// subtree, so this scans `/sys/class/devfreq` for the entry whose backing device is bound to the
	// GPU driver (`adreno` on the kernels gyro targets). Public because Render/GpuFloor.h resolves its
	// pair of nodes off the same directory.
	[[nodiscard]] static std::string MsmDevfreqDirectory();

	// How stale a reading is allowed to be before the next `Sample` re-enters the kernel. Ten
	// milliseconds is well under a refresh and well over how fast the governor moves the clock, so a
	// sample is at most a frame or two old and the frame thread pays a syscall at most once per handful
	// of frames.
	static constexpr Duration RefreshInterval = std::chrono::milliseconds{ 10 };

private:
	Fd m_Fd;
	std::uint32_t m_Divisor = 1;
	std::uint32_t m_LastMhz = 0;

	// The wall time of the last read, or the epoch where none has happened — which no `Now()` returns,
	// so it doubles as "never sampled" and forces the first `Sample` to read.
	Instant m_Sampled{};
};
