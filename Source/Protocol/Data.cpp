#include "Protocol/Data.h"

Wayland::Server::WlDataSourceHandler* ClientDataDeviceManager::OnCreateDataSource()
{
	// Owns itself from here, on `ClientCompositor::OnCreateSurface`'s argument: returning null would end
	// the client with `no_memory`, and there is nothing here that can fail short of the allocator.
	return new ClientDataSource{};
}

Wayland::Server::WlDataDeviceHandler* ClientDataDeviceManager::OnGetDataDevice(Wayland::Server::WlSeat seat)
{
	// The seat is not read because there is nothing to read it against: gyro advertises none, so the
	// only way to reach this is a client naming a seat object from somewhere else, and libwayland has
	// already refused that.
	(void)seat;

	return new ClientDataDevice{};
}

Wayland::Server::WlDataDeviceManagerHandler* DataDeviceManagerGlobal::OnBind(wl_client& client, std::uint32_t version)
{
	(void)client;
	(void)version;

	return new ClientDataDeviceManager{};
}
