#include "Protocol/Host.h"

#include <cerrno>
#include <utility>

#include "Protocol/Surface.h"

Result<void> ClientHost::Open(SceneStore& scene, ITextures& textures)
{
	// The texture space is not touched here — nothing is adopted until a client commits, and it arrives
	// again on every `Advance`, which is where a request that needs it runs.
	(void)textures;

	// **The floor is authored before the socket can be reached rather than lazily at the first
	// window.** A container created on demand is one whose failure lands in the middle of a client's
	// commit, where the only answer left is to end that client for something gyro did; here it is a
	// compositor that says so and does not start.
	if (const Result<void> floor = m_Floor.Open(scene); !floor)
	{
		return floor;
	}

	m_Context.SetFloor(m_Floor.Container());

	wl_display* const display = m_Server.Display();

	if (display == nullptr)
	{
		return Failure(EBADF, "advertising globals on a Wayland server that was never opened");
	}

	m_CompositorGlobal = Wayland::Server::WlCompositor::Advertise(*display, CompositorVersion, m_Compositor);

	if (m_CompositorGlobal == nullptr)
	{
		// A compositor with no `wl_compositor` is a socket clients connect to and cannot use, which is
		// worse than one that refused to start: a person sees applications failing to open with no
		// message anywhere that says why. So this is fatal rather than degraded.
		return Failure(ENOMEM, "advertising wl_compositor");
	}

	m_ShmGlobal = Wayland::Server::WlShm::Advertise(*display, ShmVersion, m_Shm);

	if (m_ShmGlobal == nullptr)
	{
		// Fatal for `wl_compositor`'s reason and one more: `wl_shm` is the only way a client hands over
		// pixels at all, so a compositor without it is one every application connects to and then hangs
		// against, with nothing on screen and nothing in a log to explain it.
		return Failure(ENOMEM, "advertising wl_shm");
	}

	m_ShellGlobal = Wayland::Server::XdgWmBase::Advertise(*display, ShellVersion, m_Shell);

	if (m_ShellGlobal == nullptr)
	{
		// Fatal for the same reason as the other two: a toolkit that finds no shell exits rather than
		// drawing, so a compositor missing this one is a socket every application connects to and
		// immediately abandons.
		return Failure(ENOMEM, "advertising xdg_wm_base");
	}

	m_DataGlobal = Wayland::Server::WlDataDeviceManager::Advertise(*display, DataDeviceManagerVersion, m_Data);

	if (m_DataGlobal == nullptr)
	{
		// Fatal, and it is the one global here that transfers nothing: GTK refuses to open a display
		// without it, so a compositor that came up missing this would start, log nothing, and be a
		// socket every GTK application walks away from. [Data.h](Data.h) carries the rest.
		return Failure(ENOMEM, "advertising wl_data_device_manager");
	}

	// **The seat is opened before it is advertised, and a failure to compile the layout is fatal.** A
	// seat that hands a client a keymap descriptor it cannot map is worse than no seat at all: the
	// client believes it has a keyboard, and the compositor's own log is the only place the reason
	// exists. [Keymap.h](Keymap.h) has what makes this fail — XKB data missing from the machine, or an
	// environment naming a layout that does not exist.
	if (const Result<void> seat = m_Seat.Open(*display); !seat)
	{
		return seat;
	}

	m_SeatGlobal = Wayland::Server::WlSeat::Advertise(*display, SeatVersion, m_Seat);

	if (m_SeatGlobal == nullptr)
	{
		// Fatal, though less obviously than the others: a client that finds no seat starts and draws.
		// What it cannot do is be used, and a window a person can see and cannot type into is the state
		// this compositor exists to avoid rather than one to run in.
		return Failure(ENOMEM, "advertising wl_seat");
	}

	return {};
}

void ClientHost::OnKey(const KeyEvent& event, bool consumed)
{
	m_Seat.Key(event, consumed);
}

void ClientHost::OnReached(EntityId entity, Instant at)
{
	if (ClientSurface* const surface = m_Context.SurfaceOf(entity); surface != nullptr)
	{
		surface->Present(at);
	}
}

Wake ClientHost::Advance(SceneStore& scene, ITextures& textures, Instant now)
{
	(void)now;

	// The world, reachable for exactly the length of this call. Every request below runs inside the
	// dispatch, so a `wl_surface.commit` finds the texture space on the stack rather than in a
	// reference this object had to keep — which is the arrangement `ISceneAuthor` is shaped for and
	// [Context.h](Context.h) carries the argument for.
	const HostContext::Dispatching dispatching{ m_Context, scene, textures };

	// **A failed dispatch is swallowed here and cannot be otherwise**, which is `ISceneAuthor`'s shape
	// rather than an omission: `Advance` runs on every wake and returns no `Result`, because a failure
	// on that path is one nothing is in a position to act on. What `Server::Poll` calls a failure is the
	// event loop itself faulting — a client behaving badly is ended inside libwayland and never arrives
	// here — so the reachable case is a broken descriptor, and the answer to that is the same as the
	// answer to no clients at all: author nothing and wait.
	[[maybe_unused]] const Result<void> polled = m_Server.Poll();

	// **After the requests rather than before them**, which is the ordering that makes a window
	// typeable in the wakeup it opened in: the commit that maps it is what offers it focus, so the
	// comparison has to run downstream of the dispatch that performed it. See [Seat.h](Seat.h) for why
	// the change is noticed by comparing rather than by a signal out of `Scene`.
	m_Seat.SyncFocus(scene.Focus().Focused());

	return Wake::Never();
}

Result<std::unique_ptr<ClientHost>> MakeClientHost(std::string_view socket)
{
	auto host = std::make_unique<ClientHost>();

	if (const Result<void> opened = host->Listen(socket); !opened)
	{
		return std::unexpected{ opened.error() };
	}

	return host;
}
