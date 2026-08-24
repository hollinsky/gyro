#include "Virtual/Output.h"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/Clock.h"
#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Signal.h"
#include "Core/Time.h"
#include "Geometry/Space.h"
#include "Seam/Allocator.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/PresentationInfo.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Seam/SyncPoint.h"
#include "Testing/Test.h"
#include "Virtual/Buffer.h"
#include "Virtual/Heap.h"

// The presenter, against an allocator that needs no kernel.
//
// **Deliberately not against `UdmabufAllocator`.** What is being tested here is the ring, the
// retirement rule, and the reconfiguration ordering, none of which is about where the memory came
// from — and pinning these to `/dev/udmabuf` would make the ring untestable in exactly the
// environment decision 102 says will not have it. Udmabuf.Test.cpp owns the kernel half.
// `Integration` is where the two meet, when there is a renderer to put between them.

namespace
{
constexpr Duration Period = std::chrono::nanoseconds{ 16'666'666 };

[[nodiscard]] OutputConfiguration Configured()
{
	return OutputConfiguration{ .Resolution = { 64, 32 },
		                        .Period = Period,
		                        .Format = PixelFormat{ FormatXrgb8888, 0, ModifierLinear } };
}

// One full-output layer, which is the only shape this presenter accepts. Every field is named
// because the warning set treats a partly-initialised aggregate as an error, and here that is the
// right call twice over: a layer with a defaulted `Acquire` is a frame presented against no fence.
[[nodiscard]] PresentLayer Whole(std::uint32_t target)
{
	return PresentLayer{ .Target = target,
		                 .Blend = BlendMode::Opaque,
		                 .Acquire = SyncPoint::Immediate(),
		                 .Source = { {}, { 64.0F, 32.0F } },
		                 .Destination = { {}, { 64, 32 } },
		                 .Damage = {} };
}

// The observers a `FrameOutput` would have connected. A `Connection` is a member of the observing
// object and the method is a template argument, per Core/Signal.h — there is no lambda form, and
// the reason is the one that matters here: a signal outlives its observer during a reconfiguration,
// so the link has to be something the observer's destructor unhooks.
struct Observer
{
	std::optional<PresentationInfo> LastPresented;
	std::optional<OutputConfiguration> LastReconfigured;
	std::vector<std::string_view> Order;
	int Presents = 0;

	Connection<const PresentationInfo&> OnPresented;
	Connection<const OutputConfiguration&> OnReconfigured;
	Connection<> OnInvalidated;

	explicit Observer(VirtualOutput& output)
	{
		OnPresented.ConnectTo<&Observer::Presented>(output.Presented, *this);
		OnReconfigured.ConnectTo<&Observer::Reconfigured>(output.Reconfigured, *this);
		OnInvalidated.ConnectTo<&Observer::Invalidated>(output.TargetsInvalidated, *this);
	}

	void Presented(const PresentationInfo& info)
	{
		LastPresented = info;
		++Presents;
		Order.emplace_back("presented");
	}

	void Reconfigured(const OutputConfiguration& achieved)
	{
		LastReconfigured = achieved;
		Order.emplace_back("reconfigured");
	}

	void Invalidated() { Order.emplace_back("invalidated"); }
};
} // namespace

GYRO_TEST(VirtualOutput, AllocatesItsRingUpFront)
{
	ManualClock clock;
	HeapAllocator allocator;
	const VirtualOutput output{ clock, allocator, Configured() };

	GYRO_CHECK(output.Status().has_value());
	GYRO_CHECK_EQ(output.Targets().size(), std::size_t{ DefaultVirtualTargets });
	GYRO_CHECK_EQ(allocator.Allocations, std::uint64_t{ DefaultVirtualTargets });
	GYRO_CHECK_EQ(output.FreeTargets(), DefaultVirtualTargets);

	// Every target is describable and importable, which is the contract a renderer's `BindTargets`
	// refuses on.
	for (const RenderTarget& target : output.Targets())
	{
		GYRO_CHECK(target.IsValid());
		GYRO_CHECK(target.AsDmabuf() != nullptr);
	}
}

GYRO_TEST(VirtualOutput, AFailedAllocationLeavesNoTargetsAndSaysWhy)
{
	ManualClock clock;
	HeapAllocator allocator;
	allocator.Refuse = ENOMEM;

	const VirtualOutput output{ clock, allocator, Configured() };

	// Partial success is not expressible: half a set is a numbering with holes rather than a smaller
	// set, so the whole thing is dropped and the frame loop sees the state it already tolerates.
	GYRO_CHECK(output.Targets().empty());
	GYRO_REQUIRE(!output.Status().has_value());
	GYRO_CHECK_EQ(output.Status().error().Code(), ENOMEM);
}

