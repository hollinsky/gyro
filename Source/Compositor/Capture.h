#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Core/Result.h"
#include "Frame/Admission.h"
#include "Geometry/Space.h"
#include "Scene/Capture.h"
#include "Seam/Capture.h"
#include "Seam/RenderTarget.h"

// Input/Chord.h's screenshot verb, on this side of the frame boundary: a slab per output, and a
// thread that turns a filled one into a PAM.
//
// Seam/Capture.h is where the argument lives — why this is a debug hatch rather than the day-to-day
// screencapture decision 64 describes, what the forced composite costs, and what a readback still
// cannot see. What this file adds is the two halves that may not run on the frame thread:
//
//   - **The slabs are sized at `Remember`**, which the composition root calls when it configures an
//     output, so `Reserve` on the frame path is a bounds check and a pointer. A capture of a panel
//     whose mode changed since the last `Remember` is refused rather than resized, because resizing
//     is an allocation and Core/FrameSection.h aborts on one.
//   - **The file is written by this object's own thread.** Virtual/Pam.h opens, writes and renames,
//     and Docs/Open.md's *spdlog async sink* entry is what happens if any of that runs under
//     `SCHED_FIFO`: the write punts to io-wq and comes back as a frame gyro missed. Virtual/Dump.h
//     makes the same split for the same reason and is the shape this follows.
//
// **The client buffers are held continuously and the press only writes them out**, which is the one
// design decision in this file that is not the frame side's. Seam/Capture.h photographs the glass;
// Scene/Capture.h supplies the other half of the diff, the buffer a client actually handed over. If
// that half were armed by the press the way the frame half is, it would record the commits *after*
// the interesting one — a window that repainted wrong and then went quiet has nothing left to offer,
// and a stale row is exactly the bug that makes a window go quiet. So this keeps the newest `wl_shm`
// buffer of every surface, always, and the chord writes the table out. Core/Trace.h's ring is the
// same argument for the same reason: an instrument you switch on records the run after the one you
// wanted.
//
// The cost is a copy of every `wl_shm` commit on the dispatch thread and a resident megabyte or so
// per window, which is why it is behind `--capture` rather than on: Options.h already says a run
// declares up front that it is being debugged, and this is the second thing that declaration buys.
//
// **One capture in flight per output, and a second press while one is outstanding is dropped.** A
// queue would be Virtual/Dump.h's, and it is the right structure for a *cadence* — a stream of frames
// where a hole is a gap in a recording. A screenshot is one frame at an instant a person chose, so
// the useful behaviour when the disk is slow is for the second press to do nothing rather than to
// take a picture of a moment that has passed by the time it lands.
class PamCapture final : public ICaptureSink, public ISurfaceCapture
{
public:
	PamCapture(std::string directory, std::size_t outputs);

	// Stops and joins, so a capture that goes out of scope has written what it accepted.
	~PamCapture() override;

	// Start the writer. Nothing is allocated and no thread exists until this is called.
	[[nodiscard]] Result<void> Open();

	// Drain what is outstanding, then join. Idempotent, and safe once the frame thread is joined.
	void Close() noexcept;

	// Size an output's slab. Called by the composition root when it configures one, on the dispatch
	// thread, and never from the frame path.
	[[nodiscard]] Result<void> Remember(std::size_t index, PixelSize<DeviceSpace> size, PixelFormat format);

	// Arm every output that has a slab. Called from the dispatch thread when the chord fires; the
	// frame thread is woken by the composition root, because this object has no doorbell of its own.
	//
	// Returns how many outputs were armed, which is zero on a run where no output has been remembered
	// — the answer the chord reports rather than leaving a person pressing a key that does nothing.
	std::size_t Request() noexcept;

	[[nodiscard]] bool Wanted(std::uint32_t output) const noexcept override;

	[[nodiscard]] std::span<std::byte> Reserve(
		std::uint32_t output,
		PixelSize<DeviceSpace> size,
		PixelFormat format,
		std::uint32_t stride
	) noexcept override;

	void Publish(std::uint32_t output, std::uint64_t sequence, bool complete) noexcept override;

	// Scene/Capture.h. Both run on the dispatch thread and only there: `Offer` arrives inside a
	// `wl_surface.commit` and `Request` inside the chord, which is the same loop one step earlier.
	[[nodiscard]] bool Wanted() const noexcept override { return true; }

	void Offer(const SurfaceCapture& buffer) noexcept override;

	// Buffers written, which is counted apart from frames because zero of them is a meaningful answer:
	// a run whose clients are all on `zwp_linux_dmabuf_v1` writes pictures and no buffers, and a person
	// reading the log needs to be told that rather than left looking for files.
	[[nodiscard]] std::uint64_t Buffers() const noexcept { return m_Buffers.load(std::memory_order_relaxed); }

