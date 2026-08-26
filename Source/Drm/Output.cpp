#define _POSIX_C_SOURCE 200809L

#include "Drm/Output.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <sys/ioctl.h>
#include <xf86drm.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <utility>

#include "Core/Clock.h"

namespace Drm
{
namespace
{
// How often a held commit asks to be looked at again. Nested/Output.cpp's figure and its reasoning:
// nothing becomes readable when the GPU finishes, so this path polls, and half a millisecond is short
// against a refresh and long against the cost of asking.
constexpr Duration CompletionPoll = std::chrono::microseconds{ 500 };

// SRC_X and friends are 16.16 fixed point; CRTC_X and friends are whole pixels.
[[nodiscard]] constexpr std::uint64_t Fixed(std::int64_t pixels) noexcept
{
	return static_cast<std::uint64_t>(pixels) << 16;
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
	if (m_Device != nullptr)
	{
		m_Device->Detach(m_Crtc);
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
		return Failure(ENODEV, std::format("{} offers no mode", m_Pipeline->Name));
	}

	m_Mode = *mode;

	if (::drmModeCreatePropertyBlob(m_Device->Descriptor().Value, &m_Mode, sizeof(m_Mode), &m_ModeBlob) != 0)
	{
		return Failure(errno, "creating the mode blob");
	}

	m_Configuration = wanted;
	m_Configuration.Resolution = PixelSize<DeviceSpace>{ m_Mode.hdisplay, m_Mode.vdisplay };
	m_Configuration.Period = PeriodOf(m_Mode);

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
	const PlaneProperties& plane = m_Pipeline->PlaneProps;

	const auto add = [&](std::uint32_t property, std::uint64_t value) {
		if (property != 0 && m_PropertyCount < MaxCommitProperties)
		{
			m_Properties[m_PropertyCount] = property;
			m_Values[m_PropertyCount] = value;
			++m_PropertyCount;
		}
	};

	add(plane.FbId, m_Targets[0].Framebuffer);
	add(plane.CrtcId, m_Crtc);
	add(plane.SrcX, 0);
	add(plane.SrcY, 0);
	add(plane.SrcW, Fixed(m_Configuration.Resolution.Width));
	add(plane.SrcH, Fixed(m_Configuration.Resolution.Height));
	add(plane.CrtcX, 0);
	add(plane.CrtcY, 0);
	add(plane.CrtcW, static_cast<std::uint64_t>(m_Configuration.Resolution.Width));
	add(plane.CrtcH, static_cast<std::uint64_t>(m_Configuration.Resolution.Height));

	// The fence slot is last and is always present where the hardware has one, because its *value*
	// changes per frame and an absent property would change the property count instead — which is the
	// one part of the commit that is supposed to be fixed. A frame with nothing to wait for writes -1,
	// which is what the kernel reads as no fence.
	if (plane.InFenceFd != 0)
	{
		add(plane.InFenceFd, static_cast<std::uint64_t>(-1));
		m_Fenced = true;
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
	m_Scanout = 0;

	m_Device->Attach(m_Crtc, *this);

	return {};
}

Result<void> DrmOutput::BuildTargets()
{
	const std::uint32_t code = m_Configuration.Format.IsValid() ? m_Configuration.Format.Code : FormatXrgb8888;
	const std::span<const std::uint64_t> modifiers = ModifiersFor(m_Pipeline->Formats, code);

	if (modifiers.empty())
	{
		return Failure(EINVAL, std::format("{} cannot scan out {}", m_Pipeline->Name, PixelFormat{ code }));
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

			return Failure(errno, std::format("building a framebuffer for {}", described.Format));
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

	m_TargetCount = 0;
	m_Next = 0;
	m_Scanout.reset();
	m_InFlight.reset();
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

Result<void> DrmOutput::Present(std::span<const PresentLayer> layers)
{
	if (!m_Configuration.Powered)
	{
		return Failure(EINVAL, "presenting to an output that is powered off");
	}

	if (m_TargetCount == 0)
	{
		return Failure(EBUSY, "presenting to an output that is mid-reconfiguration");
	}

	// **One layer, and more than one is refused rather than partly honoured.** Decision 5's plane
	// assignment is what the format catalog is for and it is not built: this output drives one primary
	// plane. Seam/Presenter.h calls a set that cannot be expressed `EINVAL` — a bug in the assigner
	// rather than a condition to retry — and refusing it here is what keeps such a bug from being
	// invisible on the backend that has planes.
	if (layers.size() != 1)
	{
		return Failure(EINVAL, "this output scans out one layer");
	}

	const PresentLayer& layer = layers.front();

	if (layer.Target >= m_TargetCount || m_Targets[layer.Target].Framebuffer == 0)
	{
		return Failure(EINVAL, "presenting a target this output does not own");
	}

	// KMS refuses a second nonblocking commit on a CRTC that has not flipped, which is the rule
	// `CommitDepth`'s default of one already holds the loop to. Saying it here as well is what keeps a
	// backend that raises that number from finding out as a kernel error.
	if (m_InFlight.has_value() || m_Pending.Waiting)
	{
		return Failure(EBUSY, "this output already has a commit outstanding");
	}

	Fd fence;

	if (m_Fenced)
	{
		Result<Fd> exported = m_Fences.Export(layer.Acquire);

		if (exported)
		{
			fence = std::move(*exported);
		}
		else if (m_Completion != nullptr && !m_Completion->IsComplete(layer.Acquire))
		{
			// The device would not give up a fence and the pixels are not there yet. Holding is the only
			// honest response — committing now scans out a half-drawn frame — and the cost is the frame
			// of overlap this backend exists to have. `Settle` releases it.
			m_Pending = Pending{ .Layer = layer, .Waiting = true };
			++m_HeldCommits;

			return {};
		}
	}
	else if (m_Completion != nullptr && !m_Completion->IsComplete(layer.Acquire))
	{
		m_Pending = Pending{ .Layer = layer, .Waiting = true };
		++m_HeldCommits;

		return {};
	}

	return Flip(layer, fence.Borrow());
}

Result<void> DrmOutput::Flip(const PresentLayer& layer, RawFd fence)
{
	m_Values[0] = m_Targets[layer.Target].Framebuffer;

	if (m_Fenced)
	{
		// Last, by construction — see `Open`. -1 is the kernel's *no fence*, and it is what an immediate
		// point writes rather than the property being left out.
		m_Values[m_PropertyCount - 1] = static_cast<std::uint64_t>(fence.IsValid() ? fence.Value : -1);
	}

	std::uint32_t object = m_Pipeline->Plane;
	std::uint32_t count = m_PropertyCount;

	drm_mode_atomic atomic{};
	atomic.flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
	atomic.count_objs = 1;
	atomic.objs_ptr = reinterpret_cast<std::uintptr_t>(&object);
	atomic.count_props_ptr = reinterpret_cast<std::uintptr_t>(&count);
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

	m_Targets[layer.Target].State = TargetState::Committed;
	m_InFlight = layer.Target;
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

	const PlaneProperties& plane = m_Pipeline->PlaneProps;
	const PixelSize<DeviceSpace> size = m_Configuration.Resolution;

	::drmModeAtomicAddProperty(request, m_Pipeline->Connector, m_Pipeline->ConnectorProps.CrtcId, m_Crtc);
	::drmModeAtomicAddProperty(request, m_Crtc, m_Pipeline->CrtcProps.ModeId, m_ModeBlob);
	::drmModeAtomicAddProperty(request, m_Crtc, m_Pipeline->CrtcProps.Active, m_Configuration.Powered ? 1 : 0);

	::drmModeAtomicAddProperty(request, m_Pipeline->Plane, plane.FbId, m_Targets[0].Framebuffer);
	::drmModeAtomicAddProperty(request, m_Pipeline->Plane, plane.CrtcId, m_Crtc);
	::drmModeAtomicAddProperty(request, m_Pipeline->Plane, plane.SrcX, 0);
	::drmModeAtomicAddProperty(request, m_Pipeline->Plane, plane.SrcY, 0);
	::drmModeAtomicAddProperty(request, m_Pipeline->Plane, plane.SrcW, Fixed(size.Width));
	::drmModeAtomicAddProperty(request, m_Pipeline->Plane, plane.SrcH, Fixed(size.Height));
	::drmModeAtomicAddProperty(request, m_Pipeline->Plane, plane.CrtcX, 0);
	::drmModeAtomicAddProperty(request, m_Pipeline->Plane, plane.CrtcY, 0);
	::drmModeAtomicAddProperty(request, m_Pipeline->Plane, plane.CrtcW, static_cast<std::uint64_t>(size.Width));
	::drmModeAtomicAddProperty(request, m_Pipeline->Plane, plane.CrtcH, static_cast<std::uint64_t>(size.Height));

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
	// **What is not built is a mode set, and this reports that rather than performing one.**
	// Seam/Presenter.h has this initiated on the frame thread and performed elsewhere, because
	// `atomic_check` runs synchronously on the caller and a driver may take every modeset lock on the
	// device inside it. That is a thread and a completion path, and it is the next change.
	//
	// A request that the current configuration already satisfies is answered as achieved, which is the
	// ordinary case: the frame loop reconfigures when a generation moves, and a generation moves for
	// reasons that do not always change a mode. Anything else is answered with what this output still
	// has, which `SatisfiedBy` reads as *not honoured* — no signal is invented and no mode is claimed.
	m_Request = m_Configuration;
	m_Request->Generation = wanted.Generation;
}

void DrmOutput::Settle()
{
	if (m_Pending.Waiting && (m_Completion == nullptr || m_Completion->IsComplete(m_Pending.Layer.Acquire)))
	{
		const PresentLayer layer = m_Pending.Layer;
		m_Pending = Pending{};

		// A commit that fails here has nowhere to report to — the frame loop has already been told the
		// present was accepted — so the image goes back to the ring and the output stays flip-idle,
		// which the loop's next iteration serves as an ordinary frame.
		if (const Result<void> flipped = Flip(layer, RawFd{}); !flipped)
		{
			m_Targets[layer.Target].State = TargetState::Free;
		}
	}

	if (m_Request.has_value())
	{
		const OutputConfiguration answered = *m_Request;
		m_Request.reset();

		m_Configuration.Generation = answered.Generation;

		Reconfigured.Emit(m_Configuration);
	}
}

Instant DrmOutput::NextEvent() const noexcept
{
	// The device's file is what wakes this backend for everything else. The two things that are not on
	// it are a composite gyro is waiting for and a reconfiguration nobody has been told about, and both
	// are answered by asking to be looked at again.
	if (m_Pending.Waiting || m_Request.has_value())
	{
		const MonotonicClock clock;

		return Advanced(clock.Now(), m_Pending.Waiting ? CompletionPoll : Duration::zero());
	}

	return Instant{ Duration::max() };
}

void DrmOutput::OnPresented(Instant at, std::uint32_t sequence, bool hardwareClock)
{
	if (!m_InFlight.has_value())
	{
		// A completion for a commit this output did not make. It happens across a teardown and is worth
		// dropping rather than crediting: an observation with no frame behind it is a prediction built on
		// a frame that never happened.
		return;
	}

	const std::uint32_t flipped = *m_InFlight;
	m_InFlight.reset();

	// The image that was on the glass is off it now, and the one that was committed is on. Two slots
	// rather than one, because freeing the wrong one hands the renderer the picture the panel is
	// currently showing.
	if (m_Scanout.has_value() && *m_Scanout != flipped)
	{
		m_Targets[*m_Scanout].State = TargetState::Free;
	}

	m_Targets[flipped].State = TargetState::Scanout;
	m_Scanout = flipped;

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