GYRO_TEST(VirtualOutput, TheFrameIsHeldUntilTheConsumerReleasesIt)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured(), { .Targets = 2 } };

	GYRO_REQUIRE_EQ(output.FreeTargets(), 2U);

	const std::optional<std::uint32_t> first = output.AcquireTarget();
	GYRO_REQUIRE(first.has_value());

	const std::array layers{ Whole(*first) };
	GYRO_REQUIRE(output.Present(layers).has_value());
	GYRO_CHECK(output.IsFramePending());

	// Nothing has landed yet, so nothing is held and nothing is free but the other image.
	GYRO_CHECK(!output.PresentedFrame().has_value());
	GYRO_CHECK_EQ(output.FreeTargets(), 1U);

	clock.Advance(Period * 2);
	output.Advance(clock.Now());

	GYRO_CHECK(!output.IsFramePending());
	GYRO_REQUIRE(output.PresentedFrame().has_value());
	GYRO_CHECK_EQ(output.PresentedFrame()->Target, *first);

	// **This is the difference from a panel.** A headless target would be free the moment the next
	// flip retired it; here the consumer is still reading, so the image stays out of the ring until
	// it says otherwise — which is the backpressure `AcquireTarget` answering nothing describes.
	GYRO_CHECK_EQ(output.FreeTargets(), 1U);

	output.Release(*first);
	GYRO_CHECK_EQ(output.FreeTargets(), 2U);
	GYRO_CHECK(!output.PresentedFrame().has_value());
}

GYRO_TEST(VirtualOutput, AStalledConsumerStarvesTheRingRatherThanLosingAFrame)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured(), { .Targets = 2 } };

	// Present both images and release neither, which is a writer two frames behind.
	for (int frame = 0; frame < 2; ++frame)
	{
		const std::optional<std::uint32_t> target = output.AcquireTarget();
		GYRO_REQUIRE(target.has_value());

		const std::array layers{ Whole(*target) };
		GYRO_REQUIRE(output.Present(layers).has_value());

		clock.Advance(Period);
		output.Advance(clock.Now());
	}

	GYRO_CHECK_EQ(output.FreeTargets(), 0U);

	// The frame loop's response to this is to skip the output, which is only available to it because
	// the answer is an empty optional rather than a wait or an error.
	GYRO_CHECK(!output.AcquireTarget().has_value());
}

GYRO_TEST(VirtualOutput, OneLayerAndOnlyOne)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };

	const std::optional<std::uint32_t> first = output.AcquireTarget();
	const std::optional<std::uint32_t> second = output.AcquireTarget();
	GYRO_REQUIRE(first.has_value() && second.has_value());

	// An empty set is as much a bug in the assigner as a doubled one; a virtual output has one
	// surface and always owes exactly one image.
	GYRO_CHECK_EQ(output.Present({}).error().Code(), EINVAL);

	const std::array pair{ Whole(*first), Whole(*second) };
	GYRO_CHECK_EQ(output.Present(pair).error().Code(), EINVAL);

	const std::array one{ Whole(*first) };
	GYRO_CHECK(output.Present(one).has_value());
}

GYRO_TEST(VirtualOutput, PresentRefusesATargetNobodyAcquired)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };

	const std::array unacquired{ Whole(0) };
	GYRO_CHECK_EQ(output.Present(unacquired).error().Code(), EINVAL);

	const std::array past{ Whole(MaxTargets + 1) };
	GYRO_CHECK_EQ(output.Present(past).error().Code(), EINVAL);
}

GYRO_TEST(VirtualOutput, ASecondCommitBeforeTheFirstLandsIsEbusy)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };

	const std::optional<std::uint32_t> first = output.AcquireTarget();
	const std::optional<std::uint32_t> second = output.AcquireTarget();
	GYRO_REQUIRE(first.has_value() && second.has_value());

	const std::array one{ Whole(*first) };
	GYRO_REQUIRE(output.Present(one).has_value());

	// Transient rather than inexpressible: the assigner did nothing wrong, the output is simply not
	// ready. Seam/Presenter.h separates the two and the frame loop branches on the difference.
	const std::array two{ Whole(*second) };
	GYRO_CHECK_EQ(output.Present(two).error().Code(), EBUSY);
}

