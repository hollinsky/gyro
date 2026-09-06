#include "Protocol/Host.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <utility>

#include "Protocol/Presentation.h"
#include "Protocol/Shell.h"
#include "Protocol/Surface.h"
#include "Protocol/Viewporter.h"

Result<void> ClientHost::Open(SceneStore& scene, ITextures& textures)
{
	// The texture space is read once and not retained — nothing is adopted until a client commits, and
	// it arrives again on every `Advance`, which is where a request that needs it runs. What is taken
	// here is the pair of facts a client needs *before* it has committed anything: which layouts gyro
	// will import and which device to allocate them on. See the dmabuf global below.
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

	m_SubcompositorGlobal =
		Wayland::Server::WlSubcompositor::Advertise(*display, SubcompositorVersion, m_Subcompositor);

	if (m_SubcompositorGlobal == nullptr)
	{
		// Fatal for `wl_compositor`'s reason. A toolkit that finds no `wl_subcompositor` mostly still
		// draws — GTK falls back to compositing its own decorations — but a video player will not put a
		// frame on a plane, and a compositor whose behaviour depends on which global happened to be
		// advertised is one nobody can reason about from a bug report.
		return Failure(ENOMEM, "advertising wl_subcompositor");
	}

	m_ViewporterGlobal = Wayland::Server::WpViewporter::Advertise(*display, ViewporterVersion, m_Viewporter);

	if (m_ViewporterGlobal == nullptr)
	{
		// Fatal for `wl_compositor`'s reason, and this one is not a degradation a client works around:
		// a toolkit that states its surface size with a viewport destination and finds no viewporter
		// sends no size at all, so the window arrives at its buffer's pixel count — which on a 2x
		// output is a window twice as wide and twice as tall as the person asked for. [Viewporter.h]
		// (Viewporter.h) has the shape of that, and it is Firefox.
		return Failure(ENOMEM, "advertising wp_viewporter");
	}

	m_PresentationGlobal = Wayland::Server::WpPresentation::Advertise(*display, PresentationVersion, m_Presentation);

	if (m_PresentationGlobal == nullptr)
	{
		// Fatal for `wl_compositor`'s reason, and one that is specific to this compositor: gyro's own
		// nested backend requires this global of whatever it is a client of, so a run that came up
		// without it is one gyro cannot be developed inside. A client that merely wants pixels on screen
		// degrades — it paces off frame callbacks and cannot measure its own latency — but a compositor
		// whose set of globals depends on which allocation succeeded at startup is the thing
		// [Host.h](Host.h) refuses across the board.
		return Failure(ENOMEM, "advertising wp_presentation");
	}

	m_ShmGlobal = Wayland::Server::WlShm::Advertise(*display, ShmVersion, m_Shm);

	if (m_ShmGlobal == nullptr)
	{
		// Fatal for `wl_compositor`'s reason and one more: `wl_shm` is the only way a client hands over
		// pixels at all, so a compositor without it is one every application connects to and then hangs
		// against, with nothing on screen and nothing in a log to explain it.
		return Failure(ENOMEM, "advertising wl_shm");
	}

	// **Built before the global is advertised, because the version it goes up at is an answer about
	// what was built.** A failure here is a descriptor the kernel refused, and it degrades rather than
	// stops: the global still appears, at version 3, carrying the pair list. What that costs is the
	// thing this whole path exists for — a client on Mesa has no other way to learn which device to
	// allocate against, so it falls back to software rendering — so it is a warning with the cause in
	// it rather than a silence.
	if (const Result<void> feedback = m_Dmabuf.Describe(textures); !feedback)
	{
		spdlog::warn(
			"no dmabuf feedback, so clients cannot be told which device to allocate against and will "
			"render in software: {}",
			feedback.error()
		);
	}

	m_DmabufGlobal = Wayland::Server::ZwpLinuxDmabufV1::Advertise(*display, m_Dmabuf.Version(), m_Dmabuf);

	if (m_DmabufGlobal == nullptr)
	{
		// Fatal for `wl_compositor`'s reason: the only way this fails is an allocation refusing at
		// startup, and a compositor that came up missing one global is one whose behaviour depends on
		// which one. The list of formats it advertises may legitimately be empty — that is a machine
		// with no GPU, where a client reads the empty list and draws into shared memory instead — and
		// that is a different thing from the global not being there.
		return Failure(ENOMEM, "advertising zwp_linux_dmabuf_v1");
	}

	// **Explicit synchronization, and its absence is a degradation rather than a failure.** The global
	// exists only where a DRM node opened and the kernel can arm a wait on a syncobj point; on a machine
	// with neither there is nothing to synchronize — no dmabuf clients, because the feedback above named
	// no device — and a global that would have to break the ordering it promises is worse than none.
	//
	// The node is the one clients were just told to allocate against, which is the same number for the
	// same reason it is in [Nested/Sync.h](../Nested/Sync.h): a syncobj is DRM core, so any node can
	// import a handle, and matching the client's own device is a courtesy rather than a requirement.
	if (const Result<void> sync = m_Sync.Open(textures.MainDevice(), *m_Server.EventLoop()); !sync)
	{
		spdlog::info(
			"no explicit synchronization, so clients fall back to implicit fences and a late one can "
			"hold up the composite: {}",
			sync.error()
		);
	}
	else
	{
		m_Context.SetSync(m_Sync);

		m_SyncobjGlobal = Wayland::Server::WpLinuxDrmSyncobjManagerV1::Advertise(*display, SyncobjVersion, m_Syncobj);

		if (m_SyncobjGlobal == nullptr)
		{
			return Failure(ENOMEM, "advertising wp_linux_drm_syncobj_manager_v1");
		}

		spdlog::info("explicit synchronization on {}", m_Sync.Path());
	}

	m_ShellGlobal = Wayland::Server::XdgWmBase::Advertise(*display, ShellVersion, m_Shell);

	if (m_ShellGlobal == nullptr)
	{
		// Fatal for the same reason as the other two: a toolkit that finds no shell exits rather than
		// drawing, so a compositor missing this one is a socket every application connects to and
		// immediately abandons.
		return Failure(ENOMEM, "advertising xdg_wm_base");
	}

	// **The loop before the global**, because the first `set_selection` may arrive on the first dispatch
	// after this returns and the clipboard has nowhere to arm its background read without one.
	m_Data.Open(*m_Server.EventLoop());

	m_DataGlobal = Wayland::Server::WlDataDeviceManager::Advertise(*display, DataDeviceManagerVersion, m_Data);

	if (m_DataGlobal == nullptr)
	{
		// Fatal for two reasons and the second is the older one: a person cannot copy or paste anything
		// without it, and GTK refuses to open a display at all when it is missing — so a compositor that
		// came up without this would start, log nothing, and be a socket every GTK application walks away
		// from. [Data.h](Data.h) carries the rest.
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

	m_ForeignGlobal = Wayland::Server::ExtForeignToplevelListV1::Advertise(*display, ForeignToplevelVersion, m_Foreign);

	if (m_ForeignGlobal == nullptr)
	{
		// Fatal like the rest, and it is the first global no application will ever see: [Tier.h](Tier.h)
		// puts it in the System tier, so the registry it appears in is a shell's. Failing the run anyway
		// rather than carrying on without it — a compositor that came up unable to describe its own
		// windows is one whose shell will start, find nothing to list, and show an empty taskbar with no
		// error anywhere.
		return Failure(ENOMEM, "advertising ext_foreign_toplevel_list_v1");
	}

	m_BindingsGlobal = Wayland::Server::GyroBindingsV1::Advertise(*display, BindingsVersion, m_Bindings);

	if (m_BindingsGlobal == nullptr)
	{
		// Fatal like the rest, and the failure it prevents is the quietest one here: a shell that came up
		// without this binds nothing, is never summoned, and looks from the outside like a shell that
		// simply did not start.
		return Failure(ENOMEM, "advertising gyro_bindings_v1");
	}

	m_ChromeGlobal = Wayland::Server::GyroChromeManagerV1::Advertise(*display, ChromeVersion, m_Chrome);

	if (m_ChromeGlobal == nullptr)
	{
		// Fatal, and this one fails *loudly* where the last fails quietly: a shell that came up without it
		// still maps its launcher, and what a person gets is an opaque window in the middle of the screen
		// that lands in their alt-tab and falls behind the next thing they click.
		return Failure(ENOMEM, "advertising gyro_chrome_manager_v1");
	}

	return {};
}

void ClientHost::OnKey(const KeyEvent& event, bool consumed)
{
	// **The keymap is read before the seat folds this key into it**, which is what makes a chord mean
	// what a person's hands were doing: the modifiers held when `space` went down are the ones its own
	// press has not yet changed. `Seat::Key` does the folding immediately below.
	const bool claimed = !consumed && m_Bindings.Takes(event, m_Seat.Layout());

	m_Seat.Key(event, consumed || claimed);
}

void ClientHost::OnFocusCycle(FocusCycle step)
{
	m_FocusCycle.push_back(step);
}

void ClientHost::OnPointerMotion(const PointerMotion& event)
{
	m_Seat.Moved(event.When);
}

void ClientHost::OnPointerPosition(const PointerPosition& event)
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

void ClientHost::OnReached(EntityId entity, std::size_t output, const OutputPresentation& shown)
{
	ClientSurface* const surface = m_Context.SurfaceOf(entity);

	if (surface == nullptr)
	{
		return;
	}

	SurfacePresentation presented{ .At = shown.At,
		                           .Refresh = shown.Period,
		                           .Vblank = shown.Vblank,
		                           .Vsync = shown.Vsync,
		                           .HardwareClock = shown.HardwareClock,
		                           .ZeroCopy = shown.ZeroCopy };

	// **The index is a subscript into the advertised set, which is the world's order**, so a reach bit,
	// a `SceneOutput` and a global are the same number — [Output.h](Output.h) states that and
	// `HostOutputs::Sync` is what keeps it true. A report from before a hotplug can still name an index
	// the set no longer has, which is why this is a bounds check rather than an assertion: the frame
	// callback is answered either way, and what the client loses is knowing which panel showed it.
	const std::span<const std::unique_ptr<HostOutput>> advertised = m_Outputs.All();

	if (output < advertised.size())
	{
		const HostOutput& panel = *advertised[output];

		// **The mode's own period where the backend measured nothing**, which is the one substitution
		// here. Zero from a backend means *I do not know* and the protocol's zero means the same to a
		// client — but gyro does know what mode it programmed, and a nominal period is a far better
		// prediction of the next refresh than none. What it is not is a measurement, which is why the
		// observed figure wins wherever there is one.
		if (presented.Refresh <= Duration::zero())
		{
			presented.Refresh = panel.Facts().Period;
		}

		// Null on a stale resource, which is a surface whose client libwayland has already dropped. The
		// event simply does not go out, exactly as it does not for a client that never bound the global.
		if (const wl_client* const client = surface->Object().WireClient(); client != nullptr)
		{
			presented.Output = panel.ResourceFor(*client);
		}
	}

	surface->Present(presented);
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

	// **`Alt+Tab`, applied here rather than where the key was read**, for the reason the buttons are: the
	// stack it walks is the world's, and the world is only in hand inside this call. After the requests,
	// so a window that mapped in this wakeup is already somewhere the walk can reach — and before the
	// comparison below, so one iteration tells a client about the window a person landed on rather than
	// about every window they passed through on the way.
	//
	// **Each step raises what it lands on**, which is the same argument `FocusByClick` makes one line
	// further down: with every window centred on the same point by the Floorplanner and no shell drawing
	// a switcher, a step that only moved focus would be a gesture with nothing on screen behind it.
	for (const FocusCycle step : m_FocusCycle)
	{
		const EntityId landed = step == FocusCycle::End  ? scene.Focus().EndCycle() :
		                        step == FocusCycle::Next ? scene.Focus().CycleNext() :
		                                                   scene.Focus().CyclePrevious();

		if (!landed.IsNull())
		{
			static_cast<void>(scene.Raise(landed));
		}
	}

	m_FocusCycle.clear();

	// **After the pointer, because the press this routed is one of the things that moves focus** (162),
	// and after the requests for the reason that makes a window typeable in the wakeup it opened in: the
	// commit that maps it is what offers it focus. Both writers run before the comparison, so one
	// iteration sends one `enter` however many times focus changed inside it. See [Seat.h](Seat.h) for
	// why the change is noticed by comparing rather than by a signal out of `Scene`, and for what the
	// other order costs — a keystroke delivered to the window a person just clicked away from.
	const EntityId focused = scene.Focus().Focused();

	// **And the one window that is not focused any more and is not being told so yet**, which is
	// decision 188's transition reaching the two comparisons that push a fact at a client: the keyboard
	// crosses to the arriving session the instant a fade is authored, and the departing window keeps
	// its lit titlebar and its blinking caret for as long as a person can still see it. `Scene/Focus.h`
	// has why it is one window rather than a session's whole set.
	const EntityId leaving = scene.Focus().Leaving();

	m_Seat.SyncFocus(focused, leaving);

	// **Beside the seat's comparison and against the same answer**, for the reason the shell's walk is
	// there too: the selection is offered to whoever has the keyboard, so a window that got the keys in
	// this wakeup has to be told what is on the clipboard in it as well. A person's first Ctrl+V in a
	// window they have just clicked into is otherwise the one paste that does nothing.
	m_Data.Sync(focused);

	// **Beside the seat's comparison and against the same answer**, because the two are one fact told to
	// two different objects: a `wl_keyboard.enter` says where the keys are going and an `activated`
	// state says which titlebar is lit, and a window that got one without the other is one a person can
	// type into and cannot tell they are typing into. [Shell.h](Shell.h) has why a menu leaves its own
	// window activated.
	SyncWindows(m_Context, scene, focused, leaving);

	// **Last, because it reports on everything above it.** A window that mapped in this wakeup, one that
	// retired in it and a title a client changed in it are all settled by the time this runs, so a shell
	// hears one account of the iteration rather than a running commentary on it. It costs nothing on a
	// machine with no shell, which is every machine today: there are no lists bound, and the walk is over
	// an empty vector.
	m_Foreign.Sync();

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

	if (const Result<void> bound = m_Server.Bind(socket); !bound)
	{
		return bound;
	}

	// **A warning rather than a failure, because the run is a compositor without it.** The System
	// listener is what a shell arrives on and nothing else needs one — every application on this
	// machine reaches the socket above — so a runtime directory that will not take a second name costs
	// this run its shell and not its windows. Said out loud because the alternative is somebody's shell
	// failing to connect to a compositor that came up reporting nothing wrong.
	if (const Result<void> system = m_Server.BindSystem(); !system)
	{
		spdlog::warn("no shell can connect to this run: {}", system.error());
	}

	return {};
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
