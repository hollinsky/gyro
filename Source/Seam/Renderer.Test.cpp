#include "Seam/Renderer.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "Core/Result.h"
#include "Core/Time.h"
#include "Geometry/Region.h"
#include "Geometry/Space.h"
#include "Seam/RenderTarget.h"
#include "Seam/SyncPoint.h"
#include "Testing/Test.h"

// What is worth testing here is the part that is not in the signatures. Decision 79 pairs one
// presenter with two writers whose devices have nothing in common, so the assertions are about the
// contract that has to hold across both: a renderer refuses memory it cannot bind rather than
// assuming, completion is polled and never waited on, costs arrive on two different schedules, and
// the draw list is a preorder tree whose groups a walker can find without being told where they are.
//
// Two fakes, because one would prove the interface is implementable and not that it is *substitutable*
// — which is the whole claim decision 79 makes. FakeBlitter is Blit's shape: mapped memory, immediate
// completion, no second device to cost. FakeDevice is the Vulkan one's: dmabuf only, a timeline, and
// timestamps that resolve some frames after the submission that wrote them.

namespace
{
using namespace std::chrono_literals;

class FakeBlitter : public IRenderer
{
public:
	[[nodiscard]] Result<void> BindTargets(std::span<const RenderTarget> targets) override
	{
		for (const RenderTarget& target : targets)
		{
			if (!target.IsMapped())
			{
				return Failure(EINVAL, "a blitter cannot bind a dmabuf");
			}
		}

		m_Bound.assign(targets.begin(), targets.end());

		return {};
	}

	void ReleaseTargets() noexcept override { m_Bound.clear(); }

	[[nodiscard]] Result<Submission> Record(const RecordRequest& request) override
	{
		if (request.Target >= m_Bound.size())
		{
			return Failure(EINVAL, "recording into an unbound target");
		}

		m_Recorded.push_back(request.Target);
		m_Items.assign(request.Items.begin(), request.Items.end());
		m_Drew = !request.Damage.IsEmpty();

		// Finished on the CPU before returning, which is what makes the point immediate.
		return Submission{ SyncPoint::Immediate(), 2ms };
	}

	[[nodiscard]] bool IsComplete(SyncPoint point) const override { return point.IsImmediate(); }

	[[nodiscard]] std::size_t CollectCosts(std::span<GpuCost>) override { return 0; }

	[[nodiscard]] const std::vector<DrawItem>& Items() const { return m_Items; }

	[[nodiscard]] const std::vector<std::uint32_t>& Recorded() const { return m_Recorded; }

	[[nodiscard]] bool Drew() const { return m_Drew; }

private:
	std::vector<RenderTarget> m_Bound;
	std::vector<DrawItem> m_Items;
	std::vector<std::uint32_t> m_Recorded;
	bool m_Drew = false;
};

class FakeDevice : public IRenderer
{
public:
	[[nodiscard]] Result<void> BindTargets(std::span<const RenderTarget> targets) override
	{
		for (const RenderTarget& target : targets)
		{
			if (target.IsMapped())
			{
				return Failure(EINVAL, "a device cannot bind a CPU mapping");
			}
		}

		m_Bound.assign(targets.begin(), targets.end());

		return {};
	}

	void ReleaseTargets() noexcept override { m_Bound.clear(); }

	[[nodiscard]] Result<Submission> Record(const RecordRequest& request) override
	{
		if (request.Target >= m_Bound.size())
		{
			return Failure(EINVAL, "recording into an unbound target");
		}

		// The point is named before the work exists, which is Seam/SyncPoint.h's whole reason.
		++m_Value;
		m_Pending.push_back({ 7ms, request.CostGeneration, request.Mode });

		return Submission{ SyncPoint{ RawFd{ 31 }, m_Value }, 3ms };
	}

	[[nodiscard]] bool IsComplete(SyncPoint point) const override
	{
		return point.IsImmediate() || point.Value <= m_Signalled;
	}

