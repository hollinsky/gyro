#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "Core/Buffer.h"
#include "Core/Time.h"

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
// keeps holding those buffers for another frame. See Docs/Decisions.md decision 75.
//
// **The presented pair has a producer now, and the rest still does not.** *(2026-08-23.)* The entry
// above held off the presented sequence, the per-output measured costs and the VRR servo's
// observations together, on the grounds that none of them had anything writing it. `Frame/Loop.h`'s
// `OnPresented` is that producer for the first of the three, so it lands and the other two keep
// waiting — a field nobody writes is still a field that is wrong in detail by the time somebody does.
//
// **Then the panel's own three came with it too.** *(2026-08-30.)* This paragraph read *what did not
// come with it is everything else `PresentationInfo` carries* — the vblank counter, the observed
// period and the honesty flags being the frame clock's inputs and nothing the dispatch side could use
// — and the premise was that nothing over there wanted them. Serving `wp_presentation` is what wanted
// them: a client asking when its frame was seen is answered with a retrace counter, a refresh figure
// and whether the timestamp came from hardware, and a compositor holding all three and sending zeroes
// is one whose own nested backend then reports a clock it cannot trust. So they cross.
//
// **What is still not fused is what the numbers are *for*.** The published sequence stays the
// identifier — it is the only number the dispatch side can turn back into the surfaces that were in
// that frame, and the vblank counter beside it is a fact about the panel that gets *forwarded* rather
// than looked anything up by. That is the rule
// [Structure.md](../../Docs/Structure.md#region-is-in-geometry-and-reachability-is-why) states about
// these two types being *near enough to fuse and the table says not to*, holding on the axis it was
// always about: no `Seam` type reaches this module, no target index, no discard case, and nothing here
// is an input to a prediction.

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

// How many outputs one report speaks for.
//
// **The same number `Frame/Admission.h` calls `MaxOutputs`, restated rather than shared, and the
// duplication is the honest form.** The publication waist may not depend on `Frame` — the whole point
// of it is that neither half names the other — and there is no module below both that has a reason to
// hold a panel count. So the agreement is written twice and checked once: `Frame/Loop.h` includes both
// headers and static_asserts that the loop's set fits in a report, which is the one place where being
// wrong about it would be a run of outputs silently reporting nothing.
inline constexpr std::size_t OutputsPerReport = 16;

static_assert((ReportQueueDepth & (ReportQueueDepth - 1)) == 0, "so the index is a mask");

// One output's most recent frame on the glass.
//
// Decision 75's *S was presented at T*, and then what the panel says about itself. `Sequence` is the
// **published** sequence the frame was drawn from rather than the panel's vblank counter, and it is
// the one field anything is looked up by: the dispatch side authored that snapshot, so it is the one
// number it can turn back into the surfaces the frame contained — a frame callback for each,
// `wp_presentation_feedback` at `At`, and the client damage those surfaces had cleared by it.
//
// **Everything after `At` is forwarded rather than read.** The retrace counter, the observed period
// and the three honesty flags are what a client's `presented` event has to carry and what nothing on
// this side of the boundary derives anything from — see the paragraph at the top of this file for why
// they cross at all, which is a reversal rather than the original reading.
//
// **Zero is *no news* rather than *sequence zero*.** Publication/Ring.h begins at one for exactly this
// reason, and an output that presented nothing since the last report says so by leaving this alone.
// That matters because a report goes out every frame whether or not any output flipped, so the common
// case is a run of zeroes and it must not read as a regression.
struct PresentedFrame
{
	std::uint64_t Sequence = 0;

	// When it reached the glass, in the one timebase — Core/Time.h's, converted by whichever backend
	// produced it and never observed in another domain. This is what a client's
	// `wp_presentation_feedback.presented` is filled from, which is why it is a `Core` type here and
	// not a copy of the `Seam` record the frame clock reads.
	Instant At{};

	// The panel's own frame counter for that flip — the vblank sequence on KMS, the host's feedback
	// sequence nested. **It is not what identifies the frame and must never be used as though it were**:
	// `Sequence` above is gyro's number and this is the display's, they count different things at
	// different rates, and an output that has no such counter leaves it zero.
	//
	// It crosses because `wp_presentation_feedback.presented` carries one and there is nowhere else on
	// this side of the boundary it could be obtained from. A client differences two of them to know
	// whether it missed a refresh, which is a question about the panel rather than about gyro.
	std::uint64_t Vblank = 0;

	// The interval the output actually ran at, as the backend observed it. Zero where it genuinely does
	// not know — the same meaning `Seam/PresentationInfo.h` gives the field, and the reason a client's
	// `refresh` falls back to the mode's nominal period rather than to a guess.
	Duration Period{};

	// The honesty half, and it is here for the same reason it exists there: a fabricated timestamp is
	// indistinguishable from a real one, so a backend that does not know says so. A client reads these
	// to decide how much to trust the instant above, and gyro nested inside gyro is the client that
	// reads them hardest — `Frame/FrameClock.h` turns `HardwareClock` into whether its own latency
	// assertions mean anything.
	bool Vsync = false;
	bool HardwareClock = false;
	bool ZeroCopy = false;

