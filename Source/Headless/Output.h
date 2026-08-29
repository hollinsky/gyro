#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Core/Clock.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Geometry/Space.h"
#include "Headless/Planes.h"
#include "Headless/Vblank.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/PresentationInfo.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"

// One simulated output: an `IPresenter` whose glass is a buffer and whose vblanks are arithmetic.
//
// **It is a real implementation of the seam rather than a test double, and the difference is what the
// module is for.** Frame/Loop.Test.cpp's `FakePresenter` is a double — it flips when a test says
// `Flip()`, holds a target per present, and models nothing. This one is told a period and a phase and
// works out the rest, which is what lets Docs/Architecture.md#outputs-are-independent-periodic-tasks'
// claim be swept rather than asserted at one arrangement, and what makes an injected miss a property
// of the panel rather than of the test that wanted it.
//
// **The target ring is the hardware's, not a counter.** A buffer on a plane is held until the *next*
// flip retires it, so a double-buffered output has no free target between its present and its flip and
// therefore cannot run ahead at all — which is the bound Frame/Loop.h names when it declines to treat
// a refused `AcquireTarget` as an error, and which decision 30's speculative early rendering is
// measured against. Modelling that as "one free per present" would make the lead unbounded in the one
// place it is supposed to be pinned.
//
// **Reconfiguration is initiated here and performed later, which is decision 73 in a backend.** The
// verb records and returns; the transition completes on a drain some milliseconds later, and until it
// does the output holds its last frame and refuses commits with `EBUSY`. A headless backend that
// reconfigured inline would be the one implementation of this seam whose asynchrony nothing exercises,
// and the DRM backend would be where the ordering is discovered.

// Modes this simulated panel will accept. An empty list means it accepts whatever it is asked for,
// which is the ordinary sweep case; a populated one is how a request the hardware could not honour
// becomes testable, since Seam/OutputConfiguration.h has no failure signal and reports that by the
// achieved configuration differing from the wanted one.
inline constexpr std::size_t MaxModes = 8;

struct OutputMode
{
	PixelSize<DeviceSpace> Resolution{};
	Duration Period{};

	friend constexpr bool operator==(OutputMode, OutputMode) noexcept = default;
};

struct HeadlessOutputPolicy
{
	// How many images the presenter owns. Two is the case that pins the lead; see the header.
	std::uint32_t Targets = 2;

	// What the panel *actually* runs at, as an offset from whatever period it was configured to. Real
	// panels do not run at nominal rates — Docs/Architecture.md#phase-drift-is-not-something-to-track
	// opens on that observation — and this is the one knob that makes a simulated one honest about it.
	Duration PeriodError{};

	// Where vblank zero sits relative to the instant the output was configured. This is the phase, and
	// it is the parameter a schedulability sweep varies.
	Duration Phase{};

	// How early a commit must be programmed to make the vblank it is aiming at.
	//
	// Zero by default because nothing sits between gyro and the presentation on a simulated panel. It
	// is settable because a non-zero lead is how a sweep asks what a real one costs, and `Resolve`
	// reports it as the achieved configuration's `LatchLead` so the frame clock predicts against the
	// same number this output enforces.
	Duration LatchLead{};

	// How long a transition takes to complete. Docs/Architecture.md puts a mode set and a DPMS
	// transition in the same class — on the order of 100 ms — which is the class defined by what may
	// not happen inside the frame thread's non-preemptible chunk.
	Duration ModesetLatency{};

	// Whether the timestamps this output reports come from something with a view of a display.
	//
	// True for a simulated panel, where the flip instant is exact by construction. **False is what a
	// virtual output reports**, and Seam/PresentationInfo.h is explicit that this is per output rather
	// than per machine: a virtual output's clock is flow control and is never precise while the panel
	// beside it is. It gates `FrameClock::IsPrecise`, and a backend that filled it in optimistically
	// would produce a schedule with no error bar.
	bool HardwareClock = true;

	// Whether the presented buffer reached a plane rather than being composited by something
	// downstream. False here: a headless output composites into its own memory and there is no
	// downstream to be honest about.
	bool ZeroCopy = false;
};