	[[nodiscard]] std::size_t CollectCosts(std::span<GpuCost> into) override
	{
		std::size_t written = 0;

		while (written < into.size() && !m_Pending.empty() && m_Resolved > 0)
		{
			into[written] = m_Pending.front();
			m_Pending.pop_front();
			--m_Resolved;
			++written;
		}

		return written;
	}

	// The queue finishing, some indefinite time after the thread stopped talking about it.
	void Signal(std::uint64_t value) { m_Signalled = value; }

	// A timestamp query becoming readable, which happens later still and out of step with the flip.
	void Resolve(std::size_t count) { m_Resolved += count; }

private:
	std::vector<RenderTarget> m_Bound;
	std::deque<GpuCost> m_Pending;
	std::uint64_t m_Value = 0;
	std::uint64_t m_Signalled = 0;
	std::size_t m_Resolved = 0;
};

[[nodiscard]] RenderTarget MappedTarget()
{
	MappedImage image;
	image.Pixels = reinterpret_cast<std::byte*>(std::uintptr_t{ 0x1000 });
	image.Stride = 1920 * 4;
	image.Length = 1920 * 1080 * 4;

	return { { 1920, 1080 }, { FormatXrgb8888, 0, ModifierLinear }, image };
}

[[nodiscard]] RenderTarget DmabufTarget()
{
	DmabufImage image;
	image.Planes[0] = { RawFd{ 12 }, 0, 1920 * 4 };
	image.PlaneCount = 1;

	return { { 1920, 1080 }, { FormatXrgb8888, 0, ModifierLinear }, image };
}

[[nodiscard]] RecordRequest FullFrame(std::span<const DrawItem> items)
{
	RecordRequest request;
	request.Target = 0;
	request.Damage.Add({ { 0, 0 }, { 1920, 1080 } });
	request.Items = items;

	return request;
}

[[nodiscard]] DrawItem Solid(Rect<DeviceSpace> where)
{
	DrawItem item;
	item.Content = DrawSolid{ 1.0F, 0.0F, 0.0F, 1.0F };
	item.Shape = Quad::FromRect(where);
	item.Extent = { where.Extent.Width, where.Extent.Height };

	return item;
}

[[nodiscard]] DrawItem Group(std::uint32_t count, Rect<DeviceSpace> bound, float opacity)
{
	DrawItem item;
	item.Content = DrawGroup{ count };
	item.Shape = Quad::FromRect(bound);
	item.Extent = { bound.Extent.Width, bound.Extent.Height };
	item.Opacity = opacity;

	return item;
}

// What a renderer does with a group: the members are a contiguous run, found by arithmetic rather
// than by a walk. Written here because it is the encoding the test is asserting, and a renderer that
// read Count as a child count would take a submenu's panel for a sibling of the menu.
[[nodiscard]] std::span<const DrawItem> Members(std::span<const DrawItem> items, std::size_t index)
{
	const auto* group = std::get_if<DrawGroup>(&items[index].Content);

	if (group == nullptr)
	{
		return {};
	}

	return items.subspan(index + 1, group->Count);
}
} // namespace

// Decision 79's price, paid at the one place it can be caught. A renderer handed the memory kind it
// cannot use is a composition-root miswiring, and the variant in RenderTarget exists so that it is a
// branch somebody wrote rather than a cast that works on the machine it was written on.
GYRO_TEST(Renderer, ARendererRefusesMemoryItCannotBindRatherThanAssuming)
{
	FakeBlitter blitter;
	FakeDevice device;

	const RenderTarget mapped[] = { MappedTarget() };
	const RenderTarget dmabuf[] = { DmabufTarget() };

	GYRO_CHECK(blitter.BindTargets(mapped).has_value());
	GYRO_CHECK(device.BindTargets(dmabuf).has_value());

	const Result<void> blitterRefused = blitter.BindTargets(dmabuf);
	const Result<void> deviceRefused = device.BindTargets(mapped);

	GYRO_REQUIRE(!blitterRefused.has_value());
	GYRO_REQUIRE(!deviceRefused.has_value());
	GYRO_CHECK_EQ(blitterRefused.error().Code(), EINVAL);
	GYRO_CHECK_EQ(deviceRefused.error().Code(), EINVAL);
}

