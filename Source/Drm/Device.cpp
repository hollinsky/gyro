// open, close and the DRM ioctls behind libdrm. POSIX is named for Core/Clock.cpp's reason — what is
// wanted from the platform is stated rather than inherited from a build flag — and the DRM half is
// exactly why this module is not in the portable tier.
#define _POSIX_C_SOURCE 200809L

#include "Drm/Device.h"

#include <drm/drm.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <utility>

#include "Drm/Output.h"

namespace Drm
{
namespace
{
// Where the kernel puts card nodes, and how many of them are worth trying. A literal rather than a
// udev enumeration for Nested/Sync.cpp's reason: what is being looked for is a device with a panel on
// it, and gyro is not building a device database.
constexpr std::string_view DriDirectory = "/dev/dri";
constexpr int MaxCards = 8;

// A libdrm handle released by the right function. Every `drmModeGet*` returns an allocation and the
// error paths through a scan are numerous enough that a `free` per exit is a leak waiting for the
// first `continue` somebody adds.
template<typename T, void (*Free)(T*)>
class Owned
{
public:
	Owned() = default;

	explicit Owned(T* value) noexcept : m_Value{ value } {}

	~Owned()
	{
		if (m_Value != nullptr)
		{
			Free(m_Value);
		}
	}

	Owned(const Owned&) = delete;
	Owned& operator=(const Owned&) = delete;

	Owned(Owned&& other) noexcept : m_Value{ std::exchange(other.m_Value, nullptr) } {}

	Owned& operator=(Owned&& other) noexcept
	{
		if (this != &other)
		{
			if (m_Value != nullptr)
			{
				Free(m_Value);
			}

			m_Value = std::exchange(other.m_Value, nullptr);
		}

		return *this;
	}

	[[nodiscard]] T* Get() const noexcept { return m_Value; }

	[[nodiscard]] T& operator*() const noexcept { return *m_Value; }

	[[nodiscard]] explicit operator bool() const noexcept { return m_Value != nullptr; }

	T* operator->() const noexcept { return m_Value; }

private:
	T* m_Value = nullptr;
};

using OwnedResources = Owned<drmModeRes, drmModeFreeResources>;
using OwnedConnector = Owned<drmModeConnector, drmModeFreeConnector>;
using OwnedEncoder = Owned<drmModeEncoder, drmModeFreeEncoder>;
using OwnedPlaneResources = Owned<drmModePlaneRes, drmModeFreePlaneResources>;
using OwnedPlane = Owned<drmModePlane, drmModeFreePlane>;
using OwnedObjectProperties = Owned<drmModeObjectProperties, drmModeFreeObjectProperties>;
using OwnedProperty = Owned<drmModePropertyRes, drmModeFreeProperty>;
using OwnedBlob = Owned<drmModePropertyBlobRes, drmModeFreePropertyBlob>;

// Every property on one object, by name, with the value each one currently holds.
//
// **Read once per object at startup and never again**, which is what makes a per-frame commit a list
// of integers. The current value is carried out with the id because two of them are wanted at scan
// time — a plane's `type` and its `IN_FORMATS` blob id — and reading them a second time would mean a
// second pass over the same ioctl.
class Properties
{
public:
	Properties(RawFd device, std::uint32_t object, std::uint32_t type)
		: m_Handle{ drmModeObjectGetProperties(device.Value, object, type) }
	{
		if (!m_Handle)
		{
			return;
		}

		m_Names.reserve(m_Handle->count_props);

		for (std::uint32_t index = 0; index < m_Handle->count_props; ++index)
		{
			OwnedProperty property{ drmModeGetProperty(device.Value, m_Handle->props[index]) };

			if (!property)
			{
				continue;
			}

			m_Names.push_back(
				Entry{ .Name = property->name, .Id = property->prop_id, .Value = m_Handle->prop_values[index] }
			);
		}
	}

	[[nodiscard]] std::uint32_t Id(std::string_view name) const noexcept
	{
		const Entry* const entry = Find(name);

		return entry != nullptr ? entry->Id : 0;
	}

	[[nodiscard]] std::uint64_t Value(std::string_view name) const noexcept
	{
		const Entry* const entry = Find(name);

		return entry != nullptr ? entry->Value : 0;
	}

private:
	struct Entry
	{
		std::string Name;
		std::uint32_t Id = 0;
		std::uint64_t Value = 0;
	};

