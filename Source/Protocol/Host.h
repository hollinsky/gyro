#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/Fd.h"
#include "Core/Input.h"
#include "Core/Result.h"
#include "Core/Session.h"
#include "Core/Signal.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Protocol/Bindings.h"
#include "Protocol/Chrome.h"
#include "Protocol/Compositor.h"
#include "Protocol/Context.h"
#include "Protocol/Data.h"
#include "Protocol/Dmabuf.h"
#include "Protocol/ExplicitSync.h"
#include "Protocol/Floor.h"
#include "Protocol/Foreign.h"
#include "Protocol/Output.h"
#include "Protocol/Presentation.h"
#include "Protocol/Seat.h"
#include "Protocol/Server.h"
#include "Protocol/Shell.h"
#include "Protocol/Shm.h"
#include "Protocol/Subcompositor.h"
#include "Protocol/Viewporter.h"
#include "Scene/Author.h"
#include "Scene/Return.h"

// The author with clients behind it: gyro's Wayland server standing where a gym stands.
//
// **This is the second `ISceneAuthor` and the one the interface was generalised for.** A gym is gyro
// authoring for itself; this is a person's windows. They meet at `SceneStore` rather than at the
// contract — the host will turn a `wl_surface.commit` into a node with the same `SceneCommit` a gym
// retargets a lane with — which is what lets the dispatch loop step either one without knowing which
// it has. [Scene/Author.h](../Scene/Author.h) carries that argument; this is the implementor.
//
// **Reading the clients happens inside `Advance`, and that is the whole reason it is an author rather
// than something the composition root pumps beside one.** A request handler needs the store and the
// texture space at the instant it runs — a commit is a change to the scene, a `wl_shm` buffer is an
// `Adopt` — and `ISceneAuthor` hands both in as arguments precisely so that nobody retains them. Poll
// the socket anywhere else and the host would have to hold a `SceneStore&` across the gap, which is the
// reference the interface is shaped to avoid. So the order inside one step is the order the world
// wants: the loop drains what the frame thread returned, this reads what the clients asked for, and the
// serializer walks what both of them left behind.
//
// **The flush is the root's, not this object's**, and the deadlock it avoids is the one worth naming.
// Events gyro owes back are queued during the step and have to reach the client before the dispatch
// thread parks. Flushing at the top of the *next* `Advance` would hold a frame callback until something
// woke dispatch — and when the world has settled, the only thing that would have woken it is the client
// acting on the callback it never received. `Flush` is therefore public and the root calls it
// immediately before the wait, which is the one place that knows the thread is about to sleep.
//
// **Three globals are a window on screen, and the fourth is what a person does with two of them.**
// `wl_compositor` is where a `wl_surface` and a `wl_region` come from, `wl_shm` is where its pixels
// do, and `xdg_wm_base` is what says the surface is a window — so a toolkit can start, negotiate a
// size, draw a frame and be placed. `wl_data_device_manager` is beside them because copying out of
// one window and pasting into another is the plainest thing two applications ever do together, and
// because GTK gives up on a display that does not advertise one; [Data.h](Data.h) and
// [Clipboard.h](Clipboard.h) carry what gyro keeps of a selection and why it outlives the application
// that made it. The floor a window is placed on is authored here, once, because with no session agent
// there is one session and this object is the whole of it.
//
// **A window redraws now, and what it redraws against is the return leg.** `Observe` connects this
// host to the fact `Scene/Return.h` derives — *the pixels this entity committed reached the glass, at
// this instant, on this panel* — and two events are what that fact is worth to a client: a
// `wl_surface.frame` callback, which says *draw again*, and a `wp_presentation_feedback`, which says
// when what was drawn was seen and how much the number is worth. The connection is the composition
// root's to make for decision 115's reason: `Scene` may not name `Protocol`, so the signal carries an
// entity and this module is what knows which surface that is — and an output index, which this module
// turns into the `wl_output` the client itself bound.
//
// **A window can now be typed into and pointed at.** `wl_seat` carries a keyboard and a pointer:
// focus is `Scene`'s (`Scene/Focus.h`), what is under the pointer is `Scene/Hit.h`'s, the layout is
// xkbcommon's, and this object is what turns those into a `wl_keyboard.enter`, a `wl_pointer.enter`
// and the events after them. What a person still cannot do is *raise* a window by clicking it — focus
// follows what opened last rather than what was clicked — or drag one, or paste: click-to-focus,
// interactive move and resize, and the data device all come after this.
//
// The global is a member rather than something the root passes in, because its lifetime is the
// server's: `wl_compositor` exists for as long as there is a socket to reach it through, and unlike a
// `wl_output` there is no event that should make it come or go.
// Where this run's listeners come from, which is the one thing about a host the command line settles.
//
// **The two do not mix, and the composition root refuses a run that asks for both.** A socket gyro
// bound itself has no uid to admit connections against and no agent whose going away ends it, so a
// client on it belongs to no session — which is fine as the whole of a development run and is not
// fine beside sessions that are being tracked properly.
enum class HostListener : std::uint8_t
{
	// gyro binds `wayland-N` under its own `XDG_RUNTIME_DIR`. Every client is unattributed, and this is
	// what the binary did before there was an agent to offer anything.
	Own,

