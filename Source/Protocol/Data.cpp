#include "Protocol/Data.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

#include "Protocol/Context.h"
#include "Protocol/Surface.h"

void ClientDataSource::OnGone()
{
	if (m_Clipboard != nullptr)
	{
		// **The selection outlives this object where gyro kept a copy of it**, which is the whole of
		// [Clipboard.h](Clipboard.h): the application a person copied from is closing and what they
		// copied is still on the clipboard.
		m_Clipboard->Forget(*this);
	}

	delete this;
}

bool ClientDataSource::Offers(std::string_view mime) const noexcept
{
	return std::find(m_Mimes.begin(), m_Mimes.end(), mime) != m_Mimes.end();
}

void ClientDataSource::Cancel()
{
	if (m_Cancelled)
	{
		return;
	}

	m_Cancelled = true;

	Object().Cancelled();
}

void ClientDataSource::Spend(SessionClipboard& clipboard) noexcept
{
	m_Spent = true;
	m_Clipboard = &clipboard;
}

void ClientDataOffer::Describe()
{
	if (m_Clipboard == nullptr)
	{
		return;
	}

	for (const std::string& mime : m_Clipboard->Offered())
	{
		Object().Offer(mime.c_str());
	}
}

void ClientDataOffer::OnReceive(std::string_view mimeType, Fd fd)
{
	// **A stale offer answers with end-of-file rather than with the current selection.** A client is
	// expected to destroy an offer when the next one arrives and is not obliged to be quick about it,
	// so this is the check that keeps a slow paste from reading what somebody copied afterwards. The
	// descriptor is closed by going out of scope, which is exactly the empty paste the protocol gives a
	// receiver asking for a type nobody has.
	if (m_Clipboard == nullptr || m_Clipboard->Generation() != m_Generation)
	{
		return;
	}

	m_Clipboard->Serve(mimeType, std::move(fd), Object().WireClient());
}

void ClientDataOffer::OnAccept(std::uint32_t serial, std::optional<std::string_view> mimeType)
{
	static_cast<void>(serial);
	static_cast<void>(mimeType);
}

void ClientDataOffer::OnSetActions(
	Wayland::Server::WlDataDeviceManagerDndAction dndActions,
	Wayland::Server::WlDataDeviceManagerDndAction preferredAction
)
{
	static_cast<void>(dndActions);
	static_cast<void>(preferredAction);
}

void ClientDataDevice::OnGone()
{
	m_Manager->Remove(*this);

	delete this;
}

void ClientDataDevice::OnBound()
{
	m_Manager->Add(*this);

	// Only where this client already has the keyboard. A device bound by a client that does not is told
	// nothing, which is the protocol's own rule and is what stops an application in the background from
	// learning what is on the clipboard by binding an object.
	m_Manager->Introduce(*this);
}

void ClientDataDevice::OnSetSelection(Wayland::Server::WlDataSource source, std::uint32_t serial)
{
	// **The serial is read and not checked**, per [Data.h](Data.h): what refusing would stop is an
	// application overwriting the clipboard in the background, and what it would break is a person
	// copying in a window that has just lost focus.
	static_cast<void>(serial);

	SessionClipboard* const clipboard = m_Manager->ClipboardFor(m_Client);

	if (clipboard == nullptr)
	{
		return;
	}

	auto* const offering = source.IsValid() ? static_cast<ClientDataSource*>(source.Implementation()) : nullptr;

	// A `wl_data_source` may be used once, which the protocol says in as many words. Version 3 has an
	// error for it and gyro does not post one: a client that reuses a source has already been given the
	// selection it asked for, and ending the connection over the second attempt would lose a window for
	// a mistake nobody can see.
	if (offering != nullptr && offering->IsSpent())
	{
		return;
	}

	clipboard->Set(offering);

	if (offering != nullptr)
	{
		offering->Spend(*clipboard);
	}

	m_Manager->Republish(*clipboard);
}

void ClientDataDevice::OnStartDrag(
	Wayland::Server::WlDataSource source,
	Wayland::Server::WlSurface origin,
	Wayland::Server::WlSurface icon,
	std::uint32_t serial
)
{
	static_cast<void>(origin);
	static_cast<void>(icon);
	static_cast<void>(serial);

	// **Cancelled rather than ignored**, and the difference is what the application does next: a
	// toolkit told nothing waits for an `enter` that will never arrive and leaves a person holding a
	// button with a drag that has begun nowhere. `cancelled` is the protocol's own way of saying the
	// operation is over, and every toolkit already handles it — the drag simply does not start.
	if (source.IsValid())
	{
		if (auto* const dragged = static_cast<ClientDataSource*>(source.Implementation()); dragged != nullptr)
		{
			dragged->Cancel();
		}
	}

	spdlog::debug("refusing a drag: gyro serves the clipboard and drag-and-drop is not built yet");
}