class HeadlessOutput final : public IPresenter
{
public:
	// The clock is held rather than passed per call because `Reconfigure` has to know when it was asked,
	// and it is the only verb on this seam with no instant of its own. Core/Clock.h's rule is satisfied:
	// this is a subsystem handed a clock by the composition root, not one reaching for an ambient now.
	HeadlessOutput(const IClock& clock, const OutputConfiguration& configuration, HeadlessOutputPolicy policy = {})
		: m_Clock{ &clock }, m_Policy{ policy }
	{
		m_Policy.Targets = std::clamp(m_Policy.Targets, 1U, MaxTargets);
		m_Catalog = PlaneCatalog::Typical(configuration.Resolution);
		Adopt(configuration, clock.Now());
	}

	[[nodiscard]] std::span<const RenderTarget> Targets() const override { return { m_Targets.data(), m_TargetCount }; }

	[[nodiscard]] std::optional<std::uint32_t> AcquireTarget() override
	{
		for (std::uint32_t index = 0; index < m_TargetCount; ++index)
		{
			const std::uint32_t candidate = (m_Next + index) % m_TargetCount;

			if (m_State[candidate] == TargetState::Free)
			{
				m_State[candidate] = TargetState::Acquired;
				m_Next = (candidate + 1) % m_TargetCount;

				return candidate;
			}
		}

		return std::nullopt;
	}

	using IPresenter::Present;

	Result<void> Present(std::span<const PresentLayer> layers, PresentTrace) override
	{
		if (!m_Configuration.Powered)
		{
			return Failure(EINVAL, "present on an unpowered output");
		}

		if (m_Transition != Transition::None)
		{
			return Failure(EBUSY, "output is mid-reconfiguration");
		}

		// KMS refuses a second nonblocking commit on a CRTC that has not flipped, and so does this. The
		// frame loop's own `IsFlipPending` guard should make it unreachable, which is exactly why it is
		// worth stating at the seam: the redundant check is what catches the day the guard moves.
		if (m_FlipPending)
		{
			return Failure(EBUSY, "a flip is already queued on this output");
		}

		if (const Result<void> expressible = m_Catalog.Test(layers, Targets()); !expressible)
		{
			return expressible;
		}

		for (const PresentLayer& layer : layers)
		{
			if (m_State[layer.Target.Index] != TargetState::Acquired)
			{
				return Failure(EINVAL, "layer names a target that was not acquired");
			}
		}

		for (const PresentLayer& layer : layers)
		{
			m_State[layer.Target.Index] = TargetState::Queued;
		}

		m_Queued = layers.size();

		for (std::size_t index = 0; index < layers.size(); ++index)
		{
			m_QueuedLayers[index] = layers[index];
		}

		m_FlipPending = true;
		m_FlipSequence = m_Vblank.Latch(m_Clock->Now(), m_Policy.LatchLead);
		++Commits;

		return {};
	}

	void Reconfigure(const OutputConfiguration& wanted) override
	{
		m_Wanted = wanted;
		m_Transition = Transition::Requested;
		m_TransitionAt = Advanced(m_Clock->Now(), m_Policy.ModesetLatency);
	}

	// What is on the glass, which is what a golden-image test reads and what a frame dump writes. Empty
	// where nothing has flipped yet, which is a black screen rather than an error.
	[[nodiscard]] std::span<const std::byte> Scanout() const noexcept
	{
		if (m_Scanout >= m_TargetCount)
		{
			return {};
		}

		const MappedImage* image = m_Targets[m_Scanout].AsMapped();

		return image != nullptr ? std::span<const std::byte>{ image->Pixels, image->Length } :
		                          std::span<const std::byte>{};
	}

	[[nodiscard]] const OutputConfiguration& Configuration() const noexcept { return m_Configuration; }

	[[nodiscard]] const VblankTimeline& Vblank() const noexcept { return m_Vblank; }

	[[nodiscard]] PlaneCatalog& Catalog() noexcept { return m_Catalog; }

	// The modes this panel will accept, and the variable-refresh window it will report once one is set.
	// Both are what a transition is resolved against; see `Resolve`.
	void SetModes(std::span<const OutputMode> modes) noexcept
	{
		m_ModeCount = std::min(modes.size(), MaxModes);

		for (std::size_t index = 0; index < m_ModeCount; ++index)
		{
			m_Modes[index] = modes[index];
		}
	}

	void SetRefreshRange(VariableRefresh range) noexcept { m_PanelRange = range; }

