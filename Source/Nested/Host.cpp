// poll is POSIX, and it is here for the one thing a nested session cannot do without: a startup
// question whose answer arrives on a socket, asked before there is a loop to arrive into.
#define _POSIX_C_SOURCE 200809L

#include "Nested/Host.h"

#include <poll.h>
#include <spdlog/spdlog.h>
#include <time.h>

#include <algorithm>
#include <cerrno>

#include "Core/Clock.h"
#include "Nested/Output.h"

namespace Nested
{
namespace
{
// How long a startup question may go unanswered before gyro says so.
//
// Generous, because a host that is itself starting up is the ordinary case and a false failure here
// is a session that will not come up. Finite, because the alternative is a process wedged before its
// first frame with nothing in the log — and this is the backend somebody is developing in, so the
// answer they need is *the host stopped talking* rather than a hang.
constexpr Duration RoundtripTimeout = std::chrono::seconds{ 5 };

// The lowest `zwp_linux_dmabuf_v1` that has feedback. Below it there are only the flat `format` and
// `modifier` events, which say what the host can import and not what it *prefers* or which device it
// wants the import on — and decision 120 rests on being told the constraint rather than inferring it.
constexpr std::uint32_t FeedbackVersion = 4;

// Bind at what both ends know. A host is entitled to advertise less than these bindings describe, and
// a request newer than the bound version is refused by the generated code before it reaches the wire.
[[nodiscard]] constexpr std::uint32_t Negotiated(std::uint32_t advertised, std::uint32_t supported) noexcept
{
	return advertised < supported ? advertised : supported;
}
} // namespace

void NestedHost::Registry::OnGlobal(std::uint32_t name, std::string_view interface, std::uint32_t version)
{
	// Only what gyro binds. A desktop advertises forty globals and a table that grew with somebody
	// else's protocol suite would be a vector allocating on the connection's behalf for nothing.
	static constexpr std::string_view Wanted[] = {
		Wayland::WlCompositor::WireName,
		Wayland::XdgWmBase::WireName,
		Wayland::ZwpLinuxDmabufV1::WireName,
		Wayland::WpPresentation::WireName,
		Wayland::WpLinuxDrmSyncobjManagerV1::WireName,
		Wayland::ZxdgDecorationManagerV1::WireName,
	};

	if (std::ranges::find(Wanted, interface) == std::ranges::end(Wanted) || m_Count >= m_Slots.size())
	{
		return;
	}

	// The interface name is a view into the receive buffer and is valid only for this call, so what is
	// kept is the *literal* the comparison matched — which has static storage and outlives everything.
	const std::string_view stable = *std::ranges::find(Wanted, interface);

	// A host may advertise one interface twice; the highest version wins, which is the one gyro would
	// rather have.
	for (std::size_t index = 0; index < m_Count; ++index)
	{
		if (m_Slots[index].Interface == stable)
		{
			if (version > m_Slots[index].Global.Version)
			{
				m_Slots[index].Global = Advertised{ .Name = name, .Version = version };
			}

			return;
		}
	}

	m_Slots[m_Count] = Slot{ .Interface = stable, .Global = Advertised{ .Name = name, .Version = version } };
	++m_Count;
}

void NestedHost::Registry::OnGlobalRemove(std::uint32_t name)
{
	// Recorded rather than acted on. A global gyro has already bound stays usable until it is
	// destroyed — the protocol says requests to it are ignored rather than fatal — and nothing here
	// rebinds, so forgetting the advertisement is the whole of the response.
	for (std::size_t index = 0; index < m_Count; ++index)
	{
		if (m_Slots[index].Global.Name == name)
		{
			m_Slots[index] = m_Slots[m_Count - 1];
			--m_Count;

			return;
		}
	}
}

NestedHost::Registry::Advertised NestedHost::Registry::Find(std::string_view interface) const noexcept
{
	for (std::size_t index = 0; index < m_Count; ++index)
	{
		if (m_Slots[index].Interface == interface)
		{
			return m_Slots[index].Global;
		}
	}

	return {};
}

NestedHost::~NestedHost() = default;

RawFd NestedHost::Descriptor() const noexcept
{
	return m_Connection.Descriptor();
}

Result<void> NestedHost::Open()
{
	if (const Result<void> opened = m_Connection.Open(); !opened)
	{
		return opened;
	}

	const Wayland::WlDisplay display{ m_Connection, Wire::ObjectId::Display, Wayland::WlDisplay::WireVersion };
	const Wayland::WlRegistry registry = display.GetRegistry(m_Registry);

	if (!registry.IsValid())
	{
		return Failure(ENOTCONN, "asking the wayland host for its registry");
	}

	// The first roundtrip is what turns *the host will tell us eventually* into *the host has told
	// us*: every global is announced before the sync it was asked for answers.
	if (const Result<void> settled = Roundtrip(); !settled)
	{
		return settled;
	}

	if (const Result<void> bound = Bind(); !bound)
	{
		return bound;
	}

	// The second is for the feedback, which is a conversation of its own — a table, a device, a
	// tranche per preference band — and which nothing can be allocated without.
	m_FeedbackObject = m_Globals.Dmabuf.GetDefaultFeedback(m_Feedback);

	if (const Result<void> settled = Roundtrip(); !settled)
	{
		return settled;
	}

	if (m_Feedback.Generations() == 0 || m_Feedback.Support().IsEmpty())
	{
		return Failure(ENOPROTOOPT, "the wayland host offered no dmabuf format for a nested output to allocate");
	}

	// `clock_id` rides the same roundtrip. Decision 57 converts at ingest and there is nothing to
	// convert from if the host is quoting a clock this process does not read, so this is recorded
	// rather than assumed and the honesty lands in `PresentationInfo::HardwareClock`.
	m_Monotonic = m_Presentation.Clock == static_cast<std::uint32_t>(CLOCK_MONOTONIC);

	if (!m_Monotonic)
	{
		spdlog::warn(
			"the host presents against clock {} rather than CLOCK_MONOTONIC, so its timestamps are not in gyro's "
			"timebase and the frame clock will not treat them as precise",
			m_Presentation.Clock
		);
	}

	spdlog::info("host dmabuf support: {}", Describe(m_Feedback.Support()));

	// The node the release timelines are minted on, named by the tranche gyro is most likely to
	// allocate from. Absence is a fallback rather than a failure — Nested/Output.h holds the commit
	// instead — so this reports and carries on.
	const std::uint64_t device = m_Feedback.Support().Tranches.front().Device;

	if (Result<DrmSyncobjDevice> node = DrmSyncobjDevice::Open(device); node)
	{
		m_SyncDevice = std::move(*node);
	}
	else if (m_Globals.Syncobj.IsValid())
	{
		spdlog::warn("no DRM node for release timelines: {}", node.error());
	}

	if (HasExplicitSync())
	{
		spdlog::info("explicit sync is live, with release timelines on {}", m_SyncDevice.Path());
	}
	else
	{
		// Said once and said plainly, because it changes what every later number means. A commit that
		// waits for the composite before it goes out is a frame the host learns about after it was
		// drawn rather than while it was being drawn, and the presentation timestamps that come back
		// carry gyro's own polling inside them.
		spdlog::warn(
			"no explicit sync{}: commits are held until the composite lands, so this window runs a frame behind "
			"and its pacing figures are not to be trusted",
			m_Globals.Syncobj.IsValid() ? " device" : " protocol on this host"
		);
	}

	return {};
}

Result<void> NestedHost::Bind()
{
	const Wayland::WlRegistry registry = m_Registry.Object();

	struct Required
	{
		std::string_view Interface;
		std::uint32_t Minimum;
	};

	// What a nested output cannot be built without, each with the version that makes it useful rather
	// than merely present. `wl_compositor` 4 is `damage_buffer`, which is what makes damage mean the
	// buffer's own coordinates rather than the surface's.
	static constexpr Required Needed[] = {
		{ Wayland::WlCompositor::WireName, 4 },
		{ Wayland::XdgWmBase::WireName, 1 },
		{ Wayland::ZwpLinuxDmabufV1::WireName, FeedbackVersion },
		{ Wayland::WpPresentation::WireName, 1 },
	};

	for (const Required& required : Needed)
	{
		const Registry::Advertised global = m_Registry.Find(required.Interface);

		if (global.Version < required.Minimum)
		{
			// The interface and the version, because those are the two different failures and the fix
			// differs: a missing global is a host that cannot host gyro, and a version too low is one
			// that is out of date.
			return Failure(
				ENOPROTOOPT,
				global.Version == 0 ? "the wayland host does not offer an interface a nested output needs" :
									  "the wayland host offers an interface at a version too old for a nested output"
			);
		}
	}

	const Registry::Advertised compositor = m_Registry.Find(Wayland::WlCompositor::WireName);
	const Registry::Advertised shell = m_Registry.Find(Wayland::XdgWmBase::WireName);
	const Registry::Advertised dmabuf = m_Registry.Find(Wayland::ZwpLinuxDmabufV1::WireName);
	const Registry::Advertised presentation = m_Registry.Find(Wayland::WpPresentation::WireName);

	m_Globals.Compositor = registry.Bind<Wayland::WlCompositor>(compositor.Name, Negotiated(compositor.Version, 6));
	m_Globals.Shell = registry.Bind<Wayland::XdgWmBase>(shell.Name, Negotiated(shell.Version, 6), m_Shell);
	m_Globals.Dmabuf =
		registry.Bind<Wayland::ZwpLinuxDmabufV1>(dmabuf.Name, Negotiated(dmabuf.Version, 5), m_DmabufEvents);
	m_Globals.Presentation =
		registry.Bind<Wayland::WpPresentation>(presentation.Name, Negotiated(presentation.Version, 1), m_Presentation);

	// Optional, both of them, and both absent on a host that is perfectly usable without.
	if (const Registry::Advertised syncobj = m_Registry.Find(Wayland::WpLinuxDrmSyncobjManagerV1::WireName);
	    syncobj.Version != 0)
	{
		m_Globals.Syncobj = registry.Bind<Wayland::WpLinuxDrmSyncobjManagerV1>(syncobj.Name, 1);
	}

	if (const Registry::Advertised decoration = m_Registry.Find(Wayland::ZxdgDecorationManagerV1::WireName);
	    decoration.Version != 0)
	{
		m_Globals.Decoration = registry.Bind<Wayland::ZxdgDecorationManagerV1>(decoration.Name, 1);
	}

	if (!m_Globals.Compositor.IsValid() || !m_Globals.Shell.IsValid() || !m_Globals.Dmabuf.IsValid() ||
	    !m_Globals.Presentation.IsValid())
	{
		return Failure(ENOPROTOOPT, "binding the wayland globals a nested output needs");
	}

	return {};
}

void NestedHost::Adopt(NestedOutput& output)
{
	if (m_Count < m_Outputs.size())
	{
		m_Outputs[m_Count] = &output;
		++m_Count;
	}
}

Result<void> NestedHost::Flush()
{
	return m_Connection.Flush();
}

Result<void> NestedHost::Drain()
{
	const Result<void> read = m_Connection.Drain();

	// The outputs are settled even where the read failed, so that a configure or a completion that
	// *did* arrive before the failure is acted on rather than dropped along with it. Nothing they do
	// can reach the wire afterwards — the connection latches its failure and every verb returns it —
	// so this is the drain finishing its own work rather than a second chance.
	for (std::size_t index = 0; index < m_Count; ++index)
	{
		m_Outputs[index]->Settle();
	}

	if (!read)
	{
		return read;
	}

	// Last, per the header: one `sendmsg` for the pong, the ack, and whatever commit was waiting.
	return Flush();
}

Instant NestedHost::NextEvent() const noexcept
{
	Instant soonest{ Duration::max() };

	for (std::size_t index = 0; index < m_Count; ++index)
	{
		const Instant event = m_Outputs[index]->NextEvent();

		soonest = event < soonest ? event : soonest;
	}

	return soonest;
}

Result<void> NestedHost::Pump(Duration within)
{
	const RawFd socket = m_Connection.Descriptor();

	if (!socket.IsValid())
	{
		return Failure(ENOTCONN, "waiting on a wayland connection that is not open");
	}

	::pollfd watched{ .fd = socket.Value, .events = POLLIN, .revents = 0 };

	const std::int64_t milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(within).count();
	const int ready = ::poll(&watched, 1, static_cast<int>(std::max<std::int64_t>(milliseconds, 0)));

	if (ready < 0)
	{
		// A signal is not a failure. gyro installs SIGINT and SIGTERM handlers before it does anything
		// else, so this is the ordinary interruption rather than an exotic one, and the caller's
		// deadline is what decides whether to try again.
		return errno == EINTR ? Result<void>{} : Failure(errno, "waiting for the wayland host to answer");
	}

	if (ready == 0)
	{
		return Failure(ETIMEDOUT, "the wayland host stopped answering");
	}

	return m_Connection.Drain();
}

Result<void> NestedHost::Roundtrip()
{
	Barrier barrier;

	const Wayland::WlDisplay display{ m_Connection, Wire::ObjectId::Display, Wayland::WlDisplay::WireVersion };
	const Wayland::WlCallback callback = display.Sync(barrier);

	if (!callback.IsValid())
	{
		return Failure(ENOTCONN, "marshalling a wayland roundtrip");
	}

	if (const Result<void> flushed = Flush(); !flushed)
	{
		return flushed;
	}

	// The deadline is the whole roundtrip's rather than one `poll`'s, so a host that dribbles out a
	// byte at a time cannot extend it indefinitely — and `EINTR` costs the interruption rather than
	// resetting the clock.
	MonotonicClock clock;
	const Instant deadline = Advanced(clock.Now(), RoundtripTimeout);

	while (!barrier.Done)
	{
		const Instant now = clock.Now();

		if (now >= deadline)
		{
			return Failure(ETIMEDOUT, "the wayland host stopped answering");
		}

		if (const Result<void> pumped = Pump(Elapsed(now, deadline)); !pumped)
		{
			return pumped;
		}
	}

	// The host destroys a `wl_callback` the moment it has answered, so the id is freed on its side
	// already; unbinding here is what lets Wire/Connection.h recycle it when the `delete_id` lands.
	m_Connection.Unbind(callback.Id());

	return {};
}
} // namespace Nested
