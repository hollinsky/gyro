#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Core/Result.h"
#include "Geometry/Space.h"
#include "Seam/Pixel.h"
#include "Virtual/Output.h"
#include "Virtual/Sink.h"

// The consumer that is a file: every frame an output presents, written as a PAM.
//
// Virtual/Sink.h names this as the case it deliberately left undone — *a sink that hands a frame to
// another thread* — and defers it because the shape it imagined hands over the **descriptor**, which
// races the frame thread's `AcquireTarget` on the same ring. This hands over a **copy**, which races
// nothing: the image is released back to the output inside `OnFrame` exactly as `CapturingSink`
// releases it, and what the writer thread owns afterwards is bytes in this object's own slab. That
// is the whole reason the deferral does not apply, and it is why a recording path that wants the
// descriptor is still open.
//
// **Two threads, and the split is the point.** The frame thread does one bounded `memcpy` and one
// atomic store; the writer thread does the `open`, the `write`, and the `rename`. Docs/Open.md's
// *spdlog async sink* entry is the hazard being avoided — a file write from a `SCHED_FIFO` thread
// punts to io-wq and comes back as a frame gyro missed — and a dump whose own cost showed up in the
// timing it was opened to inspect would be an instrument that reads its own weight.
//
// **A full queue drops the newest frame and counts it.** The alternative is for the frame thread to
// wait for the writer, which turns a slow disk into a stutter — and a dump that perturbs the thing
// it is measuring is worse than a dump with holes. The holes are visible rather than silent, because
// a file is named for the frame's own sequence: a gap in `frame-00000042.pam` is a gap in the run.
// `Dropped()` says how many, and the composition root reports it.
class FrameDump final : public IFrameSink
{
public:
	// Deep enough that an ordinary write finishes well inside a frame period and shallow enough that
	// the slab stays a fraction of a target ring — four 1080p images is 33 MB, against the 25 MB the
	// three-deep ring being dumped already holds. Deeper buys nothing against a disk that cannot keep
	// up, because a queue only absorbs a burst; a sustained deficit drops either way.
	static constexpr std::size_t DefaultDepth = 4;

	// Nothing is allocated and no thread exists until `Open`, so this is a member the way
	// `VirtualOutput` is one — constructed where the root builds its outputs, started once the
	// configuration it was built for is known to be real.
	FrameDump(std::string directory, PixelSize<DeviceSpace> size, PixelFormat format, std::size_t depth = DefaultDepth);

	// Stops and joins, so a dump that goes out of scope has written everything it accepted.
	~FrameDump() override;

	// Reserve the slab and start the writer.
	//
	// `EINVAL` for a shape that cannot be held — an empty extent, or a format Seam/Pixel.h cannot
	// decode a sample width for — and `EALREADY` for a second call. The directory is not touched
	// here: Virtual/Pam.h creates it at the first frame, which keeps a run that presented nothing
	// from leaving an empty directory behind.
	[[nodiscard]] Result<void> Open();

	// Drain what has been queued, then join. Idempotent, and safe to call from the thread that
	// constructed this once the frame thread is no longer running.
	//
	// **`OnFrame` must not run after this begins.** The composition root joins the frame thread
	// before it closes the dump, which is the ordering that makes the drain a drain rather than a
	// race with a producer still pushing.
	void Close() noexcept;

	// The frame thread's half: bracket the buffer, copy the rows, publish the slot, give the image
	// back. No allocation, no syscall on the copy path, and no file.
	void OnFrame(VirtualOutput& output, const VirtualFrame& frame) override;

	// Block until everything queued has been written.
	//
	// **Not for the frame thread**, which is the whole point of the queue — this exists for a caller
	// that wants a barrier rather than a cadence: a test asserting what reached disk, or a tool that
	// wants the directory complete before it reads it. Called from the producer's thread and never
	// concurrently with `OnFrame`, since a flush that raced a push would return with the pushed frame
	// still outstanding.
	void Flush() noexcept;