	// Captures that reached a file, and captures the filesystem refused. Read after `Close` has joined
	// the writer, which is when the composition root reports them.
	[[nodiscard]] std::uint64_t Written() const noexcept { return m_Written.load(std::memory_order_relaxed); }

	[[nodiscard]] std::uint64_t Failed() const noexcept { return m_Failed.load(std::memory_order_relaxed); }

	// Why the first refusal happened. Written by the writer thread and read after the join.
	[[nodiscard]] const std::optional<Error>& FirstFailure() const noexcept { return m_FirstFailure; }

	[[nodiscard]] std::string_view Directory() const noexcept { return m_Directory; }

private:
	// **A three-state cell rather than two flags, because three parties touch it.** The dispatch
	// thread moves `Idle` to `Armed`, the frame thread moves `Armed` to `Filled` or back to `Idle`,
	// and the writer moves `Filled` back to `Idle`. Two booleans would let a press that arrived
	// between the readback and the write arm a slab the writer is still reading out of.
	enum class Slot : std::uint8_t
	{
		Idle,
		Armed,
		Filled,
	};

	struct Output
	{
		std::vector<std::byte> Pixels;
		PixelSize<DeviceSpace> Size{};
		PixelFormat Format{};
		std::uint32_t Stride = 0;

		// Set by the frame thread before the release that publishes the slot, read by the writer after
		// its acquire — ordinary memory for the reason Virtual/Dump.h's is.
		std::uint64_t Sequence = 0;

		std::atomic<Slot> State{ Slot::Idle };
	};

	// One surface's newest committed buffer, and what its client said it changed.
	//
	// Held behind a `shared_ptr` so the press costs no second copy: the table drops its reference and
	// the writer's queue takes one, and whichever outlives the other frees it.
	struct Buffer
	{
		std::uint32_t Surface = 0;
		std::vector<std::byte> Pixels;
		PixelSize<BufferSpace> Size{};
		std::uint32_t Stride = 0;
		TextureAlpha Alpha = TextureAlpha::Premultiplied;
		std::vector<PixelRect<BufferSpace>> Damage;

		// Which commit of this surface it was, counted by this object. It is what says whether a buffer
		// and a frame are the same moment: nothing changes a texture but a commit, so a buffer whose
		// ordinal did not move between two presses is the one the picture was drawn from.
		std::uint64_t Commit = 0;
	};

	void Write();

	void WriteBuffers();

	[[nodiscard]] std::string Destination(std::size_t index);

	std::string m_Directory;

	// Fixed rather than sized, because `Output` holds an atomic and is therefore neither copyable nor
	// movable — and because the ceiling is the frame loop's own. Only the pixel slabs are allocated,
	// and only for the outputs `Remember` was called for.
	std::array<Output, MaxOutputs> m_Outputs{};

	std::size_t m_Count = 0;

	// What the writer parks on. A counter for Virtual/Dump.h's reason: a flag another capture had
	// already set and cleared is one the writer sleeps through.
	alignas(64) std::atomic<std::uint32_t> m_Signal{ 0 };

	std::atomic<bool> m_Stopping{ false };
	std::thread m_Writer;

	std::atomic<std::uint64_t> m_Written{ 0 };
	std::atomic<std::uint64_t> m_Failed{ 0 };

	std::optional<Error> m_FirstFailure{};

	// SPEC: how many surfaces one press writes out. It bounds the resident cost rather than estimating
	// a working set — thirty-two windows of `wl_shm` pixels is a couple of hundred megabytes at the
	// worst plausible size, and a machine with more windows than this open is one where the picture,
	// not the buffers, is the thing to read first.
	static constexpr std::size_t MaxBuffers = 32;

	// The newest buffer per surface. Dispatch-thread only — `Offer` writes it and `Request` empties it
	// into the queue below, and both are the same thread one step apart.
	std::vector<std::shared_ptr<const Buffer>> m_Held;

	std::uint64_t m_Commits = 0;

	// Which press a written buffer belongs to, so a directory holds one set per keystroke.
	std::uint64_t m_Press = 0;

	// What the writer takes. A lock rather than the slot machinery above it, because this side is not
	// the frame thread: the dispatch loop owes no deadline, so the honest structure is the simple one.
	std::mutex m_Lock;
	std::vector<std::pair<std::uint64_t, std::shared_ptr<const Buffer>>> m_Queued;

	std::atomic<std::uint64_t> m_Buffers{ 0 };
};
