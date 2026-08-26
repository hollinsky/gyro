#include "Protocol/Shell.h"

#include <wayland-server-core.h>

#include <optional>
#include <span>

#include "Core/ColorState.h"
#include "Core/Texture.h"
#include "Protocol/Floor.h"
#include "Scene/Commit.h"
#include "Scene/Entity.h"
#include "Scene/Store.h"
#include "World/Content.h"

namespace
{
// A serial the client can hand back. libwayland's own counter, which is the same one every other
// event in this connection is stamped from — a second sequence here would be two answers to *which
// configure is this*.
[[nodiscard]] std::uint32_t NextSerial(wl_client* client) noexcept
{
	return client == nullptr ? 0U : wl_display_next_serial(wl_client_get_display(client));
}
} // namespace

void ClientXdgToplevel::OnGone()
{
	if (m_Surface != nullptr)
	{
		m_Surface->Orphan();
	}

	delete this;
}

void ClientXdgToplevel::OnShowWindowMenu(
	Wayland::Server::WlSeat seat,
	std::uint32_t serial,
	std::int32_t x,
	std::int32_t y
)
{
	(void)seat;
	(void)serial;
	(void)x;
	(void)y;
}

void ClientXdgToplevel::OnMove(Wayland::Server::WlSeat seat, std::uint32_t serial)
{
	(void)seat;
	(void)serial;
}

void ClientXdgToplevel::OnResize(
	Wayland::Server::WlSeat seat,
	std::uint32_t serial,
	Wayland::Server::XdgToplevelResizeEdge edges
)
{
	(void)seat;
	(void)serial;
	(void)edges;
}

void ClientXdgToplevel::OnSetMaxSize(std::int32_t width, std::int32_t height)
{
	// Negative is the one thing the protocol calls an error here; zero is *no limit* and is what almost
	// every client sends. gyro never resizes a window, so the numbers themselves have no reader yet —
	// what is worth keeping is the refusal, because a client that sent a negative and was not told is a
	// client whose own layout arithmetic has already gone wrong.
	if (width < 0 || height < 0)
	{
		Object().PostError(
			Wayland::Server::XdgToplevelError::InvalidSize, "xdg_toplevel.set_max_size with a negative extent"
		);
	}
}

void ClientXdgToplevel::OnSetMinSize(std::int32_t width, std::int32_t height)
{
	if (width < 0 || height < 0)
	{
		Object().PostError(
			Wayland::Server::XdgToplevelError::InvalidSize, "xdg_toplevel.set_min_size with a negative extent"
		);
	}
}

ClientXdgSurface::~ClientXdgSurface()
{
	Unmap();

	if (m_Surface != nullptr)
	{
		m_Surface->ForgetRole(*this);
	}

	// The toplevel outlives this only in the case the protocol calls an error — destroying the
	// `xdg_surface` before its role object — and it must not be left pointing at freed memory while it
	// waits for its own destroy request to arrive.
	if (m_Toplevel != nullptr)
	{
		m_Toplevel->Forget();
	}
}

Wayland::Server::XdgToplevelHandler* ClientXdgSurface::OnGetToplevel()
{
	if (m_Toplevel != nullptr)
	{
		Object().PostError(
			Wayland::Server::XdgSurfaceError::AlreadyConstructed,
			"xdg_surface.get_toplevel on a surface that already has a role object"
		);
	}

	m_Toplevel = new ClientXdgToplevel{ *this };

	return m_Toplevel;
}

Wayland::Server::XdgPopupHandler*
ClientXdgSurface::OnGetPopup(Wayland::Server::XdgSurface parent, Wayland::Server::XdgPositioner positioner)
{
	(void)parent;
	(void)positioner;

	return new ClientXdgPopup{};
}

void ClientXdgSurface::OnSetWindowGeometry(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
	if (width <= 0 || height <= 0)
	{
		Object().PostError(
			Wayland::Server::XdgSurfaceError::InvalidSize,
			"xdg_surface.set_window_geometry with an extent that is zero or negative"
		);

		return;
	}

	m_PendingGeometry = { { x, y }, { width, height } };
	m_GeometryStaged = true;
}

void ClientXdgSurface::OnAckConfigure(std::uint32_t serial)
{
	// **Any serial gyro has actually sent, not only the newest.** A client may be several configures
	// behind — the protocol says older ones are simply superseded — so the check that matters is that
	// the number came from here at all, and a serial from the future is a client acknowledging
	// something nobody said.
	if (!m_Configured || serial > m_Serial)
	{
		Object().PostError(
			Wayland::Server::XdgSurfaceError::InvalidSerial,
			"xdg_surface.ack_configure with a serial this compositor never sent"
		);

		return;
	}

	m_Acked = true;
}

