#include "Virtual/Imported.h"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "Core/Clock.h"
#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Geometry/Space.h"
#include "Seam/Buffer.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Seam/SyncPoint.h"
#include "Testing/Test.h"
#include "Virtual/Heap.h"
#include "Virtual/Output.h"

// The provider that hands over a client's ring, and the presenter driven off one.
//
// **The client is played by `HeapAllocator` and its descriptors are duplicated before they arrive**,
// which is not ceremony: a registration carries descriptors over `SCM_RIGHTS`, so what gyro holds is
// its own copy of the client's file and closing it is right. Duplicating here means the test really
// does hand ownership across, and a buffer this file drops does not take the stand-in client's
// allocation with it.

namespace
{
constexpr Duration Period = std::chrono::nanoseconds{ 16'666'666 };
constexpr PixelFormat Linear{ FormatXrgb8888, 0, ModifierLinear };

[[nodiscard]] OutputConfiguration Configured(PixelSize<DeviceSpace> size = { 64, 32 })
{
	return OutputConfiguration{ .Resolution = size, .Period = Period, .Format = Linear };
}

// One image as a client would have sent it: an allocation of its own, described, with a descriptor
// gyro now owns.
[[nodiscard]] Result<DmabufBuffer>
Registered(HeapAllocator& client, PixelSize<DeviceSpace> size = { 64, 32 }, PixelFormat format = Linear)
{
	const std::uint64_t modifier = format.Modifier;
	Result<DmabufBuffer> allocated = client.Allocate(size, format.Code, { &modifier, 1 });

	if (!allocated)
	{
		return std::unexpected{ allocated.error() };
	}

	Fd handed{ ::dup(allocated->Descriptor().Value) };

	std::array planes{ ImportedPlane{ .Descriptor = std::move(handed), .Offset = 0, .Stride = allocated->Stride() } };

	return ImportBuffer(size, allocated->Format(), planes);
}

[[nodiscard]] std::vector<DmabufBuffer>
Ring(HeapAllocator& client, std::size_t count, PixelSize<DeviceSpace> size = { 64, 32 })
{
	std::vector<DmabufBuffer> ring;

	for (std::size_t index = 0; index < count; ++index)
	{
		Result<DmabufBuffer> buffer = Registered(client, size);

		if (buffer)
		{
			ring.emplace_back(std::move(*buffer));
		}
	}

	return ring;
}

[[nodiscard]] PresentLayer Whole(std::uint32_t target)
{
	return PresentLayer{ .Target = target,
		                 .Blend = BlendMode::Opaque,
		                 .Acquire = SyncPoint::Immediate(),
		                 .Source = { {}, { 64.0F, 32.0F } },
		                 .Destination = { {}, { 64, 32 } },
		                 .Damage = {} };
}
} // namespace

GYRO_TEST(ImportBuffer, DescribesWhatTheClientSent)
{
	HeapAllocator client;
	Result<DmabufBuffer> imported = Registered(client);

	GYRO_REQUIRE(imported.has_value());
	GYRO_CHECK(imported->IsValid());
	GYRO_CHECK_EQ(imported->Size(), PixelSize<DeviceSpace>{ 64, 32 });
	GYRO_CHECK_EQ(imported->Format(), Linear);

	const RenderTarget described = imported->Describe();
	GYRO_REQUIRE(described.AsDmabuf() != nullptr);
	GYRO_CHECK_EQ(described.AsDmabuf()->PlaneCount, std::uint32_t{ 1 });
	GYRO_CHECK(described.AsDmabuf()->Planes[0].Descriptor.IsValid());

	// No mapping, and the target face that wants one fails rather than writing through nothing: these
	// pages belong to another process's allocator and nothing here may assume they are mappable.
	GYRO_CHECK(!imported->IsMapped());
	GYRO_CHECK(!imported->DescribeMapped().IsValid());
}

GYRO_TEST(ImportBuffer, RefusesWhatIsNotAnImage)
{
	std::array<ImportedPlane, 0> none{};
	GYRO_CHECK(!ImportBuffer({ 64, 32 }, Linear, none).has_value());

	std::array noDescriptor{ ImportedPlane{ .Descriptor = {}, .Offset = 0, .Stride = 256 } };
	GYRO_CHECK(!ImportBuffer({ 64, 32 }, Linear, noDescriptor).has_value());

	HeapAllocator client;
	Result<DmabufBuffer> allocated = client.Allocate({ 64, 32 }, FormatXrgb8888, std::array{ ModifierLinear });
	GYRO_REQUIRE(allocated.has_value());

	std::array noStride{ ImportedPlane{
		.Descriptor = Fd{ ::dup(allocated->Descriptor().Value) }, .Offset = 0, .Stride = 0 } };
	GYRO_CHECK(!ImportBuffer({ 64, 32 }, Linear, noStride).has_value());

	std::array noSize{ ImportedPlane{
		.Descriptor = Fd{ ::dup(allocated->Descriptor().Value) }, .Offset = 0, .Stride = 256 } };
	GYRO_CHECK(!ImportBuffer({}, Linear, noSize).has_value());
}

