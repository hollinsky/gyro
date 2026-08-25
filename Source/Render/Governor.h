#pragma once

#include <cstdint>

#include "Core/Clock.h"
#include "Core/Time.h"
#include "Render/GpuFloor.h"

class VulkanDevice;
class VulkanTextures;

// What the machine did when gyro said a deadline, and what gyro takes because of it.
//
// **This is the party decision 142 leaves unnamed: something has to choose between the two arms.**
// Render/Deadline.h states the deadline and Render/GpuFloor.h commands the minimum clock, and the
// decision is explicit that which of them a machine gets is *measured at startup, never inferred from
// the driver's name*. A name does not predict the behaviour — amdgpu implements `->set_deadline` and
// drops it one layer down, so it passes any presence test and fails in fact — and a name is the wrong
// granularity besides, because i915 gaining the plumbing should retire gyro's floor by itself rather
// than waiting for somebody to edit a table. So this submits real work twice, once urgent and once
// not, and reads the clock both times.
//
// **What it is measuring is a step and not a slope.** msm's response is `get_freq() * 2` fired by a
// timer three milliseconds before the deadline; a driver that solved for a frequency would do
// something similar in magnitude. Neither looks like the few percent two identical batches differ by,
// so `Margin` is a tenth and everything under it is *no response* — which is the same reading whether
// the driver dropped the number or the part was already at its ceiling.
//
// **An unclear probe takes the floor, and that is decision 142's own direction.** A false negative
// spends power gyro did not need; a false positive drops frames. Only one of those is visible to a
// person, so every path that cannot answer — no timeline to name a point on, an export the device
// refused, a submission that failed — comes out `Ignored` and commands the clock.
//
// **It costs about half a second of boot, and it is spent only where it could buy something.** The
// floor is opened first, and a machine whose minimum is already its ceiling, or whose node gyro cannot
// read, is one where the answer changes nothing — so the GPU work never runs. Where it does run, the
// batch is calibrated to a few milliseconds rather than fixed, because a fixed fragment count is
// fifteen milliseconds on the part this was written against and half a millisecond on a discrete one,
// and a batch shorter than msm's own timer lead measures nothing on the one driver that answers.
//
// **Half a second is a lot for a boot service and it is the number to argue with first.** What it buys
// is that a kernel gaining the plumbing retires gyro's floor on the next boot rather than on the next
// release, which is decision 142's reason for measuring at all. What would make it cheaper is a
// remembered answer keyed by the part and the kernel — which is a cache of a measurement rather than
// the table decision 142 rejected, and is a separate question about where a boot service is allowed to
// keep state. Docs/Open.md's capability probe entry is where the two meet, since that probe has to run
// the real chain here anyway.
//
// **Rejected: probing without a workload — set a deadline on an empty submission and watch.** The
// governor has nothing to raise the clock *for*, and every driver answers the same way, which is the
// probe agreeing with itself rather than measuring anything.
enum class DeadlineResponse : std::uint8_t
{
	// Not asked. The floor was not worth probing for, so no GPU work ran.
	Unprobed,

	// Asked and answered: the clock did not move, so the deadline reaches no hardware here.
	Ignored,

	// The clock stepped up when gyro said the work was urgent.
	Honoured,
};

// What the probe saw, kept so the startup line can print it and a person can disagree with it.
struct GovernorReading
{
	DeadlineResponse Response = DeadlineResponse::Unprobed;

	// The median peak clock across the rounds that stated no deadline, and across the rounds that did.
	// Both in MHz, and both zero where nothing was measured.
	std::uint32_t QuietMhz = 0;
	std::uint32_t UrgentMhz = 0;

	// The longest composite any measured frame took, and how long the whole probe cost the boot. The
	// first is the figure that says the arms really did leave idle inside the period — a batch that had
	// grown past it would be the occupancy-bound shape again — and the second is the number to hold
	// this against if it ever has to get cheaper.
	Duration Batch{};
	Duration Elapsed{};
};

