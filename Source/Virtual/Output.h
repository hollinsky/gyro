#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "Core/Clock.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Headless/Vblank.h"
#include "Seam/Allocator.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Seam/SyncPoint.h"
#include "Virtual/Buffer.h"

// An output whose glass is somebody else's problem: a file, an encoder, a test.
//
// It is a real `IPresenter` rather than a fixture, and Seam/RenderTarget.h is where that stops being
// an ambition — *a virtual output renders into dmabufs a client owns and a local output renders into
// dmabufs gyro allocated; only the source of the constraint differs*. This is the case where the
// consumer imposes no constraint at all, which makes it the degenerate one rather than a new kind.
// See Docs/Decisions.md decision 102.
//
// **The ring retires on release, and that is the one thing it does not share with the headless
// panel.** A headless target is held until the *next* flip, because a plane is scanning it out; that
// is the bound Frame/Loop.h leans on when it declines to treat a refused `AcquireTarget` as an
// error. Here the target is held until the consumer says it is done — a writer has finished writing,
// an encoder has finished reading — which is precisely the backpressure Seam/Presenter.h describes
// as the ordinary reason `AcquireTarget` answers nothing. A consumer that is done inside the
// notification releases immediately and the ring behaves like any other; one that holds two images
// stalls the output, and stalling is the correct answer rather than a dropped frame, because the
// frame loop skips an output it cannot acquire for.
//
// **There is one plane and there is no catalog.** Headless/Planes.h simulates a controller that
// might refuse; this is a backend that genuinely has a single surface to fill, so a layer set with a
// second layer in it is `EINVAL` — Seam/Presenter.h's *the layers cannot be expressed*, which is a
// bug in the assigner rather than a condition to retry. Reaching for the synthetic catalog would be
// modelling hardware this does not have.
//
// **The `IEventSource` is Virtual/Device.h's and not this file's.** Seam/EventSource.h puts the
// descriptor at *device* granularity, so it belongs to the party that owns the set rather than to
// one output — which is why `Advance` and `NextEvent` are public here. *(Revised 2026-08-22.)* This
// paragraph used to say the source was unwritten, and named the trigger for writing it as the frame
// loop driving one of these for real; an integration test that publishes a scene and reads the
// pixels back is that, so the device exists.

// What a consumer's pipeline depth wants, not what a panel's flip queue forces. Three rather than
// headless's two: a recording should not stall the compositor because the writer is one frame
// behind, and two is what makes stalling the *only* behaviour. A test that wants the stall says two.
inline constexpr std::uint32_t DefaultVirtualTargets = 3;

// The ring's ceiling, matching Headless/Output.h's for the reason that file gives — a flip queue past
// four is headroom nobody has needed.
// Which face of its images an output shows at the render seam.
//
// **Both are true of the same memory, and the composition root is the party that knows which to
// ask for.** Virtual/Buffer.h has the full argument: a `udmabuf` image is a descriptor a Vulkan
// device imports *and* a mapping a CPU writes, and Seam/RenderTarget.h's discriminated memory makes
// a renderer branch on which it was handed rather than cast. Decision 79 puts a CPU blitter in front
// of the boot console, so both renderers are real and neither is the default in a way the other can
// live with.
//
// It sits on the presenter rather than being negotiated at the seam because the alternative is worse
// in the direction the seam exists to protect: a third memory alternative carrying both would widen
// the waist for one backend and give every renderer a case to write. Docs/Structure.md#orchestration
// already has the root constructing the renderer and the presenter together — it knows which it
// built, and `Frame` still never finds out.
enum class TargetFace : std::uint8_t
{
	// What a Vulkan device imports. The descriptor, its offset, and its stride.
	Dmabuf,

	// What a blitter writes. The mapping's address, its stride, and its true length.
	Mapped,
};

struct VirtualOutputPolicy
{
	// How many images the ring holds. Clamped into `[1, MaxTargets]`.
	std::uint32_t Targets = DefaultVirtualTargets;

	// Where the first frame boundary sits relative to construction. A recording usually wants zero;
	// it is here because the cadence is a timeline like any other and a phase is what a timeline
	// takes.
	Duration Phase{};

	// What the format falls back to when the configuration names none. `XR24` linear, which is what
	// the allocator can produce and what lavapipe imports.
	PixelFormat Fallback{ FormatXrgb8888, 0, ModifierLinear };