	// What the last accepted commit put on which planes, for a test that wants to assert the assignment
	// rather than the pixels.
	[[nodiscard]] std::span<const PresentLayer> QueuedLayers() const noexcept
	{
		return { m_QueuedLayers.data(), m_Queued };
	}

	[[nodiscard]] bool IsFlipPending() const noexcept { return m_FlipPending; }

	// How many images are free to draw into, without taking one.
	//
	// It is the ring's whole state in one number, and it is worth having as an observation rather than
	// as a sequence of `AcquireTarget` calls: acquiring to find out changes the answer, which is how a
	// test asserting the bound ends up asserting its own side effects.
	[[nodiscard]] std::uint32_t FreeTargets() const noexcept
	{
		std::uint32_t free = 0;

		for (std::uint32_t index = 0; index < m_TargetCount; ++index)
		{
			free += m_State[index] == TargetState::Free ? 1U : 0U;
		}

		return free;
	}

	// How many commits this output has accepted, which is what a sweep divides misses by.
	std::uint64_t Commits = 0;

private:
	friend class HeadlessDevice;

	enum class TargetState : std::uint8_t
	{
		Free,
		Acquired,
		Queued,
		Scanout,
	};

	enum class Transition : std::uint8_t
	{
		None,
		Requested,
		Invalidated,
	};

	// Everything this output has to say at `now`, emitted in the order a driver would produce it.
	//
	// Called from the device's `Drain` and therefore on the frame thread, which is what Seam/Presenter.h
	// requires of all three signals and the reason none of them is emitted from the verb that caused
	// them.
	void Advance(Instant now)
	{
		if (m_Transition == Transition::Requested)
		{
			// The images go before the new set exists, which is the order Seam/IRenderer::ReleaseTargets
			// is written for: the descriptors in the old set are the presenter's and are about to stop
			// being valid.
			m_Transition = Transition::Invalidated;
			ReleaseTargets();
			TargetsInvalidated.Emit();
		}

		if (m_Transition == Transition::Invalidated && now >= m_TransitionAt)
		{
			m_Transition = Transition::None;
			Adopt(Resolve(m_Wanted), now);
			Reconfigured.Emit(m_Configuration);
		}

		if (!m_FlipPending || now < m_Vblank.At(m_FlipSequence))
		{
			return;
		}

		Retire();

		Presented.Emit(
			{ .PresentedAt = m_Vblank.At(m_FlipSequence),
		      .Period = m_Vblank.IntervalBefore(m_FlipSequence),
		      .Sequence = m_FlipSequence,
		      .Vsync = true,
		      .HardwareClock = m_Policy.HardwareClock,
		      .ZeroCopy = m_Policy.ZeroCopy }
		);
	}

	// When this output next has something to say, so that the device can fold it into the wake it owes.
	// `Unscheduled` where it is quiet, which is the identity of that fold rather than a case to test.
	[[nodiscard]] Instant NextEvent() const noexcept
	{
		if (m_Transition == Transition::Requested)
		{
			return m_Clock->Now();
		}

		if (m_Transition == Transition::Invalidated)
		{
			return m_TransitionAt;
		}

		return m_FlipPending ? m_Vblank.At(m_FlipSequence) : Instant{ Duration::max() };
	}

	// The flip lands: what was on the glass is free, and what was queued is on the glass.
	void Retire() noexcept
	{
		m_FlipPending = false;

		for (std::uint32_t index = 0; index < m_TargetCount; ++index)
		{
			if (m_State[index] == TargetState::Scanout)
			{
				m_State[index] = TargetState::Free;
			}
		}

		for (std::size_t index = 0; index < m_Queued; ++index)
		{
			const std::uint32_t target = m_QueuedLayers[index].Target.Index;

			if (target < m_TargetCount)
			{
				m_State[target] = TargetState::Scanout;
				m_Scanout = target;
			}
		}
	}