// The descriptors in a target set belong to the presenter and are closed when it invalidates them, so
// the imports have to go at the invalidation rather than when a replacement set arrives.
GYRO_TEST(Renderer, ReleasedTargetsCannotBeRecordedInto)
{
	FakeBlitter blitter;

	const RenderTarget mapped[] = { MappedTarget() };
	const DrawItem items[] = { Solid({ { 0.0F, 0.0F }, { 1920.0F, 1080.0F } }) };

	GYRO_REQUIRE(blitter.BindTargets(mapped).has_value());
	GYRO_REQUIRE(blitter.Record(FullFrame(items)).has_value());

	blitter.ReleaseTargets();

	const Result<Submission> refused = blitter.Record(FullFrame(items));

	GYRO_REQUIRE(!refused.has_value());
	GYRO_CHECK_EQ(refused.error().Code(), EINVAL);
}

// The two devices report on two schedules, which is why Frame/Budget.h files them through two members
// and why only one of them carries a generation. The CPU figure is known when the call returns; the
// GPU figure outlives the frame that produced it.
GYRO_TEST(Renderer, TheCpuCostReturnsAndTheGpuCostIsCollected)
{
	FakeDevice device;

	const RenderTarget dmabuf[] = { DmabufTarget() };
	const DrawItem items[] = { Solid({ { 0.0F, 0.0F }, { 1920.0F, 1080.0F } }) };

	GYRO_REQUIRE(device.BindTargets(dmabuf).has_value());

	RecordRequest request = FullFrame(items);
	request.CostGeneration = 4;
	request.Mode = RenderMode::Floor;

	const Result<Submission> submitted = device.Record(request);

	GYRO_REQUIRE(submitted.has_value());
	GYRO_CHECK_EQ(submitted->RecordCost, Duration{ 3ms });
	GYRO_CHECK(!submitted->Point.IsImmediate());

	// Nothing has resolved yet, and a collection that found something here would be reporting a
	// measurement the GPU has not taken.
	GpuCost costs[4];
	GYRO_CHECK_EQ(device.CollectCosts(costs), std::size_t{ 0 });

	device.Resolve(1);

	GYRO_REQUIRE_EQ(device.CollectCosts(costs), std::size_t{ 1 });
	GYRO_CHECK_EQ(costs[0].Generation, std::uint32_t{ 4 });
	GYRO_CHECK(costs[0].Mode == RenderMode::Floor);
	GYRO_CHECK_EQ(costs[0].Cost, Duration{ 7ms });
}

// A renderer with no second device is not a stub returning nothing yet; it is a correct report that
// its work had no GPU half. Blit is that renderer on every boot, and so is the null one the
// schedulability sweep runs.
GYRO_TEST(Renderer, ARendererWithNoSecondDeviceCostsNothingOnIt)
{
	FakeBlitter blitter;

	const RenderTarget mapped[] = { MappedTarget() };
	const DrawItem items[] = { Solid({ { 0.0F, 0.0F }, { 1920.0F, 1080.0F } }) };

	GYRO_REQUIRE(blitter.BindTargets(mapped).has_value());

	const Result<Submission> submitted = blitter.Record(FullFrame(items));

	GYRO_REQUIRE(submitted.has_value());
	GYRO_CHECK(submitted->Point.IsImmediate());

	GpuCost costs[4];
	GYRO_CHECK_EQ(blitter.CollectCosts(costs), std::size_t{ 0 });
}

