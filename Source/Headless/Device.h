#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "Core/Clock.h"
#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Headless/Output.h"
#include "Seam/EventSource.h"
#include "Seam/OutputConfiguration.h"

// The headless backend's device: the outputs it owns, and the one source their completions arrive on.
//
// **One source for N outputs, which is decision 80 rather than an economy.** Seam/EventSource.h has
// the granularity argument in full: a presenter is one output's, but a backend's event descriptor is
// one *device's* — one DRM file carries page flips for every CRTC on it, one host connection carries
// feedback for every window. Headless has neither file nor connection and is modelled at the same
// granularity anyway, because the granularity is the thing being exercised. A loop that registered a
// source per output would work here and fail on the backend this is standing in for.
//
// **The descriptor is invalid, and Seam/EventSource.h says that is an ordinary answer.** Nothing
// becomes readable when a simulated vblank falls due; the flip is a function of the clock, and the
// drain reports what the clock says has happened. That is also the second argument in that file
// against handing the loop a readiness set — a source with no descriptor could never appear in one, so
// headless would never present.

// Sized to match what the frame loop will schedule. Headless cannot name `Frame::MaxOutputs` — the
// module graph does not have that edge and should not — so the number is repeated with the reason
// rather than reached for.
inline constexpr std::size_t MaxHeadlessOutputs = 16;

class HeadlessDevice final : public IEventSource
{
public:
	explicit HeadlessDevice(const IClock& clock) noexcept : m_Clock{ &clock } {}

	// Add an output.
	//
	// A pointer because a presenter is neither copyable nor movable — so the caller needs its address
	// rather than a value — and because a full device has to be able to say so. Past capacity is a
	// miswired composition root rather than a runtime condition, and `nullptr` is what makes that a
	// crash at the call site that made the mistake instead of a write past the end of the array.
	[[nodiscard]] HeadlessOutput* Add(const OutputConfiguration& configuration, HeadlessOutputPolicy policy = {})
	{
		if (m_Count >= MaxHeadlessOutputs)
		{
			return nullptr;
		}

		m_Slots[m_Count].emplace(*m_Clock, configuration, policy);
		++m_Count;

		return &*m_Slots[m_Count - 1];
	}

	[[nodiscard]] std::size_t Count() const noexcept { return m_Count; }

	[[nodiscard]] HeadlessOutput& At(std::size_t index) noexcept { return *m_Slots[index]; }

	[[nodiscard]] RawFd Descriptor() const noexcept override { return {}; }

	// Emit everything that has fallen due, oldest first.
	//
	// **Ordered across outputs rather than in slot order**, which costs an insertion sort over a set of
	// at most sixteen and buys the property a reader assumes without checking: the flips a drain reports
	// arrive in the order the panels produced them. Nothing downstream depends on it today — each
	// output observes its own presenter and the loop reads its clock after the whole drain — and a
	// backend whose event order was an artefact of construction order is one whose first ordering bug
	// would be found on hardware.
	Result<void> Drain() override
	{
		const Instant now = m_Clock->Now();

		std::array<std::uint8_t, MaxHeadlessOutputs> order{};
		const std::size_t due = OrderByEvent(order, now);

		for (std::size_t position = 0; position < due; ++position)
		{
			m_Slots[order[position]]->Advance(now);
		}

		return {};
	}

	// The earliest instant any output has something to say, or `Duration::max()`'s instant where none
	// does.
	//
	// **This is not a `Wake` and must not become one.** Core/Wake.h is what the frame loop folds to
	// decide when *gyro* needs to be running; this is when the simulated hardware next moves, which is
	// a different question with a different owner. A test driving a ManualClock takes the earlier of the
	// two, and the composition root running headless against a real clock ignores this entirely — the
	// loop's own timer wakes it, and by then the flip has fallen due.
	[[nodiscard]] Instant NextEvent() const noexcept
	{
		Instant earliest{ Duration::max() };

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			earliest = std::min(earliest, m_Slots[index]->NextEvent());
		}

		return earliest;
	}

private:
	[[nodiscard]] std::size_t OrderByEvent(std::array<std::uint8_t, MaxHeadlessOutputs>& order, Instant now) const
	{
		std::size_t count = 0;

		for (std::size_t index = 0; index < m_Count; ++index)
		{
			const Instant event = m_Slots[index]->NextEvent();

			if (event > now)
			{
				continue;
			}

			std::size_t position = count;

			while (position > 0 && m_Slots[order[position - 1]]->NextEvent() > event)
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

	// `optional` rather than a vector of pointers, because a presenter is neither copyable nor movable
	// and this is the storage that needs neither: `emplace` constructs in place, and the slots outlive
	// every reference handed out by `Add`.
	std::array<std::optional<HeadlessOutput>, MaxHeadlessOutputs> m_Slots{};
	std::size_t m_Count = 0;
};
