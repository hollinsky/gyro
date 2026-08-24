#pragma once

#include <memory>
#include <string_view>

#include "Core/Result.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Protocol/Compositor.h"
#include "Protocol/Context.h"
#include "Protocol/Floor.h"
#include "Protocol/Server.h"
#include "Protocol/Shell.h"
#include "Protocol/Shm.h"
#include "Scene/Author.h"

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
// **Three globals are advertised, and together they are a window on screen.** `wl_compositor` is where
// a `wl_surface` and a `wl_region` come from, `wl_shm` is where its pixels do, and `xdg_wm_base` is
// what says the surface is a window — so a toolkit can now start, negotiate a size, draw a frame and
// be placed. The floor it is placed on is authored here, once, because with no session agent there is
// one session and this object is the whole of it.
//
// What a person cannot do yet is *use* the window: there is no seat, so nothing routes a click or a
// keystroke, and no frame callback answers, so an application draws its first frame and then waits for
// a signal that never comes. Both are the next step.
//
// The global is a member rather than something the root passes in, because its lifetime is the
// server's: `wl_compositor` exists for as long as there is a socket to reach it through, and unlike a
// `wl_output` there is no event that should make it come or go.
class ClientHost final : public ISceneAuthor
{
public:
	ClientHost() = default;

	// The socket clients reach this compositor through, for the log line that tells a person where to
	// point one.
	[[nodiscard]] std::string_view SocketName() const noexcept { return m_Server.SocketName(); }

	// The one descriptor the composition root adds to the dispatch thread's wait. Borrowed from
	// libwayland and valid for as long as this host is.
	[[nodiscard]] int PollFd() const noexcept { return m_Server.PollFd(); }

	// Push everything owed back out to the clients. The root's to call, immediately before it sleeps.
	void Flush() noexcept { m_Server.Flush(); }

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
	// Bind the socket. The factory's alone — a host that reached the dispatch loop unbound would be one
	// whose descriptor the root already put in its wait, so there is no second moment this could
	// usefully happen at.
	[[nodiscard]] Result<void> Listen(std::string_view socket) { return m_Server.Open(socket); }

	friend Result<std::unique_ptr<ClientHost>> MakeClientHost(std::string_view socket);

	// **Declared before the server, so that they are destroyed after it.** A binding is what libwayland
	// calls to answer a bind, and the display is what can still be calling: `~Server` destroys the
	// display, which drops every client and every global with it. Member order is the whole of the
	// guarantee that the thing being called into still exists while that is happening.
	// What a request handler reaches the world through, for the one call it is inside. Declared first
	// because the bindings below point at it.
	HostContext m_Context;

	CompositorGlobal m_Compositor{ m_Context };
	wl_global* m_CompositorGlobal = nullptr;

	ShmGlobal m_Shm;
	wl_global* m_ShmGlobal = nullptr;

	ShellGlobal m_Shell{ m_Context };
	wl_global* m_ShellGlobal = nullptr;

	// gyro's own node, authored before any client can reach the socket and outliving all of them.
	SessionFloor m_Floor;

	Server m_Server;
};

// Bring up the server and bind its socket, or fail before anything else in the run is constructed. An
// empty `socket` takes the first free `wayland-N`, which is what a client with nothing set finds.
//
// **The socket is bound here rather than in `Open` above**, because a compositor that cannot offer
// clients a socket should say so and stop rather than reach the point of laying out monitors — and
// because the root needs `PollFd` to wire the wait, which is before the loop calls `Open` at all.
[[nodiscard]] Result<std::unique_ptr<ClientHost>> MakeClientHost(std::string_view socket = {});
