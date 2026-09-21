#define _POSIX_C_SOURCE 200809L

#include "Drm/Output.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <spdlog/spdlog.h>
#include <sys/ioctl.h>
#include <xf86drm.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

#include "Core/Clock.h"
#include "Core/Trace.h"

namespace Drm
{
namespace
{
// What the driver's own commit path costs on top of the mode's blanking interval: the time from the
// composite's fence signalling to the register write that arms the flip, plus the guard the driver
// keeps clear of the vblank it is writing before.
//
// **Measured rather than read**, which is the one figure in this file that is. Across five modes of
// two panels on one i915 — blanking from 303 to 806 microseconds, composites from 0.7 to 2.1
// milliseconds, twenty-five thousand commits — the latch threshold sits at the blanking interval plus
// 288 to 387 microseconds, with no dependence on the connector or the mode. i915's own
// `VBLANK_EVASION_TIME_US` is 100 of those and the rest is the commit worker being scheduled, which is
// why this is a figure and not a constant of the hardware.
//
// **Half a millisecond rather than the 387 that was seen**, because the two directions are not the
// same shape. The part that is scheduling latency grows under load, and below the threshold the
// failure is total — every commit measured under it missed, in every mode — while above it the cost is
// one microsecond of pointer lag per microsecond of overstatement. Being early is linear; being late
// is a cliff. It wants to become a ratchet that starts here and steps up on a miss, which is
// Docs/Open.md's entry rather than this line.
constexpr Duration CommitPath = std::chrono::microseconds{ 500 };
} // namespace

namespace
{
// How often a held commit asks to be looked at again. Nested/Output.cpp's figure and its reasoning:
// nothing becomes readable when the GPU finishes, so this path polls, and half a millisecond is short
// against a refresh and long against the cost of asking.
constexpr Duration CompletionPoll = std::chrono::microseconds{ 500 };

// How long to wait on a commit before looking at it unprompted, where the output has no period to
// measure against — which is only ever an output that has not finished coming up. A real panel's
// period is what the backstop is actually scaled to; see `NextEvent`.
constexpr Duration CommitBackstop = std::chrono::milliseconds{ 50 };

// SPEC: how often a panel changing power is looked at while its commit is in the kernel.
//
// **A poll, for the held path's reason: nothing becomes readable when a blocking modeset returns.** A
// power change asks for no page-flip event, so the commit thread finishing is the only completion there
// is. The flip backstop cannot stand in for this — it is measured from when the commit was armed, so
// once a panel has taken longer than two refreshes to light, which is the ordinary case, it names an
// instant already past and the loop would spin on the `SCHED_FIFO` thread until the ioctl came back. Ten
// milliseconds against a transition Architecture.md puts near a hundred is a handful of looks per
// transition, and a person does not see ten milliseconds on a screen that is coming on.
constexpr Duration PowerPoll = std::chrono::milliseconds{ 10 };

// SRC_X and friends are 16.16 fixed point; CRTC_X and friends are whole pixels.
[[nodiscard]] constexpr std::uint64_t Fixed(std::int64_t pixels) noexcept
{
	return static_cast<std::uint64_t>(pixels) << 16;
}

// The same conversion from the real-valued side, which is what a source rectangle actually is.
//
// **Rounded rather than truncated, and clamped at zero.** Seam/Presenter.h keeps a layer's source in
// floats precisely because a crop samples between texels, so the sixteen fractional bits are the point
// rather than an artefact — truncating them would move a promoted layer up and left by up to a texel
// against the composite that was drawn around it, which reads as a one-pixel seam that appears the
// frame a window is promoted and vanishes the frame it is not.
[[nodiscard]] inline std::uint64_t Fixed(float pixels) noexcept
{
	const float scaled = std::round(pixels * 65536.0F);

	return scaled <= 0.0F ? 0 : static_cast<std::uint64_t>(scaled);
}

// The one interruptible-retry wrapper this module needs; a DRM ioctl is interruptible and gyro
// installs handlers for SIGINT and SIGTERM, so a signal at shutdown is the ordinary case.
[[nodiscard]] int Ioctl(RawFd device, unsigned long request, void* argument) noexcept
{
	int result = 0;

	do
	{
		result = ::ioctl(device.Value, request, argument);
	} while (result != 0 && errno == EINTR);

	return result;
}
} // namespace

DrmOutput::DrmOutput(
	DrmDevice& device,
	const Pipeline& pipeline,
	IDmabufAllocator& allocator,
	const IRenderer* completion,
	std::uint32_t targets
)
	: m_Device{ &device }, m_Pipeline{ &pipeline }, m_Allocator{ &allocator }, m_Completion{ completion },
	  m_Crtc{ pipeline.Crtc }, m_Fences{ device.Descriptor() },
	  m_Wanted{ std::clamp(targets, std::uint32_t{ 2 }, MaxTargets) }
{}

DrmOutput::~DrmOutput()
{
	// **First, and by hand rather than by member order.** A commit still in the kernel has the panel
	// scanning out a framebuffer `DropTargets` below is about to remove, and the request it is reading
	// points into this object. Joining here is a wait of at most one refresh, on the teardown path.
	m_Commit.Stop();

	if (m_Device != nullptr)
	{
		m_Device->Detach(m_Crtc);

		// This panel has stopped reading, and everything it was the last reader of is free now.
		m_Device->Scanout().Detach(*this);
	}

	DropTargets();

	if (m_ModeBlob != 0 && m_Device != nullptr)
	{
		::drmModeDestroyPropertyBlob(m_Device->Descriptor().Value, m_ModeBlob);
	}
}

Result<void> DrmOutput::Open(const OutputConfiguration& wanted)
{
	const drmModeModeInfo* const mode = ChooseMode(m_Pipeline->Modes, wanted.Resolution, wanted.Period);

	if (mode == nullptr)
	{
		return Failure(ENODEV, "no mode is offered by", Subject{ m_Pipeline->Name });
	}

	m_Mode = *mode;

	// Register as a reader before anything can be promoted onto this panel. An unregistered output is
	// one the card's scanout table believes has stopped reading, which is a framebuffer removed
	// underneath a plane rather than freed — see Seam/Scanout.h.
	m_Device->Scanout().Attach(*this);

	if (::drmModeCreatePropertyBlob(m_Device->Descriptor().Value, &m_Mode, sizeof(m_Mode), &m_ModeBlob) != 0)
	{
		return Failure(errno, "creating the mode blob");
	}

	m_Configuration = wanted;
	m_Configuration.Resolution = PixelSize<DeviceSpace>{ m_Mode.hdisplay, m_Mode.vdisplay };
	m_Configuration.Period = PeriodOf(m_Mode);
	m_Configuration.LatchLead = BlankingOf(m_Mode) + CommitPath;

	// The refresh range is *learned* rather than requested — Seam/OutputConfiguration.h — and nothing
	// here learns one yet: `VRR_ENABLED` is a property this pipeline may carry, but the range behind it
	// comes from the connector's EDID and gyro does not parse one. So a panel reports fixed refresh,
	// which is honest, rather than a range invented from the mode.
	m_Configuration.Refresh = VariableRefresh{};

	if (const Result<void> built = BuildTargets(); !built)
	{
		return built;
	}

	// The plane state one flip writes, resolved once. The order is the order the ioctl sends and never
	// changes; `Present` overwrites the values in place.
	//
	// **The primary and everything above it, and nothing below.** gyro's composite is layer zero and
	// belongs on the primary — a modeset means something there and nowhere else — so a plane the driver
	// puts *under* the primary could only ever hold something beneath the composite, which the composite
	// would then have to be transparent over. That is a real arrangement and it is not this one; it is
	// left in Open.md rather than half-built, and the cost is one overlay unused on the hardware that
	// has such a plane.
	std::uint32_t offset = 0;

	for (const Plane& plane : m_Pipeline->Planes)
	{
		if (m_PlaneCount == 0 && plane.Kind != PlaneKind::Primary)
		{
			continue;
		}

		if (m_PlaneCount == MaxLayers)
		{
			break;
		}

		// **A cursor plane is taken like any other, and the pointer reaches it without anything here
		// knowing what a pointer is.** This kind used to be skipped by name, on the reading that several
		// drivers accept one size on it, ignore the source rectangle and take a format nothing else
		// takes — so an arbitrary layer put there would be a promotion that works on one machine and is
		// refused on the next. Every clause of that is true and the conclusion did not follow. A cursor
		// plane sits at the top of `zpos`, and the inventory is sorted by `zpos`, so it is the last slot
		// this loop fills; the promoted set is a suffix and the layers go onto planes bottom-first, so
		// the only layer that can ever reach it is the frontmost one, and only on a frame that has
		// already used every plane beneath it. A large window cannot land here while a lower plane is
		// free. What does land here is whatever is in front of everything else, which on a screen with a
		// pointer is the pointer — arrived at by the ordering rather than by naming the node, which is
		// what keeps decision 152's partition free of per-surface state.
		//
		// The rejected alternative is packing the promoted layers against the *top* planes instead, so
		// that a frame with slack reaches this one. That hands a full-screen window to a plane that
		// takes `AR24` alone, and an atomic test is all-or-nothing — one refused layer costs the whole
		// partition — so it would lose the offload on exactly the frames that have it today.
		//
		// What this kind is still worth naming for is the *format* pre-filter in `Expressible`, which is
		// the cheap half of not proposing a layer this plane will refuse.

		const PlaneProperties& properties = plane.Props;

		PlaneCommit& block = m_Planes[m_PlaneCount];
		block.Object = plane.Id;
		block.Offset = offset;
		block.Fenced = properties.InFenceFd != 0;
		block.Formats = plane.Formats;

		const auto add = [&](std::uint32_t property, std::uint64_t value) {
			m_Properties[offset] = property;
			m_Values[offset] = value;
			++offset;
		};

		// Fixed order, and `IsComplete` is what makes every one of these non-zero. The fence slot is
		// last and is present whenever the plane has one, because its *value* changes per frame while
		// the property count may not — a frame with nothing to wait for writes -1, which is what the
		// kernel reads as no fence.
		add(properties.FbId, 0);
		add(properties.CrtcId, 0);
		add(properties.SrcX, 0);
		add(properties.SrcY, 0);
		add(properties.SrcW, 0);
		add(properties.SrcH, 0);
		add(properties.CrtcX, 0);
		add(properties.CrtcY, 0);
		add(properties.CrtcW, 0);
		add(properties.CrtcH, 0);

		if (block.Fenced)
		{
			add(properties.InFenceFd, static_cast<std::uint64_t>(-1));
		}

		block.Count = offset - block.Offset;

		m_Objects[m_PlaneCount] = block.Object;
		m_Counts[m_PlaneCount] = block.Count;

		++m_PlaneCount;
	}

	if (m_PlaneCount == 0)
	{
		return Failure(EINVAL, "this pipeline has no primary plane to scan out of");
	}

	// Whether an acquire fence can be handed to the hardware at all is the *composite's* plane, since
	// that is the layer the renderer's work is behind. A promoted layer on a plane with no fence
	// property is held by the same path for the same reason.
	m_Fenced = m_Planes[0].Fenced;

	// The composite is layer zero and starts as the whole screen out of target zero.
	{
		const PresentLayer initial{
			.Target = LayerSource{ 0 },
			.Blend = BlendMode::Opaque,
			.Acquire = SyncPoint{},
			.Source = {},
			.Destination = { {}, m_Configuration.Resolution },
			.Damage = {},
			.Color = m_Configuration.Color,
		};

		Program({ &initial, 1 }, {});
	}

	// **Bare first, and `-EINVAL` is the kernel saying this was not an adoption.** Seam/Presenter.h's
	// rule: the flag is never a constant, because on AMD silicon below `IP_VERSION(3, 2, 0)` any commit
	// carrying it resets every plane on the CRTC.
	Result<void> programmed = Modeset(false);

	if (!programmed && programmed.error().Code() == EINVAL)
	{
		programmed = Modeset(true);
	}

	if (!programmed)
	{
		return programmed;
	}

	m_Targets[0].State = TargetState::Scanout;
	m_ScanoutMask = std::uint32_t{ 1 } << 0;

	m_Device->Attach(m_Crtc, *this);

	// **After the mode set rather than before it.** This runs on whichever thread the composition root
	// builds outputs on, which is where the one allocation a thread costs is ordinary — and starting it
	// earlier would put a thread behind an output that then failed to come up.
	m_Commit.Start(&DrmOutput::Issue, this);

	return {};
}

Result<void> DrmOutput::BuildTargets()
{
	const std::uint32_t code = m_Configuration.Format.IsValid() ? m_Configuration.Format.Code : FormatXrgb8888;
	const std::span<const std::uint64_t> modifiers = ModifiersFor(m_Pipeline->Primary().Formats, code);

	if (modifiers.empty())
	{
		return Failure(
			EINVAL, "this pipeline cannot scan out", Subject::Of("{} on {}", PixelFormat{ code }, m_Pipeline->Name)
		);
	}

	const PixelSize<DeviceSpace> size = m_Configuration.Resolution;

	for (std::uint32_t index = 0; index < m_Wanted; ++index)
	{
		// **The whole candidate set at once, and the device chooses.** Decision 120's reversal: the
		// plane's list says which layouts can reach the glass, and the GPU says which of those it likes
		// to render into. Handing them over one at a time in the plane's order composites every frame
		// into whatever the display engine happened to list first, which measured at two and a half
		// times the GPU cost on a tiled part.
		Result<DmabufBuffer> allocated = m_Allocator->Allocate(size, code, modifiers);

		if (!allocated)
		{
			DropTargets();

			return std::unexpected{ allocated.error() };
		}

		const RenderTarget described = allocated->Describe();
		const DmabufImage* const image = described.AsDmabuf();

		if (image == nullptr)
		{
			DropTargets();

			return Failure(EINVAL, "the allocator produced an image the display cannot scan out");
		}

		std::array<std::uint32_t, MaxImagePlanes> handles{};
		std::array<std::uint32_t, MaxImagePlanes> strides{};
		std::array<std::uint32_t, MaxImagePlanes> offsets{};
		std::array<std::uint64_t, MaxImagePlanes> layouts{};

		for (std::uint32_t plane = 0; plane < image->PlaneCount; ++plane)
		{
			// A framebuffer names GEM handles rather than descriptors, so every plane's dmabuf is
			// imported onto the card node. The handles are the device's and are released with the
			// framebuffer, which is why nothing here closes one by hand: `drmModeRmFB` is what ends
			// their life, and a handle closed early is a framebuffer the kernel still believes in.
			if (::drmPrimeFDToHandle(
					m_Device->Descriptor().Value, image->Planes[plane].Descriptor.Value, &handles[plane]
				) != 0)
			{
				DropTargets();

				return Failure(errno, "importing a target onto the display device");
			}

			strides[plane] = image->Planes[plane].Stride;
			offsets[plane] = image->Planes[plane].Offset;
			layouts[plane] = described.Format.Modifier;
		}

		std::uint32_t framebuffer = 0;

		const int added = ::drmModeAddFB2WithModifiers(
			m_Device->Descriptor().Value,
			static_cast<std::uint32_t>(size.Width),
			static_cast<std::uint32_t>(size.Height),
			code,
			handles.data(),
			strides.data(),
			offsets.data(),
			layouts.data(),
			&framebuffer,
			described.Format.Modifier != ModifierInvalid ? DRM_MODE_FB_MODIFIERS : 0U
		);

		if (added != 0)
		{
			DropTargets();

			return FailFromErrno("building a framebuffer for", Subject::Of("{}", described.Format));
		}

		m_Targets[index].Buffer = std::move(*allocated);
		m_Targets[index].Framebuffer = framebuffer;
		m_Targets[index].State = TargetState::Free;
		m_Descriptions[index] = described;
		++m_TargetCount;
	}

	// What was actually allocated, which is not necessarily what was asked for: the device chose the
	// modifier and this is where the achieved configuration learns it.
	m_Configuration.Format = m_Descriptions[0].Format;

	return {};
}

void DrmOutput::DropTargets() noexcept
{
	for (std::uint32_t index = 0; index < m_TargetCount; ++index)
	{
		if (m_Targets[index].Framebuffer != 0 && m_Device != nullptr)
		{
			::drmModeRmFB(m_Device->Descriptor().Value, m_Targets[index].Framebuffer);
		}

		m_Targets[index] = Target{};
		m_Descriptions[index] = RenderTarget{};
	}

	// **Tagged with what was in flight when it ran**, because this clears `m_Flipping` with no event
	// behind it: a one here is a commit the kernel is still holding and this output has forgotten.
	TraceMark("targets dropped", m_Trace, TraceTag(m_Flipping ? 1 : 0));

	m_TargetCount = 0;
	m_Next = 0;
	m_ScanoutMask = 0;
	m_InFlightMask = 0;
	m_Flipping = false;
	m_Watchdog.Disarm();
}

std::span<const RenderTarget> DrmOutput::Targets() const
{
	return { m_Descriptions.data(), m_TargetCount };
}

std::optional<std::uint32_t> DrmOutput::AcquireTarget()
{
	// Round robin from where the last one was taken, so that a ring of three cycles rather than
	// alternating between two while the third stays cold — the buffer nothing has touched for a while
	// is the one whose pages the kernel is most likely to have moved.
	for (std::uint32_t attempt = 0; attempt < m_TargetCount; ++attempt)
	{
		const std::uint32_t index = (m_Next + attempt) % m_TargetCount;

		if (m_Targets[index].State != TargetState::Free)
		{
			continue;
		}

		m_Targets[index].State = TargetState::Acquired;
		m_Next = (index + 1) % m_TargetCount;

		return index;
	}

	// **Nothing is an ordinary answer.** A ring whose images are all on the screen or in flight has
	// none free, and the frame loop's response is to skip this output rather than to wait.
	return std::nullopt;
}

Result<void> DrmOutput::Present(std::span<const PresentLayer> layers, PresentTrace trace)
{
	// Adopted before any record is written, because the records this call does not write — a flip
	// landing, a held commit abandoned — happen on later calls with no trace in hand.
	m_Trace = trace.Trace;

	if (!m_Configuration.Powered)
	{
		return Failure(EINVAL, "presenting to an output that is powered off");
	}

	if (m_TargetCount == 0)
	{
		return Failure(EBUSY, "presenting to an output that is mid-reconfiguration");
	}

	if (const Result<void> expressible = Expressible(layers); !expressible)
	{
		return expressible;
	}

	// KMS refuses a second commit on a CRTC that has not flipped, which is the rule `CommitDepth`'s
	// default of one already holds the loop to. Saying it here as well is what keeps a backend that
	// raises that number from finding out as a kernel error.
	//
	// **The slot is a third term now**, because the ioctl outlives the flip event it produces: the tail
	// sends the completion and then cleans up, so the loop can be woken, serve this output and arrive
	// back here microseconds before the commit thread is free. A refusal is the honest answer and the
	// loop already treats `EBUSY` as transient — but the ordinary path never sees it, because a whole
	// refresh separates one present from the next and `Settle` has reaped long before.
	if (m_Flipping || m_Pending.Waiting || !m_Commit.IsIdle())
	{
		return Failure(EBUSY, "this output already has a commit outstanding");
	}

	// One exported fence per layer, held here so that every descriptor outlives the ioctl that reads
	// it. `MaxLayers` of them, on the stack, because the frame section forbids the vector.
	std::array<Fd, MaxLayers> fences;
	bool wait = false;

	for (std::size_t index = 0; index < layers.size(); ++index)
	{
		const PresentLayer& layer = layers[index];

		if (m_Planes[index].Fenced)
		{
			if (Result<Fd> exported = m_Fences.Export(layer.Acquire); exported)
			{
				fences[index] = std::move(*exported);

				continue;
			}
		}

		// Either the plane has no fence property or the device would not give one up. Both come to the
		// same question: are the pixels there yet.
		wait = wait || (m_Completion != nullptr && !m_Completion->IsComplete(layer.Acquire));
	}

	if (wait)
	{
		// The pixels are not there and the hardware cannot be told to wait for them. Holding is the only
		// honest response — committing now scans out a half-drawn frame — and the cost is the frame of
		// overlap this backend exists to have. `Settle` releases it.
		//
		// **Held whole rather than per layer.** A partition is one picture, and committing the layers
		// that happen to be ready would put a promoted window on the screen a frame before the composite
		// that draws the rest of it.
		// **A slice rather than a mark, because its width is the lie in the picture.** The loop is about
		// to be told this present succeeded and will open a flight lane for it, but no ioctl has been
		// issued — so for exactly as long as this slice runs, the lane draws a frame as in the panel's
		// hands that is in fact still in gyro's. `Settle` closes it whichever way the hold ends.
		TraceOpen("held", m_Trace, TraceTag(trace.Frame));

		m_Pending =
			Pending{ .Count = static_cast<std::uint32_t>(layers.size()), .Waiting = true, .Frame = trace.Frame };
		std::ranges::copy(layers, m_Pending.Layers.begin());
		++m_HeldCommits;

		return {};
	}

	return Flip(layers, fences, trace.Frame);
}

std::uint32_t DrmOutput::Framebuffer(const PresentLayer& layer) const noexcept
{
	if (layer.Target.IsTexture())
	{
		const DrmScanout* const scanout = m_Device != nullptr ? &m_Device->Scanout() : nullptr;

		return scanout != nullptr ? scanout->Find(layer.Target.Texture) : 0;
	}

	return layer.Target.Index < m_TargetCount ? m_Targets[layer.Target.Index].Framebuffer : 0;
}

PixelSize<DeviceSpace> DrmOutput::Extent(const PresentLayer& layer) const noexcept
{
	if (!layer.Target.IsTexture())
	{
		return layer.Target.Index < m_TargetCount ? m_Descriptions[layer.Target.Index].Size : PixelSize<DeviceSpace>{};
	}

	// A promoted layer's image is a client's buffer and this output has no description of it. Decision
	// 152's predicate is that the promotion resamples not at all, so the destination the assigner
	// computed is the image's own extent — which is why a whole-image source needs no lookup here.
	return layer.Destination.Extent;
}

void DrmOutput::Record(std::span<const PresentLayer> layers) noexcept
{
	const std::uint32_t next = 1 - m_Recording.load(std::memory_order_relaxed);

	for (std::uint32_t index = 0; index < MaxLayers; ++index)
	{
		const std::uint32_t framebuffer = index < layers.size() ? Framebuffer(layers[index]) : 0;

		m_Committed[next][index].store(framebuffer, std::memory_order_relaxed);
	}

	m_Recording.store(next, std::memory_order_release);
}

bool DrmOutput::Holds(std::uint32_t framebuffer) const noexcept
{
	if (framebuffer == 0)
	{
		return false;
	}

	// Both halves, whichever is current: the newer is what was just committed and the older is what it
	// has not yet replaced on the glass.
	(void)m_Recording.load(std::memory_order_acquire);

	for (const std::array<std::atomic<std::uint32_t>, MaxLayers>& set : m_Committed)
	{
		for (const std::atomic<std::uint32_t>& held : set)
		{
			if (held.load(std::memory_order_relaxed) == framebuffer)
			{
				return true;
			}
		}
	}

	return false;
}

Result<void> DrmOutput::Expressible(std::span<const PresentLayer> layers) const noexcept
{
	// **Refused rather than partly honoured.** Seam/Presenter.h calls a set that cannot be expressed
	// `EINVAL` — a bug in the assigner rather than a condition to retry — and refusing here is what
	// keeps such a bug from being invisible on the backend that actually has planes.
	if (layers.empty() || layers.size() > m_PlaneCount)
	{
		return Failure(EINVAL, "this output has no plane for every layer of that partition");
	}

	for (std::uint32_t index = 0; index < layers.size(); ++index)
	{
		const PresentLayer& layer = layers[index];

		// A promoted layer that has no framebuffer on this card is refused here with the rest of the
		// partition, which is decision 153's rule: the frame thread names an id, and an id that did not
		// import costs decision 35's one composited frame rather than a hole on the screen.
		if (Framebuffer(layer) == 0)
		{
			return Failure(EINVAL, "presenting an image this output cannot scan out");
		}

		// **Does the plane this layer would land on say it takes these pixels at all.** `IN_FORMATS` is
		// already decoded per plane and the layout is already kept per image, so this is a walk over a
		// short list against two integers — no ioctl, nothing allocated, and an answer that does not
		// change while the plane and the buffer both live.
		//
		// **It is here because an atomic test is all-or-nothing.** One layer the driver will not take
		// costs the whole partition and the frame composites, so the layers most likely to be refused
		// are worth refusing *before* the ioctl rather than after it — which is both the 200 µs the test
		// costs on the `SCHED_FIFO` frame thread and, with the narrowing retry above this, the
		// difference between giving up one layer and giving up all of them.
		//
		// A plane that reports no format table at all is left alone rather than refused: an empty
		// catalog is a driver that did not answer, and inventing a refusal from silence would disable
		// promotion on hardware that works.
		if (!Advertised(m_Planes[index].Formats, Layout(layer)))
		{
			return Failure(EINVAL, "a plane in that partition does not scan out that layer's layout");
		}
	}

	return {};
}

PixelFormat DrmOutput::Layout(const PresentLayer& layer) const noexcept
{
	if (layer.Target.IsTexture())
	{
		const DrmScanout* const scanout = m_Device != nullptr ? &m_Device->Scanout() : nullptr;

		return scanout != nullptr ? scanout->Layout(layer.Target.Texture) : PixelFormat{};
	}

	return layer.Target.Index < m_TargetCount ? m_Descriptions[layer.Target.Index].Format : PixelFormat{};
}

Result<void> DrmOutput::TestLayers(std::span<const PresentLayer> layers)
{
	if (const Result<void> expressible = Expressible(layers); !expressible)
	{
		return expressible;
	}

	// **Refused while a commit is in flight, because this ioctl would otherwise *block* on it.** A
	// blocking commit holds the modeset locks it acquired until it returns — `drm_mode_atomic_ioctl`
	// drops them after `drm_atomic_commit` rather than after the swap — and a `TEST_ONLY` commit naming
	// the same CRTC takes the same locks. That wait would land on the `SCHED_FIFO` frame thread inside
	// Core/FrameSection.h's guard, which is decision 29's `B(L)` and admits nothing that is not a
	// composite.
	//
	// The window is small — the loop serves this output at its next deadline, most of a refresh after
	// the flip event, and the commit's tail is done microseconds after that event — so what this costs
	// in practice is nothing. When it does fire, decision 152's promotion falls back to compositing for
	// one frame, which is the ordinary answer a refusal already has and is silent to the person.
	if (!m_Commit.IsIdle())
	{
		return Failure(EBUSY, "testing a partition while this output has a commit in flight");
	}

	// **The blocks the real commit would use, filled with the values the real commit would write.**
	// Testing a partition assembled differently from the one that will be presented tests a different
	// question, and the difference would show up as a promotion the kernel accepted in the test and
	// refused on the frame. No fences: `TEST_ONLY` does not consume them and a test that exported one
	// would leak a descriptor per proposal.
	Program(layers, {});

	const Result<void> answer = Commit(DRM_MODE_ATOMIC_TEST_ONLY);

	// **Kept rather than logged, and kept here rather than in the caller.** Frame/Loop.h already marks
	// the refusal on this output's trace row with the sentence and the errno, which is what tells a
	// reader that promotion is being refused at all; what it cannot say is *what was proposed*, because
	// a `PresentLayer` names a texture id and the numbers that went to the kernel are this file's. See
	// `m_Refused`.
	if (!answer)
	{
		KeepRefusal(layers, answer.error().Code());
	}

	// The blocks are left holding a partition that was never committed, so the next `Present` must
	// rewrite them — which it does unconditionally. Saying so here is for the reader; nothing depends
	// on the state surviving.
	return answer;
}

void DrmOutput::KeepRefusal(std::span<const PresentLayer> layers, int code) noexcept
{
	// **Read out of `m_Values` rather than recomputed from the layers**, which is the whole point: the
	// question is what the ioctl carried, and a second derivation from the same inputs would agree with
	// the first even where both are wrong.
	RefusedProposal kept{};

	kept.Code = code;
	kept.Count = static_cast<std::uint32_t>(layers.size());

	if (kept.Count > MaxLayers)
	{
		kept.Count = MaxLayers;
	}

	for (std::uint32_t index = 0; index < kept.Count; ++index)
	{
		const PlaneCommit& block = m_Planes[index];
		const std::size_t at = block.Offset;

		kept.Layers[index] = RefusedLayer{
			.Plane = block.Object,
			.Framebuffer = static_cast<std::uint32_t>(m_Values[at + 0]),
			.SrcX = m_Values[at + 2],
			.SrcY = m_Values[at + 3],
			.SrcW = m_Values[at + 4],
			.SrcH = m_Values[at + 5],
			.CrtcX = static_cast<std::int64_t>(m_Values[at + 6]),
			.CrtcY = static_cast<std::int64_t>(m_Values[at + 7]),
			.CrtcW = m_Values[at + 8],
			.CrtcH = m_Values[at + 9],
		};
	}

	m_Refused = kept;
	m_RefusalPending = true;
}

void DrmOutput::ReportRefusal()
{
	if (!m_RefusalPending)
	{
		return;
	}

	m_RefusalPending = false;

	// The standing case: a driver that refuses the same partition every frame says so once. A window
	// that resizes changes the rectangles and earns a second line, which is the pair that separates
	// *this arrangement is refused* from *everything is refused*.
	if (m_Refused.SameAs(m_Reported))
	{
		return;
	}

	m_Reported = m_Refused;

	spdlog::warn(
		"the display engine refused a partition of {} layer(s) on crtc {}: {}",
		m_Refused.Count,
		m_Crtc,
		std::strerror(m_Refused.Code)
	);

	for (std::uint32_t index = 0; index < m_Refused.Count; ++index)
	{
		const RefusedLayer& layer = m_Refused.Layers[index];

		// **The format and the modifier from the kernel rather than from gyro's own record**, which is
		// the half that cannot be got wrong twice in the same direction: what the display engine is
		// judging is the framebuffer it holds, and asking it what that framebuffer is describes the
		// object under test rather than the intent behind it. An ioctl is free here — the drain is
		// outside Core/FrameSection.h's guard — and a driver too old for `GETFB2` answers nothing, which
		// prints as zeroes rather than as a second failure to explain.
		std::uint32_t format = 0;
		std::uint64_t modifier = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;

		if (drmModeFB2Ptr framebuffer = ::drmModeGetFB2(m_Device->Descriptor().Value, layer.Framebuffer))
		{
			format = framebuffer->pixel_format;
			modifier = framebuffer->modifier;
			width = framebuffer->width;
			height = framebuffer->height;

			::drmModeFreeFB2(framebuffer);
		}

		// **The framebuffer's own extent beside the source rectangle, which is the comparison that
		// answers this.** A source that runs off the end of the image it names is the refusal a plane
		// makes and a shader does not — a sampler clamps and the display engine will not — so printing
		// the two apart and leaving a reader to hold them in their head is printing the harder half of
		// the question. The source is given in whole texels beside the raw fixed point for the same
		// reason: a fractional edge is refused too, and reading one out of a 16.16 integer by eye is how
		// it gets missed.
		spdlog::warn(
			"  layer {} on plane {}: fb {} {:c}{:c}{:c}{:c} {}x{} modifier 0x{:016x} src {}x{}+{}+{} (0x{:x} "
			"0x{:x} 0x{:x} 0x{:x}) dst {}x{}+{}+{}",
			index,
			layer.Plane,
			layer.Framebuffer,
			static_cast<char>(format & 0xFFU),
			static_cast<char>((format >> 8U) & 0xFFU),
			static_cast<char>((format >> 16U) & 0xFFU),
			static_cast<char>((format >> 24U) & 0xFFU),
			width,
			height,
			modifier,
			layer.SrcW >> 16U,
			layer.SrcH >> 16U,
			layer.SrcX >> 16U,
			layer.SrcY >> 16U,
			layer.SrcX,
			layer.SrcY,
			layer.SrcW,
			layer.SrcH,
			layer.CrtcW,
			layer.CrtcH,
			layer.CrtcX,
			layer.CrtcY
		);
	}
}

void DrmOutput::Program(std::span<const PresentLayer> layers, std::span<const Fd> fences) noexcept
{
	for (std::uint32_t index = 0; index < m_PlaneCount; ++index)
	{
		const PlaneCommit& block = m_Planes[index];
		const std::uint32_t at = block.Offset;

		if (index >= layers.size())
		{
			// Off: no framebuffer and no CRTC. The rest of the block is written zero as well, so that a
			// plane turned off carries no stale geometry into the commit that turns it back on.
			std::ranges::fill_n(m_Values.begin() + at, static_cast<std::ptrdiff_t>(block.Count), 0);

			if (block.Fenced)
			{
				m_Values[at + PropertiesPerPlane - 1] = static_cast<std::uint64_t>(-1);
			}

			continue;
		}

		const PresentLayer& layer = layers[index];

		// An empty source means the whole image, which is what Seam/Presenter.h says and what every
		// caller that is not cropping writes.
		const PixelSize<DeviceSpace> extent = Extent(layer);
		const Rect<DeviceSpace> source =
			layer.Source.IsEmpty() ?
				Rect<DeviceSpace>{ {}, { static_cast<float>(extent.Width), static_cast<float>(extent.Height) } } :
				layer.Source;

		m_Values[at + 0] = Framebuffer(layer);
		m_Values[at + 1] = m_Crtc;
		m_Values[at + 2] = Fixed(source.Origin.X);
		m_Values[at + 3] = Fixed(source.Origin.Y);
		m_Values[at + 4] = Fixed(source.Extent.Width);
		m_Values[at + 5] = Fixed(source.Extent.Height);
		m_Values[at + 6] = static_cast<std::uint64_t>(layer.Destination.Origin.X);
		m_Values[at + 7] = static_cast<std::uint64_t>(layer.Destination.Origin.Y);
		m_Values[at + 8] = static_cast<std::uint64_t>(layer.Destination.Extent.Width);
		m_Values[at + 9] = static_cast<std::uint64_t>(layer.Destination.Extent.Height);

		if (block.Fenced)
		{
			// -1 is the kernel's *no fence*, and it is what an immediate point writes rather than the
			// property being left out.
			const bool have = index < fences.size() && fences[index].Borrow().IsValid();

			m_Values[at + PropertiesPerPlane - 1] =
				static_cast<std::uint64_t>(have ? fences[index].Borrow().Value : -1);
		}
	}
}

Result<void> DrmOutput::Commit(std::uint32_t flags) noexcept
{
	drm_mode_atomic atomic{};
	atomic.flags = flags;
	atomic.count_objs = m_PlaneCount;
	atomic.objs_ptr = reinterpret_cast<std::uintptr_t>(m_Objects.data());
	atomic.count_props_ptr = reinterpret_cast<std::uintptr_t>(m_Counts.data());
	atomic.props_ptr = reinterpret_cast<std::uintptr_t>(m_Properties.data());
	atomic.prop_values_ptr = reinterpret_cast<std::uintptr_t>(m_Values.data());

	// The completion is resolved by CRTC in the device's drain, so there is nothing to carry here. A
	// pointer would be a pointer the kernel holds across a teardown.
	atomic.user_data = 0;

	if (Ioctl(m_Device->Descriptor(), DRM_IOCTL_MODE_ATOMIC, &atomic) != 0)
	{
		// The vocabulary Seam/Presenter.h names: EBUSY is transient and the loop retries, ENODEV and
		// EACCES are the composition root's problem, EINVAL is a bug in what was assembled.
		return Failure(errno, "committing a page flip");
	}

	return {};
}

int DrmOutput::Issue(void* context, const CommitRequest& request) noexcept
{
	const DrmOutput& self = *static_cast<const DrmOutput*>(context);

	drm_mode_atomic atomic{};
	atomic.flags = request.Flags;
	atomic.count_objs = request.ObjectCount;
	atomic.objs_ptr = reinterpret_cast<std::uintptr_t>(request.Objects.data());
	atomic.count_props_ptr = reinterpret_cast<std::uintptr_t>(request.Counts.data());
	atomic.props_ptr = reinterpret_cast<std::uintptr_t>(request.Properties.data());
	atomic.prop_values_ptr = reinterpret_cast<std::uintptr_t>(request.Values.data());

	// The completion is resolved by CRTC in the device's drain, so there is nothing to carry here. A
	// pointer would be a pointer the kernel holds across a teardown.
	atomic.user_data = 0;

	return Ioctl(self.m_Device->Descriptor(), DRM_IOCTL_MODE_ATOMIC, &atomic) != 0 ? errno : 0;
}

Result<void> DrmOutput::Flip(std::span<const PresentLayer> layers, std::span<Fd> fences, std::uint64_t frame)
{
	// The request is assembled from the same blocks a synchronous commit used, which is what keeps the
	// property ids and the array sizes one fact rather than two that can drift apart.
	static_assert(CommitRequest::PropertiesPerPlane == PropertiesPerPlane);
	static_assert(CommitRequest::MaxProperties == MaxCommitProperties);

	Program(layers, fences);

	CommitRequest request;

	// **Blocking**, which is the whole change: no `DRM_MODE_ATOMIC_NONBLOCK`, so the thread that makes
	// this call is the one that waits on the fence, evades the vblank and writes the registers — at a
	// priority gyro chose rather than a kworker's. See Drm/Commit.h.
	request.Flags = DRM_MODE_PAGE_FLIP_EVENT;
	request.ObjectCount = m_PlaneCount;
	request.Objects = m_Objects;
	request.Counts = m_Counts;
	request.Properties = m_Properties;
	request.Values = m_Values;

	// Moved rather than copied: `Program` wrote these descriptors' numbers into the values above, and
	// they have to stay open until the kernel has read them on the other thread.
	for (std::size_t index = 0; index < fences.size() && index < MaxLayers; ++index)
	{
		request.Fences[index] = std::move(fences[index]);
	}

	if (!m_Commit.Arm(std::move(request)))
	{
		return Failure(EBUSY, "handing a commit to a thread that still has one");
	}

	// **When the commit was handed over**, which is no longer when the kernel took it — that happens on
	// the commit thread, and `commit path` on its own row is what says how long it took.
	TraceMark("flip issued", m_Trace, TraceTag(frame));
	m_FlippingFrame = frame;

	m_InFlightMask = 0;

	for (const PresentLayer& layer : layers)
	{
		// A promoted layer's image is a client's and is not in this output's ring, so there is no target
		// state to move and no bit to set: what holds it alive is `Holds` below rather than the mask.
		if (layer.Target.IsTexture())
		{
			continue;
		}

		m_Targets[layer.Target.Index].State = TargetState::Committed;
		m_InFlightMask |= std::uint32_t{ 1 } << layer.Target.Index;
	}

	Record(layers);

	m_Flipping = true;
	m_Pending = Pending{};
	++Commits;

	return {};
}

Result<void> DrmOutput::Modeset(bool allowModeset)
{
	// libdrm's atomic API rather than the bare ioctl, and the split is deliberate: this runs before the
	// frame thread exists, where an allocation is ordinary, and assembling three objects' properties by
	// hand would be the same code twice. See the header.
	drmModeAtomicReq* const request = ::drmModeAtomicAlloc();

	if (request == nullptr)
	{
		return Failure(ENOMEM, "allocating an atomic request");
	}

	const PlaneProperties& plane = m_Pipeline->Primary().Props;
	const PixelSize<DeviceSpace> size = m_Configuration.Resolution;

	::drmModeAtomicAddProperty(request, m_Pipeline->Connector, m_Pipeline->ConnectorProps.CrtcId, m_Crtc);
	::drmModeAtomicAddProperty(request, m_Crtc, m_Pipeline->CrtcProps.ModeId, m_ModeBlob);
	::drmModeAtomicAddProperty(request, m_Crtc, m_Pipeline->CrtcProps.Active, m_Configuration.Powered ? 1 : 0);

	::drmModeAtomicAddProperty(request, m_Pipeline->Primary().Id, plane.FbId, m_Targets[0].Framebuffer);
	::drmModeAtomicAddProperty(request, m_Pipeline->Primary().Id, plane.CrtcId, m_Crtc);
	::drmModeAtomicAddProperty(request, m_Pipeline->Primary().Id, plane.SrcX, 0);
	::drmModeAtomicAddProperty(request, m_Pipeline->Primary().Id, plane.SrcY, 0);
	::drmModeAtomicAddProperty(
		request, m_Pipeline->Primary().Id, plane.SrcW, Fixed(static_cast<std::int64_t>(size.Width))
	);
	::drmModeAtomicAddProperty(
		request, m_Pipeline->Primary().Id, plane.SrcH, Fixed(static_cast<std::int64_t>(size.Height))
	);
	::drmModeAtomicAddProperty(request, m_Pipeline->Primary().Id, plane.CrtcX, 0);
	::drmModeAtomicAddProperty(request, m_Pipeline->Primary().Id, plane.CrtcY, 0);
	::drmModeAtomicAddProperty(request, m_Pipeline->Primary().Id, plane.CrtcW, static_cast<std::uint64_t>(size.Width));
	::drmModeAtomicAddProperty(request, m_Pipeline->Primary().Id, plane.CrtcH, static_cast<std::uint64_t>(size.Height));

	// Blocking and eventless: this is the one commit whose completion nothing is waiting for, because
	// the frame loop does not exist yet.
	const int committed = ::drmModeAtomicCommit(
		m_Device->Descriptor().Value, request, allowModeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0U, nullptr
	);

	const int failure = errno;

	::drmModeAtomicFree(request);

	if (committed != 0)
	{
		return Failure(failure, allowModeset ? "setting the mode" : "adopting the mode already set");
	}

	return {};
}

void DrmOutput::Reconfigure(const OutputConfiguration& wanted)
{
	// **Power is performed and a mode is still not, and the answer says which.** Seam/Presenter.h has
	// this initiated on the frame thread and performed elsewhere, because `atomic_check` runs
	// synchronously on the caller and a driver may take every modeset lock on the device inside it. The
	// commit thread is that elsewhere: `Settle` hands it `ACTIVE` once the CRTC is quiet, and `Reap`
	// answers when the ioctl returns.
	//
	// Everything else a request carries is answered with what this output still has, which
	// `SatisfiedBy` reads as *not honoured* — no signal is invented and no mode is claimed. Only the
	// request is kept here; nothing is issued from inside the frame section.
	m_Request = wanted;
}

void DrmOutput::ArmPower(bool powered) noexcept
{
	CommitRequest request;

	request.Flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	request.ObjectCount = 1;
	request.Objects[0] = m_Crtc;
	request.Counts[0] = 1;
	request.Properties[0] = m_Pipeline->CrtcProps.Active;
	request.Values[0] = powered ? 1 : 0;

	// A slot that is not free leaves the request standing, and the next drain asks again. `IsQuiet` was
	// true on the way in, so this is the rule being checked rather than a case that happens.
	if (!m_Commit.Arm(std::move(request)))
	{
		return;
	}

	TraceMark(powered ? "power on issued" : "power off issued", m_Trace);

	m_Powering = true;
}

bool DrmOutput::IsQuiet() const noexcept
{
	return !m_Flipping && !m_Pending.Waiting && m_Commit.IsIdle();
}

void DrmOutput::Reap()
{
	const std::optional<CommitOutcome> outcome = m_Commit.Reap();

	if (outcome && m_Powering)
	{
		// **The answer to a power change, which is the ioctl's return and nothing else.** No flip was asked
		// for, so there is no event to wait on and no target moved: the images the panel was scanning out
		// are the ones it scans out when it comes back.
		m_Powering = false;

		const OutputConfiguration answered = *m_Request;
		m_Request.reset();

		const double took = std::chrono::duration<double, std::milli>{ outcome->Elapsed }.count();

		if (outcome->Error == 0)
		{
			m_Configuration.Powered = answered.Powered;

			// The panel's counter is not continuous across a stretch it spent dark, so the measured period
			// starts again rather than dividing the whole dark interval by whatever the counter says.
			m_HavePrevious = false;

			// **Said out loud with how long it took**, because that figure is Docs/Open.md's *wake latency
			// per rung*: hardware-dependent, unmeasured, and the delay between a keypress and a lit panel.
			// On the drain, outside the frame section, which is where `ReportRefusal` already speaks.
			spdlog::info("crtc {} is {}, after {:.1f} ms in the kernel", m_Crtc, answered.Powered ? "on" : "off", took);
		}
		else
		{
			// Not honoured, and answered as such: the generation is echoed with the power it still has.
			spdlog::warn(
				"crtc {} could not be turned {} ({:.1f} ms): {}",
				m_Crtc,
				answered.Powered ? "on" : "off",
				took,
				std::strerror(outcome->Error)
			);
		}

		m_Configuration.Generation = answered.Generation;

		Reconfigured.Emit(m_Configuration);

		return;
	}

	if (!outcome)
	{
		// **Nothing to take, which is every iteration but one and is where the watchdog lives.** The
		// commit that owes this output a page flip reported success on some earlier drain, so there is no
		// outcome left to read and the only question still open is whether the event it promised ever
		// turned up. Asked here rather than in `Settle` because this is already the function that unwinds
		// a frame the hardware will not show, and a flip event that never arrives is one of those.
		//
		// It is tested immediately after `DrmDevice::Read` has drained the descriptor the event would be
		// on, which is what makes even a one-refresh deadline safe: gyro has just looked.
		if (m_Flipping && m_Watchdog.HasExpired(MonotonicClock{}.Now()))
		{
			AbandonFlip();
		}

		return;
	}

	if (outcome->Error == 0)
	{
		// The kernel took it, and because the commit is blocking it has already waited for flip-done —
		// so the page flip is on the device's descriptor rather than in the panel's future. Everything
		// else about this frame arrives as that event.
		//
		// **Except where it does not**, which is the whole of Drm/Watchdog.h: a driver whose own wait
		// timed out returns zero and sends nothing, and an output waiting on an event that will never
		// come has no descriptor to become readable and nothing to put it back on `NextEvent`. Armed
		// from now rather than from `ArmedAt`, because the interval being bounded starts at the kernel's
		// answer and the ioctl before it may legitimately have taken seconds.
		if (m_Flipping)
		{
			m_Watchdog.Arm(MonotonicClock{}.Now(), m_Configuration.Period);
		}

		return;
	}

	// **A commit the kernel refused, discovered a frame after the loop was told it was accepted.** The
	// vocabulary is `Present`'s, and the reader wants the code: `EBUSY` is a race to back off from and
	// `EINVAL` is a request to fix, and they read identically without it.
	TraceMark("commit refused", m_Trace, TraceTag(static_cast<std::uint64_t>(outcome->Error)));

	// The images go back to the ring — all but the ones the panel is still showing, because a commit
	// that failed changed nothing on the glass and the last frame is still up there.
	for (std::uint32_t index = 0; index < m_TargetCount; ++index)
	{
		const std::uint32_t bit = std::uint32_t{ 1 } << index;

		if ((m_InFlightMask & bit) != 0 && (m_ScanoutMask & bit) == 0)
		{
			m_Targets[index].State = TargetState::Free;
		}
	}

	m_InFlightMask = 0;
	m_Flipping = false;
	m_Watchdog.Disarm();

	// The lane this frame was flying in is closed by the loop's own handler, which is why nothing here
	// closes it: `Missed` is the signal, and Frame/Loop.h owns what a missed frame does to the picture.
	Missed.Emit();
}

void DrmOutput::AbandonFlip()
{
	// **Tagged with the frame that was lost**, because this row and `flip event` are the two endings a
	// commit can have and a reader comparing them wants the same number on both.
	TraceMark("flip event never arrived", m_Trace, TraceTag(m_FlippingFrame));

	const std::uint32_t flipped = m_InFlightMask;

	m_InFlightMask = 0;
	m_Flipping = false;
	m_Watchdog.Disarm();

	// The same set difference `OnPresented` runs, and for the same reason: the images that were on the
	// glass are off it and the ones that were committed are on. See the declaration for why this
	// believes the flip over the missing event.
	for (std::uint32_t index = 0; index < m_TargetCount; ++index)
	{
		const std::uint32_t bit = std::uint32_t{ 1 } << index;

		if ((m_ScanoutMask & bit) != 0 && (flipped & bit) == 0)
		{
			m_Targets[index].State = TargetState::Free;
		}

		if ((flipped & bit) != 0)
		{
			m_Targets[index].State = TargetState::Scanout;
		}
	}

	m_ScanoutMask = flipped;

	Missed.Emit();
}

void DrmOutput::Settle()
{
	// **The frame thread's refusal, said out loud on the thread that may say things.** This is the first
	// thing in the drain rather than the last because it is not part of settling anything — it is a
	// message the previous frame left behind, and burying it under the commit bookkeeping would put it
	// after a `Reap` that can end this output.
	ReportRefusal();

	// First, so that a commit which finished since the last drain has freed its slot before anything
	// below asks whether this output is busy.
	Reap();

	const auto ready = [&] {
		if (m_Completion == nullptr)
		{
			return true;
		}

		// Every layer of the held partition, because it is committed whole. One layer still recording is
		// a partition that would put a promoted window on the screen ahead of the composite drawing the
		// rest of the picture.
		for (std::uint32_t index = 0; index < m_Pending.Count; ++index)
		{
			if (!m_Completion->IsComplete(m_Pending.Layers[index].Acquire))
			{
				return false;
			}
		}

		return true;
	};

	if (m_Pending.Waiting && ready())
	{
		const std::array<PresentLayer, MaxLayers> layers = m_Pending.Layers;
		const std::uint32_t count = m_Pending.Count;
		const std::uint64_t frame = m_Pending.Frame;
		m_Pending = Pending{};

		// The wait is over whichever way the rest of this goes — the marks below are outcomes, and they
		// read better beside the slice than inside it.
		TraceClose(m_Trace);

		const std::span<const PresentLayer> held{ layers.data(), count };

		// **Re-tested rather than trusted, because the hold outlives the answer.** `Present` established
		// that every layer resolves, and then the partition sat here for as long as the pixels took. A
		// ring dropped and rebuilt in that window — the mode set, once it is built — leaves an index this
		// output no longer owns, and a promoted layer's framebuffer can be removed by the same wait. The
		// whole partition is abandoned in that case: the images it named went with the ring, so there is
		// nothing to hand back, and the loop's next iteration draws the frame again.
		const Result<void> expressible = Expressible(held);

		if (!expressible)
		{
			// The other silent exit from a held commit: the ring went out from under it. The loop was
			// told this frame was presented and it never will be. Tagged with the frame rather than the
			// errno, because the frame is what a reader searches for and the refusal has one cause — the
			// images the partition named went with the ring.
			TraceMark("held commit abandoned", m_Trace, TraceTag(frame));
		}

		if (expressible)
		{
			// A commit that fails here has nowhere to report to — the frame loop has already been told the
			// present was accepted — so the images go back to the ring and the output stays flip-idle, which
			// the loop's next iteration serves as an ordinary frame.
			if (const Result<void> flipped = Flip(held, {}, frame); !flipped)
			{
				// The comment below says this has nowhere to report to. It has the ring.
				TraceMark(
					flipped.error().Sentence(), m_Trace, TraceTag(static_cast<std::uint64_t>(flipped.error().Code()))
				);

				for (std::uint32_t index = 0; index < count; ++index)
				{
					if (!layers[index].Target.IsTexture())
					{
						m_Targets[layers[index].Target.Index].State = TargetState::Free;
					}
				}
			}
		}
	}

	// **A reconfiguration begins only on a quiet CRTC**, because KMS refuses a second commit on one whose
	// first has not finished and the commit thread holds one slot. The loop stops serving an output the
	// moment it asks, so quiet is at most the flip already in the air.
	if (m_Request.has_value() && !m_Powering && IsQuiet())
	{
		if (m_Request->Powered != m_Configuration.Powered)
		{
			ArmPower(m_Request->Powered);
		}
		else
		{
			// Nothing this output can change: the power is already what was asked and a mode is not built.
			// Answered at once with what it has, which is the reading `SatisfiedBy` needs.
			const OutputConfiguration answered = *m_Request;
			m_Request.reset();

			m_Configuration.Generation = answered.Generation;

			Reconfigured.Emit(m_Configuration);
		}
	}
}

Instant DrmOutput::NextEvent() const noexcept
{
	// The device's file is what wakes this backend for everything else. What is not on it is a composite
	// gyro is waiting for, a power change the kernel is still inside, and a reconfiguration that can begin
	// now — and all three are answered by asking to be looked at again.
	if (m_Pending.Waiting)
	{
		const MonotonicClock clock;

		return Advanced(clock.Now(), CompletionPoll);
	}

	if (m_Powering)
	{
		const MonotonicClock clock;

		return m_Commit.IsComplete() ? clock.Now() : Advanced(clock.Now(), PowerPoll);
	}

	// **Only once it can begin.** A request waiting for a flip is woken by that flip's event on the
	// device's file, and answering *now* for it instead would spin the loop for the rest of the refresh.
	if (m_Request.has_value() && IsQuiet())
	{
		const MonotonicClock clock;

		return clock.Now();
	}

	// **A commit that fails produces no page flip, and silence is the thing that must not happen.** The
	// loop marks this output flip-pending at the commit and will not serve it again until something
	// clears that, so a refusal nobody hears is a panel that stops drawing for good. A commit that
	// *succeeds* is answered by its flip long before this falls due, which is what makes one backstop
	// cheaper than the poll a held commit needs: this wakes the loop once per failed commit rather than
	// thirty times per good one.
	if (!m_Commit.IsIdle())
	{
		const Duration period = m_Configuration.Period > Duration::zero() ? m_Configuration.Period : CommitBackstop;

		return Advanced(m_Commit.ArmedAt(), period * 2);
	}

	// **And the same argument one step further along.** The backstop above covers a commit the thread is
	// still inside; this covers one it has finished and reported success for, whose page flip has not
	// arrived. Without it that output is on nobody's books at all — the commit thread is idle, so
	// nothing above fires, and the event that would make the device's descriptor readable is the very
	// thing that has gone missing. Drm/Watchdog.h has what that costs.
	if (m_Flipping)
	{
		return m_Watchdog.Deadline();
	}

	return Instant{ Duration::max() };
}

void DrmOutput::OnPresented(Instant at, std::uint32_t sequence, bool hardwareClock)
{
	if (!m_Flipping)
	{
		// **Instrumented because it is one of the readings this is meant to separate.** A completion
		// arriving for a commit this output does not believe it made is either a duplicate the kernel
		// sent or a flip whose bookkeeping was cleared under it, and both are silent today. Tagged with
		// the kernel's sequence because there is no frame to name — that is what unclaimed means.
		TraceMark("flip event unclaimed", m_Trace, TraceTag(sequence));

		// A completion for a commit this output did not make. It happens across a teardown and is worth
		// dropping rather than crediting: an observation with no frame behind it is a prediction built on
		// a frame that never happened.
		return;
	}

	// Named for the frame whose commit this answers rather than for the kernel's vblank counter — the
	// counter is already the glass row's `refresh` attribute, and the frame is the words every other
	// row says. Stamped at the drain rather than at `at`, because the vblank instant is already drawn
	// twice and *when gyro heard* is the number no other row carries.
	TraceMark("flip event", m_Trace, TraceTag(m_FlippingFrame));

	const std::uint32_t flipped = m_InFlightMask;
	m_InFlightMask = 0;
	m_Flipping = false;
	m_Watchdog.Disarm();

	// The images that were on the glass are off it now, and the ones that were committed are on. A set
	// difference rather than a comparison, because freeing an image the panel is still showing hands the
	// renderer the picture that is on screen — and a partition retires several at once.
	for (std::uint32_t index = 0; index < m_TargetCount; ++index)
	{
		const std::uint32_t bit = std::uint32_t{ 1 } << index;

		if ((m_ScanoutMask & bit) != 0 && (flipped & bit) == 0)
		{
			m_Targets[index].State = TargetState::Free;
		}

		if ((flipped & bit) != 0)
		{
			m_Targets[index].State = TargetState::Scanout;
		}
	}

	m_ScanoutMask = flipped;

	PresentationInfo info{};
	info.PresentedAt = at;
	info.Sequence = sequence;
	info.Vsync = true;
	info.HardwareClock = hardwareClock;

	// True by construction on this backend: what was committed is what the display engine is scanning,
	// with nothing between them.
	info.ZeroCopy = true;

	// **Measured rather than echoed from the mode**, per Seam/PresentationInfo.h — a servo that closed
	// its loop on the value it commanded would report convergence it never achieved. Divided by the
	// sequence delta so that a missed vblank reports the panel's period rather than twice it, and left
	// at zero across a sequence that went backwards or stood still, which the clock reads as *the
	// backend does not know*.
	if (m_HavePrevious && sequence > m_LastSequence)
	{
		const Duration elapsed = Elapsed(m_LastPresented, at);
		const std::int64_t frames = static_cast<std::int64_t>(sequence - m_LastSequence);

		info.Period = Duration{ elapsed.count() / frames };
	}

	m_LastPresented = at;
	m_LastSequence = sequence;
	m_HavePrevious = true;

	Presented.Emit(info);
}
} // namespace Drm