	// Every listener arrives from a session agent over the control socket, and there may be none for as
	// long as the run lasts. Docs/Architecture.md#listener-handover.
	Handover,
};

// What one `Alt+Tab` key did, on its way to the world. Three values rather than a signed step, because
// the end of a gesture is not a distance and a person walking backwards is not walking minus one.
enum class FocusCycle : std::uint8_t
{
	// The window a person used before this one, and the one before that.
	Next,

	// Back towards the window they started on.
	Previous,

	// The hand came off `Alt`: where the walk stopped is where they meant to be.
	End,
};

class ClientHost final : public ISceneAuthor
{
public:
	ClientHost() = default;

	// **The clipboard is torn down here rather than by a member going out of scope**, because its
	// background fetches and pastes are event sources on the display's own loop — and the display is a
	// member too. A source outliving its loop is a use-after-free at shutdown rather than a leak, and a
	// destructor body runs while every member is still alive.
	~ClientHost() override { m_Data.Close(); }

	// The socket clients reach this compositor through, for the log line that tells a person where to
	// point one.
	[[nodiscard]] std::string_view SocketName() const noexcept { return m_Server.SocketName(); }

	// The socket a shell reaches this compositor through, or empty where this run has none — which is
	// every run under the handover, where whose listener carries `Trust::System` is the session agent's
	// to decide and is still open. See `Server::BindSystem`.
	[[nodiscard]] std::string_view SystemSocketName() const noexcept { return m_Server.SystemSocketName(); }

	// The one descriptor the composition root adds to the dispatch thread's wait. Borrowed from
	// libwayland and valid for as long as this host is.
	//
	// **It is the event loop's rather than a socket's**, which is what lets a run under the handover
	// wire its wait at startup and take listeners hours later: an offer arriving adds a file to *this*
	// loop, behind this number, exactly as a client connecting does.
	[[nodiscard]] int PollFd() const noexcept { return m_Server.PollFd(); }

