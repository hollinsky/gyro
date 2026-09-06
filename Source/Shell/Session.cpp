#include "Shell/Session.h"

#include <poll.h>

#include <cerrno>
#include <string>
#include <vector>

namespace
{
// The globals a shell cannot do without, in the order they are looked for, so that a machine missing
// two of them names the first rather than a different one each run.
constexpr std::string_view RequiredInterfaces[] = {
	Wayland::WlCompositor::WireName, Wayland::WlShm::WireName,          Wayland::XdgWmBase::WireName,
	Wayland::WlSeat::WireName,       Wayland::GyroBindingsV1::WireName, Wayland::GyroChromeManagerV1::WireName,
};
} // namespace

// Everything the connection dispatches into. One object per interface bound, held for the life of
// the session because the connection holds their addresses.
//
// **Five small listeners rather than one object implementing five interfaces.** Two of these
// protocols declare an event called `name` — a seat's and an output's — and a class deriving from
// both would answer them with one function, which would compile and would quietly make the panel's
// name the seat's. Separate objects make that impossible to write.
struct Session::Events
{
	struct Advertised
	{
		std::uint32_t Name = 0;
		std::string Interface;
		std::uint32_t Version = 0;
	};

	class RegistryEvents final : public Wayland::WlRegistryListener
	{
	public:
		void OnGlobal(std::uint32_t name, std::string_view interface, std::uint32_t version) override
		{
			Globals.push_back(Advertised{ .Name = name, .Interface = std::string{ interface }, .Version = version });
		}

		// **Nothing.** A global going away is a monitor unplugged or a session ending, and neither is
		// something this shell acts on yet: the bar reads a size when it opens and gyro places it
		// wherever there is an output. What it must not do is tear anything down, because a proxy
		// stays valid until the client destroys it.
		void OnGlobalRemove(std::uint32_t) override {}

		std::vector<Advertised> Globals;
	};

	// The format list arrives at bind whether or not anybody wanted it. `ARGB8888` is guaranteed by
	// the protocol, so nothing here selects — this exists because the event is not optional.
	class ShmEvents final : public Wayland::WlShmIgnoring
	{};

	// **The one event in this file that would end the client if it went unanswered.** A compositor is
	// free to disconnect a client that does not pong, and a shell is the client whose disappearance
	// leaves a person with no way to start anything.
	class ShellEvents final : public Wayland::XdgWmBaseListener
	{
	public:
		void OnPing(std::uint32_t serial) override { Object().Pong(serial); }
	};

	// What the seat says it has. Nothing here acts on it — the bar asks for a keyboard because a
	// launcher without one is not one — and it is recorded so a run against a seat with no keyboard
	// capability says so rather than presenting a bar that swallows every key.
	class SeatEvents final : public Wayland::WlSeatListener
	{
	public:
		void OnCapabilities(Wayland::WlSeatCapability capabilities) override { Capabilities = capabilities; }

		void OnName(std::string_view) override {}

		Wayland::WlSeatCapability Capabilities{};
	};

	class OutputEvents final : public Wayland::WlOutputListener
	{
	public:
		void OnGeometry(
			std::int32_t,
			std::int32_t,
			std::int32_t,
			std::int32_t,
			Wayland::WlOutputSubpixel,
			std::string_view,
			std::string_view,
			Wayland::WlOutputTransform
		) override
		{}

		// The mode flagged current, rather than any mode that arrives: the rest are the modes the
		// connector could be set to, and taking the last one would size the bar against a resolution
		// nothing is displaying.
		void OnMode(Wayland::WlOutputMode flags, std::int32_t width, std::int32_t height, std::int32_t) override
		{
			using enum Wayland::WlOutputMode;

			if ((flags & Current) == Current)
			{
				Width = width;
				Height = height;
			}
		}

		void OnScale(std::int32_t factor) override { Scale = factor > 0 ? factor : 1; }

		void OnName(std::string_view) override {}

		void OnDescription(std::string_view) override {}

		void OnDone() override {}

		std::int32_t Width = 0;
		std::int32_t Height = 0;
		std::int32_t Scale = 1;
	};

	[[nodiscard]] const Advertised* Find(std::string_view interface) const noexcept
	{
		for (const Advertised& global : Registry.Globals)
		{
			if (global.Interface == interface)
			{
				return &global;
			}
		}

		return nullptr;
	}

	RegistryEvents Registry;
	ShmEvents Shm;
	ShellEvents Shell;
	SeatEvents Seat;
	OutputEvents Output;
};

Session::Session() : m_Events{ std::make_unique<Events>() }
{}

Session::~Session() = default;