	// Spelled, for the reason the record above is: decision 49 holds open a shared mapping, and
	// uninitialised bytes crossing one are what that rules out.
	std::uint8_t Reserved[5] = {};

	friend constexpr bool operator==(PresentedFrame, PresentedFrame) noexcept = default;
};

static_assert(std::is_trivially_copyable_v<PresentedFrame> && std::is_standard_layout_v<PresentedFrame>);
static_assert(sizeof(PresentedFrame) == 5 * sizeof(std::uint64_t), "No padding to leave uninitialised");

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

	// How many outputs the run below speaks for, which is decision 84's rule arriving on the return
	// leg: a run whose length is not the reader's output set is *no* information rather than partial
	// information. A hotplug replaces both halves of the loop at once, so a report staged before one
	// and merged after it would otherwise attribute a timestamp to whichever panel now sits at that
	// index. The merge below replaces the run instead of blending it, and the reader ignores a run that
	// is not its own.
	std::uint32_t OutputCount = 0;

	// The client buffers the frame thread has finished with ahead of the watermark — the individual
	// hold that Docs/Architecture.md keeps beside the watermark rather than encoding into it, because
	// withholding the watermark to express one would stall reclamation of every unrelated commit below.
	BufferId Releases[ReleasesPerReport] = {};

	// What reached the glass, indexed by the output's position in the set both halves agreed on — the
	// same positional convention the snapshot's per-output wake and placement runs use, and for the
	// same reason: an identity here would be a second answer to a question the ordering already has.
	PresentedFrame Presentations[OutputsPerReport] = {};

	[[nodiscard]] std::span<const BufferId> Released() const noexcept { return { Releases, ReleaseCount }; }

	[[nodiscard]] std::span<const PresentedFrame> Presented() const noexcept { return { Presentations, OutputCount }; }
};

static_assert(std::is_trivially_copyable_v<FrameReport> && std::is_standard_layout_v<FrameReport>);
static_assert(
	sizeof(FrameReport) == 16 + ReleasesPerReport * sizeof(BufferId) + OutputsPerReport * sizeof(PresentedFrame),
	"No padding to leave uninitialised"
);

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
	// merges by maximum into a report that is still staged. So does the presented run, and neither can
	// be refused — only the holds can, because only they are a set rather than a high-water mark.
	std::size_t Post(
		std::uint64_t watermark,
		std::span<const BufferId> releases = {},
		std::span<const PresentedFrame> presented = {}
	) noexcept
	{
		m_Staged.Watermark = std::max(m_Staged.Watermark, watermark);

		Stage(presented);

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

	// The frame side, like Post, and that is not a formality. This reads the staged report, which is
	// unsynchronised precisely because it has exactly one writer, so a dispatch-side caller — a
	// diagnostic, an assertion, a log line — would not get a stale answer, it would be a data race. The
	// const is ownership of the report, not permission to cross the boundary.
	//
	// Nothing in the design reads it at all; it is here so a test can prove the merge happened rather
	// than infer it from what came out.
	[[nodiscard]] bool HasStagedReport() const noexcept
	{
		return m_Staged.Watermark != 0 || m_Staged.ReleaseCount != 0 || m_Staged.OutputCount != 0;
	}

private:
	// Merge this iteration's presented run into the staged one.
	//
	// **Per output it is a maximum, for the watermark's reason rather than by analogy to it.** The frame
	// thread acquires snapshots in order and renders from the newest it holds, so the sequence an output
	// presents is monotone; the dispatch side derives *everything up to and including P has been shown*,
	// which makes an intermediate value carry nothing the later one does not. What is genuinely lost is
	// the exactness of one timestamp — a surface that was last in sequence 5 is reported at sequence 6's
	// presentation instant, one frame late — and that only happens when dispatch has already fallen
	// sixteen frames behind, which is the state where the queue is full and this merge runs at all.
	//
	// **A run of a different length replaces rather than blends**, which is the `OutputCount` field's
	// whole reason: the indices are positional, so two runs from different output sets do not describe
	// the same panels and merging them by index would report one output's flip against another's.
	void Stage(std::span<const PresentedFrame> presented) noexcept
	{
		if (presented.empty())
		{
			return;
		}

		const std::uint32_t count = static_cast<std::uint32_t>(std::min(presented.size(), OutputsPerReport));

		if (count != m_Staged.OutputCount)
		{
			for (PresentedFrame& frame : m_Staged.Presentations)
			{
				frame = {};
			}

			m_Staged.OutputCount = count;
		}

		for (std::uint32_t index = 0; index < count; ++index)
		{
			if (presented[index].Sequence > m_Staged.Presentations[index].Sequence)
			{
				m_Staged.Presentations[index] = presented[index];
			}
		}
	}

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
