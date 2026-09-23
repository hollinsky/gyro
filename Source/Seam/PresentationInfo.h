#pragma once

#include <cstdint>
#include <format>
#include <type_traits>

#include "Core/Time.h"

// What one frame reaching the glass says about itself.
//
// This is the sole input to the prediction every deadline in the system is derived from:
// `FrameClock::Observe` takes one of these and everything else — `NextDeadline`, `NextWakeup`,
// admission control's slack, the VRR servo's convergence — is computed from the sequence of them. A
// field that is wrong here is not wrong locally; it is a mis-scheduled output.
//
// **Every instant is already in the one timebase.** Decision 57 converts at ingest and nowhere else,
// so a backend that receives microseconds from `wp_presentation_feedback` or a `drm_event` timestamp
// converts there and hands over an `Instant`. Nothing downstream of this type ever sees a raw number
// again, which is what makes input-to-photon a subtraction rather than an estimate.
//
// **`Period` is what the panel did, never what gyro asked for.** The VRR servo commands a period on
// the clock and learns from the next one of these what actually happened; the two disagree for the
// length of every servo ramp and permanently on a panel that will not comply. A backend that echoed
// the commanded value here would close the servo's loop on its own output, which is a servo that
// reports convergence it never achieved. See Docs/Architecture.md#vrr-as-a-scheduling-degree-of-
// freedom.
//
// **The flags are the honesty half, and `HardwareClock` is the one that matters.** A host compositor
// may not implement presentation feedback and a driver may not report monotonic timestamps; what the
// clock does about that is downgrade — skip the latency assertions, lower the log severity, stop
// pretending — and it can only do that if it is told. **A backend that does not know must say so
// rather than fill the field in**, because a fabricated timestamp is indistinguishable from a real
// one and produces a schedule with no error bar.
//
// **What is deliberately absent.** No output identity: the presenter is the output's, so a receiver
// that connected to this signal already knows which one it is hearing from, and carrying the id would
// invite a second answer to a question that has one. No target index: which images are free is the
// presenter's own bookkeeping and `AcquireTarget()` is where it is asked, so a second answer here
// could disagree with it. And no discard case — a frame that never reached glass produces no
// PresentationInfo at all, which is exactly the state `FrameClock::Invalidate` already describes.

struct PresentationInfo
{
	// When the frame reached the glass. For KMS this is the page-flip event timestamp with
	// `DRM_CAP_TIMESTAMP_MONOTONIC`; nested it is `wp_presentation_feedback.presented`.
	Instant PresentedAt{};

	// The interval the output actually ran at, as observed. Zero where the backend genuinely does not
	// know, which is a state the clock can represent and a lie is not.
	Duration Period{};

	// The output's own frame counter — vblank sequence on KMS, the feedback's sequence nested. It is
	// what `PresentationAt(sequence)` is asked about, so it must be the same counter that speculative
	// early rendering names a future frame by.
	std::uint64_t Sequence = 0;

	// The flip was synchronized to the display's refresh rather than applied immediately.
	bool Vsync = false;

	// The timestamp came from the display hardware rather than from software noticing afterwards. This
	// is what `FrameClock::IsPrecise()` reads, and it is per output rather than per machine — a
	// virtual output's clock is flow control and is never precise while the panel beside it is.
	bool HardwareClock = false;

	// The display itself signalled that it began scanning this frame out, rather than software deciding
	// when it probably had — a vblank interrupt on KMS, the host's `hw_completion` nested. **Separate
	// from `HardwareClock` because the two fail independently**: every KMS driver reports monotonic
	// timestamps, and a driver with no interrupt behind its vblank reports them from a timer. This is
	// the flag a client's `wp_presentation_feedback` is told as `hw_completion`, and a backend that
	// cannot tell the two cases apart leaves it false.
	bool HardwareCompletion = false;

	// The buffer was scanned out directly rather than composited by whatever is downstream. Nested it
	// is the host telling gyro its dmabuf reached a plane; on KMS it is true by construction. It is
	// information about the *host's* behaviour and never about gyro's own plane assignment.
	bool ZeroCopy = false;

	friend constexpr bool operator==(PresentationInfo, PresentationInfo) noexcept = default;
};

// Prints as presented 12500000ns seq 4210 period 8333333ns vsync hw-clock hw-completion. Durations print in the
// timebase's own units, which is what Core/Time.h's formatter does and what keeps two of them
// comparable by eye.
template<>
struct std::formatter<PresentationInfo>
{
	static constexpr auto parse(std::format_parse_context& context) { return context.begin(); }

	template<typename Context>
	auto format(const PresentationInfo& info, Context& context) const
	{
		auto out = std::format_to(
			context.out(), "presented {} seq {} period {}", info.PresentedAt, info.Sequence, info.Period
		);

		if (info.Vsync)
		{
			out = std::format_to(out, " vsync");
		}

		if (info.HardwareClock)
		{
			out = std::format_to(out, " hw-clock");
		}

		if (info.HardwareCompletion)
		{
			out = std::format_to(out, " hw-completion");
		}

		if (info.ZeroCopy)
		{
			out = std::format_to(out, " zero-copy");
		}

		return out;
	}
};

// The contract everything downstream assumes.
static_assert(std::is_trivially_copyable_v<PresentationInfo> && std::is_standard_layout_v<PresentationInfo>);
static_assert(std::is_aggregate_v<PresentationInfo>);
static_assert(std::formattable<PresentationInfo, char>, "A report prints the frame rather than <unprintable>");

// A default one is the *absence* of an observation rather than an observation of zero, and the clock
// distinguishes them by never being handed this. Asserted so the meaning of the default is written
// down where a backend author reading the type will find it.
static_assert(
	PresentationInfo{}.Period == Duration::zero() && !PresentationInfo{}.HardwareClock &&
	!PresentationInfo{}.HardwareCompletion
);