	// Frames the frame thread handed over.
	[[nodiscard]] std::uint64_t Queued() const noexcept { return m_Queued.load(std::memory_order_relaxed); }

	// Frames that reached a file.
	[[nodiscard]] std::uint64_t Written() const noexcept { return m_Written.load(std::memory_order_relaxed); }

	// Frames the writer could not keep up with. Anything above zero means the pictures are a sample
	// of the run rather than the whole of it, which a reader has to be told.
	[[nodiscard]] std::uint64_t Dropped() const noexcept { return m_Dropped.load(std::memory_order_relaxed); }

	// Frames whose pixels could not be read at all — the wrong size, the wrong format, or a buffer
	// with no mapping. `CapturingSink`'s counter for `CapturingSink`'s reason: the party that can say
	// whether a skip is a bug is the caller, and a sink that aborted would take the frame thread.
	[[nodiscard]] std::uint64_t Skipped() const noexcept { return m_Skipped.load(std::memory_order_relaxed); }

	// Frames the writer took and the filesystem refused.
	[[nodiscard]] std::uint64_t Failed() const noexcept { return m_Failed.load(std::memory_order_relaxed); }

	// Why the first refusal happened, for a caller that wants to say more than a count.
	//
	// Written by the writer thread and read after `Close` has joined it, which is the whole of the
	// synchronisation — reading it while the writer runs is a data race and there is no caller that
	// wants to.
	[[nodiscard]] const std::optional<Error>& FirstFailure() const noexcept { return m_FirstFailure; }

	[[nodiscard]] std::string_view Directory() const noexcept { return m_Directory; }

private:
	// What the writer needs about a queued image that is not in the pixels. Touched by the producer
	// before the release store that publishes the slot and by the consumer after the acquire load
	// that observes it, which is what makes it ordinary memory rather than an atomic.
	struct Slot
	{
		std::uint64_t Sequence = 0;
	};

	void Write();

	[[nodiscard]] std::span<std::byte> Pixels(std::size_t slot) noexcept;

	std::string m_Directory;
	PixelSize<DeviceSpace> m_Size{};
	PixelFormat m_Format{};

	// Tight, so a slot's size is a function of the mode rather than of the allocator's padding. The
	// same choice `CapturingSink` makes, for the same reason one file over.
	std::uint32_t m_Stride = 0;
	std::size_t m_Depth = 0;

	std::vector<std::byte> m_Slabs;
	std::vector<Slot> m_Slots;

	// A single-producer, single-consumer ring of monotone counts. The producer owns `m_Head` and the
	// consumer owns `m_Tail`; neither ever writes the other's, which is what keeps the whole thing to
	// a release store on one side and an acquire load on the other with no read-modify-write between
	// them. Separately aligned because the two are written every frame by two different cores and
	// sharing a line would make the cheap half of this expensive.
	alignas(64) std::atomic<std::uint64_t> m_Head{ 0 };
	alignas(64) std::atomic<std::uint64_t> m_Tail{ 0 };

	// What the writer parks on. A counter rather than a flag, because the wait has to be able to
	// observe *that something changed* rather than *what the state is* — a writer that checked the
	// queue, found it empty, and then waited on a flag another frame had already set and cleared
	// would sleep through the frame that woke it.
	//
	// Both a push and a stop bump it. `notify_one` is a futex wake and is skipped outright when the
	// writer is not parked, so the frame thread's cost is an increment on the common path and one
	// wake on the path where the disk has caught up — the same trade `Interrupt::Raise` makes for
	// the 8-byte write it does from a signal handler.
	alignas(64) std::atomic<std::uint32_t> m_Signal{ 0 };

	std::atomic<bool> m_Stopping{ false };
	std::thread m_Writer;

	std::atomic<std::uint64_t> m_Queued{ 0 };
	std::atomic<std::uint64_t> m_Written{ 0 };
	std::atomic<std::uint64_t> m_Dropped{ 0 };
	std::atomic<std::uint64_t> m_Skipped{ 0 };
	std::atomic<std::uint64_t> m_Failed{ 0 };

	std::optional<Error> m_FirstFailure{};
};
