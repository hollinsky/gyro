#pragma once

#include <algorithm>
#include <utility>
#include <vector>

#include "Core/Handle.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"

class ClientSurface;

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

	// What a window is parented into, per [Floor.h](Floor.h). Set once when the host opens, and
	// unlike the two above it does not come and go — the floor is gyro's own node and outlives every
	// client that hangs something under it.
	[[nodiscard]] EntityId Floor() const noexcept { return m_Floor; }

	void SetFloor(EntityId floor) noexcept { m_Floor = floor; }

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
	EntityId m_Floor{};

	// The mapped windows, keyed by the entity their pixels are. Borrowed pointers: a surface unbinds
	// itself as it unmaps and again as it goes away, so nothing here outlives what it names.
	std::vector<std::pair<EntityId, ClientSurface*>> m_Surfaces;
};
