#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// The GPU's minimum clock, commanded — decision 142's governing half where the deadline does not
// reach the hardware.
//
// **This is the lever gyro takes only because the honest one does nothing.** Render/Deadline.h states
// what gyro knows, and a driver holding that deadline *and* its own utilization history has both terms
// of the problem and sits closer to the part than gyro does. But msm is the only driver that wires the
// hint to frequency; `drm_sched` forwards it to hardware fences that amdgpu and xe do not implement,
// and i915 has no plumbing at all. So on every part gyro runs on today the hint is a no-op, and this
// is the only thing that moves the clock — which it does by setting a power policy on the whole
// machine's behalf, and that cost is the reason a probe decides rather than a table.
//
// **What it costs is much less than it looks like.** `intel_rps_park` sets `idle_freq`, and
// `idle_freq` is the hardware minimum rather than the sysfs softlimit — two different fields, so a
// floor does not raise the *parked* frequency. The part still falls to its bottom between frames and
// the floor applies from unpark, which for a 5.4 ms composite in a 16.7 ms period is a third of the
// wall clock rather than all of it.
//
// **It restores what it found, and the destructor is the whole of that promise.** A floor written into
// sysfs outlives the process that wrote it, so a gyro that exits without putting the old value back
// leaves the machine pinned until the next boot — which on a development machine means every run of
// the compositor ratchets the idle power of the desktop it was launched from. `SIGKILL` defeats this
// and is accepted: the restore rides the shutdown gyro already sequences, and a systemd `ExecStopPost`
// is where a service would put the belt to this file's braces.
//
// **The node is root-owned and gyro is not expected to chown it.** `gt_min_freq_mhz` is `0644
// root:root` on an ordinary system, so `Command` on a machine that has not granted the gyro uid write
// access fails and says so once, by path, and the session runs at whatever clock the governor gives
// it. That is decision 142's recoverable direction with the sign it deserves here: a floor gyro cannot
// set is the state gyro was already in.
//
// **Two drivers, and the omissions are named rather than silent.** i915 and xe expose a minimum
// frequency as a number in a file, which is what this writes. amdgpu's equivalent is
// `power_dpm_force_performance_level`, a *word* rather than a number and a coarser instrument than
// this wants, so it comes up invalid and logs. msm is the one driver that honours the deadline, so a
// probe on it would never select the floor anyway.
class GpuFloor
{
public:
	GpuFloor() = default;

	~GpuFloor() { Release(); }

	GpuFloor(const GpuFloor&) = delete;
	GpuFloor& operator=(const GpuFloor&) = delete;

	GpuFloor(GpuFloor&& other) noexcept;
	GpuFloor& operator=(GpuFloor&& other) noexcept;

	// Open the floor for the device behind this DRM primary minor, reading the value already there and
	// the ceiling to bound a request against. Invalid on a driver with no such node, a node that will
	// not open, or a value that will not parse — each after a line naming which.
	//
	// Read-only opening is deliberate: whether the node is *writable* is not asked until `Command`, so
	// a machine that cannot be commanded still reports its ceiling to the probe and to the log.
	[[nodiscard]] static GpuFloor Open(std::int64_t primaryMinor);

	// Open a pair of nodes directly. The seam `Open` resolves onto, and the one a test drives against
	// files it wrote.
	[[nodiscard]] static GpuFloor OpenPaths(const std::string& floor, const std::string& ceiling);

	[[nodiscard]] bool IsValid() const noexcept { return !m_Floor.empty(); }

	// The top of the part's range in MHz — RP0 on i915, `rp0_freq` on xe — or zero where it could not
	// be read. What a caller asking for "as fast as it goes" passes to `Command`, and the number that
	// makes decision 142's complaint legible in a log line: 350 of 1250.
	[[nodiscard]] std::uint32_t Ceiling() const noexcept { return m_Ceiling; }

	// What the node held when it was opened, which is what `Release` puts back.
	[[nodiscard]] std::uint32_t Original() const noexcept { return m_Original; }

	// What gyro has commanded, or zero where it has commanded nothing.
	[[nodiscard]] std::uint32_t Commanded() const noexcept { return m_Commanded; }

	// Raise the minimum to `mhz`, clamped to the ceiling. False where the node refused the write —
	// which is a permission answer far more often than anything else — and true where it took it,
	// including where `mhz` was already the value there.
	//
	// **Never lowers.** A request below what the node already held would be gyro reducing a floor
	// somebody else set on purpose, and the one number this file is entitled to move is its own.
	[[nodiscard]] bool Command(std::uint32_t mhz);

	// Put back what was found. Idempotent, and safe to call having commanded nothing.
	void Release() noexcept;

	// One driver's frequency pair, resolved from its name and the DRM minor. Pure and public for the
	// mapping to be tested without a `/sys` to read, exactly as Render/GpuClock.h's is: `Count`
	// candidates in preference order — i915 carries a legacy node and a newer per-`gt` one — with the
	// floor and the ceiling at the same index, because a kernel that moved one moved both.
	struct Nodes
	{
		std::array<std::string, 2> Floor{};
		std::array<std::string, 2> Ceiling{};
		std::uint32_t Count = 0;
	};

	[[nodiscard]] static Nodes Resolve(std::string_view driver, std::int64_t primaryMinor);

private:
	[[nodiscard]] bool Write(std::uint32_t mhz) const;

	// The path rather than a descriptor, and a sysfs attribute is why: a write has to be a fresh
	// `open` with `O_WRONLY` at offset zero to replace the value rather than append to it, and holding
	// a writable descriptor open for a whole session would fail at startup on the machines where the
	// interesting answer is that the *write* was refused.
	std::string m_Floor;

	std::uint32_t m_Ceiling = 0;
	std::uint32_t m_Original = 0;
	std::uint32_t m_Commanded = 0;
};
