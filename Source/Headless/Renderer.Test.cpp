#include "Headless/Renderer.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <span>

#include "Core/ColorState.h"
#include "Core/Time.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Testing/Test.h"

// It draws nothing, so what is worth testing is the only thing it produces: durations, filed the way
// Frame/Budget.h reads them back. The CPU half returns by value because the caller files it the moment
// the call returns; the GPU half is collected later and carries its own mode and generation, because
// neither is knowable when it resolves.

namespace
{
using namespace std::chrono_literals;

std::array<std::byte, 64> Storage{};

RenderTarget Mapped()
{
	return RenderTarget{ .Size = { 4, 4 },
		                 .Format = { FormatXrgb8888, 0, ModifierLinear },
		                 .Memory = MappedImage{ .Pixels = Storage.data(), .Stride = 16, .Length = Storage.size() } };
}

RecordRequest Frame(RenderMode mode = RenderMode::Planned, std::uint32_t generation = 0)
{
	return RecordRequest{ .Target = 0, .Mode = mode, .CostGeneration = generation, .Damage = {}, .Items = {}, .Captures = {} };
}

// A renderer is neither copyable nor movable, per Seam/Renderer.h — Docs/Architecture.md#device-
// migration puts every device handle behind a unit that is replaced by being destroyed — so the helper
// binds one in place rather than handing one back.
void Bind(SimulatedRenderer& renderer)
{
	const std::array<RenderTarget, 1> targets{ Mapped() };
	(void)renderer.BindTargets(targets, ColorState::Srgb());
}
} // namespace

GYRO_TEST(SimulatedRenderer, RecordChargesTheModesCost)
{
	SimulatedRenderer renderer{ SimulatedRendererPolicy{ .PlannedCpu = 3ms, .FloorCpu = 1ms } };
	Bind(renderer);

	const Result<Submission> planned = renderer.Record(Frame(RenderMode::Planned));
	GYRO_REQUIRE(planned.has_value());
	GYRO_CHECK_EQ(planned->RecordCost, 3ms);

	const Result<Submission> floor = renderer.Record(Frame(RenderMode::Floor));
	GYRO_REQUIRE(floor.has_value());
	GYRO_CHECK_EQ(floor->RecordCost, 1ms);
	GYRO_CHECK_EQ(renderer.LastMode, RenderMode::Floor);
}

// Seam/SyncPoint.h's null point means nothing to wait for, and this names the CPU-finished case. A
// fabricated descriptor would be a lie a presenter written to honour it could pass to a real syscall.
GYRO_TEST(SimulatedRenderer, PointsAreImmediateAndComplete)
{
	SimulatedRenderer renderer{ SimulatedRendererPolicy{} };
	Bind(renderer);

	const Result<Submission> submission = renderer.Record(Frame());
	GYRO_REQUIRE(submission.has_value());
	GYRO_CHECK(submission->Point.IsImmediate());
	GYRO_CHECK(renderer.IsComplete(submission->Point));
}

// The injected overrun: a single frame that costs more than the policy says, which is what proves the
// scheduler recovers monotonically rather than cascading.
GYRO_TEST(SimulatedRenderer, AnOverrunAppliesToOneFrameOnly)
{
	SimulatedRenderer renderer{ SimulatedRendererPolicy{ .PlannedCpu = 2ms } };
	Bind(renderer);
	renderer.Overrun = 40ms;

	const Result<Submission> late = renderer.Record(Frame());
	GYRO_REQUIRE(late.has_value());
	GYRO_CHECK_EQ(late->RecordCost, 40ms);

	const Result<Submission> ordinary = renderer.Record(Frame());
	GYRO_REQUIRE(ordinary.has_value());
	GYRO_CHECK_EQ(ordinary->RecordCost, 2ms);
}

