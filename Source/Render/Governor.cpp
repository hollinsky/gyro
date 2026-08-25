#include "Render/Governor.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "Core/Result.h"
#include "Geometry/Region.h"
#include "Geometry/Space.h"
#include "Render/Device.h"
#include "Render/GpuClock.h"
#include "Render/Renderer.h"
#include "Render/Textures.h"
#include "Seam/Pixel.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"

namespace
{
// A panel's worth of pixels, because the question is what the part does with a frame rather than what
// it does with a benchmark. The fragment count is then set by how many of these are stacked, which is
// the term the calibration moves.
constexpr PixelSize<DeviceSpace> ProbeSize{ 1920, 1080 };

// Linear alone, because the probe never reads the pixels back and a modifier every driver renders
// into is worth more here than a fast one. A device that refuses even this comes up unmeasured, which
// takes the floor.
constexpr std::array<std::uint64_t, 1> Candidates{ ModifierLinear };

// What the calibration starts from, and the bound it may not scale past. The ceiling is not a
// performance limit — it is the point past which a machine so fast that four hundred full-screen
// layers still finish inside six milliseconds is better described by the batch length in the log than
// by a probe that keeps doubling.
constexpr std::uint32_t SeedLayers = 4;
constexpr std::uint32_t MaxLayers = 512;

// How the layer count is solved for: two passes of a few frames each, because growing the stack moves
// the clock it was measured at and one division lands wide.
constexpr std::uint32_t CalibrationPasses = 2;
constexpr std::uint32_t CalibrationFrames = 4;

// One arm of one round.
struct Run
{
	// The highest clock read while the queue was draining, in MHz. The peak rather than the mean
	// because the part is parked either side of the batch and a mean would be measuring how long the
	// probe spun rather than what the governor granted.
	std::uint32_t PeakMhz = 0;

