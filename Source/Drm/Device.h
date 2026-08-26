#pragma once

#include <xf86drmMode.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Core/Time.h"
#include "Drm/Catalog.h"
#include "Seam/EventSource.h"

// The DRM device: one file, every CRTC on the card, and the events all of them speak through.
//
// **One device and N presenters, which is the granularity Seam/EventSource.h names outright.** A DRM
// file carries page-flip completions for every CRTC on it, so the source belongs here and the
// presenters are what it emits into. A source hung off each output would have every output reading
// its siblings' events off one shared file and discarding them.
//
// **Master comes from first-open and there is no session protocol behind it.** gyro is a boot service
// with no VT to switch away from — Docs/Architecture.md#boot-and-the-display-lifetime — so the party
// that opens the node first is master for the life of the file description, and nothing can take it
// away. Decision 7 deferred choosing a D-Bus client until the DRM backend needed one; it does not.
// `drmSetMaster` is called anyway, and its failure is *not* fatal: a device gyro is already master of
// answers `EINVAL` on some kernels and success on others, and the only thing that actually decides the
// question is whether the atomic commit is permitted.
//
// **Everything the pipeline needs is read once, here, and never read back.** Property ids, the plane
// catalog, the mode list: they are fixed for the life of the device, and a per-frame commit that
// looked one up by name would be doing string comparisons inside the frame section. Seam/Presenter.h's
// rule — gyro never reads hardware state back to decide — is what makes that safe: the only state that
// matters is what gyro last committed, and gyro is what committed it.

namespace Drm
{
class DrmOutput;

// The properties a commit sets, resolved to ids once. Zero is *this device has no such property*,
// which for `InFenceFd` and `FbDamageClips` is an ordinary answer on older hardware and for the
// others is a device that cannot be driven atomically at all.
struct ConnectorProperties
{
	std::uint32_t CrtcId = 0;
	std::uint32_t Dpms = 0;
};

struct CrtcProperties
{
	std::uint32_t ModeId = 0;
	std::uint32_t Active = 0;
	std::uint32_t VrrEnabled = 0;
};

struct PlaneProperties
{
	std::uint32_t FbId = 0;
	std::uint32_t CrtcId = 0;
	std::uint32_t SrcX = 0;
	std::uint32_t SrcY = 0;
	std::uint32_t SrcW = 0;
	std::uint32_t SrcH = 0;
	std::uint32_t CrtcX = 0;
	std::uint32_t CrtcY = 0;
	std::uint32_t CrtcW = 0;
	std::uint32_t CrtcH = 0;
	std::uint32_t InFenceFd = 0;
	std::uint32_t FbDamageClips = 0;

	// Whether a commit can be assembled at all. The fence and the damage clips are absent from plenty
	// of working hardware and are checked at their own call sites.
	[[nodiscard]] bool IsComplete() const noexcept
	{
		return FbId != 0 && CrtcId != 0 && SrcW != 0 && SrcH != 0 && CrtcW != 0 && CrtcH != 0;
	}
};

// One connector, the CRTC driving it and the plane it scans out of: everything an output is, before
// there is an output.
//
// **The modes are copied rather than borrowed.** `drmModeGetConnector` returns an allocation that has
// to be freed, and a mode list that outlives its connector by pointing into freed memory is the kind
// of defect that shows up as a resolution nobody configured. They are small and there are a few dozen.
struct Pipeline
{
	std::uint32_t Connector = 0;
	std::uint32_t Crtc = 0;
	std::uint32_t Plane = 0;

	// Which bit of a plane's `possible_crtcs` this CRTC is, which is also the index the kernel orders
	// its CRTC list by. Kept because it is the only way to ask that question again later.
	std::uint32_t CrtcIndex = 0;

	// `eDP-1`, `HDMI-A-2`. What a person configuring an output types and what every log line about
	// this output says, so it is built once here rather than formatted from two numbers at each site.
	std::string Name;

	std::vector<drmModeModeInfo> Modes;
	std::vector<PlaneFormat> Formats;

	ConnectorProperties ConnectorProps{};
	CrtcProperties CrtcProps{};
	PlaneProperties PlaneProps{};

	// The panel's own physical size, for the scale an output adapter derives. Millimetres, as EDID
	// states it, and zero where the connector does not say — which is most projectors and every
	// virtual connector.
	std::uint32_t WidthMm = 0;
	std::uint32_t HeightMm = 0;
};

class DrmDevice final : public IEventSource
{
public:
	DrmDevice() = default;