std::int32_t Session::Scale() const noexcept
{
	return m_Events->Output.Scale;
}

std::int32_t Session::Width() const noexcept
{
	return m_Events->Output.Width;
}

std::int32_t Session::Height() const noexcept
{
	return m_Events->Output.Height;
}

Result<void> Session::Flush()
{
	return m_Connection.Flush();
}

Result<void> Session::Roundtrip()
{
	// A callback that has fired, rather than a count of bytes or a timeout. `wl_display.sync` is
	// answered behind every event already queued, so `done` is the host saying *you have seen
	// everything I had sent when you asked*.
	class Done final : public Wayland::WlCallbackListener
	{
	public:
		void OnDone(std::uint32_t) override { Fired = true; }

		bool Fired = false;
	};

	Done done;

	if (!m_Display.Sync(done).IsValid())
	{
		return Failure(EPROTO, "asking the compositor for a round trip");
	}

	while (!done.Fired)
	{
		if (Result<void> flushed = m_Connection.Flush(); !flushed)
		{
			return flushed;
		}

		pollfd waiting{ .fd = m_Connection.Descriptor().Value, .events = POLLIN, .revents = 0 };

		if (::poll(&waiting, 1, -1) < 0)
		{
			// A signal is not a failure: the shell is woken by whatever the process was sent and the
			// round trip is still owed. Anything else is a socket that has stopped being one.
			if (errno == EINTR)
			{
				continue;
			}

			return Failure(errno, "waiting for the compositor");
		}

		if (Result<void> drained = m_Connection.Drain(); !drained)
		{
			return drained;
		}
	}

	return {};
}

Result<void> Session::Open()
{
	if (Result<void> opened = m_Connection.Open(); !opened)
	{
		return opened;
	}

	m_Display = Wayland::WlDisplay{ m_Connection, Wire::ObjectId::Display, Wayland::WlDisplay::WireVersion };
	m_Registry = m_Display.GetRegistry(m_Events->Registry);

	if (!m_Registry.IsValid())
	{
		return Failure(EPROTO, "asking the compositor for its registry");
	}

	if (Result<void> settled = Roundtrip(); !settled)
	{
		return settled;
	}

	for (const std::string_view interface : RequiredInterfaces)
	{
		if (m_Events->Find(interface) == nullptr)
		{
			// ENOPROTOOPT rather than ENOENT: what is missing is an interface on a connection that
			// opened, and the subject is the whole of the diagnosis — `gyro_chrome_v1` says the shell
			// reached the socket applications reach.
			return Failure(ENOPROTOOPT, "binding what a shell needs", interface);
		}
	}

	const Events::Advertised* const compositor = m_Events->Find(Wayland::WlCompositor::WireName);
	const Events::Advertised* const shm = m_Events->Find(Wayland::WlShm::WireName);
	const Events::Advertised* const shell = m_Events->Find(Wayland::XdgWmBase::WireName);
	const Events::Advertised* const seat = m_Events->Find(Wayland::WlSeat::WireName);
	const Events::Advertised* const bindings = m_Events->Find(Wayland::GyroBindingsV1::WireName);
	const Events::Advertised* const chrome = m_Events->Find(Wayland::GyroChromeManagerV1::WireName);

	m_Compositor = m_Registry.Bind<Wayland::WlCompositor>(compositor->Name, compositor->Version);
	m_Shm = m_Registry.Bind<Wayland::WlShm>(shm->Name, shm->Version, m_Events->Shm);
	m_Shell = m_Registry.Bind<Wayland::XdgWmBase>(shell->Name, shell->Version, m_Events->Shell);
	m_Seat = m_Registry.Bind<Wayland::WlSeat>(seat->Name, seat->Version, m_Events->Seat);
	m_Bindings = m_Registry.Bind<Wayland::GyroBindingsV1>(bindings->Name, bindings->Version);
	m_Chrome = m_Registry.Bind<Wayland::GyroChromeManagerV1>(chrome->Name, chrome->Version);

	// **An output is not required and its absence is not an error.** gyro serves a session before any
	// panel is lit, and a shell that refused to start would be one a person could not use to fix that.
	// What it costs is the size the bar is drawn at, and `Width` answering zero is what the caller
	// falls back from.
	if (const Events::Advertised* const output = m_Events->Find(Wayland::WlOutput::WireName); output != nullptr)
	{
		m_Output = m_Registry.Bind<Wayland::WlOutput>(output->Name, output->Version, m_Events->Output);
	}

	// The second trip is what makes the output's mode and scale readable: those events are sent in
	// answer to the bind above, so they are behind a sync that was asked for before it.
	return Roundtrip();
}