	[[nodiscard]] const Entry* Find(std::string_view name) const noexcept
	{
		for (const Entry& entry : m_Names)
		{
			if (entry.Name == name)
			{
				return &entry;
			}
		}

		return nullptr;
	}

	OwnedObjectProperties m_Handle;
	std::vector<Entry> m_Names;
};

// `eDP-1`, `HDMI-A-2`: the kernel's own name for the connector, assembled the way every other tool on
// the machine assembles it, so that what gyro logs is what `drm_info` prints.
[[nodiscard]] std::string ConnectorName(const drmModeConnector& connector)
{
	const char* const type = drmModeGetConnectorTypeName(connector.connector_type);

	return std::format("{}-{}", type != nullptr ? type : "Unknown", connector.connector_type_id);
}

// What one plane accepts, decoded.
//
// **`IN_FORMATS` is what a modern driver states its real constraints in** — the pairs of format and
// modifier it can actually scan out. Its absence is an old driver, and what it leaves behind is the
// plane's implicit format list under no modifier at all, which is the legacy contract and is the
// honest thing to report rather than inventing `DRM_FORMAT_MOD_LINEAR` on its behalf.
[[nodiscard]] std::vector<PlaneFormat>
PlaneFormats(RawFd device, const drmModePlane& plane, const Properties& properties)
{
	std::vector<PlaneFormat> formats;

	if (const std::uint64_t blobId = properties.Value("IN_FORMATS"); blobId != 0)
	{
		if (OwnedBlob blob{ drmModeGetPropertyBlob(device.Value, static_cast<std::uint32_t>(blobId)) }; blob)
		{
			formats = DecodeFormats(
				std::span{ static_cast<const std::byte*>(blob->data), static_cast<std::size_t>(blob->length) }
			);
		}
	}

	if (formats.empty())
	{
		formats.reserve(plane.count_formats);

		for (std::uint32_t format = 0; format < plane.count_formats; ++format)
		{
			formats.push_back(PlaneFormat{ .Code = plane.formats[format], .Modifiers = { ModifierInvalid } });
		}
	}

	return formats;
}

// Every plane this CRTC is allowed to drive, sorted bottom of the stack first, or empty where it has
// no primary.
//
// **A plane taken by an earlier pipeline is skipped whatever its kind**, because `possible_crtcs` is
// permission rather than exclusivity: a plane legal on two CRTCs would otherwise be handed to both,
// and the second output's commits would be programming the first output's picture.
//
// **A pipeline with no primary is no pipeline.** Decision 152's partition always has a composite in it
// and the composite belongs on the primary, so an inventory of overlays alone describes an output that
// cannot show anything — which is a connector to skip rather than a set of planes to hand out.
[[nodiscard]] std::vector<Plane> ScanPlanes(RawFd device, std::uint32_t crtcIndex, std::span<const std::uint32_t> taken)
{
	OwnedPlaneResources planes{ drmModeGetPlaneResources(device.Value) };

	if (!planes)
	{
		return {};
	}

	std::vector<Plane> inventory;
	bool primary = false;

	for (std::uint32_t index = 0; index < planes->count_planes; ++index)
	{
		const std::uint32_t id = planes->planes[index];

		if (std::ranges::find(taken, id) != taken.end())
		{
			continue;
		}

		OwnedPlane plane{ drmModeGetPlane(device.Value, id) };

		if (!plane || (plane->possible_crtcs & (std::uint32_t{ 1 } << crtcIndex)) == 0)
		{
			continue;
		}

		const Properties properties{ device, id, DRM_MODE_OBJECT_PLANE };

		Plane entry{
			.Id = id,
			.Kind = KindOf(properties.Value("type")),
			.ZPos = properties.Value("zpos"),
			.Formats = PlaneFormats(device, *plane, properties),
			.Props =
				PlaneProperties{
					.FbId = properties.Id("FB_ID"),
					.CrtcId = properties.Id("CRTC_ID"),
					.SrcX = properties.Id("SRC_X"),
					.SrcY = properties.Id("SRC_Y"),
					.SrcW = properties.Id("SRC_W"),
					.SrcH = properties.Id("SRC_H"),
					.CrtcX = properties.Id("CRTC_X"),
					.CrtcY = properties.Id("CRTC_Y"),
					.CrtcW = properties.Id("CRTC_W"),
					.CrtcH = properties.Id("CRTC_H"),
					.InFenceFd = properties.Id("IN_FENCE_FD"),
					.FbDamageClips = properties.Id("FB_DAMAGE_CLIPS"),
				},
		};

		// A plane missing the properties a commit sets is one no layer can be programmed onto. Dropping
		// it costs a promotion; keeping it would cost the whole frame at the first commit that used it.
		if (!entry.Props.IsComplete())
		{
			continue;
		}

		primary = primary || entry.Kind == PlaneKind::Primary;

		inventory.push_back(std::move(entry));
	}

	if (!primary)
	{
		return {};
	}

	// **Sorted by what the driver reports, and ties broken by id so the order is the same on every
	// run.** A device with no `zpos` at all answers zero for every plane, which leaves the kernel's own
	// enumeration order — the one thing available when the hardware states nothing.
	std::ranges::stable_sort(inventory, [](const Plane& left, const Plane& right) noexcept {
		return left.ZPos != right.ZPos ? left.ZPos < right.ZPos : left.Id < right.Id;
	});

	return inventory;
}

// The CRTC that already drives this connector, or the first free one that can. Preferring the current
// one is what makes an adoption possible: the firmware left a mode on a CRTC and gyro is about to
// commit the same one back, which the kernel then does not treat as a modeset at all.
[[nodiscard]] bool AssignCrtc(
	RawFd device,
	const drmModeRes& resources,
	const drmModeConnector& connector,
	std::span<const std::uint32_t> taken,
	std::uint32_t& crtc,
	std::uint32_t& index
)
{
	const auto isFree = [&](std::uint32_t candidate) { return std::ranges::find(taken, candidate) == taken.end(); };

	const auto indexOf = [&](std::uint32_t candidate) -> std::uint32_t {
		for (int slot = 0; slot < resources.count_crtcs; ++slot)
		{
			if (resources.crtcs[slot] == candidate)
			{
				return static_cast<std::uint32_t>(slot);
			}
		}

		return 0;
	};

	if (connector.encoder_id != 0)
	{
		if (OwnedEncoder encoder{ drmModeGetEncoder(device.Value, connector.encoder_id) };
		    encoder && encoder->crtc_id != 0 && isFree(encoder->crtc_id))
		{
			crtc = encoder->crtc_id;
			index = indexOf(crtc);

			return true;
		}
	}

	for (int slot = 0; slot < connector.count_encoders; ++slot)
	{
		OwnedEncoder encoder{ drmModeGetEncoder(device.Value, connector.encoders[slot]) };

		if (!encoder)
		{
			continue;
		}

		for (int candidate = 0; candidate < resources.count_crtcs; ++candidate)
		{
			if ((encoder->possible_crtcs & (std::uint32_t{ 1 } << candidate)) == 0)
			{
				continue;
			}

			if (!isFree(resources.crtcs[candidate]))
			{
				continue;
			}

			crtc = resources.crtcs[candidate];
			index = static_cast<std::uint32_t>(candidate);

			return true;
		}
	}

	return false;
}

// Everything connected on this device, with a CRTC and a plane each. A connector that cannot be given
// one is skipped rather than reported: a laptop panel and an external monitor on a card with one CRTC
// is an ordinary machine, and the one that got a CRTC still has to light up.
[[nodiscard]] std::vector<Pipeline> ScanPipelines(RawFd device)
{
	std::vector<Pipeline> pipelines;

	OwnedResources resources{ drmModeGetResources(device.Value) };

	if (!resources)
	{
		return pipelines;
	}

	std::vector<std::uint32_t> takenCrtcs;
	std::vector<std::uint32_t> takenPlanes;

	for (int index = 0; index < resources->count_connectors; ++index)
	{
		OwnedConnector connector{ drmModeGetConnector(device.Value, resources->connectors[index]) };

		if (!connector || connector->connection != DRM_MODE_CONNECTED || connector->count_modes == 0)
		{
			continue;
		}

		Pipeline pipeline{};
		pipeline.Connector = connector->connector_id;
		pipeline.Name = ConnectorName(*connector);
		pipeline.WidthMm = connector->mmWidth;
		pipeline.HeightMm = connector->mmHeight;
		pipeline.Modes.assign(connector->modes, connector->modes + connector->count_modes);

		if (!AssignCrtc(device, *resources, *connector, takenCrtcs, pipeline.Crtc, pipeline.CrtcIndex))
		{
			continue;
		}

		pipeline.Planes = ScanPlanes(device, pipeline.CrtcIndex, takenPlanes);

		if (pipeline.Planes.empty())
		{
			continue;
		}

		const Properties connectorProperties{ device, pipeline.Connector, DRM_MODE_OBJECT_CONNECTOR };
		const Properties crtcProperties{ device, pipeline.Crtc, DRM_MODE_OBJECT_CRTC };

		pipeline.ConnectorProps = ConnectorProperties{
			.CrtcId = connectorProperties.Id("CRTC_ID"),
			.Dpms = connectorProperties.Id("DPMS"),
		};

		pipeline.CrtcProps = CrtcProperties{
			.ModeId = crtcProperties.Id("MODE_ID"),
			.Active = crtcProperties.Id("ACTIVE"),
			.VrrEnabled = crtcProperties.Id("VRR_ENABLED"),
		};

		if (pipeline.ConnectorProps.CrtcId == 0 || pipeline.CrtcProps.ModeId == 0 || pipeline.CrtcProps.Active == 0)
		{
			continue;
		}

		takenCrtcs.push_back(pipeline.Crtc);

		for (const Plane& plane : pipeline.Planes)
		{
			takenPlanes.push_back(plane.Id);
		}
		pipelines.push_back(std::move(pipeline));
	}

	return pipelines;
}

} // namespace

Result<std::unique_ptr<DrmDevice>> DrmDevice::OpenNode(const std::string& path, bool requireConnector)
{
	// Non-blocking because the frame thread drains this file and must never sit in a read. Cloexec
	// because gyro spawns nothing today and a DRM master descriptor leaking into a child is how a
	// display ends up owned by a process nobody can find.
	Fd device{ ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK) };