// Decision 35's record-time check reads completion before it starts a frame, so the answer has to be
// available without waiting — and immediate has to mean complete, or the floor path would poll a point
// that is never going to be signalled by anything.
GYRO_TEST(Renderer, CompletionIsPolledAndImmediateIsComplete)
{
	FakeBlitter blitter;
	FakeDevice device;

	const RenderTarget mapped[] = { MappedTarget() };
	const RenderTarget dmabuf[] = { DmabufTarget() };
	const DrawItem items[] = { Solid({ { 0.0F, 0.0F }, { 1920.0F, 1080.0F } }) };

	GYRO_REQUIRE(blitter.BindTargets(mapped).has_value());
	GYRO_REQUIRE(device.BindTargets(dmabuf).has_value());

	GYRO_CHECK(blitter.IsComplete(blitter.Record(FullFrame(items))->Point));

	const Result<Submission> pending = device.Record(FullFrame(items));

	GYRO_REQUIRE(pending.has_value());
	GYRO_CHECK(!device.IsComplete(pending->Point));

	device.Signal(pending->Point.Value);

	GYRO_CHECK(device.IsComplete(pending->Point));
}

// Chunking needs nothing added to the interface, which is the claim worth pinning: it is several calls
// against one held target, and the last point is what the present waits on. If this ever needed a
// begin/end pair, decision 29's contingency would be a change to every backend rather than to the loop.
GYRO_TEST(Renderer, ChunkingIsSeveralRecordsAgainstOneHeldTarget)
{
	FakeDevice device;

	const RenderTarget dmabuf[] = { DmabufTarget() };
	const DrawItem items[] = { Solid({ { 0.0F, 0.0F }, { 960.0F, 1080.0F } }) };

	GYRO_REQUIRE(device.BindTargets(dmabuf).has_value());

	const Result<Submission> first = device.Record(FullFrame(items));
	const Result<Submission> second = device.Record(FullFrame(items));

	GYRO_REQUIRE(first.has_value() && second.has_value());
	GYRO_CHECK_EQ(first->Point.Timeline, second->Point.Timeline);
	GYRO_CHECK(first->Point.Value < second->Point.Value);

	// The first chunk finishing is not the frame finishing, which is the reason the caller waits on the
	// last point rather than on any of them.
	device.Signal(first->Point.Value);

	GYRO_CHECK(device.IsComplete(first->Point));
	GYRO_CHECK(!device.IsComplete(second->Point));
}

// Z is the list order, so the only assertion available is that the order survived — which is the one
// that matters, exactly as it is one seam over in Seam/Presenter.Test.cpp.
GYRO_TEST(Renderer, TheListIsBottomFirstAndTheOrderSurvives)
{
	FakeBlitter blitter;

	const RenderTarget mapped[] = { MappedTarget() };
	const DrawItem items[] = {
		Solid({ { 0.0F, 0.0F }, { 1920.0F, 1080.0F } }),
		Solid({ { 100.0F, 100.0F }, { 64.0F, 64.0F } }),
	};

	GYRO_REQUIRE(blitter.BindTargets(mapped).has_value());
	GYRO_REQUIRE(blitter.Record(FullFrame(items)).has_value());
	GYRO_REQUIRE_EQ(blitter.Items().size(), std::size_t{ 2 });

	GYRO_CHECK_EQ(blitter.Items()[0].Shape.Bounds(), Rect<DeviceSpace>{ { 0.0F, 0.0F }, { 1920.0F, 1080.0F } });
	GYRO_CHECK_EQ(blitter.Items()[1].Shape.Bounds(), Rect<DeviceSpace>{ { 100.0F, 100.0F }, { 64.0F, 64.0F } });
}