	// Serve a session's clients on the listener its agent offered. The composition root's to call, on
	// the dispatch thread, because it is the party holding both this host and the control socket the
	// offer arrived on.
	// **The floor is authored first and the listener is taken second**, so a window cannot arrive before
	// the container it hangs under — and a session that could not be given a floor is not served at all,
	// which is a failure the agent is told about rather than one a person meets as an application that
	// starts and never appears.
	//
	// `trust` is what the offered socket grants the clients that arrive on it, forwarded verbatim to
	// [Server.h](Server.h) — the host does not read it and has no business doing so, since a global's
	// tier is [Tier.h](Tier.h)'s and the filter is the display's. Defaulted to `User`, which is every
	// caller: the listener that carries `System` is Docs/Open.md's and does not exist yet.
	[[nodiscard]] Result<void>
	Adopt(SceneStore& scene, Fd listener, std::uint32_t uid, SessionId session, Trust trust = Trust::User)
	{
		if (const Result<void> floor = m_Floors.Open(scene, session); !floor)
		{
			return floor;
		}

		Result<void> adopted = m_Server.Adopt(std::move(listener), uid, session, trust);

		if (!adopted)
		{
			m_Floors.Close(scene, session);
		}

		return adopted;
	}

	// The agent went away, so the session did. Ends every client that arrived on that listener, which
	// retires their windows through the path a client exiting already takes — and then retires the floor
	// they hung under, which is the same path one level up.
	//
	// **The clients go first and the floor second, and the order is what a person sees.** Retiring the
	// floor first would retire every window with it and then retire each of them again as its client
	// died, which decision 114 makes idempotent — so the order costs nothing to correctness and buys
	// the windows their own exit rather than the floor's.
	void Release(SceneStore& scene, SessionId session) noexcept
	{
		m_Server.Release(session);

		// **After the clients and for their reason**: ending them destroys their sources and offers,
		// which deregister from the clipboard on the way out, so what this drops is a clipboard nothing
		// still points at. What a person logging out gets from it is that what they copied does not stay
		// on a machine somebody else is still using.
		m_Data.Close(session);

		m_Floors.Close(scene, session);
	}

	// Push everything owed back out to the clients. The root's to call, immediately before it sleeps.
	void Flush() noexcept { m_Server.Flush(); }

	// Where a committed `wl_shm` buffer goes, per [Scene/Capture.h](../Scene/Capture.h). Wired by the
	// composition root when a run asked for captures, and never afterwards — a host with none offers
	// nothing, which is every run without `--capture`.
	void Capture(ISurfaceCapture& sink) noexcept { m_Context.SetCapture(sink); }

	// One key, already past the compositor's own chord, on its way to whatever has focus. The root's to
	// call, because it is what holds both the devices and this host — and it calls it for a consumed key
	// too, because the modifier state is a fact about a person's hands rather than about who is
	// listening. [Seat.h](Seat.h) carries the routing.
	//
	// **A shell's claimed chords are matched here, between the compositor's own keys and the focused
	// window's.** That order is the whole of the policy: gyro's escape hatch is not something a client
	// can take, and a shell's shortcut is not something the application in front of a person can
	// swallow. [Bindings.h](Bindings.h) carries why a chord is a keysym rather than a keycode.
	void OnKey(const KeyEvent& event, bool consumed);

	// One step of `Alt+Tab`, or the hand coming off `Alt`. The root's to call, because the chord that
	// read the key is its own — and it is a verb of this host's rather than something the root does to
	// the world directly, because the world is only in hand inside `Advance`, which is the pointer's
	// argument below applied to a keystroke. [Input/Chord.h](../Input/Chord.h) has why the binding is
	// what it is; [Scene/Focus.h](../Scene/Focus.h) has what the walk does.
	void OnFocusCycle(FocusCycle step);

	// The pointer's three, and they are the root's to call for `OnKey`'s reason. None of them routes
	// anything: a displacement has already moved `Scene/Pointer.h`, and what the seat takes from a
	// motion is only the instant it happened at — the buttons and the scroll increments are held until
	// `Advance`, where the world a click lands in is in hand. [Seat.h](Seat.h) carries why.
	void OnPointerMotion(const PointerMotion& event);
	void OnPointerButton(const PointerButton& event);
	void OnPointerScroll(const PointerScroll& event);

	// The pointer was *placed* rather than pushed: a device that states a position, which under the
	// nested backend is the host compositor moving its own pointer over gyro's window. The root has
	// already landed it on an output and moved the world's pointer, so what is left here is the same
	// thing a displacement leaves — the instant, which is what the seat routes against in `Advance`.
	void OnPointerPosition(const PointerPosition& event);