GYRO_TEST(ImportedTargets, HandsOverTheRingInOrderAndThenSaysThereIsNoMore)
{
	HeapAllocator client;
	ImportedTargets targets{ Ring(client, 2) };

	GYRO_CHECK_EQ(targets.Remaining(), std::size_t{ 2 });
	GYRO_CHECK(targets.Supports(Linear));

	const std::array candidates{ ModifierLinear };

	GYRO_CHECK(targets.Allocate({ 64, 32 }, FormatXrgb8888, candidates).has_value());
	GYRO_CHECK(targets.Allocate({ 64, 32 }, FormatXrgb8888, candidates).has_value());
	GYRO_CHECK_EQ(targets.Remaining(), std::size_t{ 0 });

	const Result<DmabufBuffer> exhausted = targets.Allocate({ 64, 32 }, FormatXrgb8888, candidates);
	GYRO_REQUIRE(!exhausted.has_value());
	GYRO_CHECK_EQ(exhausted.error().Code(), ENOENT);
}

GYRO_TEST(ImportedTargets, RefusesAnImageTheRendererDidNotAskFor)
{
	HeapAllocator client;
	ImportedTargets targets{ Ring(client, 1) };

	const std::array candidates{ ModifierLinear };

	// A size the output does not render, and the ring is not consumed by the refusal — the disagreement
	// is at registration and the image is still the client's.
	const Result<DmabufBuffer> wrongSize = targets.Allocate({ 128, 32 }, FormatXrgb8888, candidates);
	GYRO_REQUIRE(!wrongSize.has_value());
	GYRO_CHECK_EQ(wrongSize.error().Code(), EINVAL);
	GYRO_CHECK_EQ(targets.Remaining(), std::size_t{ 1 });

	const Result<DmabufBuffer> wrongFormat = targets.Allocate({ 64, 32 }, FormatArgb8888, candidates);
	GYRO_REQUIRE(!wrongFormat.has_value());
	GYRO_CHECK_EQ(wrongFormat.error().Code(), EINVAL);

	const std::array unacceptable{ std::uint64_t{ 0x0100'0000'0000'0001 } };
	const Result<DmabufBuffer> wrongModifier = targets.Allocate({ 64, 32 }, FormatXrgb8888, unacceptable);
	GYRO_REQUIRE(!wrongModifier.has_value());
	GYRO_CHECK_EQ(wrongModifier.error().Code(), EINVAL);
}

GYRO_TEST(ImportedTargets, ThePresenterRingsAClientsImagesLikeAnyOther)
{
	ManualClock clock;
	HeapAllocator client;
	ImportedTargets targets{ Ring(client, 2) };

	VirtualOutput output{ clock, targets, Configured(), { .Targets = 2 } };

	GYRO_REQUIRE(output.Status().has_value());
	GYRO_REQUIRE_EQ(output.Targets().size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(output.FreeTargets(), 2U);

	const std::optional<std::uint32_t> first = output.AcquireTarget();
	GYRO_REQUIRE(first.has_value());

	const std::array layers{ Whole(*first) };
	GYRO_REQUIRE(output.Present(layers).has_value());

	clock.Advance(Period);
	output.Advance(clock.Now());

	// Held until the consumer lets go, which is the property that makes an encoder's backpressure the
	// output's cadence rather than a queue nobody bounds.
	GYRO_REQUIRE(output.PresentedFrame().has_value());
	GYRO_CHECK_EQ(output.FreeTargets(), 1U);

	output.Release(output.PresentedFrame()->Target);
	GYRO_CHECK_EQ(output.FreeTargets(), 2U);
}

GYRO_TEST(ImportedTargets, AShortRingLeavesNoTargetsAndSaysWhy)
{
	ManualClock clock;
	HeapAllocator client;
	ImportedTargets targets{ Ring(client, 1) };

	// Two asked for, one supplied. Partial success is not expressible for the reason a refused
	// allocation is not: a set is what `AcquireTarget` indexes into.
	const VirtualOutput output{ clock, targets, Configured(), { .Targets = 2 } };

	GYRO_CHECK(output.Targets().empty());
	GYRO_REQUIRE(!output.Status().has_value());
	GYRO_CHECK_EQ(output.Status().error().Code(), ENOENT);
}

GYRO_TEST(ImportedTargets, AModeSetIsARenegotiationAndRestockIsHowItEnds)
{
	ManualClock clock;
	HeapAllocator client;
	ImportedTargets targets{ Ring(client, 2) };

	VirtualOutput output{ clock, targets, Configured(), { .Targets = 2 } };
	GYRO_REQUIRE_EQ(output.Targets().size(), std::size_t{ 2 });

	// gyro cannot resize a ring it did not allocate, so the output comes up empty and says so rather
	// than rendering the new mode into the old images.
	output.Reconfigure(Configured({ 128, 64 }));
	output.Advance(clock.Now());
	output.Advance(clock.Now());

	GYRO_CHECK(output.Targets().empty());
	GYRO_REQUIRE(!output.Status().has_value());
	GYRO_CHECK_EQ(output.Status().error().Code(), ENOENT);

	targets.Restock(Ring(client, 2, { 128, 64 }));

	output.Reconfigure(Configured({ 128, 64 }));
	output.Advance(clock.Now());
	output.Advance(clock.Now());

	GYRO_CHECK(output.Status().has_value());
	GYRO_CHECK_EQ(output.Targets().size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(output.Targets().front().Size, PixelSize<DeviceSpace>{ 128, 64 });
}