// Decision 60's flattening, expressed. A group names the run that composes into its offscreen, and
// nesting works because a nested group's own run is inside its parent's — which is what makes a
// dismissing menu with a submenu open one image rather than two overlapping fades.
GYRO_TEST(Renderer, AGroupNamesTheRunThatFlattensIntoIt)
{
	const DrawItem items[] = {
		Solid({ { 0.0F, 0.0F }, { 1920.0F, 1080.0F } }),            // the desktop, outside every group
		Group(3, { { 200.0F, 200.0F }, { 400.0F, 300.0F } }, 0.5F), // a menu fading out
		Solid({ { 200.0F, 200.0F }, { 400.0F, 300.0F } }),          // its panel
		Group(1, { { 300.0F, 260.0F }, { 200.0F, 160.0F } }, 1.0F), // with a submenu still open
		Solid({ { 300.0F, 260.0F }, { 200.0F, 160.0F } }),          // the submenu's panel
	};

	GYRO_CHECK(Members(items, 0).empty());

	const std::span<const DrawItem> menu = Members(items, 1);

	GYRO_REQUIRE_EQ(menu.size(), std::size_t{ 3 });
	GYRO_CHECK_EQ(menu.back().Shape.Bounds(), items[4].Shape.Bounds());

	// The count is the run and not the children, so the submenu's own panel is inside the menu's run
	// rather than beside it. Reading it the other way walks off the end of a list this size.
	const std::span<const DrawItem> submenu = Members(items, 3);

	GYRO_REQUIRE_EQ(submenu.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(submenu.front().Shape.Bounds(), items[4].Shape.Bounds());

	// The group's opacity is the group's, and the members keep their own. That difference is the whole
	// of decision 60: a group fade is one alpha applied to a flattened image, not an alpha per node.
	GYRO_CHECK_EQ(items[1].Opacity, 0.5F);
	GYRO_CHECK_EQ(items[2].Opacity, 1.0F);
}

// A target reached through the ring may be stale by age with nothing having moved, so an empty damage
// region is the caller saying there is nothing to redraw — not the caller skipping the frame, which it
// would express by not calling at all.
GYRO_TEST(Renderer, EmptyDamageIsNothingToRedrawRatherThanNothingToDo)
{
	FakeBlitter blitter;

	const RenderTarget mapped[] = { MappedTarget() };
	const DrawItem items[] = { Solid({ { 0.0F, 0.0F }, { 1920.0F, 1080.0F } }) };

	GYRO_REQUIRE(blitter.BindTargets(mapped).has_value());

	RecordRequest quiet;
	quiet.Items = items;

	GYRO_REQUIRE(blitter.Record(quiet).has_value());
	GYRO_CHECK(!blitter.Drew());
	GYRO_CHECK_EQ(blitter.Recorded().size(), std::size_t{ 1 });
}

// The bound rounds outward and has one home for doing so. Rounding a moving edge inward is how a
// one-pixel trail is left behind, which Geometry/Region.h says at length about damage and is the same
// hazard here.
GYRO_TEST(Renderer, ThePixelBoundRoundsOutward)
{
	const Quad quad = Quad::FromRect({ { 10.25F, 20.75F }, { 64.5F, 32.5F } });

	GYRO_CHECK_EQ(quad.PixelBounds(), PixelRect<DeviceSpace>{ { 10, 20 }, { 65, 34 } });
}

// A projected quad is not a rectangle, and its bound is what damage and the effect cost model both
// key on — decision 55 says a transform costs the axis-aligned bound of a projected quad and this is
// that bound.
GYRO_TEST(Renderer, AProjectedQuadStillHasABound)
{
	Quad quad;
	quad.Corners[0] = { 100.0F, 100.0F };
	quad.Corners[1] = { 300.0F, 60.0F };
	quad.Corners[2] = { 300.0F, 240.0F };
	quad.Corners[3] = { 100.0F, 200.0F };
	quad.Weights[1] = 0.8F;
	quad.Weights[2] = 0.8F;

	GYRO_CHECK_EQ(quad.Bounds(), Rect<DeviceSpace>{ { 100.0F, 60.0F }, { 200.0F, 180.0F } });
	GYRO_CHECK_EQ(quad.PixelBounds(), PixelRect<DeviceSpace>{ { 100, 60 }, { 200, 180 } });
}
