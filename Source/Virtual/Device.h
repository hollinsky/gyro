#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "Core/Clock.h"
#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Seam/EventSource.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/Renderer.h"
#include "Virtual/Output.h"
#include "Virtual/Sink.h"

// The virtual backend's device: the outputs it owns, the consumers they feed, and the one source
// their completions arrive on.
//
// **Virtual/Output.h said when to write this and this is that moment.** That file declines to have an
// `IEventSource` on the grounds that Seam/EventSource.h puts the descriptor at *device* granularity,
// so a source for a set of one would be `HeadlessDevice`'s ordering sort copied for no reason — and
// it names the trigger: *when a second output shares a consumer, or when the frame loop drives one of
// these for real.* An integration test that publishes a scene and reads the pixels back is the frame
// loop driving one for real; `FrameLoop::Listen` takes `IEventSource*` and there was nothing to hand
// it.
//
// **The descriptor is invalid, and Seam/EventSource.h says that is an ordinary answer.** Nothing
// becomes readable when a virtual frame boundary falls due — the boundary is a function of the clock,
// exactly as a simulated vblank is — so the drain reports what the clock says has happened. What a
// virtual output has instead of a file is `NextEvent()`, and the composition root folds it into the
// wake for the reason Docs/Open.md's *the headless backend has no descriptor* entry gives about the
// same shape one module over.
//
// **A frame is not handed to its consumer until the composite has finished.** Seam/Presenter.h's
// `PresentLayer::Acquire` is what a KMS commit hands the kernel as an in-fence, and there is no
// kernel here to honour it: a virtual output is a presenter whose consumer reads the pixels with a
// memcpy, so something has to be the thing that waits. It is this, and what it asks is
// `IRenderer::IsComplete` — the seam's own poll, held by the party that submitted the work. Inventing
// a narrower interface for it would be a second answer to a question the seam already answers, which
// is how a ring ends up with a frame two parties disagree about.
//
// **The poll is a poll and never a wait**, for `IsComplete`'s own reason: the drain runs on the frame
// thread, and blocking there would put the GPU's schedule on the `SCHED_FIFO` thread. A frame whose
// point is outstanding is simply not delivered this drain, and `NextEvent()` asks to be called again
// a period later — bounded latency for the consumer, no spin for the thread, and no possibility of
// the loop folding to `Never()` with a frame stuck in hand.

// Sized to match what the frame loop will schedule, for `MaxHeadlessOutputs`' reason: `Virtual`
// cannot name `Frame::MaxOutputs` — the module graph does not have that edge and should not — so the
// number is repeated with the reason rather than reached for.
inline constexpr std::size_t MaxVirtualOutputs = 16;

class VirtualDevice final : public IEventSource
{
public:
	explicit VirtualDevice(const IClock& clock) noexcept : m_Clock{ &clock } {}

	// Add an output and the consumer it feeds.
	//
	// The sink is required rather than optional, because Virtual/Sink.h's `DiscardingSink` is what
	// *no consumer* actually means and it is a different thing from no sink at all: an output nobody
	// releases for stalls after one ring, which is correct behaviour that would be very confusing to
	// arrive at by leaving an argument out.
	//
	// `completion` is the renderer whose submissions land in this output's targets, or null where
	// every point is immediate — a CPU blitter, or a device that cannot export a timeline. Null means
	// frames are delivered the moment they are presented, which is exactly right for those and wrong
	// for anything else.
	//
	// A pointer back, because a presenter is neither copyable nor movable and because a full device
	// has to be able to say so. Past capacity is a miswired composition root rather than a runtime
	// condition, and `nullptr` makes that a crash at the call site that made the mistake.
	[[nodiscard]] VirtualOutput*
	Add(const OutputConfiguration& configuration,
	    IDmabufAllocator& allocator,
	    IFrameSink& sink,
	    const IRenderer* completion = nullptr,
	    VirtualOutputPolicy policy = {})
	{
		if (m_Count >= MaxVirtualOutputs)
		{
			return nullptr;
		}

		Slot& slot = m_Slots[m_Count];
		slot.Output.emplace(*m_Clock, allocator, configuration, policy);
		slot.Sink = &sink;
		slot.Completion = completion;
		slot.Delivered.reset();
		++m_Count;

		return &*slot.Output;
	}

	[[nodiscard]] std::size_t Count() const noexcept { return m_Count; }

	[[nodiscard]] VirtualOutput& At(std::size_t index) noexcept { return *m_Slots[index].Output; }

	[[nodiscard]] RawFd Descriptor() const noexcept override { return {}; }