// Collected rather than returned, and carrying the mode and generation it was recorded under.
GYRO_TEST(SimulatedRenderer, GpuCostsCarryTheirModeAndGeneration)
{
	SimulatedRenderer renderer{ SimulatedRendererPolicy{ .PlannedGpu = 5ms, .FloorGpu = 2ms } };
	Bind(renderer);

	(void)renderer.Record(Frame(RenderMode::Planned, 7));
	(void)renderer.Record(Frame(RenderMode::Floor, 7));

	std::array<GpuCost, 4> costs{};
	GYRO_REQUIRE_EQ(renderer.CollectCosts(costs), std::size_t{ 2 });

	GYRO_CHECK_EQ(costs[0], (GpuCost{ 5ms, 7, RenderMode::Planned }));
	GYRO_CHECK_EQ(costs[1], (GpuCost{ 2ms, 7, RenderMode::Floor }));

	// Drained, so the next collection has nothing to say.
	GYRO_CHECK_EQ(renderer.CollectCosts(costs), std::size_t{ 0 });
}

// A timestamp is read back some frames after the submission that wrote it, which is what lets Budget's
// generation drop a sample taken under a configuration that no longer exists.
GYRO_TEST(SimulatedRenderer, ADelayedCostResolvesLate)
{
	SimulatedRenderer renderer{ SimulatedRendererPolicy{ .PlannedGpu = 5ms, .CostDelay = 2 } };
	Bind(renderer);
	std::array<GpuCost, 4> costs{};

	(void)renderer.Record(Frame(RenderMode::Planned, 1));
	GYRO_CHECK_EQ(renderer.CollectCosts(costs), std::size_t{ 0 });

	(void)renderer.Record(Frame(RenderMode::Planned, 1));
	GYRO_CHECK_EQ(renderer.CollectCosts(costs), std::size_t{ 0 });

	// The third submission makes the first collectable, and it still names the generation it was
	// recorded under rather than the one in force now.
	(void)renderer.Record(Frame(RenderMode::Planned, 2));
	GYRO_REQUIRE_EQ(renderer.CollectCosts(costs), std::size_t{ 1 });
	GYRO_CHECK_EQ(costs[0].Generation, 1U);
}

// Never more than the caller's buffer holds; the rest wait for the next call.
GYRO_TEST(SimulatedRenderer, CollectionIsBoundedByTheCallersBuffer)
{
	SimulatedRenderer renderer{ SimulatedRendererPolicy{ .PlannedGpu = 1ms } };
	Bind(renderer);

	for (int frame = 0; frame < 5; ++frame)
	{
		(void)renderer.Record(Frame());
	}

	std::array<GpuCost, 2> costs{};
	GYRO_CHECK_EQ(renderer.CollectCosts(costs), std::size_t{ 2 });
	GYRO_CHECK_EQ(renderer.CollectCosts(costs), std::size_t{ 2 });
	GYRO_CHECK_EQ(renderer.CollectCosts(costs), std::size_t{ 1 });
	GYRO_CHECK_EQ(renderer.CollectCosts(costs), std::size_t{ 0 });
}

// Seam/Renderer.h obliges a renderer to refuse what it cannot bind rather than to assume. A dmabuf
// reaching this one is a composition-root miswiring.
GYRO_TEST(SimulatedRenderer, ADmabufTargetIsRefused)
{
	SimulatedRenderer renderer;
	const std::array<RenderTarget, 1> targets{ RenderTarget{
		.Size = { 4, 4 },
		.Format = { FormatXrgb8888, 0, ModifierLinear },
		.Memory = DmabufImage{ .Planes = { DmabufPlane{ RawFd{ 7 }, 0, 16 } }, .PlaneCount = 1 } } };

	const Result<void> refused = renderer.BindTargets(targets, ColorState::Srgb());
	GYRO_REQUIRE(!refused.has_value());
	GYRO_CHECK_EQ(refused.error().Code(), EINVAL);
}

// After a release, Record refuses until something is bound — which is the state a target invalidation
// leaves the renderer in.
GYRO_TEST(SimulatedRenderer, RecordRefusesAnUnboundTarget)
{
	SimulatedRenderer renderer{ SimulatedRendererPolicy{} };
	Bind(renderer);

	GYRO_CHECK(renderer.Record(Frame()).has_value());

	renderer.ReleaseTargets();

	const Result<Submission> refused = renderer.Record(Frame());
	GYRO_REQUIRE(!refused.has_value());
	GYRO_CHECK_EQ(refused.error().Code(), EINVAL);
}
