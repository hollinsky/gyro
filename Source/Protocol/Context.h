#pragma once

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

#include "Core/Handle.h"
#include "Core/Session.h"
#include "Protocol/Drag.h"
#include "Protocol/Floor.h"
#include "Protocol/Popup.h"
#include "Protocol/Server.h"
#include "Protocol/Sync.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"

struct wl_client;

class ClientSurface;
class ClientXdgSurface;

// What a request handler may reach, and only while a dispatch is running.
//
// **A client's request is a change to gyro's world, and the world is handed in as an argument.**
// `ISceneAuthor::Advance` takes the store and the texture space precisely so that nobody retains
// them, and `ClientHost` reads its clients inside that call — so at the instant a `wl_surface.commit`
// arrives, both are on the stack a few frames up and neither is reachable from the surface that has
// to act on it. This is the pointer that closes that gap without becoming the retained reference the
// interface is shaped to avoid: it is set for the duration of one `Advance` and cleared on the way
// out, so a handler that somehow ran outside one finds nothing rather than something stale.
//
// **It is null outside a dispatch, and that is a real case rather than a defensive one.** The display
// is destroyed when the host is, which drops every client and runs every handler's `OnGone` — so a
// surface's destructor runs with no `Advance` around it, and asking to retire a texture there would be
// reaching into a registry the composition root may already have taken down. Nothing is owed at that
// point: the process is going away and the whole id space with it.
//
// A class rather than a bare pointer passed down the chain because the clearing is the point, and an
// RAII scope is the only spelling of it that survives an early return.
class HostContext
{
public:
	HostContext() = default;

	HostContext(const HostContext&) = delete;
	HostContext& operator=(const HostContext&) = delete;
	HostContext(HostContext&&) = delete;
	HostContext& operator=(HostContext&&) = delete;

	// The world this step was handed, or null outside one.
	[[nodiscard]] SceneStore* Store() const noexcept { return m_Store; }

	// The texture space this step was handed, or null outside one.
	[[nodiscard]] ITextures* Textures() const noexcept { return m_Textures; }

	// The DRM node clients' timelines are imported against, or null on a machine where explicit sync
	// could not be served at all.
	//
	// **Unlike the two above it is live outside a dispatch**, and the difference is what it is for: the
	// store and the texture space are the world an `Advance` was handed, while this is a descriptor the
	// host opened once and holds for its whole life. A held commit is resumed from the event loop, which
	// runs inside `Server::Poll` and therefore inside an `Advance` — but the *wait* is armed and taken
	// down at moments that are not requests, and a pointer that went null between them would leak one
	// eventfd per held commit.
	[[nodiscard]] ExplicitSync* Sync() const noexcept { return m_Sync; }

	// What a window of this client is parented into, per [Floor.h](Floor.h), or null where its session
	// has none.
	//
	// **The client is the argument because the floor is per session and a request is not.** A
	// `wl_surface.commit` says nothing about who is logged in; what says it is the connection the
	// request arrived on, which `Server` attributed to a session when it admitted it and checked the
	// peer's uid. Asking here rather than storing a session on every surface keeps one answer on the
	// machine, so a session ending cannot leave a stale copy behind on an object that outlived it.
	//
	// Null is a real answer rather than a failure: a commit can arrive from a client whose session has
	// just ended, in the window between the agent's connection closing and libwayland dropping the
	// client. The caller tests it and does not parent the window, which is a window that is never shown
	// rather than one hanging off nothing.
	[[nodiscard]] EntityId Floor(wl_client* client) const noexcept
	{
		if (m_Floors == nullptr || m_Server == nullptr)
		{
			return {};
		}

		return m_Floors->Container(m_Server->SessionOf(client));
	}

	// Which session a client's requests belong to, for the two places that need the id itself rather
	// than the floor: placing a window on an output that session is shown on, and nothing else yet.
	[[nodiscard]] SessionId Session(wl_client* client) const noexcept
	{
		return m_Server == nullptr ? SessionId::None : m_Server->SessionOf(client);
	}

	// Wired once when the host opens, and both outlive every client.
	void SetFloors(const SessionFloors& floors, const Server& server) noexcept
	{
		m_Floors = &floors;
		m_Server = &server;
	}

	// Wired once when the host opens, and only where a node opened. Left null otherwise, which is the
	// same statement `Sync()` makes to its callers and the reason the global is never advertised there.
	void SetSync(ExplicitSync& sync) noexcept { m_Sync = &sync; }

	// The open menus, per [Popup.h](Popup.h). Beside the floor for the same reason: it is one per
	// session rather than one per connection, it outlives every client that pushes onto it, and the
	// seat and the shell are both entitled to it — one to dismiss on a press, the other to refuse a
	// grab that is not the topmost.
	[[nodiscard]] PopupStack& Popups() noexcept { return m_Popups; }

