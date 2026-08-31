#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Handle.h"
#include "Protocol/Clipboard.h"
#include "Wayland/Server/Wayland.h"

struct wl_client;
struct wl_event_loop;

class DataDeviceManagerGlobal;
class HostContext;

// `wl_data_device_manager`: the clipboard, and the global a toolkit will not start without.
//
// **Copy and paste work. Drag-and-drop does not, and the split is deliberate.** This one interface
// carries two features that share nothing but a factory: a selection is a thing a person put somewhere
// with the keyboard, and a drag is a pointer gesture with an icon surface, a grab that has to
// supersede the seat's implicit one, and an action negotiation between the two clients. The second is
// [Drag.h](Drag.h)-shaped work and lands on its own; `start_drag` is refused with a log line rather
// than half-implemented, because a drag that begins and never ends leaves a toolkit holding a grab
// nothing will release.
//
// **What gyro keeps of the selection is [Clipboard.h](Clipboard.h)'s, and it is where this compositor
// stops behaving like the others.** The protocol makes the clipboard an object the *source client*
// owns, so closing the application a person copied from empties it — the single loudest complaint
// about Wayland, and the reason every desktop ships a clipboard manager that has to steal the
// selection in order to hold it. gyro holds one text type itself, at the moment of the copy, so the
// offer survives the application. The rest of that argument is in Clipboard.h; what belongs here is
// that the *protocol* shape does not change at all — a client sees an ordinary `wl_data_offer` whose
// mime list happens to still be there.
//
// **The selection goes to whoever has the keyboard, and is noticed by comparing rather than by being
// told.** The protocol has the event arrive immediately before a client receives keyboard focus and
// whenever a new selection is set while it has it, so `Sync` runs once per `Advance` and diffs the
// focused client against what it last told — which is [Seat.h](Seat.h)'s rule and
// [decision
// 149](../../Docs/Decisions.md#149-focus-is-state-the-world-holds-and-the-seat-compares-against-it-rather-than-being-told)'s
// argument reused rather than a second mechanism. The keystroke that moved focus and the step that
// notices are the same wakeup of the same thread.
//
// **Anybody may set the selection**, which is a decision rather than an omission. The protocol has a
// serial on `set_selection` and gyro sends the serials it would be checked against, so refusing a
// client that is not focused is available and is what some compositors do. It is not what happens
// here: the case it stops is an application overwriting the clipboard in the background, and the case
// it breaks is a person copying in a window that has just lost focus — a paste that comes back empty
// for no reason a person can see. The first is worth *telling somebody about* rather than refusing,
// which is what the read notice in Clipboard.h is for.
//
// **It is advertised to everyone and reaches one session**, which is the whole content of
// [Tier.h](Tier.h)'s `Session` rung: hiding the global would leave an application with no clipboard at
// all, and one person's copy reaching another person's windows is the disclosure the per-uid split
// exists to prevent. The clipboard is keyed on the session a connection was admitted under.
//
// **Advertised at version 3.** `wl_compositor`'s rule is that the number promises the *events* gyro
// sends, and 3 is where the drag-and-drop action negotiation lives — `source_actions`, `action` and
// `dnd_finished` — which is the half not built. It stays at 3 anyway because that is where every
// toolkit stops asking and because the events above 1 are ones a selection never reaches: a client
// that binds 3 and only ever copies is told exactly what a client that bound 1 would be. What version
// 3 obliges of the selection path and gyro does honour is `wl_data_source.cancelled` on replacement,
// which is what a `wl-copy` waits for before it exits.
inline constexpr std::uint32_t DataDeviceManagerVersion = 3;

// One client's `wl_data_source`: what it is offering, and the pipe it will be asked to write into.
class ClientDataSource final : public Wayland::Server::WlDataSourceIgnoring
{
public:
	explicit ClientDataSource(DataDeviceManagerGlobal& manager) noexcept : m_Manager{ &manager } {}

	// Tells the clipboard it has gone, which is the moment gyro's own copy stops being a fallback and
	// becomes the selection.
	void OnGone() override;

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	// **Accumulated in the client's own order, because the order is the client's preference** and both
	// the offer gyro republishes and the type it chooses to keep read it that way.
	void OnOffer(std::string_view mimeType) override { m_Mimes.emplace_back(mimeType); }

