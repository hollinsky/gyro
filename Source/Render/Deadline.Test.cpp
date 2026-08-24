#define _POSIX_C_SOURCE 200809L

#include "Render/Deadline.h"

#include <drm/drm.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <optional>
#include <print>
#include <string_view>

#include "Core/Fd.h"
#include "Core/Time.h"
#include "Testing/Test.h"

// Decision 142's deadline, against the kernel that takes it.
//
// **Two halves, and only one of them needs a machine.** The path and the instant conversion are
// arithmetic and are checked everywhere. The ioctl needs a DRM node, which is Nested/Sync.Test.cpp's
// situation exactly — `/dev/dri/renderD128` is world-accessible on an ordinary workstation and absent
// in a container built without one — so a test that cannot run says so by name rather than passing
// quietly.
//
// **What the kernel half can and cannot check.** That the ioctl is *accepted* is the whole of it, and
// it is worth a test because it is the part that fails silently: a struct laid out wrong or a flag a
// kernel older than 6.5 does not know comes back `EINVAL`, and gyro would have gone on stating
// deadlines nothing read. What no test here can check is whether the number moved the clock — that is
// a driver fact, it is false on every part gyro runs on today, and it is what the startup probe beside
// this exists to find out on the machine gyro is actually on.

namespace
{
constexpr const char* NodePath = "/dev/dri/renderD128";

// A syncobj minted by hand and exported, standing where the renderer's Vulkan timeline stands. The
// descriptor is what `FenceDeadline` imports, so this is the same shape the real caller hands it.
struct MintedTimeline
{
	Fd Node;
	Fd Descriptor;
	std::uint32_t Handle = 0;
};

[[nodiscard]] std::optional<MintedTimeline> Mint(std::string_view test)
{
	MintedTimeline minted;
	minted.Node = Fd{ ::open(NodePath, O_RDWR | O_CLOEXEC) };

	if (!minted.Node.IsValid())
	{
		std::println("  skipped Deadline.{}: {} ({})", test, NodePath, std::strerror(errno));

		return std::nullopt;
	}

	drm_syncobj_create create = {};

	if (::ioctl(minted.Node.Get(), DRM_IOCTL_SYNCOBJ_CREATE, &create) != 0)
	{
		std::println("  skipped Deadline.{}: SYNCOBJ_CREATE ({})", test, std::strerror(errno));

		return std::nullopt;
	}

	minted.Handle = create.handle;

	drm_syncobj_handle exported = {};
	exported.handle = create.handle;

	if (::ioctl(minted.Node.Get(), DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &exported) != 0)
	{
		std::println("  skipped Deadline.{}: SYNCOBJ_HANDLE_TO_FD ({})", test, std::strerror(errno));

		return std::nullopt;
	}

	minted.Descriptor = Fd{ exported.fd };

	return std::optional<MintedTimeline>{ std::move(minted) };
}
} // namespace

GYRO_TEST(Deadline, TheNodeIsARenderNodeNamedByItsMinor)
{
	// Render rather than primary, and it is the point of the whole path: gyro holds DRM master on the
	// primary node by first-open, and every syncobj ioctl is `DRM_RENDER_ALLOW`.
	GYRO_CHECK_EQ(FenceDeadline::NodePath(128), std::string{ "/dev/dri/renderD128" });
	GYRO_CHECK_EQ(FenceDeadline::NodePath(129), std::string{ "/dev/dri/renderD129" });
}

GYRO_TEST(Deadline, AnInstantIsTheKernelsOwnNanosecondsAndTheEpochIsZero)
{
	// Decision 36's one clock is what makes this a cast: `deadline_nsec` is absolute
	// `CLOCK_MONOTONIC`, which is the timeline Core/Time.h already stamps everything in.
	GYRO_CHECK_EQ(FenceDeadline::Nanoseconds(Monotonic::FromNanoseconds(1'009'600'000)), 1'009'600'000U);
	GYRO_CHECK_EQ(FenceDeadline::Nanoseconds(Instant{}), 0U);

	// Saturated rather than wrapped. The field is unsigned, so a negative instant reaching it as a
	// two's-complement pattern would state a deadline nine billion seconds out — a hint that reads as
	// "never urgent" and would be indistinguishable from the driver ignoring gyro entirely.
	GYRO_CHECK_EQ(FenceDeadline::Nanoseconds(Monotonic::FromNanoseconds(-5)), 0U);
}

GYRO_TEST(Deadline, ADeviceWithNoTimelineStatesNothingAndDoesNotOpenTheNode)
{
	// Decision 108's device: lavapipe exports no timeline, so there is no point to name a deadline on
	// and nothing here should touch `/dev/dri` looking for one.
	FenceDeadline deadline = FenceDeadline::OpenNode(NodePath, RawFd{});

	GYRO_CHECK(!deadline.IsValid());

	// The no-op has to be safe, because the renderer calls it unconditionally after every submit
	// rather than testing validity per frame.
	deadline.State(1, Monotonic::FromNanoseconds(1'000'000'000));
}

GYRO_TEST(Deadline, AMissingNodeComesUpInvalidRatherThanFailing)
{
	FenceDeadline deadline = FenceDeadline::OpenNode("/dev/dri/renderD999", RawFd{ 0 });

	GYRO_CHECK(!deadline.IsValid());
}

GYRO_TEST(Deadline, ATimelineIsImportedAndAPointCarriesADeadlineTheKernelAccepts)
{
	const std::optional<MintedTimeline> minted = Mint("ATimelineIsImportedAndAPointCarriesADeadlineTheKernelAccepts");

	if (!minted)
	{
		return;
	}

	FenceDeadline deadline = FenceDeadline::OpenNode(NodePath, minted->Descriptor.Borrow());
	GYRO_REQUIRE(deadline.IsValid());

	// A point with a fence behind it, because a deadline is a statement about a fence and the kernel
	// has nothing to hang one on otherwise. Signalling is how a test gets one without a GPU.
	std::array<std::uint32_t, 1> handles{ minted->Handle };
	std::array<std::uint64_t, 1> points{ 1 };

	drm_syncobj_timeline_array signal = {};
	signal.handles = reinterpret_cast<std::uintptr_t>(handles.data());
	signal.points = reinterpret_cast<std::uintptr_t>(points.data());
	signal.count_handles = 1;

	GYRO_REQUIRE(::ioctl(minted->Node.Get(), DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &signal) == 0);

	// What is being checked is that the ioctl the deadline rides on is one this kernel takes — a flag
	// it does not know, or a struct a header disagreed about, comes back `EINVAL` and gyro would never
	// have found out.
	drm_syncobj_timeline_wait wait = {};
	wait.handles = reinterpret_cast<std::uintptr_t>(handles.data());
	wait.points = reinterpret_cast<std::uintptr_t>(points.data());
	wait.timeout_nsec = 0;
	wait.count_handles = 1;
	wait.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE;
	wait.deadline_nsec = 1'000'000'000;

	errno = 0;
	const int accepted = ::ioctl(minted->Node.Get(), DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait);

	GYRO_CHECK(accepted == 0 || errno != EINVAL);

	// And the object's own path, which cannot report anything: the hint is advisory in the kernel by
	// construction, so this checks that stating it on a live import runs and returns.
	deadline.State(1, Monotonic::FromNanoseconds(1'000'000'000));
	deadline.State(2, Instant{});
}