	// What this panel will actually do with a request. An empty mode list honours anything; a populated
	// one falls back to its first entry, which is a fallback nobody asked for and is precisely what
	// `OutputConfiguration::SatisfiedBy` exists to catch.
	[[nodiscard]] OutputConfiguration Resolve(const OutputConfiguration& wanted) const noexcept
	{
		OutputConfiguration achieved = wanted;

		if (m_ModeCount > 0)
		{
			const OutputMode asked{ wanted.Resolution, wanted.Period };
			const bool supported =
				std::find(m_Modes.begin(), m_Modes.begin() + static_cast<std::ptrdiff_t>(m_ModeCount), asked) !=
				m_Modes.begin() + static_cast<std::ptrdiff_t>(m_ModeCount);

			if (!supported)
			{
				achieved.Resolution = m_Modes[0].Resolution;
				achieved.Period = m_Modes[0].Period;
			}
		}

		// Learned rather than requested, per Seam/OutputConfiguration.h: the range is a field the
		// achieved configuration fills in and the wanted one leaves alone, and a panel with no range to
		// report says so by leaving variable refresh off however it was asked.
		achieved.Refresh = wanted.Refresh.Enabled && m_PanelRange.Enabled ? m_PanelRange : VariableRefresh{};

		// The simulated hardware's own requirement, reported the same way and for the same reason. It
		// was already the number `Latch` holds a commit against, and until it crossed here the sweep was
		// injecting a miss the frame clock had no way to have predicted — which made the one instrument
		// for *correct scheduling still misses* indistinguishable from a scheduler that was simply wrong.
		achieved.LatchLead = m_Policy.LatchLead;

		return achieved;
	}

	// Take up a configuration and build the images for it. Allocating and unbounded, which is what this
	// path is permitted to be: it runs at a transition and never inside the frame section.
	void Adopt(const OutputConfiguration& configuration, Instant now)
	{
		m_Configuration = configuration;
		m_Vblank.Configure(Advanced(now, m_Policy.Phase), configuration.Period + m_Policy.PeriodError);
		m_Catalog = PlaneCatalog::Typical(configuration.Resolution);

		ReleaseTargets();

		if (!configuration.Powered || configuration.Resolution.IsEmpty())
		{
			return;
		}

		const auto width = static_cast<std::size_t>(configuration.Resolution.Width);
		const auto height = static_cast<std::size_t>(configuration.Resolution.Height);
		const std::size_t stride = width * BytesPerPixel;

		m_TargetCount = m_Policy.Targets;

		for (std::uint32_t index = 0; index < m_TargetCount; ++index)
		{
			m_Storage[index].assign(stride * height, std::byte{});
			m_Targets[index] = RenderTarget{
				.Size = configuration.Resolution,
				.Format = configuration.Format.IsValid() ? configuration.Format :
				                                           PixelFormat{ FormatXrgb8888, 0, ModifierLinear },
				.Memory = MappedImage{ .Pixels = m_Storage[index].data(),
				                       .Stride = static_cast<std::uint32_t>(stride),
				                       .Length = m_Storage[index].size() },
			};
			m_State[index] = TargetState::Free;
		}
	}

	void ReleaseTargets() noexcept
	{
		m_TargetCount = 0;
		m_Next = 0;
		m_Scanout = MaxTargets;
		m_Queued = 0;
		m_FlipPending = false;
		m_State.fill(TargetState::Free);
	}

	// XR24 and AR24 are both four, and this backend advertises nothing else. A catalog of strides
	// belongs to whichever backend negotiates one, which this does not.
	static constexpr std::size_t BytesPerPixel = 4;

	const IClock* m_Clock = nullptr;
	HeadlessOutputPolicy m_Policy{};

	OutputConfiguration m_Configuration{};
	VblankTimeline m_Vblank{};
	PlaneCatalog m_Catalog{};

	std::array<std::vector<std::byte>, MaxTargets> m_Storage{};
	std::array<RenderTarget, MaxTargets> m_Targets{};
	std::array<TargetState, MaxTargets> m_State{};
	std::uint32_t m_TargetCount = 0;
	std::uint32_t m_Next = 0;
	std::uint32_t m_Scanout = MaxTargets;

	std::array<PresentLayer, MaxPlanes> m_QueuedLayers{};
	std::size_t m_Queued = 0;

	bool m_FlipPending = false;
	std::uint64_t m_FlipSequence = 0;

	Transition m_Transition = Transition::None;
	OutputConfiguration m_Wanted{};
	Instant m_TransitionAt{};

	std::array<OutputMode, MaxModes> m_Modes{};
	std::size_t m_ModeCount = 0;
	VariableRefresh m_PanelRange{};
};