	// Which description the targets carry. Dmabuf by default, because that is the renderer decision
	// 102 stood this module up for; a `Mapped` set over an allocator that cannot map is a target set
	// that fails to build, reported through `Status()` like any other allocation failure.
	TargetFace Face = TargetFace::Dmabuf;
};

// A frame that has been handed to the consumer and not yet released.
struct VirtualFrame
{
	// Index into `Targets()`, which is also what `Release` takes.
	std::uint32_t Target = 0;

	// The frame boundary this landed on, from the same timeline `Presented` reported.
	std::uint64_t Sequence = 0;

	Instant At{};

	// What the composite finishes at, carried through from the layer that was presented.
	//
	// **A virtual output cannot honour this itself, and saying so is the point of the field.** On a
	// KMS output the acquire fence goes to the kernel as an in-fence and the atomic commit does the
	// waiting; there is no kernel on this path, so a consumer that memcpys the pixels the moment
	// `Presented` fires reads an image the GPU is still writing. Nothing here can wait for it either:
	// `IRenderer::IsComplete` is the seam's poll and the renderer is the party that holds the
	// timeline, which a presenter deliberately does not.
	//
	// So the obligation travels with the frame. Virtual/Device.h is what discharges it — it holds the
	// renderer and declines to deliver a frame whose point is outstanding — and a consumer reached
	// any other way owes itself the same check. Immediate is the common answer today, because
	// decision 108 has a device that cannot export a timeline finish inside `Record`.
	SyncPoint Acquire{};

	friend constexpr bool operator==(VirtualFrame, VirtualFrame) noexcept = default;
};

class VirtualOutput final : public IPresenter
{
public:
	// The allocator is a reference rather than a value because `IDmabufAllocator` is neither copyable
	// nor movable, and because one provider serves every output on a machine — it is the device, and
	// the composition root owns it exactly as it owns a renderer's.
	//
	// Allocation happens here and it can fail. A failed one leaves the target set *empty*, which the
	// frame loop already tolerates as the state between `TargetsInvalidated` and `Reconfigured`, and
	// `Status()` is where the reason lives. Throwing or aborting would make a machine with no
	// `/dev/udmabuf` a startup crash rather than an output that cannot be brought up.
	VirtualOutput(
		const IClock& clock,
		IDmabufAllocator& allocator,
		const OutputConfiguration& configuration,
		VirtualOutputPolicy policy = {}
	)
		: m_Clock{ &clock }, m_Allocator{ &allocator }, m_Policy{ policy }
	{
		m_Policy.Targets = std::clamp(m_Policy.Targets, 1U, MaxTargets);
		Adopt(configuration, clock.Now());
	}

	[[nodiscard]] std::span<const RenderTarget> Targets() const override
	{
		return { m_Descriptions.data(), m_TargetCount };
	}

	[[nodiscard]] std::optional<std::uint32_t> AcquireTarget() override
	{
		for (std::uint32_t offset = 0; offset < m_TargetCount; ++offset)
		{
			const std::uint32_t candidate = (m_Next + offset) % m_TargetCount;

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
			return Failure(EINVAL, "present on an unpowered virtual output");
		}

		if (m_Transition != Transition::None)
		{
			return Failure(EBUSY, "virtual output is mid-reconfiguration");
		}

		if (m_Pending)
		{
			return Failure(EBUSY, "a frame is already queued on this virtual output");
		}

		// One surface, so one layer. Stated as a refusal rather than by compositing the rest, because
		// an assigner that produced two here would otherwise be silently ignored on this backend and
		// discovered on the one with planes.
		if (layers.size() != 1)
		{
			return Failure(EINVAL, "a virtual output takes exactly one layer");
		}

		const PresentLayer& layer = layers.front();

		// A virtual output's consumer is a file or an encoder, and there is nothing here that scans a
		// client's buffer out — so a promotion is refused by name rather than resolved to target zero.
		if (layer.Target.IsTexture())
		{
			return Failure(EINVAL, "a virtual output has no scanout for a promoted texture");
		}

		if (layer.Target.Index >= m_TargetCount || m_State[layer.Target.Index] != TargetState::Acquired)
		{
			return Failure(EINVAL, "layer names a target that was not acquired");
		}

		m_State[layer.Target.Index] = TargetState::Queued;
		m_Queued = layer;
		m_Pending = true;
		m_Sequence = m_Timeline.Latch(m_Clock->Now(), Duration::zero());
		++Commits;

		return {};
	}

	void Reconfigure(const OutputConfiguration& wanted) override
	{
		m_Wanted = wanted;
		m_Transition = Transition::Requested;
	}

