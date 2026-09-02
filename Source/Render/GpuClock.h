#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "Core/Fd.h"

// The GPU's core clock, read off sysfs as a pair — what the part is doing and what it has been told to
// do — so a cost span in seconds knows the operating point it was measured at.
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
// **Two attributes rather than one, because the actual clock reads zero while the part is gated.**
// *(Revised 2026-08-28.)* A frame is a few hundred microseconds of compositing and then most of a
// refresh parked, which is the shape Render/Governor.h reproduces on purpose — so the actual clock
// sampled anywhere on the frame path is very often a reading of the GPU asleep. Measured on the Tiger
// Lake decision 142 was written against: idle, `gt_act_freq_mhz` reads 0 while `gt_cur_freq_mhz` reads
// 1150. One number cannot tell a window whether a long span was a slow part or a parked one, and the
// requested point is the better estimate of what a composite that has not started yet will run at.
// So a `Reading` carries both, and a cost window that sees `Actual` of zero beside a `Requested` of
// 1150 knows exactly which it is looking at.
//
// **There is no rate limit, and there was one.** *(Revised 2026-08-28.)* The rate limit answered a
// staleness budget of ten milliseconds against a frame period of sixteen, so it skipped a read on
// almost no frame at 60 Hz and bought its complexity nothing; and against a pair it is actively wrong,
// since a cached half and a fresh one are two samples of different instants presented as one operating
// point. What it was protecting against is also smaller than it looks: the pair costs 1.3 microseconds
// of the frame thread, measured at 1.0 for the actual attribute and 0.3 for the requested one, mean
// over two thousand reads, worst 8.5. The one real hazard is that i915 takes a runtime-PM wakeref to
// read the actual clock, so a read against a runtime-suspended part resumes it — but `autosuspend_delay
// _ms` is ten seconds and a compositor drawing frames never lets it fire. A dedicated off-thread
// sampler is still rejected: a whole thread, its own lifetime and a cross-thread hand-off, to save
// a microsecond.
//
// **Which attributes are a per-driver fact, discovered rather than configured.** The kernel driver
// bound to the DRM node names the paths — i915 and xe carry it in MHz under different nodes, msm reports Hz
// through devfreq, amdgpu reports Hz through the hwmon its SMU registers — and a driver gyro does not
// recognise reads back zero after a warning, which is the same honest nothing `Blit` and a device with
// no timestamp support already report for `Cost` itself.
//
// **amdgpu has an actual half and no requested one**, which is the first driver to fill only one side
// of the pair. Nothing it exposes means what i915's `gt_cur_freq_mhz` means — the point the governor
// has commanded — and `pp_dpm_sclk`'s starred rung is a coarser reading of the clock `freq1_input`
// already measures rather than a target, so filing it opposite the actual half would put one quantity
// on two rows and invite a reader to compare them. The loss is real and smaller than it looks: the
// second half exists to tell a slow part from a parked one, and amdgpu's actual attribute reports a
// live average rather than i915's flat zero while gated.
class GpuClock
{
public:
	GpuClock() = default;

	// The pair, in MHz. Zero for either half gyro cannot read.
	//
	// **`Actual` is what the part is doing and `Requested` is what it has been told to do**, and the gap
	// between them is the whole reason both are here: a parked GPU reports an `Actual` of zero against a
	// `Requested` the governor is still holding, and a slow one reports two numbers that agree low.
	struct Reading
	{
		std::uint32_t ActualMhz = 0;
		std::uint32_t RequestedMhz = 0;

		friend constexpr bool operator==(Reading, Reading) noexcept = default;
	};

	// Open the clock for the device behind this DRM primary minor, or come up invalid.
	//
	// The minor is `VK_EXT_physical_device_drm`'s answer for the physical device Vulkan chose, which is
	// what makes this the *right* GPU's clock on a machine with more than one. A negative minor — the
	// extension did not answer — an unrecognised driver, or a node that will not open all leave the
	// clock invalid and `Read` answering zero, each after a warning naming which it was.
	[[nodiscard]] static GpuClock Open(std::int64_t primaryMinor);

	// Open a specific pair of nodes directly, dividing raw reads by `divisor` to reach MHz. The seam
	// `Open` resolves onto, and the one a test drives against files it wrote.
	//
	// A null or unopenable `requested` is not a failure: the clock is valid on the actual node alone and
	// reports a `RequestedMhz` of zero, which is the same honest nothing every other unreadable number
	// here reports. A missing `actual` leaves the whole clock invalid, because that is the half a cost
	// span is filed against.
	[[nodiscard]] static GpuClock OpenPath(const char* actual, const char* requested, std::uint32_t divisor);

	[[nodiscard]] bool IsValid() const noexcept { return m_Fd.Get() >= 0; }

	// The pair now: two `pread`s and no cache. Zero on an invalid clock, and the last good value for a
	// half whose read momentarily fails rather than a spurious zero the cost window would have to filter
	// back out.
	//
	// **Callable from the frame thread**, which is what the rate limit above used to be for: it
	// allocates nothing, locks nothing, and costs a microsecond and change.
	[[nodiscard]] Reading Read() noexcept;

	// One driver's frequency nodes, resolved from its name and the DRM minor. Pure and public for the
	// mapping to be tested without a `/sys` to read: `Count` candidates in preference order — i915
	// carries a legacy pair and a newer one, everything else a single pair — with the actual and the
	// requested node at the same index, because a kernel that moved one moved both. That is
	// Render/GpuFloor.h's `Nodes` shape, for the same reason it has it. `Count` of zero is an
	// unrecognised driver, and the divisor turns a raw read into MHz.
	struct Source
	{
		std::array<std::string, 2> Actual{};
		std::array<std::string, 2> Requested{};
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

	// The full path to amdgpu's core-clock attribute — `<drm node>/device/hwmon/hwmonN/freq1_input`,
	// in Hz — or empty where the device registers no hwmon channel labelled `sclk`. Public for the
	// same reason `Resolve` is: it is the whole of the amdgpu mapping, and `Open` is the composition
	// over it. The `hwmonN` is scanned for rather than composed because the class numbers its
	// instances in probe order across the machine, and the channel is matched on its *label* rather
	// than on the number, so a part that also reports `mclk` cannot have the wrong clock filed
	// against its composites.
	[[nodiscard]] static std::string AmdgpuFrequencyNode(std::int64_t primaryMinor);

private:
	// One half of the pair read through, keeping `last` where the read or the parse failed.
	static void ReadInto(const Fd& fd, std::uint32_t divisor, std::uint32_t& last) noexcept;

	Fd m_Fd;
	Fd m_Requested;
	std::uint32_t m_Divisor = 1;
	std::uint32_t m_LastActualMhz = 0;
	std::uint32_t m_LastRequestedMhz = 0;
};