void ClientXdgSurface::Configure()
{
	if (m_Toplevel == nullptr)
	{
		return;
	}

	m_Serial = NextSerial(Object().WireClient());
	m_Configured = true;

	// **Zero by zero, and it is the truthful answer rather than a placeholder.** The protocol reads it
	// as *pick your own size*, which is exactly what a compositor placing windows at their natural size
	// has to say. A number invented here would be one the client is obliged to obey, so it would be
	// gyro doing layout — which is the thing decision 51 keeps out of the compositor.
	//
	// No states either: maximised, fullscreen, resizing and activated are all facts about window
	// management or input, and gyro has neither yet. An empty list is a window that is none of them.
	m_Toplevel->Object().Configure(0, 0, {});

	Object().Configure(m_Serial);
}

Size<SurfaceSpace, float> ClientXdgSurface::Natural(const ClientSurface& surface) const noexcept
{
	// The client's declared geometry is its *visible* bounds, which is what has to be centred: a
	// toolkit that draws its own shadow makes the surface substantially larger than the window, and
	// centring the surface would put the window itself off-centre by the shadow's margin.
	if (!m_Geometry.Extent.IsEmpty())
	{
		return { static_cast<float>(m_Geometry.Extent.Width), static_cast<float>(m_Geometry.Extent.Height) };
	}

	const SurfaceState& state = surface.Current();
	const std::int32_t scale = state.BufferScale > 0 ? state.BufferScale : 1;

	return { static_cast<float>(state.ContentSize.Width / scale),
		     static_cast<float>(state.ContentSize.Height / scale) };
}

void ClientXdgSurface::OnSurfaceCommitted(ClientSurface& surface)
{
	if (m_GeometryStaged)
	{
		m_Geometry = m_PendingGeometry;
		m_GeometryStaged = false;
	}

	if (m_Toplevel == nullptr)
	{
		// An `xdg_surface` with no role object yet. A commit is how the client asks to be configured and
		// is legal; a *buffer* is not, because there is nothing for it to be the content of.
		if (!surface.Current().Content.IsNull())
		{
			Object().PostError(
				Wayland::Server::XdgSurfaceError::NotConstructed,
				"a buffer committed to an xdg_surface that has no role object"
			);
		}

		return;
	}

	// The first commit after `get_toplevel` is the client asking for a size, and the answer is the
	// initial configure. It carries no buffer by the protocol's own rule, so there is nothing to map.
	if (!m_Configured)
	{
		Configure();

		return;
	}

	if (surface.Current().Content.IsNull())
	{
		// Attaching nothing unmaps the window, and the next buffer maps it again — which the protocol
		// says restarts the whole configure sequence, so the acknowledgement goes with it.
		Unmap();

		m_Configured = false;
		m_Acked = false;

		return;
	}

	if (!m_Acked)
	{
		Object().PostError(
			Wayland::Server::XdgSurfaceError::UnconfiguredBuffer,
			"a buffer committed before the configure it was drawn for was acknowledged"
		);

		return;
	}

	Map(surface);
}