	// Everything this output has to say at `now`.
	//
	// Public where Headless/Output.h's is private, because there is no device here to be its friend.
	// The obligation that comes with that is Seam/Presenter.h's and is unchanged: the three signals
	// emit from here, so whoever calls this is the frame thread.
	void Advance(Instant now)
	{
		if (m_Transition == Transition::Requested)
		{
			// The images go before the new set exists, which is the order
			// `IRenderer::ReleaseTargets` is written for: the descriptors in the old set are this
			// presenter's and are about to stop being valid.
			m_Transition = Transition::Invalidated;
			Drop();
			TargetsInvalidated.Emit();
		}

		// Adopted in the same drain, and the absence of a latency here is deliberate. Headless/Output.h
		// carries one because a simulated *panel* must exercise the asynchrony a mode set really has —
		// a backend that reconfigured inline would be the one implementation of this seam whose
		// transition nothing walks. There is no hardware to program here, so a wait would be a
		// simulation in a backend that is not simulating anything. What is preserved is the part that
		// is a contract rather than a duration: `Reconfigure` returns without having done it, the
		// images are released before the new set exists, and both completions arrive on a drain.
		if (m_Transition == Transition::Invalidated)
		{
			m_Transition = Transition::None;
			Adopt(m_Wanted, now);
			Reconfigured.Emit(m_Configuration);

			return;
		}

		if (!m_Pending || now < m_Timeline.At(m_Sequence))
		{
			return;
		}

		m_Pending = false;
		m_State[m_Queued.Target.Index] = TargetState::Held;
		m_Presented = VirtualFrame{ .Target = m_Queued.Target.Index,
			                        .Sequence = m_Sequence,
			                        .At = m_Timeline.At(m_Sequence),
			                        .Acquire = m_Queued.Acquire };

		Presented.Emit(
			{ .PresentedAt = m_Timeline.At(m_Sequence),
		      .Period = m_Timeline.IntervalBefore(m_Sequence),
		      .Sequence = m_Sequence,
		      .Vsync = true,
		      // False, and Seam/PresentationInfo.h asks for exactly this honesty: a
		      // virtual output's clock is flow control, so a `FrameClock` built on it must
		      // not report a precise prediction beside a panel that has one.
		      .HardwareClock = false,
		      // False for the same reason: nothing with a view of a display signalled this.
		      .HardwareCompletion = false,
		      // Nothing downstream put this on a plane; a consumer read it out of memory.
		      .ZeroCopy = false }
		);
	}

	// When this output next has something to say. `Duration::max()`'s instant where it is quiet, which
	// is the identity of a fold rather than a case to test.
	[[nodiscard]] Instant NextEvent() const noexcept
	{
		if (m_Transition != Transition::None)
		{
			return m_Clock->Now();
		}

		return m_Pending ? m_Timeline.At(m_Sequence) : Instant{ Duration::max() };
	}

	// The frame the consumer has been handed and has not given back, if there is one.
	[[nodiscard]] std::optional<VirtualFrame> PresentedFrame() const noexcept { return m_Presented; }

	// Give a frame back. Idempotent against a target that is not held, because the failure mode worth
	// avoiding is a consumer that releases twice quietly corrupting the ring — freeing an image that
	// has since been reacquired is how a frame gets drawn into while it is being read.
	void Release(std::uint32_t target) noexcept
	{
		if (target >= m_TargetCount || m_State[target] != TargetState::Held)
		{
			return;
		}

		m_State[target] = TargetState::Free;

		if (m_Presented && m_Presented->Target == target)
		{
			m_Presented.reset();
		}
	}

	// The buffer behind a target index, for the consumer that wants the pixels. Null past the set.
	//
	// It hands out the owner rather than the description because the description deliberately carries
	// no mapping — Seam/RenderTarget.h's dmabuf variant is descriptors and strides, which is all a
	// renderer needs, and the consumer needs the thing that can be read.
	[[nodiscard]] const DmabufBuffer* Buffer(std::uint32_t target) const noexcept
	{
		return target < m_TargetCount ? &m_Buffers[target] : nullptr;
	}

	[[nodiscard]] const OutputConfiguration& Configuration() const noexcept { return m_Configuration; }

	[[nodiscard]] const VblankTimeline& Timeline() const noexcept { return m_Timeline; }

	// Why the target set is empty, where it is. Success once a set has been built, and the allocator's
	// own error otherwise — `EACCES` on a machine with no access to `/dev/udmabuf` being the one a
	// reader will actually meet.
	[[nodiscard]] const Result<void>& Status() const noexcept { return m_Status; }