	// Advance every output that has something due, then deliver what is ready.
	//
	// **Two passes rather than one, and the order is the argument.** An output advanced late in the
	// first pass may present a frame this same drain, and a consumer that sees it in the same call is
	// what a real backend does — a page flip and its completion event are one wakeup. Ordering the
	// advances across outputs costs an insertion sort over at most sixteen and buys the property a
	// reader assumes without checking: frames arrive in the order the outputs produced them.
	// `HeadlessDevice` has the same sort for the same reason.
	Result<void> Drain() override
	{
		const Instant now = m_Clock->Now();

		std::array<std::uint8_t, MaxVirtualOutputs> order{};
		const std::size_t due = OrderByEvent(order, now);

		for (std::size_t position = 0; position < due; ++position)
		{
			m_Slots[order[position]].Output->Advance(now);
		}

		for (std::size_t position = 0; position < due; ++position)
		{
			Deliver(m_Slots[order[position]]);
		}

		// Then everything else, because an output that was not due may still be holding a frame whose
		// composite had not finished when it was last looked at — and nothing else was going to come
		// back for it. `Deliver` is idempotent against a frame already handed over, so the outputs
		// visited twice cost a comparison and the ordering above is preserved.
		for (std::size_t index = 0; index < m_Count; ++index)
		{
			Deliver(m_Slots[index]);
		}

		return {};
	}

	// When this device next has something to say.
	//
	// Public and non-virtual, exactly as `HeadlessDevice::NextEvent` is: Seam/EventSource.h has no
	// such verb and Docs/Open.md carries the question of whether it should, on the grounds that every
	// *real* source would answer `Never()`. Until that is settled the composition root folds this in,
	// which is defensible because the root is what wired a clock-driven presenter to a real clock.
	[[nodiscard]] Instant NextEvent() const noexcept
	{
		Instant soonest{ Duration::max() };

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			const Slot& slot = m_Slots[index];
			const Instant event = slot.Output->NextEvent();

			soonest = event < soonest ? event : soonest;

			if (!Awaiting(slot))
			{
				continue;
			}

			// A period rather than now, so that a frame waiting on the GPU costs the consumer bounded
			// latency instead of costing the frame thread a spin. The composite that has not finished
			// after a whole period is a stall worth seeing in the numbers rather than one to poll
			// away.
			const Instant retry = Advanced(m_Clock->Now(), slot.Output->Configuration().Period);

			soonest = retry < soonest ? retry : soonest;
		}

		return soonest;
	}

	// How many outputs are holding a presented frame their consumer has not been shown yet, which is
	// always the composite not having finished. Zero on every device whose renderer hands out
	// immediate points, and the figure to look at when a recording is running behind.
	[[nodiscard]] std::size_t Awaiting() const noexcept
	{
		std::size_t waiting = 0;

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			waiting += Awaiting(m_Slots[index]) ? 1U : 0U;
		}

		return waiting;
	}

private:
	struct Slot
	{
		std::optional<VirtualOutput> Output{};
		IFrameSink* Sink = nullptr;
		const IRenderer* Completion = nullptr;

		// The sequence last handed to the sink. An optional rather than a sentinel because a
		// timeline's first frame is sequence zero, and a sentinel of zero would silently drop it.
		std::optional<std::uint64_t> Delivered{};
	};

	[[nodiscard]] static bool Awaiting(const Slot& slot) noexcept
	{
		const std::optional<VirtualFrame> presented = slot.Output->PresentedFrame();

		return presented.has_value() && slot.Delivered != presented->Sequence;
	}

	void Deliver(Slot& slot)
	{
		const std::optional<VirtualFrame> presented = slot.Output->PresentedFrame();

		if (!presented || slot.Delivered == presented->Sequence)
		{
			return;
		}

		if (slot.Completion != nullptr && !slot.Completion->IsComplete(presented->Acquire))
		{
			return;
		}

		// Marked before the call, so that a sink which reaches back into the device cannot be handed
		// the same frame twice.
		slot.Delivered = presented->Sequence;
		slot.Sink->OnFrame(*slot.Output, *presented);
	}

	[[nodiscard]] std::size_t OrderByEvent(std::array<std::uint8_t, MaxVirtualOutputs>& order, Instant now) const
	{
		std::size_t count = 0;

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			const Instant event = m_Slots[index].Output->NextEvent();

			if (event > now)
			{
				continue;
			}

			std::size_t position = count;

			while (position > 0 && m_Slots[order[position - 1]].Output->NextEvent() > event)
			{
				order[position] = order[position - 1];
				--position;
			}

			order[position] = static_cast<std::uint8_t>(index);
			++count;
		}

		return count;
	}

	const IClock* m_Clock = nullptr;
	std::array<Slot, MaxVirtualOutputs> m_Slots{};
	std::size_t m_Count = 0;
};
