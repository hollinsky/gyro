#pragma once

#include <cstdint>
#include <format>
#include <type_traits>

#include "Core/Fd.h"

// A point on a synchronization timeline: the thing Present() waits for before the pixels are read.
//
// Docs/Architecture.md makes this uniform on purpose — **DRM syncobj timelines throughout, exported
// from Vulkan timeline semaphores.** On KMS a point becomes `IN_FENCE_FD`, nested it becomes
// `wp_linux_drm_syncobj_v1`, and headless waits on it directly. One representation, three consumers,
// no conversion at the seam.
//
// **A timeline point rather than a binary fence, because a point can be named before it exists.** The
// frame loop records and submits, then presents, and the two are separate submissions; with a fence
// it would have to wait for the submission to produce one before it could hand it over, which is a
// round trip inside the frame section. A timeline value is known in advance — it is the renderer's
// own counter — so the present is issued against a point the GPU has not reached yet, which is the
// whole reason explicit sync is worth having.
//
// **The descriptor is borrowed, and that is forced rather than chosen.** Core/Fd.h's rule is that an
// owner is move-only; a SyncPoint sits by value in a layer list that is copied and passed per frame,
// so an owning descriptor here would mean either a `dup` per frame — a syscall on the frame path — or
// a move-only aggregate that cannot be an element of a span. The timeline outlives every point on it:
// it belongs to the renderer's device and is destroyed with it, which is a lifetime the composition
// root already sequences for decision 41's migration.
//
// **What the null point means, and the one hazard in it.** A default-constructed SyncPoint names no
// timeline and says *nothing to wait for*. Two producers reach it legitimately: content that was
// finished on the CPU before the call — the console's blit, a static logo — and a buffer under
// implicit synchronization, where the kernel does the waiting inside the commit. The hazard is that
// those two are indistinguishable here and must not be: for the first, presenting immediately is
// correct, and for the second it is correct only because the *buffer* carries an implicit fence the
// backend must honour. So implicit sync is a property of the buffer and is answered where the buffer
// is described, never by reading a null SyncPoint as permission.
//
// **There is no release point, and its absence is a design claim.** KMS can hand back an
// `OUT_FENCE_PTR` saying when the previous scanout buffer is free, and that is real information — but
// it is the presenter's own, because the presenter is what owns the targets and what
// `AcquireTarget()` already asks. Exposing it would give the frame loop a second way to answer a
// question it already has one answer for, and two answers that can disagree is how a ring ends up
// with a buffer nobody believes is free.

struct SyncPoint
{
	// An exported DRM syncobj, owned by whoever created the timeline. Invalid means the point is
	// immediate.
	RawFd Timeline;

	// The value the timeline reaches when the work is done. Meaningless when Timeline is invalid;
	// zero, so that a default-constructed point is entirely default.
	std::uint64_t Value = 0;

	// Named, so that a call site passing "no wait" says so rather than passing braces.
	[[nodiscard]] static constexpr SyncPoint Immediate() noexcept { return {}; }

	[[nodiscard]] constexpr bool IsImmediate() const noexcept { return !Timeline.IsValid(); }

	// Compares both halves, including the descriptor number. Two points on two timelines are not the
	// same point even at the same value, and the only callers are a test and a backend deduplicating
	// waits within one commit.
	friend constexpr bool operator==(SyncPoint, SyncPoint) noexcept = default;
};

// Prints as sync fd 7 @ 42, or sync immediate.
//
// The context is a template parameter for the reason recorded at length in Core/Time.h: naming
// std::format_context leaves std::format working and std::formattable false, so the value goes
// missing from a test failure at exactly the moment it was wanted.
template<>
struct std::formatter<SyncPoint>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(SyncPoint point, Context& context) const
	{
		if (point.IsImmediate())
		{
			return std::format_to(context.out(), "sync immediate");
		}

		return std::format_to(context.out(), "sync {} @ {}", point.Timeline, point.Value);
	}
};

// The contract everything downstream assumes.
static_assert(
	std::is_trivially_copyable_v<SyncPoint> && std::is_standard_layout_v<SyncPoint>,
	"A sync point is copied into a layer list per frame, never owned behind one"
);
static_assert(std::is_aggregate_v<SyncPoint>, "Built at the call site by naming its parts");
static_assert(std::formattable<SyncPoint, char>, "A report prints the point rather than <unprintable>");

static_assert(SyncPoint{}.IsImmediate() && SyncPoint::Immediate().IsImmediate());
static_assert(!SyncPoint{ RawFd{ 7 }, 42 }.IsImmediate());
static_assert(SyncPoint{ RawFd{ 7 }, 42 } == SyncPoint{ RawFd{ 7 }, 42 });
static_assert(SyncPoint{ RawFd{ 7 }, 42 } != SyncPoint{ RawFd{ 8 }, 42 }, "Two timelines are not one timeline");
static_assert(SyncPoint{ RawFd{ 7 }, 42 } != SyncPoint{ RawFd{ 7 }, 43 });
