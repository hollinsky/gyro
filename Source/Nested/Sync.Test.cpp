#include "Nested/Sync.h"

#include <cerrno>
#include <cstring>
#include <optional>
#include <print>
#include <string_view>
#include <utility>

#include "Core/Result.h"
#include "Testing/Test.h"

// The release timelines a nested surface names, against the kernel that mints them.
//
// **These need a DRM node and will not always have it**, which is Virtual/Udmabuf.Test.cpp's
// situation with a different node: `/dev/dri/renderD128` is world-accessible on an ordinary
// workstation and absent entirely in a container built without it. A test that cannot run says so by
// name rather than passing quietly, for that file's reason — a green run that skipped everything is
// exactly what decision 36's build-time checks exist to prevent.
//
// What is *not* gated is the absence itself: `DrmSyncobjDevice::Open` failing is a supported state
// that puts the whole backend on the held-commit path, and that path is exercised in Output.Test.cpp
// without any node at all.

namespace
{
[[nodiscard]] std::optional<Nested::DrmSyncobjDevice> Available(std::string_view test)
{
	// Zero, which is the host having named no device — so this asks the same question the fallback in
	// `Open` does rather than a narrower one.
	Result<Nested::DrmSyncobjDevice> device = Nested::DrmSyncobjDevice::Open(0);

	if (!device)
	{
		std::println(
			"  skipped Sync.{}: {} ({})", test, device.error().Context(), std::strerror(device.error().Code())
		);

		return std::nullopt;
	}

	return std::optional<Nested::DrmSyncobjDevice>{ std::move(*device) };
}
} // namespace

GYRO_TEST(Sync, ATimelineIsCreatedAtZeroAndExportsADescriptor)
{
	const std::optional<Nested::DrmSyncobjDevice> device = Available("ATimelineIsCreatedAtZeroAndExportsADescriptor");

	if (!device)
	{
		return;
	}

	GYRO_CHECK(!device->Path().empty());

	Result<Nested::DrmTimeline> timeline = device->CreateTimeline();

	GYRO_REQUIRE(timeline.has_value());
	GYRO_REQUIRE(timeline->IsValid());

	// The descriptor is what `wp_linux_drm_syncobj_v1.import_timeline` takes, and it is borrowed —
	// the caller that hands it over duplicates, because the request takes ownership.
	GYRO_CHECK(timeline->Descriptor().IsValid());

	// Nothing has signalled it, which is the whole of what `AcquireTarget` asks: a release point at
	// zero is a buffer the host has not given back.
	GYRO_CHECK_EQ(timeline->Signalled(), std::uint64_t{ 0 });
}

GYRO_TEST(Sync, ATimelineIsMoveOnlyAndTheHuskReadsAsUnsignalled)
{
	const std::optional<Nested::DrmSyncobjDevice> device = Available("ATimelineIsMoveOnlyAndTheHuskReadsAsUnsignalled");

	if (!device)
	{
		return;
	}

	Result<Nested::DrmTimeline> made = device->CreateTimeline();
	GYRO_REQUIRE(made.has_value());

	const Nested::DrmTimeline moved = std::move(*made);

	GYRO_CHECK(moved.IsValid());

	// **The husk must not answer for the timeline it gave away**, because a target set is built by
	// moving these into it and the source is destroyed on the next line. An object that still held the
	// handle would destroy a syncobj the ring is using, and every release afterwards would read as
	// *not yet* forever.
	GYRO_CHECK(!made->IsValid());
	GYRO_CHECK_EQ(made->Signalled(), std::uint64_t{ 0 });
}

GYRO_TEST(Sync, TwoTimelinesAreTwoTimelines)
{
	// The protocol asks for one timeline per buffer outright: a compositor may signal release points
	// out of order, so a shared timeline would report an image free because a *later* one was. That is
	// a property of the ring rather than of this file, and what this checks is the half this file owes
	// it — that asking twice gives two distinct objects rather than one handle twice.
	const std::optional<Nested::DrmSyncobjDevice> device = Available("TwoTimelinesAreTwoTimelines");

	if (!device)
	{
		return;
	}

	Result<Nested::DrmTimeline> first = device->CreateTimeline();
	Result<Nested::DrmTimeline> second = device->CreateTimeline();

	GYRO_REQUIRE(first.has_value() && second.has_value());
	GYRO_CHECK(first->Descriptor().Value != second->Descriptor().Value);
}

GYRO_TEST(Sync, ADefaultDeviceMintsNothingAndSaysSo)
{
	// The state a container reaches, and the one the whole fallback path rests on: absence answers
	// rather than aborting, so a nested session on a machine with no DRM node comes up on the
	// held-commit path instead of failing to start.
	const Nested::DrmSyncobjDevice none;

	GYRO_CHECK(!none.IsValid());

	const Result<Nested::DrmTimeline> refused = none.CreateTimeline();

	GYRO_REQUIRE(!refused.has_value());
	GYRO_CHECK_EQ(refused.error().Code(), ENODEV);
}
