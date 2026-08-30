#include "Protocol/Shell.h"

#include <wayland-server-core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "Animation/Author/Bundle.h"
#include "Core/ColorState.h"
#include "Core/Texture.h"
#include "Protocol/Floor.h"
#include "Protocol/Popup.h"
#include "Protocol/Seat.h"
#include "Scene/Commit.h"
#include "Scene/Entity.h"
#include "Scene/Hit.h"
#include "Scene/Output.h"
#include "Scene/Reach.h"
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

// A surface-local point rounded onto the grid a positioner works on.
//
// **Nearest rather than outward**, because what is being converted is the edge of a screen and the
// half-pixel either way is invisible: a menu one device pixel over the edge of a panel is a menu one
// pixel over the edge, and expanding the bound outward to be safe would mean a menu that is *allowed*
// to sit a pixel off screen on every machine rather than only where the arithmetic lands on a half.
[[nodiscard]] PixelPoint<SurfaceSpace> OnGrid(Point<SurfaceSpace> point) noexcept
{
	return { static_cast<std::int32_t>(std::lround(point.X)), static_cast<std::int32_t>(std::lround(point.Y)) };
}

// How much of two rectangles overlap, as an area. Sixty-four bits because the inputs are a client's
// own numbers and a toolkit that sends an anchor rectangle of two billion by two billion is a bug
// report rather than an overflow.
[[nodiscard]] std::int64_t OverlapArea(PixelRect<SurfaceSpace> left, PixelRect<SurfaceSpace> right) noexcept
{
	const std::int64_t width = std::max(0, std::min(left.Right(), right.Right()) - std::max(left.Left(), right.Left()));
	const std::int64_t height =
		std::max(0, std::min(left.Bottom(), right.Bottom()) - std::max(left.Top(), right.Top()));

	return width * height;
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

void ClientXdgToplevel::Configure(std::int32_t width, std::int32_t height) const
{
	// The states as the protocol carries them: an array of `uint32`, handed over as the bytes behind it.
	// A fixed array rather than a vector because the set is closed and tiny — one entry today, and the
	// resize grab's `resizing` beside it when that lands — and a heap allocation here would be one per
	// frame of a resize, on the thread a person's drag is being answered by.
	std::array<Wayland::Server::XdgToplevelState, 1> states{};
	std::size_t count = 0;

	if (m_Activated)
	{
		states[count++] = Wayland::Server::XdgToplevelState::Activated;
	}

	Object().Configure(width, height, std::as_bytes(std::span{ states.data(), count }));
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
	// **Decision 51's continuous manipulation, and the request that starts it is the whole of the round
	// trip.** From here until the button comes up the window is moved by gyro at the rate the pointer is
	// sampled at, with nothing going back to the client — which is what makes a dragged window stay
	// under the cursor instead of swimming behind it.
	//
	// **A refusal is silence, which is the protocol's own shape.** There is no reply to this request and
	// no error for one gyro declines: a client learns what it got from what happens next, and every
	// toolkit already handles a compositor that ignores a move — it is the state a window in a tiling
	// session is in permanently. [Seat.h](Seat.h) has the three things that have to be true.
	if (m_Surface != nullptr)
	{
		m_Surface->BeginMove(seat, serial);
	}
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

void ClientXdgPositioner::OnSetSize(std::int32_t width, std::int32_t height)
{
	// Zero is refused as well as negative, which the protocol spells out and is worth honouring rather
	// than quietly accepting: a popup of no size is one the client will attach a buffer to and never
	// see, and the several minutes a person spends looking for the menu are minutes the error would
	// have saved them.
	if (width <= 0 || height <= 0)
	{
		Object().PostError(
			Wayland::Server::XdgPositionerError::InvalidInput,
			"xdg_positioner.set_size with an extent that is not positive"
		);

		return;
	}

	m_Rules.Extent = { width, height };
}

void ClientXdgPositioner::OnSetAnchorRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
	// **Zero is legal here and negative is not**, which is the difference from the size above. A text
	// caret is an anchor rectangle one pixel wide and nothing wide is a perfectly sensible thing to
	// hang a menu off; a negative extent is a client whose own layout arithmetic has already gone wrong.
	if (width < 0 || height < 0)
	{
		Object().PostError(
			Wayland::Server::XdgPositionerError::InvalidInput, "xdg_positioner.set_anchor_rect with a negative extent"
		);

		return;
	}

	m_Rules.AnchorRect = { { x, y }, { width, height } };
	m_Rules.AnchorStated = true;
}

ClientXdgPopup::ClientXdgPopup(
	HostContext& context,
	ClientXdgSurface& surface,
	ClientXdgSurface* parent,
	PopupPlacement rules
)
	: m_Context{ &context }, m_Surface{ &surface }, m_Parent{ parent }, m_Rules{ rules }
{
	if (m_Parent != nullptr)
	{
		m_Parent->Adopt(*this);
	}
}

ClientXdgPopup::~ClientXdgPopup()
{
	Leave();

	if (m_Parent != nullptr)
	{
		m_Parent->Release(*this);
		m_Parent = nullptr;
	}

	// The `xdg_surface` outlives this only where the client destroyed the role object first, which is
	// the ordinary way a menu closes. It has a window in the world to take down either way.
	if (m_Surface != nullptr)
	{
		m_Surface->Orphan();
	}
}

EntityId ClientXdgPopup::Node() const noexcept
{
	return m_Surface != nullptr ? m_Surface->Window() : EntityId{};
}

void ClientXdgPopup::OnGrab(Wayland::Server::WlSeat seat, std::uint32_t serial)
{
	(void)seat;

	// **The serial is not checked against the event it came from, and that is a stated gap.** The
	// protocol lets a compositor dismiss a popup immediately where the serial does not name a press it
	// sent, which is the defence against an application stealing input by grabbing out of nowhere. What
	// it needs is a ledger of the serials the seat handed out and how recently, and the seat keeps one
	// counter rather than a history — so the check would be *is this number plausible*, which is not a
	// check. The exposure is one misbehaving client on a machine that has no session yet.
	(void)serial;

	if (m_Surface != nullptr && m_Surface->IsMapped())
	{
		// A grab has to be asked for before the popup is committed, because the whole of what it changes
		// is what happens at the moment the menu goes up.
		Object().PostError(
			Wayland::Server::XdgPopupError::InvalidGrab, "xdg_popup.grab on a popup that is already mapped"
		);

		return;
	}

	m_Grabbing = true;
}

void ClientXdgPopup::OnReposition(Wayland::Server::XdgPositioner positioner, std::uint32_t token)
{
	// Unreachable while `ShellVersion` is 1 — libwayland refuses the opcode before it reaches here —
	// and implemented rather than stubbed for the reason [Shell.h](Shell.h) gives at length: a
	// `reposition` that is accepted and never answered is a client blocked forever inside its own
	// event loop, which is exactly the failure this file was rewritten for.
	Wayland::Server::XdgPositionerHandler* const handler = positioner.Implementation();

	if (handler == nullptr)
	{
		return;
	}

	const PopupPlacement& rules = static_cast<ClientXdgPositioner*>(handler)->Rules();

	if (!rules.IsComplete())
	{
		if (m_Surface != nullptr)
		{
			m_Surface->Base().PostError(
				Wayland::Server::XdgWmBaseError::InvalidPositioner,
				"xdg_popup.reposition with a positioner that has no size or no anchor rectangle"
			);
		}

		return;
	}

	m_Rules = rules;
	m_RepositionToken = token;
	m_Repositioned = true;

	// **The whole answer goes out now rather than at the next commit.** `repositioned`, then the
	// popup's configure, then the surface's — that order is the protocol's, and a client waits on the
	// last of the three before it will draw anything again.
	if (m_Surface != nullptr)
	{
		m_Surface->Configure();
	}
}

void ClientXdgPopup::AnswerReposition()
{
	if (!m_Repositioned)
	{
		return;
	}

	m_Repositioned = false;

	if (Object().IsValid())
	{
		Object().Repositioned(m_RepositionToken);
	}
}

void ClientXdgPopup::Resolve()
{
	const PixelRect<SurfaceSpace> placed = PlacePopup(m_Rules, Bounds());

	if (placed == m_Placement)
	{
		return;
	}

	m_Placement = placed;
	m_PlacementPending = true;
}

PixelRect<SurfaceSpace> ClientXdgPopup::Bounds() const
{
	const SceneStore* const scene = m_Context->Store();

	if (scene == nullptr || m_Parent == nullptr || !m_Parent->IsMapped())
	{
		return {};
	}

	// **The screen, expressed in the parent's own coordinates.** The positioner speaks entirely in the
	// space of the parent's window geometry, and an output is a rectangle of global space, so one of the
	// two has to travel — and it is the output, because it is the one that does not have to survive the
	// journey exactly. `Scene/Hit.h`'s `LocalOn` is the same unprojection the pointer uses, which is
	// what keeps *where the screen edge is* and *where a click lands* from being computed two ways.
	const EntityId window = m_Parent->Window();

	PixelRect<SurfaceSpace> best{};
	std::int64_t covered = -1;

	for (const SceneOutput& output : scene->Outputs())
	{
		const std::optional<Point<SurfaceSpace>> topLeft = LocalOn(*scene, window, output.Bounds.Origin);
		const std::optional<Point<SurfaceSpace>> bottomRight =
			LocalOn(*scene, window, Point<GlobalSpace>{ output.Bounds.Right(), output.Bounds.Bottom() });

		if (!topLeft || !bottomRight)
		{
			continue;
		}

		const PixelRect<SurfaceSpace> local =
			PixelRect<SurfaceSpace>::FromEdges(OnGrid(*topLeft), OnGrid(*bottomRight));

		// **Chosen by how much of the anchor rectangle it holds**, which is *the output the control the
		// menu belongs to is on*. Not the output the popup would land on, which is the question that has
		// no answer yet — the placement is what is being computed — and not the one holding the pointer,
		// which would put a submenu on a different screen from its parent when a person's hand wandered.
		const std::int64_t area = OverlapArea(local, m_Rules.AnchorRect);

		if (area > covered)
		{
			covered = area;
			best = local;
		}
	}

	// A parent that lands on no output at all leaves `best` as the last output looked at, which is the
	// right answer: a window off the side of a desk still has a screen its menus should try to stay on,
	// and the alternative — constraining against nothing — is a menu that opens off screen too.
	return best;
}

void ClientXdgPopup::Enter()
{
	if (m_Entered || !m_Grabbing)
	{
		return;
	}

	m_Context->Popups().Push(*this);
	m_Entered = true;

	// **The keyboard goes with the grab**, which the protocol states outright: the topmost grabbing
	// popup has focus for as long as it is up. It is the same `Offer` a toplevel makes when it maps, so
	// the fall back to the window underneath when the menu closes is `SceneFocus`'s stack policy doing
	// what it already does rather than anything this object has to remember (114, 149).
	if (SceneStore* const scene = m_Context->Store(); scene != nullptr)
	{
		scene->Focus().Offer(Node());
	}
}

void ClientXdgPopup::Leave() noexcept
{
	if (m_Entered)
	{
		m_Context->Popups().Remove(*this);
		m_Entered = false;
	}
}

void ClientXdgPopup::Dismiss()
{
	if (m_Dismissed)
	{
		return;
	}

	m_Dismissed = true;

	Leave();

	if (Object().IsValid())
	{
		Object().PopupDone();
	}

	// **Down now rather than when the client gets round to it.** `popup_done` is a request to the
	// client to take its own menu away and it will, one round trip from now; leaving the rectangle on
	// screen until then means a menu that survives the click that dismissed it by a frame or two,
	// which reads as the click having missed.
	if (m_Surface != nullptr)
	{
		m_Surface->Withdraw();
	}
}

void ClientXdgPopup::ForgetParent() noexcept
{
	// Cleared first, so that the unmap below cannot walk back into the parent's own child list while
	// the parent is being destroyed through it.
	m_Parent = nullptr;

	Dismiss();
}

ClientXdgSurface::~ClientXdgSurface()
{
	// **The children first, because a menu whose window has gone has nothing left to hang on.** The
	// protocol requires a client to destroy its popups before their parent and calls doing otherwise an
	// error; gyro still has to survive the client that gets it wrong, and the way it survives is that
	// each child drops its pointer here rather than discovering later that it is dangling. Iterated over
	// a copy, since `ForgetParent` is what empties the list.
	const std::vector<ClientXdgPopup*> children = m_Children;

	m_Children.clear();

	for (ClientXdgPopup* const child : children)
	{
		child->ForgetParent();
	}

	Unmap();

	if (m_Surface != nullptr)
	{
		m_Surface->ForgetRole(*this);
	}

	// The role object outlives this only in the case the protocol calls an error — destroying the
	// `xdg_surface` before it — and it must not be left pointing at freed memory while it waits for its
	// own destroy request to arrive.
	if (m_Toplevel != nullptr)
	{
		m_Toplevel->Forget();
	}

	if (m_Popup != nullptr)
	{
		m_Popup->Forget();
	}
}

void ClientXdgSurface::Adopt(ClientXdgPopup& popup)
{
	m_Children.push_back(&popup);
}

void ClientXdgSurface::Release(ClientXdgPopup& popup) noexcept
{
	std::erase(m_Children, &popup);
}

Wayland::Server::XdgToplevelHandler* ClientXdgSurface::OnGetToplevel()
{
	if (HasRole())
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
	if (HasRole())
	{
		Object().PostError(
			Wayland::Server::XdgSurfaceError::AlreadyConstructed,
			"xdg_surface.get_popup on a surface that already has a role object"
		);
	}

	// **Every refusal below still returns an object**, which is the same rule `ClientShell` follows for
	// a `wl_surface` that was not one: the client has already chosen an id and libwayland needs
	// something behind it, and a popup with no parent and no rules maps nothing and configures nothing.
	// The connection is over by then in any case — `PostError` is what ends it.
	Wayland::Server::XdgPositionerHandler* const rules = positioner.Implementation();

	if (rules == nullptr)
	{
		m_Base.PostError(
			Wayland::Server::XdgWmBaseError::InvalidPositioner,
			"xdg_surface.get_popup with something that is not an xdg_positioner"
		);

		return new ClientXdgPopup{ *m_Context, *this, nullptr, {} };
	}

	const PopupPlacement& placement = static_cast<ClientXdgPositioner*>(rules)->Rules();

	// **Incomplete is checked here rather than at the request that left it incomplete**, because a
	// client is entitled to build a positioner across as many requests as it likes and in any order.
	// This is the first moment the compositor is allowed to have an opinion about it.
	if (!placement.IsComplete())
	{
		m_Base.PostError(
			Wayland::Server::XdgWmBaseError::InvalidPositioner,
			"xdg_surface.get_popup with a positioner that has no size or no anchor rectangle"
		);

		return new ClientXdgPopup{ *m_Context, *this, nullptr, placement };
	}

	// A null parent is legal on the wire — the protocol reserves it for a popup whose parent will be
	// declared by some other protocol, and there is no such protocol here — and every toolkit passes
	// one. Anything that is not an `xdg_surface` gyro made is the client naming the wrong id.
	Wayland::Server::XdgSurfaceHandler* const anchor = parent.Implementation();

	if (anchor == nullptr)
	{
		m_Base.PostError(
			Wayland::Server::XdgWmBaseError::InvalidPopupParent,
			"xdg_surface.get_popup with a parent that is not an xdg_surface this compositor made"
		);

		return new ClientXdgPopup{ *m_Context, *this, nullptr, placement };
	}

	auto* const anchored = static_cast<ClientXdgSurface*>(anchor);

	// **A popup may only be parented onto the topmost grabbing popup, and the check is the whole of
	// `not_the_topmost_popup`.** Without it a client can hang a menu off the window behind an open menu,
	// which on screen is a menu that appears underneath the one a person is reading — and on dismissal
	// is a chain that comes apart in the middle.
	if (ClientXdgPopup* const topmost = m_Context->Popups().Topmost();
	    topmost != nullptr && anchored != topmost->Surface())
	{
		m_Base.PostError(
			Wayland::Server::XdgWmBaseError::NotTheTopmostPopup,
			"xdg_surface.get_popup while a grabbing popup that is not the parent is open"
		);

		return new ClientXdgPopup{ *m_Context, *this, nullptr, placement };
	}

	m_Popup = new ClientXdgPopup{ *m_Context, *this, anchored, placement };

	return m_Popup;
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
	if (!HasRole())
	{
		return;
	}

	m_Serial = NextSerial(Object().WireClient());
	m_Configured = true;

	if (m_Toplevel != nullptr)
	{
		// **Zero by zero, and it is the truthful answer rather than a placeholder.** The protocol reads it
		// as *pick your own size*, which is exactly what a compositor placing windows at their natural
		// size has to say. A number invented here would be one the client is obliged to obey, so it would
		// be gyro doing layout — which is the thing decision 51 keeps out of the compositor.
		//
		// **The states are the toplevel's own and `activated` is the only one gyro has an answer to.**
		// Maximised, fullscreen and the tiled set are window management, which belongs to a shell (51);
		// `resizing` is true only inside a gesture gyro is driving and arrives with the grab that drives
		// one. Focus is neither — it is a fact about the world that `Scene/Focus.h` already holds, and a
		// window that has it and is not told draws itself grey while a person types into it.
		m_Toplevel->Configure(0, 0);
	}
	else if (m_Popup != nullptr)
	{
		// **A popup gets a number, and this is the one place gyro tells a client where it is.** It is the
		// opposite of the toplevel above and for the same reason: a size and a position resolved from what
		// the client itself described is not the compositor doing layout, it is the compositor doing the
		// arithmetic the protocol specified and nobody else is positioned to do — only gyro knows where
		// the parent is and where the screen ends.
		m_Popup->Resolve();

		const PixelRect<SurfaceSpace> placed = m_Popup->Placement();

		m_Popup->AnswerReposition();

		m_Popup->Object().Configure(placed.Origin.X, placed.Origin.Y, placed.Extent.Width, placed.Extent.Height);
	}

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

	if (!HasRole())
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

	// **A dismissed popup is inert.** The compositor has taken the menu away and told the client so;
	// what is left is a role object the client has not destroyed yet, and every commit on it is a
	// client that has not read its events. Configuring it again would put the menu back on screen.
	if (m_Popup != nullptr && m_Popup->IsDismissed())
	{
		return;
	}

	// The first commit after the role object is the client asking to be configured, and the answer is
	// the initial configure. It carries no buffer by the protocol's own rule, so there is nothing to map.
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

	// The texels behind that quad, which is the buffer undivided. Stated rather than left empty so that
	// `Frame/Projection.h` can count them: a window whose buffer scale matches the panel it is on is
	// texel for texel and can go on a plane, and a node that says nothing here can never be told apart
	// from one that is being stretched. There is no viewport yet, so the whole buffer is the source.
	const Rect<BufferSpace> source{
		{}, { static_cast<float>(state.ContentSize.Width), static_cast<float>(state.ContentSize.Height) }
	};

	const bool mapping = m_Window.IsNull();

	// **Where the window hangs, which is the whole of the difference between the two roles.** A
	// toplevel goes under the session floor and is placed by the Floorplanner (141); a popup goes under
	// the window it is anchored on and is placed by the arithmetic the client described. So a popup
	// moves with its parent for free, draws over it because the sibling list is the z order (55), and is
	// hit-tested consistently with what is drawn, because neither the frame walk nor `Scene/Hit.h` clips
	// a child to its parent.
	wl_client* const client = Object().WireClient();

	EntityId container = m_Context->Floor(client);

	if (m_Popup != nullptr)
	{
		ClientXdgSurface* const anchor = m_Popup->Parent();

		// **A popup whose parent is not on screen maps nothing and is not an error.** The protocol leaves
		// it undefined and the honest answer is to wait: the parent may still be negotiating its own first
		// buffer, and a menu parented into the floor for one frame would be a rectangle in the middle of a
		// screen with nothing under it.
		if (anchor == nullptr || !anchor->IsMapped())
		{
			return;
		}

		container = anchor->Window();
	}

	// **A container of null is a session with no floor, and the window waits rather than hanging off
	// nothing.** It is the gap between an agent's connection closing and libwayland dropping the clients
	// that arrived on its listener: the session is over, and a window mapped now would be a root of its
	// own drawn on every screen on the machine.
	if (container.IsNull())
	{
		return;
	}

	if (mapping)
	{
		const std::optional<EntityId> window = scene->CreateContainer(container, { .Extent = natural });

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
			ImageContent{ .Texture = state.Content, .Source = source, .Frame = {}, .Color = ColorState::Srgb() }
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
		// rule applied to focus (`Scene/Focus.h`): newest on top, and no parameter to pick. A popup is
		// offered focus by `Enter` below and only where it grabbed, because a tooltip that took the
		// keyboard would be a person's next keystroke going nowhere.
		if (m_Popup == nullptr)
		{
			scene->Focus().Offer(*window);

			// **And into the window registry, which is the mapped toplevels of the whole session.** A
			// popup is deliberately not in it: what the set is asked is which window is activated, and a
			// menu is part of the window it came out of rather than a second one — `FocusRestsOn` is where
			// that reading lives.
			m_Context->Add(*this);
		}
	}

	{
		// The client's own transaction: what it committed, arriving in the world as one change with one
		// origin. A client commit carries no timestamp and needs none — nothing here is a sprung channel.
		SceneCommit commit{ *scene, CommitAuthor::Client };

		static_cast<void>(commit.Attach(m_Content, state.Content, source));
		static_cast<void>(commit.Resize(m_Content, extent));
		static_cast<void>(commit.Resize(m_Window, natural));

		// **Where the surface becomes something the pointer can land on**, and it is in the client's own
		// transaction beside the resize because it is the same fact: a window that grew and reshaped in
		// one commit must never be hit-tested with one of the two halves stale, which for exactly one
		// frame would be a dead strip down the side of a window that is there.
		//
		// The container above it accepts nothing and is not told to — decision 111's toplevel is a frame
		// around the pixels, and what a client declared a region for is the surface.
		static_cast<void>(commit.AcceptInput(m_Content, state.Input));
	}

	if (mapping && m_Popup == nullptr)
	{
		// **A second transaction, because this one is gyro's.** The placement is not something the
		// client asked for — decision 141 has the Floorplanner author it, stamped with the arrival of
		// the window because nothing routed it and there is no earlier moment an entrance could point
		// at. Commits do not nest, so the client's closes above before this opens.
		SceneCommit placement{ *scene, CommitAuthor::Compositor, scene->Now() };

		PlaceOnFloor(placement, *scene, m_Context->Session(client), m_Window, natural);
	}

	if (m_Popup != nullptr && (mapping || m_Popup->PlacementPending()))
	{
		// **The popup's placement is the client's own transaction and not gyro's**, which is the
		// distinction the Floorplanner above draws in the other direction. Nothing here was decided by
		// the compositor: the position is the anchor, gravity and offset the client stated, resolved
		// against the screen it is on. It is written when it changed rather than on every commit,
		// because a menu repainting itself is not a menu that moved.
		SceneCommit placement{ *scene, CommitAuthor::Client };

		const PixelRect<SurfaceSpace> placed = m_Popup->Placement();

		static_cast<void>(placement.Move(
			m_Window, { static_cast<double>(placed.Origin.X), static_cast<double>(placed.Origin.Y), 0.0 }, Immediate()
		));

		m_Popup->PlacementApplied();
	}

	if (mapping && m_Popup != nullptr)
	{
		// **Last, because it is the step that makes the menu the thing input goes to**, and it must not
		// run before the window it names exists or before the pixels under it do.
		m_Popup->Enter();
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
	// A menu on its way out stops being one the pointer is routed to before it stops being drawn, for
	// the reason focus leaves at retirement rather than at the free (114): the click that dismissed it
	// must not be followed by a second one landing on its ghost.
	if (m_Popup != nullptr)
	{
		m_Popup->Leave();
	}

	m_Context->Unbind(m_Content);
	m_Context->Unbind(m_Window);
	m_Context->Remove(*this);

	// **What the client has been told goes with the window.** An unmapped toplevel that is committed
	// again negotiates from the start — a configure, an ack, then a buffer — and the states go with that
	// sequence, so a window that was focused when it went down must be told again rather than assumed to
	// have remembered.
	SetActivated(false);

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
	m_Popup = nullptr;

	Withdraw();
}

void ClientXdgSurface::Withdraw() noexcept
{
	Unmap();

	// The client may attach and commit again on the same `xdg_surface` after making a new role object,
	// and that is a fresh negotiation rather than a continuation of the last one.
	m_Configured = false;
	m_Acked = false;
}

void ClientXdgSurface::BeginMove(Wayland::Server::WlSeat seat, std::uint32_t serial)
{
	SceneStore* const scene = m_Context->Store();

	// Unmapped, or outside a dispatch. A client is entitled to ask before its window exists — a toolkit
	// that hands a press to its own titlebar before the first frame is ordinary — and there is nothing
	// in the world to move.
	if (scene == nullptr || m_Window.IsNull())
	{
		return;
	}

	// **The seat the client named rather than one this object went looking for.** The protocol makes it
	// an argument because a grab belongs to a seat, and `Implementation` is what refuses an id that is
	// not a `wl_seat` gyro made — the same check `get_popup` makes of a parent, and for the same reason:
	// reading user data off an arbitrary id is a type confusion a client can reach on purpose.
	Wayland::Server::WlSeatHandler* const handler = seat.Implementation();

	if (handler == nullptr)
	{
		return;
	}

	static_cast<void>(static_cast<ClientSeat*>(handler)->Seat().BeginMove(*scene, m_Window, serial));
}

bool ClientXdgSurface::SetActivated(bool activated) noexcept
{
	return m_Toplevel != nullptr && m_Toplevel->SetActivated(activated);
}

void ClientXdgSurface::OnSurfaceGone()
{
	Unmap();

	m_Surface = nullptr;
}

bool FocusRestsOn(const SceneStore& scene, EntityId window, EntityId focused)
{
	if (window.IsNull() || focused.IsNull())
	{
		return false;
	}

	EntityId at = focused;

	for (std::size_t depth = 0; !at.IsNull() && depth < MaxReachDepth; ++depth)
	{
		if (at == window)
		{
			return true;
		}

		const Entity* const entity = scene.Find(at);

		if (entity == nullptr)
		{
			return false;
		}

		at = entity->Parent;
	}

	return false;
}

void SyncActivation(HostContext& context, const SceneStore& scene, EntityId focused)
{
	// Iterated over a copy for the destructor's reason: a configure is a wire write, and a client whose
	// connection has already failed is torn down inside libwayland — which runs handlers that unmap
	// windows, and so edits the registry underneath this walk.
	const std::vector<ClientXdgSurface*> windows{ context.Windows().begin(), context.Windows().end() };

	for (ClientXdgSurface* const window : windows)
	{
		if (window->SetActivated(FocusRestsOn(scene, window->Window(), focused)))
		{
			window->Configure();
		}
	}
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

		return new ClientXdgSurface{ *m_Context, Object(), nullptr };
	}

	auto* const client = static_cast<ClientSurface*>(handler);
	auto* const role = new ClientXdgSurface{ *m_Context, Object(), client };

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