	// One contact, and the place on the screen the root resolved it to — two arguments rather than one,
	// which is decision 167's rule about a device fraction never becoming a coordinate until somebody
	// knows which output the glass is in front of. Held until `Advance` for the buttons' reason.
	void OnTouch(const TouchEvent& event, Point<GlobalSpace> at);

	// A device is gone. The root's to call, and what it costs a client is a `wl_touch.cancel` for a
	// sequence that will never produce an up — a touchscreen unplugged with a finger on it.
	void OnDeviceGone(InputDeviceId device);

	// Answer frame callbacks against what reached the glass. The root's to call once, before the first
	// step, because it is the only party that holds both this host and the loop's return leg.
	void Observe(SceneReturn& returns) { m_Reached.ConnectTo<&ClientHost::OnReached>(returns.Reached, *this); }

	[[nodiscard]] std::string_view Name() const noexcept override { return "clients"; }

	// Advertise the globals, and author nothing: a scene made of client windows starts with no clients
	// in it, and the first node arrives from a request rather than from here.
	//
	// **The globals go up here rather than at `Listen`**, which is the only ordering question there is
	// and it has slack in both directions: a client cannot send a request before there is a socket, and
	// nothing it sends is dispatched before the first `Advance`, which follows this. Doing it here is
	// what keeps every failure a client could notice on one side of the composition root's `Open`.
	[[nodiscard]] Result<void> Open(SceneStore& scene, ITextures& textures) override;

	// Read every client with a request waiting and run what it asked for.
	//
	// **`Wake::Never()` is the truthful answer and not a placeholder.** A host has no schedule of its
	// own: nothing falls due at an instant this object chose, and what makes it run again is a client
	// writing to the socket the root is already sleeping on. When a window is animating, the wake that
	// says so comes from the retarget the commit performed, through the serializer, exactly as a gym's
	// does — the loop folds this answer together with that one, so a host answering `Never()` never
	// stops a motion already in flight.
	[[nodiscard]] Wake Advance(SceneStore& scene, ITextures& textures, Instant now) override;

private:
	// The `Alt+Tab` keys of this wakeup, in the order they arrived, applied at the top of `Advance` where
	// the store is. A vector rather than a step count and a flag, so that a walk which ends and another
	// which begins inside one iteration stay two gestures rather than becoming one — which a person can
	// produce with a fast enough hand and nothing else would catch.
	std::vector<FocusCycle> m_FocusCycle;

	// One entity's pixels reached the glass. Everything a client is owed for that is a surface's, and
	// the table that turns the id into one is `HostContext`'s.
	//
	// **It runs at the top of a dispatch iteration and outside `Advance`**, which is decision 115's
	// ordering: a callback delivered here gives the client the whole of this iteration to draw into,
	// where the same callback at the bottom would give it the next one. It needs no store and no texture
	// space — sending an event is not a change to the world — so the context being unset is correct
	// rather than a gap.
	void OnReached(EntityId entity, std::size_t output, const OutputPresentation& shown);

	// Bring up the display, and bind a socket where this run is the one making it. The factory's alone:
	// what a host does at the dispatch loop's first step is advertise globals, and there is no second
	// moment either of these could usefully happen at.
	[[nodiscard]] Result<void> Listen(HostListener listener, std::string_view socket);

	friend Result<std::unique_ptr<ClientHost>> MakeClientHost(HostListener listener, std::string_view socket);

	// **Declared before the server, so that they are destroyed after it.** A binding is what libwayland
	// calls to answer a bind, and the display is what can still be calling: `~Server` destroys the
	// display, which drops every client and every global with it. Member order is the whole of the
	// guarantee that the thing being called into still exists while that is happening.
	// What a request handler reaches the world through, for the one call it is inside. Declared first
	// because the bindings below point at it.
	HostContext m_Context;