	~DrmDevice() override = default;

	// Open a card node and read everything fixed about it.
	//
	// `path` empty means *the first card node that has a connected connector*, which is what a boot
	// service with no configuration has to do: a laptop with a discrete GPU has two card nodes and only
	// one of them has the panel on it. `ENODEV` where nothing opened, `ENOTSUP` where the device
	// refuses the atomic client cap — which is a driver too old to drive, and a sentence rather than a
	// fallback, because gyro's whole presentation model is one atomic commit per frame.
	//
	// **Held behind a pointer because an `IEventSource` is neither copied nor moved**, which that seam
	// fixes rather than this type choosing: a loop registers a source by address and holds it for the
	// backend's whole life, and every output on the card holds this one too.
	[[nodiscard]] static Result<std::unique_ptr<DrmDevice>> Open(std::string_view path);

	DrmDevice(const DrmDevice&) = delete;
	DrmDevice& operator=(const DrmDevice&) = delete;
	DrmDevice(DrmDevice&&) = delete;
	DrmDevice& operator=(DrmDevice&&) = delete;

	[[nodiscard]] bool IsValid() const noexcept { return m_Device.IsValid(); }

	// Borrowed, per Core/Fd.h. Outputs commit through it and it is closed with the device, which the
	// composition root sequences by destroying the outputs first.
	[[nodiscard]] RawFd Descriptor() const noexcept override { return m_Device.Borrow(); }

	[[nodiscard]] const std::string& Path() const noexcept { return m_Path; }

	// What the driver calls itself, for the line that says what gyro is driving.
	[[nodiscard]] const std::string& Driver() const noexcept { return m_Driver; }

	// Whether page-flip timestamps are on `CLOCK_MONOTONIC`. This is `PresentationInfo::HardwareClock`
	// and it is read rather than assumed: `simpledrm` registers no vblank at all and its completion is
	// `drm_atomic_helper_fake_vblank`'s, which arrives at commit time rather than at a boundary. A
	// clock told that is precise would build a schedule with no error bar. See decision 57's revision.
	[[nodiscard]] bool HasMonotonicTimestamps() const noexcept { return m_Monotonic; }

	// The pipelines this device can drive, one per connected connector, already assigned a CRTC and a
	// primary plane. Empty is a card with nothing plugged into it, which is not a failure — a machine
	// with two GPUs has one of these for each.
	[[nodiscard]] std::span<const Pipeline> Pipelines() const noexcept { return m_Pipelines; }

	// Route this CRTC's completions to that output, for as long as the output lives.
	//
	// A flat array rather than a map: there are as many CRTCs as a card has, which is single digits,
	// and the lookup is on the frame path.
	void Attach(std::uint32_t crtc, DrmOutput& output);

	void Detach(std::uint32_t crtc) noexcept;

	// Seam/EventSource.h: read page-flip completions until the file is empty and emit each one into the
	// output that owns its CRTC.
	//
	// Failure is the vocabulary that seam names: `ENODEV` where the device is gone. A read that finds
	// nothing is success and is the ordinary result of a wakeup something else caused.
	[[nodiscard]] Result<void> Drain() override;

	// Nothing here falls due at an instant gyro chose: every completion is a real event on a real
	// descriptor. Kept so the composition root's backend can answer without a special case.
	[[nodiscard]] Instant NextEvent() const noexcept { return Instant{ Duration::max() }; }

private:
	// One CRTC's subscriber. Nothing owns the output; the composition root destroys outputs before the
	// device, and `Detach` is the output's destructor saying so.
	struct Subscriber
	{
		std::uint32_t Crtc = 0;
		DrmOutput* Output = nullptr;
	};

	// Open one node and take everything fixed off it, or say why not. `requireConnector` is what makes
	// the search for a card with a panel on it a search rather than a first hit.
	[[nodiscard]] static Result<std::unique_ptr<DrmDevice>> OpenNode(const std::string& path, bool requireConnector);

	// One page-flip completion, resolved to the output that drives its CRTC.
	void Complete(std::uint32_t crtc, std::uint32_t sequence, std::uint32_t seconds, std::uint32_t microseconds);

	Fd m_Device;
	std::string m_Path;
	std::string m_Driver;
	bool m_Monotonic = false;

	std::vector<Pipeline> m_Pipelines;
	std::vector<Subscriber> m_Subscribers;
};
} // namespace Drm
