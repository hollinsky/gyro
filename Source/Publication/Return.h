#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

#include "Core/Handle.h"

// The return half of the crossing: frame → dispatch, one report per frame.
//
// Docs/Architecture.md#the-publication-boundary designs this rather than leaving it incidental,
// because some things are frame-thread knowledge that the dispatch thread needs — `wl_surface.frame`
// callbacks, `wp_presentation_feedback` with the instant the flip actually landed, `wl_buffer.release`,
// the per-buffer hold, and the measured costs that feed budgets. None of it is state anything renders
// from, which is what keeps the forward channel's exclusivity meaningful. A third channel would be a
// design error rather than an addition, and a *concrete* record is what makes that enforceable: a
// queue templated on its payload is an invitation to instantiate a second one.
//
// **It is wait-free on the writer this time, and that inverts the sizing problem.** The frame thread
// must never wait on dispatch, so the queue is bounded and needs a policy for what happens when it
// fills. Dropping is the obvious one and it is wrong: a lost `wl_surface.frame` callback is not a
// hitch but a hang, because a client waiting on it never draws again.
//
// **So the record is a per-frame summary, and the per-surface consequences are derived on the far
// side.** Dispatch authored the snapshot, so it already knows which surfaces sequence S contained; a
// report saying *S was presented* lets it derive the callbacks and the buffer releases itself. That is
// decision 50's publish-coefficients-and-evaluate-per-output run backwards, and it collapses the
// channel from outputs × surfaces to one record per frame. What is left irreducible is the per-buffer
// hold, which Architecture already names as the one thing the watermark cannot express.
//
// **And then the bound falls out of something already true: the frame thread cannot allocate, so every
// set it holds is fixed-capacity.** The number of holds that can release in one frame has a ceiling
// whether this file states one or not. Sizing the record from that ceiling, and the queue from a
// generous number of frames of dispatch latency, leaves no drop policy to write. Two paths remain, and
// neither loses anything: a full queue is merged into the writer's own staged report — legal because
// it is the only writer — and releases that do not fit are simply *not accepted*, so the frame thread
// keeps holding those buffers for another frame. See Docs/Decisions.md decision 74.
//
// **What is deliberately not here yet.** The presented sequence and its timestamp, the per-output
// measured costs, the VRR servo's observations: all of them are this record's, and none of them has a
// producer until the frame loop exists. The project's usual line applies — a field nobody writes is a
// field that is wrong in detail by the time somebody does — and adding one later costs a recompile of
// two halves that are always built together. The *shape* is what is being fixed now, because the shape
// is what decides the sizing and the loss policy, and that is the part that cannot be changed later.

struct BufferTag
{
	static constexpr std::string_view Name = "Buffer";
};

// A client buffer, named so that a release cannot free the wrong one.
//
// The identity is the dispatch side's — it mints one when it imports a client buffer and consumes one
// when the hold comes back — and the frame thread only carries it across. Publication names the type
// anyway for the reason Publication/Snapshot.h names the runs: the waist is where the schema both
// halves bind to belongs, and neither half may reach the other to find it. Generational rather than a
// bare index because Docs/Architecture.md admits cross-thread lifetime as the one genuinely new cost
// of the split: a client may destroy a surface while the frame thread holds its buffer, and a stale
// release must compare unequal rather than name whatever occupies the slot now.
using BufferId = Handle<BufferTag>;

// How many holds one report can carry.
//
// A hold exists only for an exit blit deferred by a frame or two
// (Docs/Animation.md#exit-pixels), so this is a ceiling on exit animations landing in the same frame,
// not on windows. Sixteen is well past what a person can close at once and past what a mass teardown
// lands in any single frame; beyond it the frame thread keeps holding, which is a state it can always
// be in and which costs a deferred blit rather than a leak.
inline constexpr std::size_t ReleasesPerReport = 16;

// How many frames of dispatch latency the queue absorbs before the writer starts merging. Dispatch
// drains this once per iteration, so filling it means dispatch has not run in sixteen frames — by
// which point merging is the correct behaviour rather than a fallback, since a stale report and a
// fresh one say the same thing about everything except the holds.
inline constexpr std::size_t ReportQueueDepth = 16;

static_assert((ReportQueueDepth & (ReportQueueDepth - 1)) == 0, "so the index is a mask");