	if (!device.IsValid())
	{
		return Failure(errno, std::format("opening {}", path));
	}

	// **Master is first-open, and this is the confirmation rather than the claim.** A node nobody else
	// holds makes the opener master; where something else already has it this fails, and the sentence
	// naming that is worth more than the atomic commit's later `EACCES`.
	if (::drmSetMaster(device.Get()) != 0 && ::drmIsMaster(device.Get()) == 0)
	{
		return Failure(EACCES, std::format("{} already has a DRM master", path));
	}

	// Universal planes first: the atomic cap implies it on current kernels and did not always, and the
	// order costs nothing. The atomic cap is the one that decides whether this backend can run at all.
	::drmSetClientCap(device.Get(), DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

	if (::drmSetClientCap(device.Get(), DRM_CLIENT_CAP_ATOMIC, 1) != 0)
	{
		return Failure(ENOTSUP, std::format("{} does not accept atomic commits", path));
	}

	auto built = std::make_unique<DrmDevice>();
	built->m_Path = path;

	// The minor the render device is paired against. A failure here is not a reason to refuse the
	// card: it costs the pairing and nothing else, and the composition root says so in one line.
	if (struct stat node{}; ::fstat(device.Get(), &node) == 0)
	{
		built->m_Minor = static_cast<std::int64_t>(::minor(node.st_rdev));
	}

	built->m_Pipelines = ScanPipelines(device.Borrow());

	if (requireConnector && built->m_Pipelines.empty())
	{
		return Failure(ENODEV, std::format("{} has nothing connected", path));
	}

	std::uint64_t monotonic = 0;
	built->m_Monotonic = ::drmGetCap(device.Get(), DRM_CAP_TIMESTAMP_MONOTONIC, &monotonic) == 0 && monotonic != 0;

	if (drmVersionPtr version = ::drmGetVersion(device.Get()); version != nullptr)
	{
		built->m_Driver.assign(version->name, static_cast<std::size_t>(version->name_len));
		::drmFreeVersion(version);
	}

	built->m_Device = std::move(device);

	return built;
}

Result<std::unique_ptr<DrmDevice>> DrmDevice::Open(std::string_view path)
{
	if (!path.empty())
	{
		return OpenNode(std::string{ path }, false);
	}

	// **The first card with something plugged into it, and the loop is why that is not the first card.**
	// A laptop with a discrete GPU has two card nodes, and on this machine the panel is on the second.
	// Opening card0 and finding no connector is a compositor that comes up and shows nothing, which on
	// a boot service is indistinguishable from a hang.
	Error last = Failure(ENODEV, "no DRM device with a connected connector").error();

	for (int index = 0; index < MaxCards; ++index)
	{
		const std::string candidate = std::format("{}/card{}", DriDirectory, index);

		Result<std::unique_ptr<DrmDevice>> opened = OpenNode(candidate, true);

		if (opened)
		{
			return opened;
		}

		// ENOENT is a card number this machine does not have, which is the ordinary way this loop ends
		// and not a reason to report anything.
		if (opened.error().Code() != ENOENT)
		{
			last = opened.error();
		}
	}

	return std::unexpected{ last };
}

void DrmDevice::Attach(std::uint32_t crtc, DrmOutput& output)
{
	Detach(crtc);

	m_Subscribers.push_back(Subscriber{ .Crtc = crtc, .Output = &output });
}

void DrmDevice::Detach(std::uint32_t crtc) noexcept
{
	std::erase_if(m_Subscribers, [crtc](const Subscriber& subscriber) { return subscriber.Crtc == crtc; });
}

Result<void> DrmDevice::Drain()
{
	if (!m_Device.IsValid())
	{
		return Failure(ENODEV, "draining a device that was never opened");
	}

	// **The read is gyro's rather than `drmHandleEvent`'s, for one reason and it is not style.** That
	// function reports a read of zero bytes and a read that failed identically, as *nothing happened*
	// — so a loop draining to empty around it spins forever the moment the device disappears, on the
	// `SCHED_FIFO` thread, which takes the machine with it. Reading the events directly is forty lines
	// and the two cases are distinguishable. The rest of this backend is libdrm's.
	//
	// Sized for a burst of completions rather than for one: a four-CRTC card that has been away for a
	// scheduling quantum has four of these waiting, and they are 32 bytes each.
	std::array<std::byte, 256> buffer{};

	while (true)
	{
		const ssize_t read = ::read(m_Device.Get(), buffer.data(), buffer.size());

		if (read < 0)
		{
			// Empty is the ordinary end of a drain and the ordinary result of a wakeup some other source
			// caused. EINTR is a signal arriving mid-read, which at shutdown is the expected case.
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			{
				return {};
			}

			return Failure(errno, "reading DRM events");
		}

		if (read == 0)
		{
			// The device is gone. Seam/EventSource.h's vocabulary for that is ENODEV, and it is the
			// composition root's problem rather than the loop's.
			return Failure(ENODEV, "the DRM device stopped answering");
		}

		std::size_t offset = 0;

		while (offset + sizeof(drm_event) <= static_cast<std::size_t>(read))
		{
			drm_event header{};
			std::memcpy(&header, buffer.data() + offset, sizeof(header));

			if (header.length < sizeof(drm_event) || offset + header.length > static_cast<std::size_t>(read))
			{
				// A header that does not describe itself. Stopping is the only safe response: the next
				// offset is unknowable, and reading on would parse the tail as a fresh event.
				return Failure(EPROTO, "a DRM event whose length does not fit the read");
			}

			if (header.type == DRM_EVENT_FLIP_COMPLETE && header.length >= sizeof(drm_event_vblank))
			{
				drm_event_vblank flip{};
				std::memcpy(&flip, buffer.data() + offset, sizeof(flip));

				Complete(flip.crtc_id, flip.sequence, flip.tv_sec, flip.tv_usec);
			}

			offset += header.length;
		}
	}
}

void DrmDevice::Complete(std::uint32_t crtc, std::uint32_t sequence, std::uint32_t seconds, std::uint32_t microseconds)
{
	// Resolved by CRTC rather than carried as the commit's user data, because the two disagree exactly
	// where it matters: a completion for a CRTC no output drives is a stale event from a pipeline that
	// has been torn down, and dereferencing a pointer the kernel has been holding since before the
	// teardown is the crash that would follow. The lookup is over single digits.
	for (const Subscriber& subscriber : m_Subscribers)
	{
		if (subscriber.Crtc != crtc)
		{
			continue;
		}

		// Decision 57: the conversion into the timebase happens at ingest and nowhere else. What arrives
		// here is `CLOCK_MONOTONIC` microseconds where the device said so, and a timestamp taken when the
		// driver noticed where it did not — which is why `HasMonotonicTimestamps` is carried into the
		// report beside it rather than assumed.
		const Instant presented =
			Monotonic::FromMicroseconds(static_cast<std::int64_t>(seconds) * 1'000'000 + microseconds);

		subscriber.Output->OnPresented(presented, sequence, m_Monotonic);

		return;
	}
}
} // namespace Drm
