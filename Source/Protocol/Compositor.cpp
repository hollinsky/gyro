#include "Protocol/Compositor.h"

#include "Protocol/Region.h"
#include "Protocol/Surface.h"

Wayland::Server::WlSurfaceHandler* ClientCompositor::OnCreateSurface()
{
	// A surface owns itself from here: it is deleted by its own `OnGone`, which runs when the client
	// destroys it or when the client goes away. Returning null would end the client with `no_memory`,
	// and there is nothing here that can fail short of the allocator.
	return new ClientSurface{};
}

Wayland::Server::WlRegionHandler* ClientCompositor::OnCreateRegion()
{
	return new ClientRegion{};
}

Wayland::Server::WlCompositorHandler* CompositorGlobal::OnBind(wl_client& client, std::uint32_t version)
{
	(void)client;
	(void)version;

	return new ClientCompositor{};
}