	Duration Elapsed{};
	bool Ran = false;
};

// A stack of full-screen opaque fills, which is the cheapest way to buy a known number of fragments
// out of a pipeline that already exists.
//
// **Opaque rather than translucent, and full-screen rather than tiled.** A blended stack would measure
// the blend hardware and a tiled one the raster setup; what is wanted is the plain shading rate the
// part is capable of, so that the calibration below converges on a batch length rather than on a
// pathology. Each layer is a different colour only so that a driver eliding a redundant draw would be
// eliding something visibly different.
[[nodiscard]] std::vector<DrawItem> Stack(std::uint32_t layers)
{
	const Rect<DeviceSpace> whole{ {}, { static_cast<float>(ProbeSize.Width), static_cast<float>(ProbeSize.Height) } };
	std::vector<DrawItem> items;
	items.reserve(layers);

	for (std::uint32_t index = 0; index < layers; ++index)
	{
		const float shade = static_cast<float>(index % 8U) / 8.0F;

		items.push_back(
			DrawItem{ .Content = DrawSolid{ shade, 1.0F - shade, 0.5F, 1.0F },
		              .Shape = Quad::FromRect(whole),
		              .Extent = { whole.Extent.Width, whole.Extent.Height },
		              .Opacity = 1.0F,
		              .Radius = 0.0F,
		              .Dress = Material::None,
		              .Lift = {},
		              .Color = ColorState::Srgb(),
		              .Sampling = {} }
		);
	}

	return items;
}

// Submit the stack once and watch the clock until the queue has drained.
//
// **The deadline is one batch length away and not a microsecond**, which is the deadline a frame loop
// would actually state for this work: the composite has to be finished by the time it is finished by.
// An impossible deadline would be a different question — *what does the driver do when it is already
// late* — and gyro does not state those.
//
// The poll is a spin because that is what the frame loop does, and `IRenderer::IsComplete` is a
// counter read rather than a wait for exactly that reason. The clock read inside it is a `pread` of a
// small sysfs file, which is why `GpuClock::Read` exists beside the rate-limited `Sample` the frame
// thread uses.
[[nodiscard]] Run Once(
	const IClock& clock,
	VulkanRenderer& renderer,
	GpuClock& frequency,
	const Region<DeviceSpace>& damage,
	std::span<const DrawItem> items,
	bool urgent
)
{
	RecordRequest request{
		.Target = 0, .Mode = RenderMode::Planned, .Quality = Tier::High, .Damage = damage, .Items = items
	};

	const Instant started = clock.Now();
	request.Deadline = urgent ? started + GpuGovernor::Batch : Instant{};

	const Result<Submission> submission = renderer.Record(request);

	if (!submission)
	{
		return {};
	}

	Run run;

	while (!renderer.IsComplete(submission->Point))
	{
		run.PeakMhz = std::max(run.PeakMhz, frequency.Read());
	}

	run.Elapsed = Elapsed(started, clock.Now());
	run.Ran = true;

	return run;
}

// The middle value, which is what an arm's frames are reduced to. A median rather than a mean because
// one frame landing behind another process's batch is an outlier the mean would carry into the
// comparison and the median discards.
template<std::size_t Count>
[[nodiscard]] std::uint32_t Median(std::array<std::uint32_t, Count>& values) noexcept
{
	std::ranges::sort(values);

	return values[Count / 2];
}

// One arm: a run of frame-shaped composites, all of them stating a deadline or none of them.
//
// **The idle between them is the measurement and not a courtesy.** Submitting back to back keeps the
// part busy, which is the one condition under which the kernel's governor already grants full clock —
// so a probe shaped that way reads the ceiling on both arms and has answered nothing. Sleeping out the
// rest of the period is what parks the GPU once per frame, which is the ratchet decision 142 is about.
//
// Zero where any submission was refused, which `Judge` reads as *take the floor*.
[[nodiscard]] std::uint32_t
Arm(const IClock& clock,
    VulkanRenderer& renderer,
    GpuClock& frequency,
    const Region<DeviceSpace>& damage,
    std::span<const DrawItem> items,
    bool urgent,
    Duration& batch)
{
	std::array<std::uint32_t, GpuGovernor::Frames - GpuGovernor::Settling> measured{};

	for (std::uint32_t frame = 0; frame < GpuGovernor::Frames; ++frame)
	{
		const Instant began = clock.Now();
		const Run run = Once(clock, renderer, frequency, damage, items, urgent);

		if (!run.Ran)
		{
			return 0;
		}

		// The settling frames are thrown away rather than averaged in: the park ratchet takes two or
		// three frames to come down from wherever the previous arm left the clock, and folding those in
		// would make the arm that ran second look faster on every machine.
		if (frame >= GpuGovernor::Settling)
		{
			measured[frame - GpuGovernor::Settling] = run.PeakMhz;
			batch = std::max(batch, run.Elapsed);
		}

		// The rest of the refresh, spent asleep. A spin here would keep the CPU hot and the GPU parked,
		// which is the same wall clock and a worse thing to do on the machine gyro is about to run on.
		if (const Duration idle = GpuGovernor::Period - Elapsed(began, clock.Now()); idle > Duration::zero())
		{
			std::this_thread::sleep_for(idle);
		}
	}

	return Median(measured);
}
} // namespace

DeadlineResponse GpuGovernor::Judge(std::uint32_t quietMhz, std::uint32_t urgentMhz) noexcept
{
	if (quietMhz == 0 || urgentMhz == 0)
	{
		// The probe ran and read nothing back — an unreadable clock node, or a part that reported zero
		// throughout because it was never actually busy. Decision 142's recoverable direction makes that
		// the same answer as a driver that dropped the number.
		return DeadlineResponse::Ignored;
	}

	const std::uint64_t stepped = std::uint64_t{ quietMhz } * (100U + Margin);

	return std::uint64_t{ urgentMhz } * 100U >= stepped ? DeadlineResponse::Honoured : DeadlineResponse::Ignored;
}