void ClientXdgSurface::Map(ClientSurface& surface)
{
	SceneStore* const scene = m_Context->Store();

	if (scene == nullptr)
	{
		return;
	}

	const SurfaceState& state = surface.Current();
	const Size<SurfaceSpace, float> natural = Natural(surface);
	const std::int32_t scale = state.BufferScale > 0 ? state.BufferScale : 1;

	// The surface's own quad, which is the buffer divided by the scale the client declared for it. Not
	// the natural size: those differ by exactly the shadow margin a toolkit draws outside its window.
	const Size<SurfaceSpace, float> extent{ static_cast<float>(state.ContentSize.Width / scale),
		                                    static_cast<float>(state.ContentSize.Height / scale) };

	const bool mapping = m_Window.IsNull();

	if (mapping)
	{
		const std::optional<EntityId> window = scene->CreateContainer(m_Context->Floor(), { .Extent = natural });

		if (!window)
		{
			Object().PostNoMemory();

			return;
		}

		// The pixels hang under the window rather than being it, which is decision 111's toplevel: a
		// container holding its subsurfaces and its own surface. The offset is the client's declared
		// geometry pushed back to the surface's origin, so that what gets centred is the window and not
		// the shadow around it.
		const std::optional<EntityId> content = scene->CreateImage(
			*window,
			{ .Position = { -static_cast<double>(m_Geometry.Origin.X), -static_cast<double>(m_Geometry.Origin.Y), 0.0 },
		      .Extent = extent },
			ImageContent{ .Texture = state.Content, .Source = {}, .Frame = {}, .Color = ColorState::Srgb() }
		);

		if (!content)
		{
			// The container is retired rather than left: a window with no pixels under it is an empty
			// rectangle a person would see and could not close.
			SceneCommit undo{ *scene, CommitAuthor::Client };

			static_cast<void>(undo.Retire(*window));

			Object().PostNoMemory();

			return;
		}

		m_Window = *window;
		m_Content = *content;

		// **Where the return leg finds its way back to a client.** What `Scene/Return.h` reports is the
		// entity whose pixels reached the glass, because it may not name a `wl_surface` (115) — so the
		// image entity is registered against the surface that draws into it, here, at the one moment both
		// identities exist and are known to be each other's.
		m_Context->Bind(*content, surface);

		// **And the window itself, which is the entity focus names.** The return leg arrives as the
		// image whose pixels reached the glass; focus is on the container, because that is decision
		// 111's toplevel and the thing a focus ring is drawn around. Both resolve to the same surface,
		// so the seat and the frame callback each ask about the entity their own half deals in.
		m_Context->Bind(*window, surface);

		// The window a person just opened is the one they are typing into, which is the Floorplanner's
		// rule applied to focus (`Scene/Focus.h`): newest on top, and no parameter to pick.
		scene->Focus().Offer(*window);
	}

	{
		// The client's own transaction: what it committed, arriving in the world as one change with one
		// origin. A client commit carries no timestamp and needs none — nothing here is a sprung channel.
		SceneCommit commit{ *scene, CommitAuthor::Client };

		static_cast<void>(commit.Attach(m_Content, state.Content));
		static_cast<void>(commit.Resize(m_Content, extent));
		static_cast<void>(commit.Resize(m_Window, natural));
	}

	if (mapping)
	{
		// **A second transaction, because this one is gyro's.** The placement is not something the
		// client asked for — decision 141 has the Floorplanner author it, stamped with the arrival of
		// the window because nothing routed it and there is no earlier moment an entrance could point
		// at. Commits do not nest, so the client's closes above before this opens.
		SceneCommit placement{ *scene, CommitAuthor::Compositor, scene->Now() };

		PlaceOnFloor(placement, *scene, m_Window, natural);
	}
}

void ClientXdgSurface::Unmap()
{
	if (m_Window.IsNull())
	{
		return;
	}

	// Before the retire rather than after it, so that nothing can report a frame against a window that
	// is on its way out. The entity itself lives on until its exit settles (114); what stops here is the
	// route from it back to a client, because the client has taken its window down and is not waiting to
	// hear about the frames it spends leaving.
	m_Context->Unbind(m_Content);
	m_Context->Unbind(m_Window);

	SceneStore* const scene = m_Context->Store();

	if (scene != nullptr)
	{
		SceneCommit commit{ *scene, CommitAuthor::Client };

		// The subtree keeps its links and its place, which is what lets a closing window go on being
		// drawn while it is still closing. The store frees it when everything on it has settled.
		// Focus goes with it, and the store does that from `Retire` rather than from here: a window is
		// unmapped by its client and retired by anything, and focus has to leave in both cases.
		static_cast<void>(commit.Retire(m_Window));
	}

	m_Window = {};
	m_Content = {};
}

void ClientXdgSurface::Orphan() noexcept
{
	m_Toplevel = nullptr;

	Unmap();

	// The client may attach and commit again on the same `xdg_surface` after making a new role object,
	// and that is a fresh negotiation rather than a continuation of the last one.
	m_Configured = false;
	m_Acked = false;
}

void ClientXdgSurface::OnSurfaceGone()
{
	Unmap();

	m_Surface = nullptr;
}

Wayland::Server::XdgSurfaceHandler* ClientShell::OnGetXdgSurface(Wayland::Server::WlSurface surface)
{
	// The id the client named may be any object at all; `Implementation` is what refuses one that is
	// not a `wl_surface` gyro made, checking the interface and the dispatch table before it reads
	// anything.
	Wayland::Server::WlSurfaceHandler* const handler = surface.Implementation();

	if (handler == nullptr)
	{
		Object().PostError(
			Wayland::Server::XdgWmBaseError::Role, "xdg_wm_base.get_xdg_surface with something that is not a wl_surface"
		);

		return new ClientXdgSurface{ *m_Context, nullptr };
	}

	auto* const client = static_cast<ClientSurface*>(handler);
	auto* const role = new ClientXdgSurface{ *m_Context, client };

	if (!client->AdoptRole(*role))
	{
		Object().PostError(
			Wayland::Server::XdgWmBaseError::Role, "xdg_wm_base.get_xdg_surface on a wl_surface that already has a role"
		);
	}

	return role;
}

Wayland::Server::XdgWmBaseHandler* ShellGlobal::OnBind(wl_client& client, std::uint32_t version)
{
	(void)client;
	(void)version;

	return new ClientShell{ *m_Context };
}
