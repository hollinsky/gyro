#pragma once

#include "Scene/Textures.h"

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

	// The texture space this step was handed, or null outside one.
	[[nodiscard]] ITextures* Textures() const noexcept { return m_Textures; }

	// One `Advance`'s worth of reachability.
	class Dispatching
	{
	public:
		Dispatching(HostContext& context, ITextures& textures) noexcept : m_Context{ &context }
		{
			m_Context->m_Textures = &textures;
		}

		~Dispatching() { m_Context->m_Textures = nullptr; }

		Dispatching(const Dispatching&) = delete;
		Dispatching& operator=(const Dispatching&) = delete;
		Dispatching(Dispatching&&) = delete;
		Dispatching& operator=(Dispatching&&) = delete;

	private:
		HostContext* m_Context = nullptr;
	};

private:
	ITextures* m_Textures = nullptr;
};
