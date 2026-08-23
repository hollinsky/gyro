#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Geometry/Space.h"
#include "Seam/RenderTarget.h"
#include "Virtual/Buffer.h"
#include "Virtual/Output.h"
#include "Virtual/Pixels.h"

// The consumer end of a virtual output: whoever the pixels are for.
//
// Virtual/Output.h describes a ring that retires *on release*, and until this file there was nothing
// in the tree to do the releasing — every existing test acquires, presents, and releases in the same
// breath, which exercises the mechanism and models no consumer at all. A sink is the party that
// mechanism was built for.
//
// **Releasing is the sink's and not the device's, because holding is a real answer.** Output.h has
// both cases already: a consumer that is done inside the notification releases immediately and the
// ring behaves like any other, and one that holds two images stalls the output — which is the
// correct behaviour rather than a dropped frame, because the frame loop skips an output it cannot
// acquire for. A device that released on the sink's behalf would make the second case inexpressible,
// and backpressure is precisely what a recording path has to get right.
//
// **A sink runs on the frame thread, so it copies and it does not syscall.** Frame/Loop.h drains
// its sources first and deliberately outside the frame section, so decision 36's allocator will not
// catch a sink that allocates — but the thread is still the `SCHED_FIFO` one, and Docs/Open.md's
// *spdlog async sink* entry is the same hazard: work that blocks here surfaces as a dropped frame
// somewhere else. That is why nothing in this file writes a file. Virtual/Pam.h does that, from
// whatever thread the caller is on, over pixels a sink already copied.
//
// **What is deliberately not here: a sink that hands a frame to another thread.** A real recording
// consumer wants the descriptor rather than a copy, decoded on its own thread, and releasing from
// there races the frame thread's `AcquireTarget` on the same ring — a handoff with the shape
// Publication already has, not a member function. Naming the limit is Seam/Renderer.h's treatment
// of texture import one seam over: the case is real, and guessing at it now would produce a channel
// nothing drives.

class IFrameSink
{
public:
	IFrameSink() = default;

	virtual ~IFrameSink() = default;

	// Neither copied nor moved: a device holds a sink by address for as long as the output lives, for
	// `IEventSource`'s reason one file over.
	IFrameSink(const IFrameSink&) = delete;
	IFrameSink& operator=(const IFrameSink&) = delete;
	IFrameSink(IFrameSink&&) = delete;
	IFrameSink& operator=(IFrameSink&&) = delete;

	// A frame has landed and its pixels may be read.
	//
	// **The GPU has already finished, and the caller is what guarantees it.** `frame.Acquire` is the
	// point the composite completes at, and Virtual/Device.h will not deliver a frame whose point its
	// renderer says is outstanding. A sink handed one directly by something other than a device owes
	// itself that check, which is `IRenderer::IsComplete` and nothing else.
	//
	// The output is here so that the sink can release. Everything else it needs — the pixels, the
	// size, the format — comes through `output.Buffer(frame.Target)`.
	virtual void OnFrame(VirtualOutput& output, const VirtualFrame& frame) = 0;
};

// Whether a sink gives the image back when it is done looking, or keeps it.
enum class FrameRetention : std::uint8_t
{
	// The ordinary consumer. The ring behaves like a panel's flip queue.
	Release,

	// The consumer that stalls the output, which a test asks for in order to watch the loop skip.
	Hold,
};

// A consumer that reads every frame and drops it. What an output wants when the thing under test is
// the schedule rather than the picture.
//
// **It is a real consumer and not a null object.** It performs the release, so an output attached to
// one keeps cycling its ring exactly as one attached to a writer would; the difference is only that
// it looks at nothing. Making "nobody is consuming" the absence of a sink instead would have quietly
// disabled the backpressure Output.h's ring is built around.
class DiscardingSink final : public IFrameSink
{
public:
	void OnFrame(VirtualOutput& output, const VirtualFrame& frame) override
	{
		++m_Frames;
		output.Release(frame.Target);
	}

	[[nodiscard]] std::uint64_t Frames() const noexcept { return m_Frames; }

private:
	std::uint64_t m_Frames = 0;
};

// A consumer that keeps the last few frames, so a test can say what was drawn.
//
// **Bounded and preallocated, because it runs on the frame thread.** The storage is sized at
// construction from the output's extent and never grows; a frame that does not fit — a
// reconfiguration to a larger mode, a format the view cannot decode — is counted as skipped rather
// than resized around. Growing here would be a heap allocation between a flip and the next frame's
// deadline, which is the cost decision 36 exists to make impossible to add by accident.
//
// **The copy is tight, so the captured stride is not the source's.** A dmabuf's stride is the
// allocator's business and is page-influenced; a capture is `width * bytes`, which is what makes two
// captures of the same output comparable with a `memcmp` and what keeps the storage figure
// predictable.
class CapturingSink final : public IFrameSink
{
public:
	// `depth` frames are kept, oldest evicted. One is the common case — *what did the last frame look
	// like* — and more than one is what a buffer-age test needs, since the question there is whether
	// the target reached on frame four still holds frame one.
	CapturingSink(
		PixelSize<DeviceSpace> size,
		PixelFormat format,
		std::size_t depth = 1,
		FrameRetention retention = FrameRetention::Release
	);

	void OnFrame(VirtualOutput& output, const VirtualFrame& frame) override;

	// One kept frame: what the presenter said about it, and what it looked like.
	struct Capture
	{
		VirtualFrame Frame{};
		ImageView Image{};
	};

	// How many are kept right now, which is `min(Copied(), depth)`.
	[[nodiscard]] std::size_t Retained() const noexcept;

	// Oldest first. An index past `Retained()` yields an empty capture rather than reading past the
	// ring, for `ImageView::At`'s reason: the predicates over an empty view all answer false.
	[[nodiscard]] Capture At(std::size_t index) const noexcept;

	[[nodiscard]] Capture Newest() const noexcept;

	// Frames whose pixels were copied.
	[[nodiscard]] std::uint64_t Copied() const noexcept { return m_Copied; }

	// Frames that arrived and could not be copied — the wrong size, the wrong format, or a buffer
	// with no mapping. Counted rather than asserted, because the party that can say whether a skip is
	// a bug is the test, and a sink that aborted would take the frame thread with it.
	[[nodiscard]] std::uint64_t Skipped() const noexcept { return m_Skipped; }

	// Give back everything a `Hold` sink is sitting on. A no-op for a releasing one.
	void ReleaseHeld(VirtualOutput& output) noexcept;

	[[nodiscard]] std::size_t Held() const noexcept { return m_Held.size(); }

private:
	// Release or record, by policy. One place, so that the skip paths retire a frame the same way the
	// copied one does — a sink that dropped a frame it could not read *and* kept the image would
	// stall the output for a reason nothing reported.
	void Retire(VirtualOutput& output, const VirtualFrame& frame);

	[[nodiscard]] std::span<const std::byte> Slot(std::size_t index) const noexcept;

	PixelSize<DeviceSpace> m_Size{};
	PixelFormat m_Format{};
	std::uint32_t m_Stride = 0;
	std::size_t m_Depth = 1;
	FrameRetention m_Retention = FrameRetention::Release;

	std::vector<std::byte> m_Pixels;
	std::vector<VirtualFrame> m_Frames;
	std::vector<std::uint32_t> m_Held;

	std::uint64_t m_Copied = 0;
	std::uint64_t m_Skipped = 0;
};
