#include "Protocol/Host.h"

#include <cerrno>
#include <utility>

#include "Protocol/Shell.h"
#include "Protocol/Surface.h"

Result<void> ClientHost::Open(SceneStore& scene, ITextures& textures)
{
	// The texture space is not touched here — nothing is adopted until a client commits, and it arrives
	// again on every `Advance`, which is where a request that needs it runs.
	(void)textures;

	m_Context.SetFloors(m_Floors, m_Server);

	// **A run that binds its own socket authors one floor now; a run under the handover authors one per
	// session, when that session's listener is adopted.** The two are the same object doing the same
	// job and not a branch in the world: a client on a socket gyro bound belongs to no session, and a
	// floor of no session is gyro's own — drawn on every output, exactly as the splash and the pointer
	// are. What the handover path must not do is author one here, because a floor nobody can reach is a
	// root every walk pays for and no window ever hangs under.
	//
	// It is authored before the socket can be reached rather than lazily at the first window, for the
	// reason `SessionFloors::Open` gives: a container created on demand is one whose failure lands in
	// the middle of a client's commit, where the only answer left is to end that client for something
	// gyro did.
	if (m_Listener == HostListener::Own)
	{
		if (const Result<void> floor = m_Floors.Open(scene, SessionId::None); !floor)
		{
			return floor;
		}
	}

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

	m_DmabufGlobal = Wayland::Server::ZwpLinuxDmabufV1::Advertise(*display, DmabufVersion, m_Dmabuf);

	if (m_DmabufGlobal == nullptr)
	{
		// Fatal for `wl_compositor`'s reason: the only way this fails is an allocation refusing at
		// startup, and a compositor that came up missing one global is one whose behaviour depends on
		// which one. The list of formats it advertises may legitimately be empty — that is a machine
		// with no GPU, where a client reads the empty list and draws into shared memory instead — and
		// that is a different thing from the global not being there.
		return Failure(ENOMEM, "advertising zwp_linux_dmabuf_v1");
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

void ClientHost::OnPointerMotion(const PointerMotion& event)
{
	m_Seat.Moved(event.When);
}

void ClientHost::OnPointerButton(const PointerButton& event)
{
	m_Seat.Button(event);
}

void ClientHost::OnPointerScroll(const PointerScroll& event)
{
	m_Seat.Scroll(event);
}

void ClientHost::OnTouch(const TouchEvent& event, Point<GlobalSpace> at)
{
	m_Seat.Touch(event, at);
}

void ClientHost::OnDeviceGone(InputDeviceId device)
{
	m_Seat.Forget(device);
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

	// **Before the pointer and before anything reads a window's position**, because the outputs are what
	// a position means: a client that has just bound `wl_output` is one whose first window is about to be
	// placed, and the scale it lays out at comes from the entry below rather than from the bind.
	//
	// Cheap when nothing moved — the store's outputs are compared by identity and a group is re-sent
	// only where something a client can see has changed — which is what lets it sit on a path that runs
	// on every wakeup.
	if (wl_display* const display = m_Server.Display(); display != nullptr)
	{
		m_Outputs.Sync(*display, scene.Outputs());

		SyncOutputEntry(m_Context, m_Outputs, scene);
	}

	// **After the requests**, because a window that mapped in this wakeup is one the pointer may already
	// be sitting on: a person who clicks the instant an application opens is clicking on the window
	// rather than through it.
	m_Seat.SyncPointer(scene, now);

	// **Beside the pointer and before the focus comparison, because a finger going down moves focus the
	// way a press does** (162). After it rather than before for no reason stronger than the order the
	// devices are usually read in — the two queues are independent, and a hand cannot be on a mouse and
	// a screen in the same wakeup often enough for the order to be a policy.
	m_Seat.SyncTouch(scene);

	// **After the pointer, because the press this routed is one of the things that moves focus** (162),
	// and after the requests for the reason that makes a window typeable in the wakeup it opened in: the
	// commit that maps it is what offers it focus. Both writers run before the comparison, so one
	// iteration sends one `enter` however many times focus changed inside it. See [Seat.h](Seat.h) for
	// why the change is noticed by comparing rather than by a signal out of `Scene`, and for what the
	// other order costs — a keystroke delivered to the window a person just clicked away from.
	const EntityId focused = scene.Focus().Focused();

	m_Seat.SyncFocus(focused);

	// **Beside the seat's comparison and against the same answer**, because the two are one fact told to
	// two different objects: a `wl_keyboard.enter` says where the keys are going and an `activated`
	// state says which titlebar is lit, and a window that got one without the other is one a person can
	// type into and cannot tell they are typing into. [Shell.h](Shell.h) has why a menu leaves its own
	// window activated.
	SyncWindows(m_Context, scene, focused);

	return Wake::Never();
}

Result<void> ClientHost::Listen(HostListener listener, std::string_view socket)
{
	// Remembered because `Open` below has to know whether there will ever be an agent to author a floor
	// per session, or whether this run's clients belong to nobody and want the one floor gyro's own.
	m_Listener = listener;

	if (const Result<void> opened = m_Server.Open(); !opened)
	{
		return opened;
	}

	// **Nothing at all under the handover, and that is the run succeeding rather than a step skipped.**
	// A machine whose session agent has not connected yet — or never will, which is an ordinary state
	// of one — is a compositor with a screen, a frame loop and no clients, and it has to reach the
	// dispatch loop to be able to take the offer when it arrives.
	if (listener == HostListener::Handover)
	{
		return {};
	}

	return m_Server.Bind(socket);
}

Result<std::unique_ptr<ClientHost>> MakeClientHost(HostListener listener, std::string_view socket)
{
	auto host = std::make_unique<ClientHost>();

	if (const Result<void> opened = host->Listen(listener, socket); !opened)
	{
		return std::unexpected{ opened.error() };
	}

	return host;
}