// The frequency policy gyro holds for the length of a session.
//
// Move-only and non-copyable through its `GpuFloor`, which is what carries the restore: the floor is
// put back when this is destroyed, so the object's lifetime *is* the promise that running gyro once
// does not permanently change the machine it was launched from.
class GpuGovernor
{
public:
	GpuGovernor() = default;

	GpuGovernor(GpuGovernor&&) noexcept = default;
	GpuGovernor& operator=(GpuGovernor&&) noexcept = default;

	// Probe this device, and take the floor where the deadline did not reach the hardware.
	//
	// **A renderer is built here and thrown away, which is the honest cost of asking the real
	// question.** What is being measured is whether *gyro's own submissions* carry urgency, so the
	// probe draws through the pipeline a frame draws through rather than through a synthetic batch that
	// might take a different path into the scheduler. The device and the table are the composition
	// root's; the target, the renderer and the draw list belong to this call and are gone when it
	// returns.
	//
	// Runs at startup and at a device rebuild, never inside a frame: it allocates, it compiles
	// pipelines, and it deliberately spins for a tenth of a second.
	[[nodiscard]] static GpuGovernor Take(const IClock& clock, VulkanDevice& device, VulkanTextures& textures);

	[[nodiscard]] const GovernorReading& Reading() const noexcept { return m_Reading; }

	[[nodiscard]] const GpuFloor& Floor() const noexcept { return m_Floor; }

	// The decision rule alone, so that the margin is a number a test can argue with on a machine with
	// no GPU. Zero on either side is `Ignored` and not `Unprobed`: a probe that ran and read nothing
	// back has answered decision 142's question in the recoverable direction, and only a probe that
	// never ran is unasked.
	[[nodiscard]] static DeadlineResponse Judge(std::uint32_t quietMhz, std::uint32_t urgentMhz) noexcept;

	// How much higher the urgent arm has to read before the difference is a driver responding rather
	// than two batches of the same work disagreeing, as a percentage of the quiet arm.
	static constexpr std::uint32_t Margin = 10;

	// How many times each arm is run. The two swap order between rounds, because a part warms into a
	// probe and a fixed quiet-then-urgent order would read that drift as a response.
	static constexpr std::uint32_t Rounds = 2;

	// **The shape of one arm, and getting this wrong is how the probe answers a question nobody
	// asked.** The first version of this submitted batches back to back and read 1250 MHz on both arms
	// of a Tiger Lake that sits at 300 MHz while compositing. That is not a driver honouring a deadline;
	// it is a driver serving a workload that is *occupancy-bound*, which decision 142 is entirely about
	// gyro never being. A probe in that regime has no headroom left to detect a response in and would
	// report the same number on a driver that answers and one that does not.
	//
	// So the arm is frame-shaped: a composite, then the rest of a refresh idle, repeated. That is what
	// puts the part through `intel_rps_park` once per frame, which is the ratchet that lands it on the
	// efficient frequency, which is the condition the floor exists for. `Settling` frames are thrown
	// away because the ratchet takes two or three parks to come to rest from wherever the previous arm
	// left the clock.
	static constexpr Duration Period = std::chrono::microseconds{ 16'667 };
	static constexpr std::uint32_t Frames = 6;
	static constexpr std::uint32_t Settling = 3;

	// What one composite is calibrated to take: about a third of the period, which is where the
	// materials gym already sits and is comfortably clear of msm's three-millisecond timer lead.
	static constexpr Duration Batch = std::chrono::microseconds{ 5'000 };

private:
	// The measurement, which leaves `m_Reading` filled in and touches no sysfs. Split from the write
	// below so that every early return inside it is a probe that could not answer, and the one caller
	// then does the same thing with all of them.
	void Probe(const IClock& clock, VulkanDevice& device, VulkanTextures& textures);

	// The reading acted on. Silent where the driver answered, and the ceiling where it did not.
	void Command();

	GovernorReading m_Reading;
	GpuFloor m_Floor;
};