	// Drag-and-drop actions, recorded and never matched against anything: a selection has no actions,
	// and the party that would negotiate these is the drag that is not built. Kept rather than dropped
	// so that the day `start_drag` lands it is reading a field rather than growing one.
	void OnSetActions(Wayland::Server::WlDataDeviceManagerDndAction dndActions) override { m_Actions = dndActions; }

	[[nodiscard]] std::span<const std::string> Mimes() const noexcept { return m_Mimes; }

	[[nodiscard]] bool Offers(std::string_view mime) const noexcept;

	// This source has been replaced, per `wl_data_source.cancelled`. Sent once — a source that has been
	// cancelled is one the client is expected to destroy, and a second event would be gyro telling an
	// application to clean up something it already has.
	void Cancel();

	// Which selection this source is, if it is one. **A source may be used exactly once**, which the
	// protocol says in as many words and which matters here because `set_selection` twice with the same
	// object would otherwise leave two clipboards pointing at it.
	[[nodiscard]] bool IsSpent() const noexcept { return m_Spent; }

	void Spend(SessionClipboard& clipboard) noexcept;

private:
	DataDeviceManagerGlobal* m_Manager = nullptr;

	// The clipboard this source is the selection of, or null. Borrowed, and dropped by `Set` on
	// whichever side goes first.
	SessionClipboard* m_Clipboard = nullptr;

	std::vector<std::string> m_Mimes;

	Wayland::Server::WlDataDeviceManagerDndAction m_Actions{};

	bool m_Spent = false;

	bool m_Cancelled = false;
};

// One `wl_data_offer`: a selection, as one client has been told about it.
//
// **It carries the generation it was made under rather than a pointer to what it describes**, because
// a client is entitled to hold one after the clipboard has moved on — the protocol has it destroy the
// offer when the next `selection` arrives, and a client that is slow about it must not be able to read
// the next person's copy. A stale offer answers `receive` with end-of-file, which is what a receiver
// already handles.
class ClientDataOffer final : public Wayland::Server::WlDataOfferIgnoring
{
public:
	ClientDataOffer(SessionClipboard& clipboard, std::uint32_t generation) noexcept
		: m_Clipboard{ &clipboard }, m_Generation{ generation }
	{}

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	// Where a paste actually happens.
	void OnReceive(std::string_view mimeType, Fd fd) override;

	// Drag-and-drop, all three of them: what a destination accepts, the action it prefers and the
	// finish that ends a transfer. A selection has none of these — there is no source to feed an
	// `accept` back to and nothing to finish — so they are accepted and dropped rather than answered.
	void OnAccept(std::uint32_t serial, std::optional<std::string_view> mimeType) override;

	void OnFinish() override {}

	void OnSetActions(
		Wayland::Server::WlDataDeviceManagerDndAction dndActions,
		Wayland::Server::WlDataDeviceManagerDndAction preferredAction
	) override;

	// Publish the current selection's types on this object, in order.
	void Describe();

private:
	SessionClipboard* m_Clipboard = nullptr;

	std::uint32_t m_Generation = 0;
};

// One client's `wl_data_device`: the seat's end of the clipboard.
class ClientDataDevice final : public Wayland::Server::WlDataDeviceIgnoring
{
public:
	ClientDataDevice(DataDeviceManagerGlobal& manager, wl_client& client) noexcept
		: m_Manager{ &manager }, m_Client{ &client }
	{}

	void OnGone() override;

	// **The current selection is offered at once where this client already has the keyboard.** A
	// toolkit binds the seat and the data device during startup and expects to know what is on the
	// clipboard without waiting for focus to move — and focus may never move again, since the window
	// that has it is the one that just opened.
	void OnBound() override;

	void OnSetSelection(Wayland::Server::WlDataSource source, std::uint32_t serial) override;

	// Not built. See the header comment: a drag is a pointer gesture rather than a second clipboard,
	// and refusing it loudly is what keeps a toolkit from waiting on an `enter` that never comes.
	void OnStartDrag(
		Wayland::Server::WlDataSource source,
		Wayland::Server::WlSurface origin,
		Wayland::Server::WlSurface icon,
		std::uint32_t serial
	) override;

