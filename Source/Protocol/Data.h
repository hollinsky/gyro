#pragma once

#include <cstdint>

#include "Wayland/Server/Wayland.h"

// `wl_data_device_manager`: copy-and-paste and drag-and-drop, and the global a toolkit will not start
// without.
//
// **Nothing here transfers anything, and that is the whole of what it is for today.** GTK4 checks for
// `wl_compositor`, `wl_shm` and this one before it will open a Wayland display at all, and gives up
// with *the Wayland compositor does not provide one or more of the required interfaces* when any is
// missing — so without this global a person double-clicks gnome-calculator and gets nothing, no
// window and no message anywhere they would look. That is the cost being paid off, and it is worth
// saying that it is the *only* one: a person still cannot copy or paste, because there is nothing to
// copy with.
//
// **The reason it can be inert is structural rather than an excuse.** Every flow this interface
// describes begins at a `wl_seat`: a selection is set with a serial from a keyboard focus, a drag
// starts from an implicit pointer grab, and `get_data_device` takes the seat as an argument. gyro
// advertises no seat, so a client cannot obtain a `wl_data_device` at all — which leaves
// `create_data_source` as the one request here a client can actually reach, and a source nobody can
// ever select is a list of MIME types with no other side. So the requests are accepted and the list
// is dropped, rather than stored against a selection that cannot exist.
//
// **Not a refusal to advertise, and not the real clipboard either.** Leaving the global out is what
// the tree does today and it costs every GTK application; writing the selection machinery now would
// mean writing the half that validates a serial against a focus that has no source, and it would be
// rewritten by the seat that gives it one. What is here is the registry entry and the object graph
// under it, so the day a seat lands the missing part is the transfer rather than the plumbing.
//
// **It is advertised to everyone because there is nothing yet that could advertise it to fewer.**
// [Architecture.md](../../Docs/Architecture.md#filtered-globals) puts the data device in the
// session-scoped tier — a clipboard is shared between the windows of one person's session and not
// across the machine — and that tier arrives with a registry whose contents are a function of the
// connection. Nothing in this file changes when it does; who is offered it does.
//
// **Advertised at version 3.** `wl_compositor`'s rule is that the number promises the *events* gyro
// sends, and here every event above version 1 — the drag-and-drop action negotiation on a source and
// an offer — is in a flow that starts at a seat, so no version of this interface is more honest than
// another while there is none. What decides it instead is the requests: 3 is where every toolkit
// stops asking, and 4 adds only a destructor for a singleton a client holds for its own lifetime.
inline constexpr std::uint32_t DataDeviceManagerVersion = 3;

// One client's `wl_data_source`: what it would offer if there were anywhere to offer it.
//
// Ignoring rather than handling: the MIME types and the drag actions are read off the wire and
// dropped, because the only party that would ever match them against a destination is the seat that
// does not exist. A source is never selected, so it is never sent `target`, `send` or `cancelled` —
// the client keeps believing it holds data nobody asks for, which is true.
class ClientDataSource final : public Wayland::Server::WlDataSourceIgnoring
{
public:
	// The source owns itself, as every object under this global does: destroyed by the client, or with
	// the client.
	void OnGone() override { delete this; }
};

// One client's `wl_data_device`: the seat's end of the clipboard, and unreachable until there is a
// seat.
//
// `get_data_device` names a `wl_seat`, and a client can only name one it got from the registry, so
// with no seat advertised nothing constructs this. It exists because the request is on the interface
// and a handler that answered it with null would end the client with `no_memory` for asking a legal
// question.
class ClientDataDevice final : public Wayland::Server::WlDataDeviceIgnoring
{
public:
	void OnGone() override { delete this; }
};

// One client's `wl_data_device_manager`.
class ClientDataDeviceManager final : public Wayland::Server::WlDataDeviceManagerIgnoring
{
public:
	void OnGone() override { delete this; }

	Wayland::Server::WlDataSourceHandler* OnCreateDataSource() override;

	Wayland::Server::WlDataDeviceHandler* OnGetDataDevice(Wayland::Server::WlSeat seat) override;
};

// The global itself, owned by whoever advertises it and outliving every client that binds it.
class DataDeviceManagerGlobal final : public Wayland::Server::WlDataDeviceManagerBinding
{
public:
	Wayland::Server::WlDataDeviceManagerHandler* OnBind(wl_client& client, std::uint32_t version) override;
};