GYRO_TEST(VirtualOutput, PresentedReportsAnImpreciseClock)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };

	Observer observer{ output };

	const std::optional<std::uint32_t> target = output.AcquireTarget();
	GYRO_REQUIRE(target.has_value());

	const std::array layers{ Whole(*target) };
	GYRO_REQUIRE(output.Present(layers).has_value());

	clock.Advance(Period);
	output.Advance(clock.Now());

	GYRO_REQUIRE_EQ(observer.Presents, 1);
	GYRO_REQUIRE(observer.LastPresented.has_value());

	// A virtual output's clock is flow control, and Seam/PresentationInfo.h asks for that to be said
	// per output rather than per machine: a `FrameClock` built on this must not report a precise
	// prediction beside a panel that has one.
	GYRO_CHECK(!observer.LastPresented->HardwareClock);
	GYRO_CHECK(!observer.LastPresented->ZeroCopy);
	GYRO_CHECK_EQ(observer.LastPresented->Period, Period);
}

GYRO_TEST(VirtualOutput, ReconfigurationInvalidatesBeforeItAdopts)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };

	Observer observer{ output };

	OutputConfiguration wanted = Configured();
	wanted.Resolution = { 128, 64 };
	output.Reconfigure(wanted);

	// Initiated, not performed: nothing has changed until the loop drains.
	GYRO_CHECK_EQ(output.Configuration().Resolution, (PixelSize<DeviceSpace>{ 64, 32 }));
	GYRO_CHECK_EQ(output.Present({}).error().Code(), EBUSY);

	output.Advance(clock.Now());

	// Both in one drain, because there is no hardware to wait for — see `Advance`. What matters is
	// the *order*, not the gap: the images go before the new set exists, which is what
	// `IRenderer::ReleaseTargets` is written for, since the descriptors in the old set are the
	// presenter's and are about to stop being valid.
	GYRO_REQUIRE_EQ(observer.Order.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(observer.Order[0], "invalidated");
	GYRO_CHECK_EQ(observer.Order[1], "reconfigured");
	GYRO_CHECK_EQ(output.Configuration().Resolution, (PixelSize<DeviceSpace>{ 128, 64 }));
	GYRO_CHECK_EQ(output.Targets().size(), std::size_t{ DefaultVirtualTargets });
	GYRO_CHECK_EQ(output.Targets()[0].Size, (PixelSize<DeviceSpace>{ 128, 64 }));
}

GYRO_TEST(VirtualOutput, AnUnpoweredOutputHasNoImagesAndNoError)
{
	ManualClock clock;
	HeapAllocator allocator;
	OutputConfiguration off = Configured();
	off.Powered = false;

	const VirtualOutput output{ clock, allocator, off };

	GYRO_CHECK(output.Targets().empty());
	// Not a failure. Reporting one here would make a deliberate power-down indistinguishable from a
	// broken allocator in the one field a reader would check.
	GYRO_CHECK(output.Status().has_value());
	GYRO_CHECK_EQ(allocator.Allocations, std::uint64_t{ 0 });
}

GYRO_TEST(VirtualOutput, ReleasingSomethingNotHeldChangesNothing)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };

	const std::optional<std::uint32_t> target = output.AcquireTarget();
	GYRO_REQUIRE(target.has_value());
	GYRO_REQUIRE_EQ(output.FreeTargets(), DefaultVirtualTargets - 1);

	// An acquired image is being drawn into. Releasing it here would put it back in the ring while
	// the renderer still holds it, which is the next frame composited over the one being read.
	output.Release(*target);
	GYRO_CHECK_EQ(output.FreeTargets(), DefaultVirtualTargets - 1);

	// Past the set, and a double release, are both no-ops rather than corruption.
	output.Release(MaxTargets + 1);
	GYRO_CHECK_EQ(output.FreeTargets(), DefaultVirtualTargets - 1);
}

GYRO_TEST(VirtualOutput, NextEventIsTheFrameBoundaryOrNothing)
{
	ManualClock clock;
	HeapAllocator allocator;
	VirtualOutput output{ clock, allocator, Configured() };

	// Quiet, which is the identity of a fold rather than a case a caller tests for.
	GYRO_CHECK_EQ(output.NextEvent(), Instant{ Duration::max() });

	const std::optional<std::uint32_t> target = output.AcquireTarget();
	GYRO_REQUIRE(target.has_value());

	const std::array layers{ Whole(*target) };
	GYRO_REQUIRE(output.Present(layers).has_value());

	GYRO_CHECK(output.NextEvent() > clock.Now());
	GYRO_CHECK(output.NextEvent() <= clock.Now() + Period);
}
