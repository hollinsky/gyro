#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "Core/Result.h"
#include "Core/Time.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Seam/SyncPoint.h"

// A renderer that draws nothing and charges what it was told to.
//
// **It is the other half of the instrument, and it lives here rather than in `Render` for the reason
// the module table gives.** `Render` is platform code — Vulkan, dmabuf import, a device that can be
// lost — and Docs/Structure.md#frame-is-portable turns on the schedulability sweep running "against
// fake clocks and a null renderer that merely charges a simulated `C`" on a machine with no GPU. A
// renderer that charges a cost is not a Vulkan device with the drawing removed; it is the second thing
// headless simulates, and it belongs beside the panel.
//
// **What it simulates is `C`, and `C` is the only thing decision 29 is about.** The record-time check
// of decision 35 composes two device figures and compares the result against a deadline; every input
// to that comparison is a duration. So a null renderer that reports durations exercises the entire
// timing path, and one that also drew pixels would exercise the same path more slowly.
//
// **Costs are scripted per mode, because the two modes are the whole of decision 35's second branch.**
// A floor composite that cost the same as a planned one would make the middle rung of the ladder
// unobservable — the branch would be taken and would change nothing — so the two are separate figures
// and a test that wants them equal says so.
//
// **The points it yields are immediate, and that is honest rather than a shortcut.**
// Seam/SyncPoint.h's null point means *nothing to wait for*, and it names the CPU-finished case
// explicitly. Nothing here submits GPU work, so there is no timeline to name and no point on it to
// wait for; a fabricated descriptor number would be a lie that a presenter written to honour it could
// eventually pass to a real syscall. What models GPU occupancy instead is the cost this reports, which
// is what `Timing` composes into `DeviceFreeAt` — the arithmetic, which is the model the frame loop
// actually schedules against.

// How many GPU samples can be waiting to be collected. The frame loop drains this once per iteration
// per output, so the queue only grows when a test deliberately resolves samples late.
inline constexpr std::size_t MaxPendingCosts = 16;

struct SimulatedRendererPolicy
{
	// The CPU half of `C`: what `Record` reports it spent, by mode. This is the figure
	// `Budget::ObserveCpu` files and the one the record-time check reads back a frame later.
	Duration PlannedCpu{};
	Duration FloorCpu{};

	// The GPU half, resolved late. Reported through `CollectCosts` rather than returned, because on a
	// real device a timestamp is read back some frames after the submission that wrote it — which is
	// why `GpuCost` carries its own generation and mode, and why `Budget` can drop a sample taken under
	// a configuration that no longer exists.
	Duration PlannedGpu{};
	Duration FloorGpu{};

	// How many `Record` calls pass before a submission's GPU cost becomes collectable. Zero makes it
	// available to the very next collection, which is the ordinary case; a larger value is how the
	// generation-drop path gets walked, since a mode set inside the delay invalidates the sample.
	std::uint32_t CostDelay = 0;
};

class SimulatedRenderer final : public IRenderer
{
public:
	SimulatedRenderer() = default;

	explicit SimulatedRenderer(SimulatedRendererPolicy policy) noexcept : m_Policy{ policy } {}

	[[nodiscard]] Result<void> BindTargets(std::span<const RenderTarget> targets) override
	{
		// Seam/Renderer.h obliges a renderer to refuse what it cannot bind rather than to assume, and
		// names a `Blit` handed a dmabuf as a composition-root miswiring rather than a frame's problem.
		// This one draws nothing and could bind either; it refuses a dmabuf anyway, because the
		// simulated backend allocates CPU mappings and a set that arrived with descriptors in it came
		// from somewhere this renderer was not wired to.
		for (const RenderTarget& target : targets)
		{
			if (!target.IsMapped())
			{
				return Failure(EINVAL, "simulated renderer binds CPU mappings only");
			}
		}

		m_Bound = targets.size();

		return {};
	}

	void ReleaseTargets() noexcept override { m_Bound = 0; }

	[[nodiscard]] Result<Submission> Record(const RecordRequest& request) override
	{
		if (request.Target >= m_Bound)
		{
			return Failure(EINVAL, "record names an unbound target");
		}

		if (Refuse)
		{
			return Failure(EBUSY, "simulated renderer refuses");
		}

		++Records;
		LastMode = request.Mode;

		// Filed now, collectable later. The mode and generation travel with the sample because neither
		// is knowable when it resolves — by then this renderer may have recorded other frames in other
		// modes against an output that was reconfigured underneath both.
		Enqueue(
			{ .Cost = request.Mode == RenderMode::Planned ? m_Policy.PlannedGpu : m_Policy.FloorGpu,
		      .Generation = request.CostGeneration,
		      .Mode = request.Mode }
		);

		return Submission{ .Point = SyncPoint::Immediate(),
			               .RecordCost =
			                   Overrun > Duration::zero() ?
			                       std::exchange(Overrun, Duration::zero()) :
			                       (request.Mode == RenderMode::Planned ? m_Policy.PlannedCpu : m_Policy.FloorCpu) };
	}

	// Immediate points are complete by definition, which is the whole of what this can answer. See the
	// header for why there is no timeline behind it to ask.
	[[nodiscard]] bool IsComplete(SyncPoint point) const override { return point.IsImmediate(); }

	[[nodiscard]] std::size_t CollectCosts(std::span<GpuCost> into) override
	{
		// Held back rather than timestamped: what a test wants to say is "resolves two frames later", and
		// a duration would make that depend on the schedule it is being used to test.
		const std::size_t collectable = m_Queued > m_Policy.CostDelay ? m_Queued - m_Policy.CostDelay : 0;
		const std::size_t written = std::min(into.size(), collectable);

		for (std::size_t index = 0; index < written; ++index)
		{
			into[index] = m_Pending[index];
		}

		m_Queued -= written;

		for (std::size_t index = 0; index < m_Queued; ++index)
		{
			m_Pending[index] = m_Pending[index + written];
		}

		return written;
	}

	// Make the next `Record` cost this much instead of what the policy says, once.
	//
	// **This is how a deliberate deadline miss is injected**, and Docs/Architecture.md#headless names
	// that as one of the two things the backend exists for: a frame that overruns is what proves the
	// scheduler recovers monotonically rather than cascading. It is a single frame rather than a new
	// policy because the interesting case is the transient — a scene that is expensive once — and a
	// policy change would be a new steady state that the budget's window absorbs.
	Duration Overrun{};

	bool Refuse = false;
	int Records = 0;
	RenderMode LastMode = RenderMode::Planned;

private:
	// Silently full past capacity, which is the recoverable direction: a dropped sample leaves the
	// budget holding the mark it already had, and the alternative is a backend that fails a frame
	// because nobody collected its telemetry.
	void Enqueue(GpuCost cost) noexcept
	{
		if (m_Queued < MaxPendingCosts)
		{
			m_Pending[m_Queued] = cost;
			++m_Queued;
		}
	}

	SimulatedRendererPolicy m_Policy{};
	std::size_t m_Bound = 0;

	std::array<GpuCost, MaxPendingCosts> m_Pending{};
	std::size_t m_Queued = 0;
};