void ClientDataDevice::Offer(SessionClipboard& clipboard)
{
	if (m_Client == nullptr)
	{
		return;
	}

	// **A nil selection is an event rather than a silence.** It is what tells a toolkit to grey out
	// Paste, and a client that was never sent one after a clipboard was cleared would offer a person a
	// menu entry that does nothing.
	if (clipboard.IsEmpty())
	{
		Object().Selection(Wayland::Server::WlDataOffer{});

		return;
	}

	auto* const offer = new ClientDataOffer{ clipboard, clipboard.Generation() };

	// An id of zero is how a resource the client never asked for is made, and the version is this
	// device's own: an offer is an extension of the object that announced it and cannot be newer than
	// what the client bound.
	const Wayland::Server::WlDataOffer made =
		Wayland::Server::WlDataOffer::Create(*m_Client, Object().Version(), 0, *offer);

	if (!made.IsValid())
	{
		// `Create` has already ended the client, and nothing adopted the handler.
		delete offer;

		return;
	}

	// The order the protocol fixes: the object, then the types it carries, then the event that hands it
	// over. A `selection` naming an offer whose types have not been sent is one a toolkit reads as an
	// empty clipboard.
	Object().DataOffer(made);

	offer->Describe();

	Object().Selection(made);
}

Wayland::Server::WlDataSourceHandler* ClientDataDeviceManager::OnCreateDataSource()
{
	// Owns itself from here, on `ClientCompositor::OnCreateSurface`'s argument: returning null would end
	// the client with `no_memory`, and there is nothing here that can fail short of the allocator.
	return new ClientDataSource{ *m_Manager };
}

Wayland::Server::WlDataDeviceHandler* ClientDataDeviceManager::OnGetDataDevice(Wayland::Server::WlSeat seat)
{
	// **The seat is not read, and there is one.** A machine with two seats would route the selection per
	// seat, since a clipboard belongs to the keyboard that filled it; gyro advertises one — decision
	// 21 splits by session rather than by seat — so resolving the id would answer a question with one
	// possible answer. libwayland has already refused an id that is not a `wl_seat`.
	static_cast<void>(seat);

	wl_client* const client = Object().WireClient();

	if (client == nullptr)
	{
		return nullptr;
	}

	return new ClientDataDevice{ *m_Manager, *client };
}

Wayland::Server::WlDataDeviceManagerHandler* DataDeviceManagerGlobal::OnBind(wl_client& client, std::uint32_t version)
{
	static_cast<void>(client);
	static_cast<void>(version);

	return new ClientDataDeviceManager{ *this };
}

void DataDeviceManagerGlobal::Add(ClientDataDevice& device)
{
	m_Devices.push_back(&device);
}

void DataDeviceManagerGlobal::Remove(ClientDataDevice& device) noexcept
{
	std::erase(m_Devices, &device);
}

SessionClipboard* DataDeviceManagerGlobal::ClipboardFor(wl_client* client)
{
	if (m_Context == nullptr)
	{
		return nullptr;
	}

	return m_Clipboards.For(*m_Context, m_Loop, m_Context->Session(client));
}

wl_client* DataDeviceManagerGlobal::ClientOf(EntityId window) const
{
	if (m_Context == nullptr || window.IsNull())
	{
		return nullptr;
	}

	const ClientSurface* const surface = m_Context->SurfaceOf(window);

	return surface == nullptr ? nullptr : surface->Object().WireClient();
}

void DataDeviceManagerGlobal::Sync(EntityId focused)
{
	wl_client* const client = ClientOf(focused);

	if (client == nullptr)
	{
		// Focus is on a window gyro authored for itself, or on nothing. Nobody is told anything and
		// `m_Told` is cleared, so the client that takes the keyboard next is told even where it is the
		// same one — it may have missed a selection set while it was not focused.
		m_Told = nullptr;

		return;
	}

	SessionClipboard* const clipboard = ClipboardFor(client);

	if (clipboard == nullptr)
	{
		return;
	}

	if (client == m_Told && clipboard->Generation() == m_ToldGeneration)
	{
		return;
	}

	m_Told = client;
	m_ToldGeneration = clipboard->Generation();

	for (ClientDataDevice* const device : m_Devices)
	{
		if (device->Client() == client)
		{
			device->Offer(*clipboard);
		}
	}
}

void DataDeviceManagerGlobal::Introduce(ClientDataDevice& device)
{
	if (m_Told == nullptr || device.Client() != m_Told)
	{
		return;
	}

	if (SessionClipboard* const clipboard = ClipboardFor(m_Told); clipboard != nullptr)
	{
		device.Offer(*clipboard);
	}
}

void DataDeviceManagerGlobal::Republish(const SessionClipboard& clipboard)
{
	// **The client that has the keyboard hears about the copy in the same wakeup it happened**, rather
	// than at the next focus change. A person who copies in one window and pastes in the same one is
	// the ordinary case, and waiting for focus to move would be a paste that offered what was on the
	// clipboard before.
	if (m_Told == nullptr)
	{
		return;
	}

	SessionClipboard* const theirs = ClipboardFor(m_Told);

	if (theirs != &clipboard)
	{
		return;
	}

	m_ToldGeneration = theirs->Generation();

	for (ClientDataDevice* const device : m_Devices)
	{
		if (device->Client() == m_Told)
		{
			device->Offer(*theirs);
		}
	}
}