// What one frame tells the dispatch thread.
//
// POD with its padding spelled, for the reason Publication/Snapshot.h gives about the forward
// direction: decision 49 holds open the option of dispatch becoming a process, and that would put this
// record in a shared mapping too. Nothing here is a pointer and nothing here is uninitialised.
struct FrameReport
{
	// The sequence the frame thread last consumed, and is rendering from. Everything strictly below it
	// is the dispatch side's to reclaim; this one is not. Monotone, so merging two reports is a max
	// and losing an intermediate value costs nothing.
	std::uint64_t Watermark = 0;

	std::uint32_t ReleaseCount = 0;
	std::uint32_t Reserved = 0;

	// The client buffers the frame thread has finished with ahead of the watermark — the individual
	// hold that Docs/Architecture.md keeps beside the watermark rather than encoding into it, because
	// withholding the watermark to express one would stall reclamation of every unrelated commit below.
	BufferId Releases[ReleasesPerReport] = {};

	[[nodiscard]] std::span<const BufferId> Released() const noexcept { return { Releases, ReleaseCount }; }
};

static_assert(std::is_trivially_copyable_v<FrameReport> && std::is_standard_layout_v<FrameReport>);
static_assert(sizeof(FrameReport) == 16 + ReleasesPerReport * sizeof(BufferId), "No padding to leave uninitialised");

class ReturnChannel
{
public:
	ReturnChannel() = default;

	// Shared between the two threads by reference, for the reason SnapshotRing is.
	ReturnChannel(const ReturnChannel&) = delete;
	ReturnChannel& operator=(const ReturnChannel&) = delete;

	// The frame side, once per frame. Wait-free, non-allocating, and it never blocks on dispatch.
	//
	// Returns how many of the offered releases were taken; the caller keeps holding the rest and offers
	// them again next frame. That is not a failure path so much as the shape of the contract: the frame
	// thread's hold set is fixed-capacity because it cannot allocate, so "still holding" is always a
	// state it can be in, and a hold kept one frame longer costs a deferred blit rather than a leak.
	//
	// The watermark is always taken, whether or not the report reaches the queue this call, because it
	// merges by maximum into a report that is still staged.
	std::size_t Post(std::uint64_t watermark, std::span<const BufferId> releases = {}) noexcept
	{
		m_Staged.Watermark = std::max(m_Staged.Watermark, watermark);

		const std::size_t room = ReleasesPerReport - m_Staged.ReleaseCount;
		const std::size_t accepted = std::min(room, releases.size());

		for (std::size_t index = 0; index < accepted; ++index)
		{
			m_Staged.Releases[m_Staged.ReleaseCount + index] = releases[index];
		}

		m_Staged.ReleaseCount += static_cast<std::uint32_t>(accepted);

		if (Push(m_Staged))
		{
			m_Staged = {};
		}

		return accepted;
	}

	// The dispatch side, drained to empty once per iteration. False when there is nothing left.
	//
	// Draining is what advances the watermark, and the acquire below is what orders the frame thread's
	// last read of a snapshot before this thread reuses its bytes — so this call is not merely how
	// dispatch learns the watermark, it is what makes acting on it sound. See Publication/Ring.h.
	[[nodiscard]] bool Take(FrameReport& into) noexcept
	{
		const std::uint64_t read = m_Read.load(std::memory_order_relaxed);

		if (read == m_Written.load(std::memory_order_acquire))
		{
			return false;
		}

		into = m_Slots[read & (ReportQueueDepth - 1)];
		m_Read.store(read + 1, std::memory_order_release);

		return true;
	}

	// Whether the writer is holding a report the queue had no room for. Nothing in the design reads
	// this — it is here so a test can prove the merge happened rather than infer it from what came out.
	[[nodiscard]] bool HasStagedReport() const noexcept
	{
		return m_Staged.Watermark != 0 || m_Staged.ReleaseCount != 0;
	}

private:
	[[nodiscard]] bool Push(const FrameReport& report) noexcept
	{
		const std::uint64_t written = m_Written.load(std::memory_order_relaxed);

		if (written - m_Read.load(std::memory_order_acquire) == ReportQueueDepth)
		{
			return false;
		}

		m_Slots[written & (ReportQueueDepth - 1)] = report;
		m_Written.store(written + 1, std::memory_order_release);

		return true;
	}

	// The writer's own, touched by no other thread. It exists so that a full queue costs a merge rather
	// than a loss, and it is legal precisely because there is exactly one writer: a report nobody has
	// been told about yet is still the frame thread's to amend.
	FrameReport m_Staged{};

	FrameReport m_Slots[ReportQueueDepth]{};
	std::atomic<std::uint64_t> m_Written{ 0 };
	std::atomic<std::uint64_t> m_Read{ 0 };
};