	void OnRelease() override {}

	[[nodiscard]] wl_client* Client() const noexcept { return m_Client; }

	// Introduce the current selection to this client: a `data_offer`, its types, and the `selection`
	// that hands it over — or a nil selection where there is nothing, which is what tells a toolkit to
	// grey out Paste.
	void Offer(SessionClipboard& clipboard);

private:
	DataDeviceManagerGlobal* m_Manager = nullptr;

	wl_client* m_Client = nullptr;
};

// One client's `wl_data_device_manager`.
class ClientDataDeviceManager final : public Wayland::Server::WlDataDeviceManagerIgnoring
{
public:
	explicit ClientDataDeviceManager(DataDeviceManagerGlobal& manager) noexcept : m_Manager{ &manager } {}

	void OnGone() override { delete this; }

	void OnRelease() override {}

	Wayland::Server::WlDataSourceHandler* OnCreateDataSource() override;

	Wayland::Server::WlDataDeviceHandler* OnGetDataDevice(Wayland::Server::WlSeat seat) override;

private:
	DataDeviceManagerGlobal* m_Manager = nullptr;
};

// The global itself, owned by the host and outliving every client that binds it.
class DataDeviceManagerGlobal final : public Wayland::Server::WlDataDeviceManagerBinding
{
public:
	explicit DataDeviceManagerGlobal(HostContext& context) noexcept : m_Context{ &context } {}

	// The loop a background fetch and a background paste are armed on. Wired once, before the global is
	// advertised, and it outlives every client.
	void Open(wl_event_loop& loop) noexcept { m_Loop = &loop; }

	Wayland::Server::WlDataDeviceManagerHandler* OnBind(wl_client& client, std::uint32_t version) override;

	// A device came or went. Borrowed pointers, and each one deregisters itself, exactly as the seat's
	// keyboards do.
	void Add(ClientDataDevice& device);
	void Remove(ClientDataDevice& device) noexcept;

	// Bring the focused client's idea of the clipboard up to date with the world's, and send nothing
	// where it has not changed. Called from `Advance` beside the seat's own focus comparison and
	// against the same answer, because a `wl_keyboard.enter` and a `selection` are one fact told to two
	// objects: a window that got the keys without being told what is on the clipboard is one where a
	// person's first Ctrl+V does nothing.
	void Sync(EntityId focused);

	// A selection was set: whoever has the keyboard is told, now rather than at the next focus change.
	void Republish(const SessionClipboard& clipboard);

	// A device was just created by a client that already has the keyboard, which `Sync` cannot notice —
	// focus did not move and the selection did not change, so nothing it compares has changed. The
	// object is new and has been told nothing, and that is the difference.
	void Introduce(ClientDataDevice& device);

	// The clipboard a client's requests reach, or null before the server is open.
	[[nodiscard]] SessionClipboard* ClipboardFor(wl_client* client);

	// The session's clipboard ends with the session.
	void Close(SessionId session) noexcept { m_Clipboards.Close(session); }

	// Everything, before the display goes: an event source that outlives its loop is a use-after-free
	// at shutdown rather than a leak.
	void Close() noexcept { m_Clipboards.Close(); }

	[[nodiscard]] HostContext& Context() const noexcept { return *m_Context; }

	[[nodiscard]] wl_event_loop* Loop() const noexcept { return m_Loop; }

private:
	// The client behind an entity, or null for a window gyro authored for itself — which is every node
	// the splash and the recovery console draw.
	[[nodiscard]] wl_client* ClientOf(EntityId window) const;

	HostContext* m_Context = nullptr;
	wl_event_loop* m_Loop = nullptr;

	SessionClipboards m_Clipboards;

	// Borrowed, and each one deregisters itself. A vector because the length is the connections on the
	// machine and it is walked once per focus change.
	std::vector<ClientDataDevice*> m_Devices;

	// **Which client has been told what the clipboard is**, which is the same distinction the seat
	// draws over focus: the world's answer can be a window no client is behind, and this is who the
	// last `selection` went to.
	wl_client* m_Told = nullptr;

	// The generation that client was told about, so that a selection set while focus did not move is
	// still delivered.
	std::uint32_t m_ToldGeneration = 0;
};