	// The return leg's observer. Declared here so it is torn down with the host, which is before the
	// dispatch loop that owns the signal — a link outliving its signal is what `Core/Signal.h` refuses
	// to make possible, and the order is what keeps it from being asked.
	Connection<EntityId, std::size_t, const OutputPresentation&> m_Reached;

	CompositorGlobal m_Compositor{ m_Context };
	wl_global* m_CompositorGlobal = nullptr;

	SubcompositorGlobal m_Subcompositor{ m_Context };
	wl_global* m_SubcompositorGlobal = nullptr;

	ViewporterGlobal m_Viewporter{ m_Context };
	wl_global* m_ViewporterGlobal = nullptr;

	PresentationGlobal m_Presentation{ m_Context };
	wl_global* m_PresentationGlobal = nullptr;

	ShmGlobal m_Shm;
	wl_global* m_ShmGlobal = nullptr;

	DmabufGlobal m_Dmabuf{ m_Context };
	wl_global* m_DmabufGlobal = nullptr;

	// The DRM node clients' timelines are imported against, and the global that lets them name one.
	// Both absent on a machine where no node opened, which is the whole of the availability rule — see
	// [ExplicitSync.h](ExplicitSync.h).
	ExplicitSync m_Sync;
	SyncobjGlobal m_Syncobj{ m_Context };
	wl_global* m_SyncobjGlobal = nullptr;

	ShellGlobal m_Shell{ m_Context };
	wl_global* m_ShellGlobal = nullptr;

	DataDeviceManagerGlobal m_Data{ m_Context };
	wl_global* m_DataGlobal = nullptr;

	SeatGlobal m_Seat{ m_Context };
	wl_global* m_SeatGlobal = nullptr;

	ForeignToplevelGlobal m_Foreign{ m_Context };
	wl_global* m_ForeignGlobal = nullptr;

	BindingsGlobal m_Bindings;
	wl_global* m_BindingsGlobal = nullptr;

	ChromeGlobal m_Chrome;
	wl_global* m_ChromeGlobal = nullptr;

	// One per session, each authored before its session's listener is taken and outliving every client
	// on it. A development run has exactly one, whose session is `None`.
	SessionFloors m_Floors;

	// Which of the two shapes this run is, which is the whole of what the command line settles about a
	// host and the one thing `Open` needs of it.
	HostListener m_Listener = HostListener::Own;

	Server m_Server;

	// One global per output, kept in step with the world inside `Advance`.
	//
	// **Not advertised in `Open` beside the others, because there are no outputs yet when it runs.**
	// The composition root lays the outputs out after the author is built, and a monitor plugged in an
	// hour later has to arrive by the same path as the ones that were there at boot — so this is a
	// comparison per iteration against what the store holds rather than a list built once.
	//
	// **Declared after the server, and the position is load-bearing.** These are the only globals here
	// that are ever withdrawn, so this is the only one whose destructor touches the display — and
	// members die in reverse, so anywhere above this line is a `wl_global_destroy` on a display that
	// has already been torn down. The same ordering is what lets a binding's own destructor reach back
	// here: the display outlives nothing else, so it is destroyed after every resource it owns has been
	// cut loose from the output it named.
	HostOutputs m_Outputs;
};

// Bring up the server, or fail before anything else in the run is constructed. An empty `socket` under
// `HostListener::Own` takes the first free `wayland-N`, which is what a client with nothing set finds;
// under `HostListener::Handover` there is no socket yet and the argument is unread.
//
// **The socket is bound here rather than in `Open` above**, because a compositor that cannot offer
// clients a socket should say so and stop rather than reach the point of laying out monitors — and
// because the root needs `PollFd` to wire the wait, which is before the loop calls `Open` at all.
[[nodiscard]] Result<std::unique_ptr<ClientHost>>
MakeClientHost(HostListener listener = HostListener::Own, std::string_view socket = {});