GpuGovernor GpuGovernor::Take(const IClock& clock, VulkanDevice& device, VulkanTextures& textures)
{
	GpuGovernor governor;
	governor.m_Floor = GpuFloor::Open(device.Description().PrimaryMinor);

	if (!governor.m_Floor.IsValid())
	{
		// `Open` has already said which of the two it was, by driver and by minor. Nothing here can act
		// on the answer, so nothing here spends a tenth of a second finding it out.
		return governor;
	}

	if (governor.m_Floor.Ceiling() == 0 || governor.m_Floor.Ceiling() <= governor.m_Floor.Original())
	{
		spdlog::info(
			"GPU frequency floor not probed: the minimum is already {} MHz against a ceiling of {}",
			governor.m_Floor.Original(),
			governor.m_Floor.Ceiling()
		);

		return governor;
	}

	if (!device.Description().ExportsTimeline || device.Description().RenderMinor < 0)
	{
		// There is nothing to probe *with*. A deadline is stated against a point on an exported
		// timeline, so a device that exports none states none — which is not an inconclusive
		// measurement but a definite no, and it takes the floor without spending the boot on it.
		governor.m_Reading.Response = DeadlineResponse::Ignored;
		spdlog::info("this device names no fence to carry a deadline, so the frequency floor is gyro's to command");
		governor.Command();

		return governor;
	}

	governor.Probe(clock, device, textures);
	governor.Command();

	return governor;
}