	// The window the pointer is moving or resizing, per [Drag.h](Drag.h). Here for the popup stack's
	// reason and one more: a gesture is read by both halves of this module and neither can hold it.
	// The seat starts one and advances it, because it owns the grab and the serial a request is checked
	// against; the shell reads it to know what size to configure a window at and where to put the
	// window the client drew. Neither may reach into the other, and one pointer means one of these.
	[[nodiscard]] WindowDrag& Drag() noexcept { return m_Drag; }

	[[nodiscard]] const WindowDrag& Drag() const noexcept { return m_Drag; }

	// The mapped toplevels, which is the set every question about *the windows on this machine* is
	// asked against. Borrowed, and each one registers itself as it maps and takes itself out as it
	// unmaps — so this is the windows a person is looking at rather than the session's history, which
	// is the same rule the surface table below is kept to.
	//
	// **It is here rather than in the shell global because a window is per session and a global is per
	// connection.** Two applications are two `xdg_wm_base` objects and one set of windows, and the
	// comparison that decides which one is activated has to see all of them at once — a per-connection
	// list would answer *the frontmost window of this client*, which is not a fact anybody wants.
	//
	// A vector and a scan, for `m_Surfaces`' reason and at the same size.
	void Add(ClientXdgSurface& window) { m_Windows.push_back(&window); }

	void Remove(ClientXdgSurface& window) noexcept { std::erase(m_Windows, &window); }

	[[nodiscard]] std::span<ClientXdgSurface* const> Windows() const noexcept { return m_Windows; }

	// The surface behind an entity, for the one direction nothing else can travel.
	//
	// **The return leg arrives as an entity and a `wl_surface.frame` callback lives on a surface**, and
	// there is no path between the two that does not pass through here: `Scene` may not name `Protocol`,
	// so what it emits is the id it was given (115), and an id is all the ledger ever held. A window
	// registers its image entity when it maps and gives it up when it unmaps, so the table is the mapped
	// windows and not the session's history.
	//
	// A vector and a scan, because the length is the windows on the machine — tens, on a desk with two
	// monitors — and it is walked once per frame that reached the glass. A hash map would be the right
	// shape at a thousand and the wrong one at this size, where the whole table is a cache line or two.
	void Bind(EntityId id, ClientSurface& surface)
	{
		if (id.IsNull())
		{
			return;
		}

		Unbind(id);

		m_Surfaces.emplace_back(id, &surface);
	}

	void Unbind(EntityId id) noexcept
	{
		std::erase_if(m_Surfaces, [id](const std::pair<EntityId, ClientSurface*>& bound) noexcept {
			return bound.first == id;
		});
	}

	[[nodiscard]] ClientSurface* SurfaceOf(EntityId id) const noexcept
	{
		const auto matches = [id](const std::pair<EntityId, ClientSurface*>& bound) noexcept {
			return bound.first == id;
		};
		const auto at = std::find_if(m_Surfaces.begin(), m_Surfaces.end(), matches);

		return at != m_Surfaces.end() ? at->second : nullptr;
	}

	// One `Advance`'s worth of reachability.
	class Dispatching
	{
	public:
		Dispatching(HostContext& context, SceneStore& scene, ITextures& textures) noexcept : m_Context{ &context }
		{
			m_Context->m_Store = &scene;
			m_Context->m_Textures = &textures;
		}

		~Dispatching()
		{
			m_Context->m_Store = nullptr;
			m_Context->m_Textures = nullptr;
		}

		Dispatching(const Dispatching&) = delete;
		Dispatching& operator=(const Dispatching&) = delete;
		Dispatching(Dispatching&&) = delete;
		Dispatching& operator=(Dispatching&&) = delete;

	private:
		HostContext* m_Context = nullptr;
	};

private:
	SceneStore* m_Store = nullptr;
	ITextures* m_Textures = nullptr;
	const SessionFloors* m_Floors = nullptr;
	const Server* m_Server = nullptr;

	// The explicit-sync device, or null where none opened. Not owned; the host holds it.
	ExplicitSync* m_Sync = nullptr;
	PopupStack m_Popups;
	WindowDrag m_Drag;

	// The mapped toplevels. Borrowed, and each one takes itself out as it unmaps.
	std::vector<ClientXdgSurface*> m_Windows;

	// The mapped windows, keyed by the entity their pixels are. Borrowed pointers: a surface unbinds
	// itself as it unmaps and again as it goes away, so nothing here outlives what it names.
	std::vector<std::pair<EntityId, ClientSurface*>> m_Surfaces;
};