	// How many images are free to draw into, without taking one. An observation rather than a
	// sequence of `AcquireTarget` calls, for Headless/Output.h's reason: acquiring to find out changes
	// the answer.
	[[nodiscard]] std::uint32_t FreeTargets() const noexcept
	{
		std::uint32_t free = 0;

		for (std::uint32_t index = 0; index < m_TargetCount; ++index)
		{
			free += m_State[index] == TargetState::Free ? 1U : 0U;
		}

		return free;
	}

	[[nodiscard]] bool IsFramePending() const noexcept { return m_Pending; }

	// How many commits this output has accepted.
	std::uint64_t Commits = 0;

private:
	enum class TargetState : std::uint8_t
	{
		Free,
		Acquired,
		Queued,
		Held,
	};

	enum class Transition : std::uint8_t
	{
		None,
		Requested,
		Invalidated,
	};

	// Take up a configuration and allocate the images for it. Unbounded and allocating, which is what
	// this path is permitted to be: it runs at a transition and never inside the frame section.
	void Adopt(const OutputConfiguration& configuration, Instant now)
	{
		m_Configuration = configuration;
		m_Timeline.Configure(Advanced(now, m_Policy.Phase), configuration.Period);

		Drop();

		if (!configuration.Powered || configuration.Resolution.IsEmpty())
		{
			// Not a failure. An unpowered output has no images by design, and reporting the last
			// allocation error here would make a deliberate power-down look like a broken allocator.
			m_Status = {};

			return;
		}

		const PixelFormat format = configuration.Format.IsValid() ? configuration.Format : m_Policy.Fallback;

		for (std::uint32_t index = 0; index < m_Policy.Targets; ++index)
		{
			const std::uint64_t modifier = format.Modifier;
			Result<DmabufBuffer> buffer =
				m_Allocator->Allocate(configuration.Resolution, format.Code, { &modifier, 1 });

			if (!buffer)
			{
				// Partial success is not expressible, for `IRenderer::BindTargets`' reason one seam
				// over: a set is what `AcquireTarget` indexes into, so half of one is a numbering
				// with holes rather than a smaller set.
				m_Status = std::unexpected{ buffer.error() };
				Drop();

				return;
			}

			const RenderTarget description =
				m_Policy.Face == TargetFace::Mapped ? buffer->DescribeMapped() : buffer->Describe();

			if (!description.IsValid())
			{
				// The only way here is a `Mapped` set over an allocator that hands out no mapping,
				// which is a miswired composition root rather than a transient condition — reported
				// as an empty target set for the same reason a refused allocation is, so that a
				// machine whose provider cannot map comes up with an output it cannot draw rather
				// than aborting before anything reaches the screen.
				m_Status = Failure(ENODEV, "this allocator cannot describe a mapped target");
				Drop();

				return;
			}

			m_Descriptions[index] = description;
			m_Buffers[index] = std::move(*buffer);
			m_State[index] = TargetState::Free;
		}

		m_TargetCount = m_Policy.Targets;
		m_Status = {};
	}

	// Release the images and reset the ring. The buffers close their descriptors here, which is what
	// makes `TargetsInvalidated` a real invalidation rather than a hint.
	void Drop() noexcept
	{
		for (std::uint32_t index = 0; index < m_TargetCount; ++index)
		{
			m_Buffers[index] = DmabufBuffer{};
			m_Descriptions[index] = RenderTarget{};
		}

		m_TargetCount = 0;
		m_Next = 0;
		m_Pending = false;
		m_Presented.reset();
		m_State.fill(TargetState::Free);
	}

	const IClock* m_Clock = nullptr;
	IDmabufAllocator* m_Allocator = nullptr;
	VirtualOutputPolicy m_Policy{};

	OutputConfiguration m_Configuration{};
	VblankTimeline m_Timeline{};
	Result<void> m_Status{};

	std::array<DmabufBuffer, MaxTargets> m_Buffers{};
	std::array<RenderTarget, MaxTargets> m_Descriptions{};
	std::array<TargetState, MaxTargets> m_State{};
	std::uint32_t m_TargetCount = 0;
	std::uint32_t m_Next = 0;

	PresentLayer m_Queued{};
	bool m_Pending = false;
	std::uint64_t m_Sequence = 0;
	std::optional<VirtualFrame> m_Presented{};

	Transition m_Transition = Transition::None;
	OutputConfiguration m_Wanted{};
};