void GpuGovernor::Probe(const IClock& clock, VulkanDevice& device, VulkanTextures& textures)
{
	const Instant opened = clock.Now();

	// Everything below leaves `Response` at `Unprobed` on the way out and is corrected at the end, so
	// that a path that returns early is one that took the floor.
	m_Reading.Response = DeadlineResponse::Ignored;

	const Result<ExportedImage> target = device.Export(ProbeSize, FormatXrgb8888, Candidates);

	if (!target)
	{
		spdlog::info("GPU frequency probe: this device would not export a target ({})", target.error().Context());

		return;
	}

	VulkanRenderer renderer{ clock, device, textures };

	if (const Result<void>& status = renderer.Status(); !status)
	{
		spdlog::info("GPU frequency probe: no renderer to probe with ({})", status.error().Context());

		return;
	}

	if (const Result<void> bound = renderer.BindTargets({ &target->Target(), 1 }, ColorState::Srgb()); !bound)
	{
		spdlog::info("GPU frequency probe: the probe target would not bind ({})", bound.error().Context());

		return;
	}

	GpuClock frequency = GpuClock::Open(device.Description().PrimaryMinor);

	Region<DeviceSpace> damage;
	damage.Add(PixelRect<DeviceSpace>{ {}, ProbeSize });

	// **The calibration is frame-shaped too, and it has to be.** What is being solved for is a layer
	// count that composites in about five milliseconds *at the clock the arms will run at*, and that
	// clock is the parked one. Calibrating against back-to-back submissions would size the batch at the
	// ceiling — four times too big on the part this was written against — and the arms would then have
	// no idle left in the period, which is the exact mistake the shape of `Arm` exists to avoid.
	//
	// **And it iterates, because the first pass changes the answer it was measured under.** Growing the
	// stack lengthens the batch, which raises the duty cycle, which moves the clock — so one division
	// lands wide. Two passes converge; a third buys nothing worth another sixty milliseconds of boot,
	// and the batch that actually ran is in the log either way rather than being assumed.
	std::uint32_t layers = SeedLayers;
	std::vector<DrawItem> items = Stack(layers);

	for (std::uint32_t pass = 0; pass < CalibrationPasses; ++pass)
	{
		Run seed;

		for (std::uint32_t frame = 0; frame < CalibrationFrames; ++frame)
		{
			const Instant began = clock.Now();
			seed = Once(clock, renderer, frequency, damage, items, false);

			if (!seed.Ran)
			{
				break;
			}

			if (const Duration idle = Period - Elapsed(began, clock.Now()); idle > Duration::zero())
			{
				std::this_thread::sleep_for(idle);
			}
		}

		if (!seed.Ran || seed.Elapsed <= Duration::zero())
		{
			spdlog::info("GPU frequency probe: the calibration submission did not complete");

			return;
		}

		const std::uint64_t scaled = std::uint64_t{ layers } * static_cast<std::uint64_t>(Batch.count()) /
		                             static_cast<std::uint64_t>(seed.Elapsed.count());

		layers = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(scaled, 1U, MaxLayers));
		items = Stack(layers);
	}

	std::uint32_t quiet = 0;
	std::uint32_t urgent = 0;
	Duration batch{};

	for (std::uint32_t round = 0; round < Rounds; ++round)
	{
		// The order swaps between rounds. A part warms into a probe, and a fixed quiet-then-urgent order
		// would read that drift as the urgent arm answering.
		const bool urgentFirst = (round % 2U) == 1U;

		const std::uint32_t first = Arm(clock, renderer, frequency, damage, items, urgentFirst, batch);
		const std::uint32_t second = Arm(clock, renderer, frequency, damage, items, !urgentFirst, batch);

		if (first == 0 || second == 0)
		{
			spdlog::info("GPU frequency probe: a submission was refused mid-probe");

			return;
		}

		// The rounds are summed rather than collected, because two of them have no median and the whole
		// point of the second is to cancel the first's ordering rather than to outvote it.
		quiet += urgentFirst ? second : first;
		urgent += urgentFirst ? first : second;
	}

	m_Reading.QuietMhz = quiet / Rounds;
	m_Reading.UrgentMhz = urgent / Rounds;
	m_Reading.Batch = batch;
	m_Reading.Response = Judge(m_Reading.QuietMhz, m_Reading.UrgentMhz);

	// Released before the reading is acted on, because the next thing that happens is a write to the
	// minimum-clock node and the probe's own image has no business still being resident for it.
	renderer.ReleaseTargets();

	m_Reading.Elapsed = Elapsed(opened, clock.Now());

	spdlog::info(
		"GPU frequency probe: {} MHz with no deadline against {} MHz with one, over {} rounds of {} frames "
		"compositing up to {:.1f} ms in a {:.1f} ms period, {:.0f} ms total — the deadline is {}",
		m_Reading.QuietMhz,
		m_Reading.UrgentMhz,
		Rounds,
		Frames,
		std::chrono::duration<double, std::milli>{ m_Reading.Batch }.count(),
		std::chrono::duration<double, std::milli>{ Period }.count(),
		std::chrono::duration<double, std::milli>{ m_Reading.Elapsed }.count(),
		m_Reading.Response == DeadlineResponse::Honoured ? "reaching this driver" : "going nowhere on this driver"
	);
}

void GpuGovernor::Command()
{
	if (m_Reading.Response == DeadlineResponse::Honoured)
	{
		// The driver holds gyro's deadline *and* its own utilization history, which is strictly more
		// than gyro has, and it sits closer to the part. Decision 142 is explicit that gyro stops here
		// rather than taking a policy that belongs to the whole machine.
		spdlog::info("GPU frequency left to the driver: it answered the deadline, so gyro states it and stops there");

		return;
	}

	// **The ceiling, and not a number derived from the frame budget.** There is no controller that
	// could derive one — nothing yet turns a composite's cost into a frequency it wanted — and the
	// alternative to the top of the range is a figure invented here that would be wrong on every part
	// but the one it was tuned on. What makes the ceiling defensible meanwhile is that the part still
	// parks between frames: `intel_rps_park` writes the *hardware* minimum rather than this softlimit,
	// so a floor applies from unpark and a five-millisecond composite in a sixteen-millisecond period
	// spends two thirds of the wall clock parked whatever this says. Running the composite at full
	// clock and parking sooner is the cheaper of the two shapes, not the more expensive one.
	static_cast<void>(m_Floor.Command(m_Floor.Ceiling()));
}
