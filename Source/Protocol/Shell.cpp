#include "Protocol/Shell.h"

#include <spdlog/spdlog.h>
#include <wayland-server-core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>

#include "Animation/Author/Bundle.h"
#include "Core/ColorState.h"
#include "Core/Texture.h"
#include "Protocol/Chrome.h"
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

// The seat behind an id a client named, or null where the id is not one gyro made. `Implementation` is
// what makes that check safe: it compares the interface *and* the dispatch table, so a resource of the
// right interface that some other party created is refused rather than reinterpreted.
[[nodiscard]] SeatGlobal* SeatOf(Wayland::Server::WlSeat seat) noexcept
{
	Wayland::Server::WlSeatHandler* const handler = seat.Implementation();

	return handler != nullptr ? &static_cast<ClientSeat*>(handler)->Seat() : nullptr;
}

// The protocol's edge mask as the four questions a gesture asks of it, or nothing where the value is
// not one the enumeration has. It is written as a switch rather than as bit tests precisely so that a
// value like *left and right together* — which the mask can express and the protocol does not define —
// falls through to the refusal instead of being interpreted.
// The edges as a person would say them, for the log line. Diagnostic gold when a toolkit turns out to
// be asking for a side nobody grabbed.
[[nodiscard]] std::string_view Names(ResizeEdges edges) noexcept
{
	if (edges.Top)
	{
		return edges.Left ? "top-left" : edges.Right ? "top-right" : "top";
	}

	if (edges.Bottom)
	{
		return edges.Left ? "bottom-left" : edges.Right ? "bottom-right" : "bottom";
	}

	return edges.Left ? "left" : edges.Right ? "right" : "no edge";
}

[[nodiscard]] std::optional<ResizeEdges> Sides(Wayland::Server::XdgToplevelResizeEdge edges) noexcept
{
	using Edge = Wayland::Server::XdgToplevelResizeEdge;

	switch (edges)
	{
		case Edge::None:
			return ResizeEdges{};

		case Edge::Top:
			return ResizeEdges{ .Top = true };

		case Edge::Bottom:
			return ResizeEdges{ .Bottom = true };

		case Edge::Left:
			return ResizeEdges{ .Left = true };

		case Edge::Right:
			return ResizeEdges{ .Right = true };

		case Edge::TopLeft:
			return ResizeEdges{ .Left = true, .Top = true };

		case Edge::TopRight:
			return ResizeEdges{ .Right = true, .Top = true };

		case Edge::BottomLeft:
			return ResizeEdges{ .Left = true, .Bottom = true };

		case Edge::BottomRight:
			return ResizeEdges{ .Right = true, .Bottom = true };
	}

	return std::nullopt;
}
} // namespace

void ClientXdgToplevel::OnGone()
{
	if (m_Surface != nullptr)
	{
		m_Surface->Orphan();
	}

	// **The chrome object outliving its toplevel is a client error the compositor still has to
	// survive** (187). The protocol says it must be destroyed first and libwayland will destroy objects
	// in whatever order a client asks, so the link is cut here rather than trusted.
	if (m_ChromeObject != nullptr)
	{
		m_ChromeObject->Forget();
		m_ChromeObject = nullptr;
	}

	delete this;
}

bool ClientXdgToplevel::IsMapped() const noexcept
{
	return m_Surface != nullptr && m_Surface->IsMapped();
}

bool ClientXdgToplevel::BecomeChrome(ClientChrome& object) noexcept
{
	if (m_Chrome)
	{
		return false;
	}

	m_Chrome = true;
	m_ChromeObject = &object;

	return true;
}

void ClientXdgToplevel::ApplyMaterial() noexcept
{
	if (!m_MaterialStaged)
	{
		return;
	}

	m_MaterialStaged = false;
	m_Material = m_PendingMaterial;
}

void ClientXdgToplevel::Configure() const
{
	// The states as the protocol carries them: an array of `uint32`, handed over as the bytes behind it.
	// A fixed array rather than a vector because the set is closed and tiny — two entries, and the rest
	// of the enumeration is window management a shell owns (51) — and a heap allocation here would be
	// one per frame of a resize, on the thread a person's drag is being answered by.
	std::array<Wayland::Server::XdgToplevelState, 2> states{};
	std::size_t count = 0;

	if (m_Activated)
	{
		states[count++] = Wayland::Server::XdgToplevelState::Activated;
	}

	// **`resizing` is a promise about latency rather than a fact about geometry.** It tells a toolkit
	// that more configures are coming, so the ones that redraw expensively — a terminal reflowing its
	// scrollback, a browser relaying out a page — may draw something cheaper until it clears. A
	// compositor that drove a resize without sending it would get the slow path on every frame of the
	// drag, which is the stutter people describe as a window being heavy to resize.
	if (m_Resizing)
	{
		states[count++] = Wayland::Server::XdgToplevelState::Resizing;
	}

	// **Before the configure, which is the protocol's own order** — a client reads the pair together and
	// the bounds are what it clamps the size in the next event against. Sent on every configure rather
	// than only where it moved: it is two integers on a batch that already carries five, and the
	// alternative is a second *has this client been told* ledger beside the one `m_Room` already is.
	//
	// **Gated here rather than left to the bindings**, which drop an event a client's version cannot
	// carry and record a fault saying so — that is a diagnostic for a mistake, and a compositor tripping
	// it on every configure of every window would bury the one that means something. `ShellVersion` is 4
	// now, but a client is entitled to bind lower and a toolkit that does is not making one.
	if (Object().Version() >= 4)
	{
		Object().ConfigureBounds(m_Room.Width, m_Room.Height);
	}

	Object().Configure(m_Size.Width, m_Size.Height, std::as_bytes(std::span{ states.data(), count }));
}

PixelSize<SurfaceSpace> ClientXdgToplevel::Clamp(PixelSize<SurfaceSpace> size) const noexcept
{
	// Each axis independently, and the maximum after the minimum. The order only matters where a client
	// has declared a maximum below its own minimum, which the protocol calls an error and `ApplyBounds`
	// refuses — so this is the arithmetic being total rather than a policy about a case that cannot
	// arrive.
	PixelSize<SurfaceSpace> clamped = size;

	if (m_Min.Width > 0)
	{
		clamped.Width = std::max(clamped.Width, m_Min.Width);
	}

	if (m_Min.Height > 0)
	{
		clamped.Height = std::max(clamped.Height, m_Min.Height);
	}

	if (m_Max.Width > 0)
	{
		clamped.Width = std::min(clamped.Width, m_Max.Width);
	}

	if (m_Max.Height > 0)
	{
		clamped.Height = std::min(clamped.Height, m_Max.Height);
	}

	return clamped;
}

void ClientXdgToplevel::ApplyBounds()
{
	if (!m_BoundsStaged)
	{
		return;
	}

	m_BoundsStaged = false;

	// **The protocol's own error, and it is checked here rather than at the request** because the two
	// bounds arrive as separate requests and a client is entitled to be inconsistent in the middle of a
	// pair: a window growing its minimum past its old maximum sends one of them first, and refusing on
	// that request would end a client that was about to be correct.
	const bool impossible = (m_PendingMax.Width > 0 && m_PendingMin.Width > m_PendingMax.Width) ||
	                        (m_PendingMax.Height > 0 && m_PendingMin.Height > m_PendingMax.Height);

	if (impossible)
	{
		Object().PostError(
			Wayland::Server::XdgToplevelError::InvalidSize, "a minimum size larger than the maximum size beside it"
		);

		return;
	}

	m_Min = m_PendingMin;
	m_Max = m_PendingMax;
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
	// **The one half of a resize that is gyro's and the one that is the client's meet here** (166): this
	// starts a gesture that computes a size every iteration, and every size it computes goes out as a
	// configure the client answers by drawing. `OnMove` above says why a refusal is silence, and it is
	// the same here.
	const std::optional<ResizeEdges> pulled = Sides(edges);

	if (!pulled)
	{
		// **The one refusal on this path that is a protocol error rather than silence, and it is the
		// protocol's own.** A value outside the enumeration is a client that has miscomputed an edge mask
		// rather than one gyro declined to serve, and left unsaid it spends the rest of its life dragging
		// a corner that does nothing.
		Object().PostError(
			Wayland::Server::XdgToplevelError::InvalidResizeEdge, "xdg_toplevel.resize with an edge outside the enum"
		);

		return;
	}

	if (m_Surface != nullptr)
	{
		m_Surface->BeginResize(seat, serial, *pulled);
	}
}

void ClientXdgToplevel::OnSetMaxSize(std::int32_t width, std::int32_t height)
{
	// Negative is the one thing the protocol calls an error here; zero is *no limit* and is what almost
	// every client sends. A client that sent a negative and was not told is a client whose own layout
	// arithmetic has already gone wrong.
	if (width < 0 || height < 0)
	{
		Object().PostError(
			Wayland::Server::XdgToplevelError::InvalidSize, "xdg_toplevel.set_max_size with a negative extent"
		);

		return;
	}

	// Staged, and landing with the commit that follows it: these are double buffered exactly as the
	// window geometry is, and a client that shrinks its minimum and redraws smaller in one commit must
	// never be seen with one half of that applied.
	m_PendingMax = { width, height };
	m_BoundsStaged = true;
}

void ClientXdgToplevel::OnSetMinSize(std::int32_t width, std::int32_t height)
{
	if (width < 0 || height < 0)
	{
		Object().PostError(
			Wayland::Server::XdgToplevelError::InvalidSize, "xdg_toplevel.set_min_size with a negative extent"
		);

		return;
	}

	m_PendingMin = { width, height };
	m_BoundsStaged = true;
}

void ClientXdgToplevel::AnswerUnchanged()
{
	// **A fresh serial rather than a repeat of the last one**, which is what `ClientXdgSurface::Configure`
	// does anyway and is the half a client acknowledges. Two requests in a row each get their own, so a
	// toolkit that fullscreens and immediately unfullscreens is not left matching one answer to two
	// questions.
	if (m_Surface != nullptr)
	{
		m_Surface->Reconfigure();
	}
}

void ClientXdgToplevel::OnSetMaximized()
{
	AnswerUnchanged();
}

void ClientXdgToplevel::OnUnsetMaximized()
{
	AnswerUnchanged();
}

void ClientXdgToplevel::OnSetFullscreen(Wayland::Server::WlOutput output)
{
	// The output is the client's *preference* for which screen to use, and gyro is not going to use one
	// — so it is read and dropped rather than recorded. Decision 51 has the choice of screen belonging
	// to a shell along with the rest of the placement.
	(void)output;

	AnswerUnchanged();
}

void ClientXdgToplevel::OnUnsetFullscreen()
{
	AnswerUnchanged();
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
	// Reachable at last: this was written against a `ShellVersion` of 1, where libwayland refused the
	// opcode before it could arrive, and implemented rather than stubbed for the reason
	// [Shell.h](Shell.h) gives at length — a `reposition` that is accepted and never answered is a
	// client blocked forever inside its own event loop, which is exactly the failure this file was
	// rewritten for.
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
		m_Surface->Reconfigure();
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
	const PixelOffset<SurfaceSpace> shift = m_Parent->PendingShift(m_Rules);

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

		// **The screen as it will stand in the parent's space, not as it stands now.** A window whose left
		// edge is being pulled has an origin that moves with every size its client produces, so a popup
		// positioned against a configure that has not been answered yet is positioned in a space that is
		// about to shift underneath it — see `ClientXdgSurface::PendingShift`. The rectangle moves the
		// opposite way to the window, because it is the world holding still.
		const PixelRect<SurfaceSpace> local =
			PixelRect<SurfaceSpace>::FromEdges(OnGrid(*topLeft) - shift, OnGrid(*bottomRight) - shift);

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
		scene->Focus().Offer(Node(), FocusKind::Window, m_Context->Session(Object().WireClient()));
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

	// The newest the client has answered, which is what makes the range in `PendingShift` a set of
	// configures still in flight. The maximum rather than the argument, because a client acknowledging
	// an older configure after a newer one has not un-answered the newer.
	m_AckedSerial = std::max(m_AckedSerial, serial);
}

void ClientXdgSurface::Reconfigure()
{
	// **Nothing before the client's first commit.** xdg-shell has the opening configure be the answer to
	// that commit, and a client is entitled to say what it wants — maximised, fullscreen, a menu moved —
	// before it has made it. Answering early would be a stray event at best; what it actually costs is
	// the surface being marked as configured, so the bufferless commit that follows reads as a window
	// being taken off a screen it was never on. Nothing is lost by waiting: `Configure` sends the
	// toplevel as it stands, so the opening configure carries whatever was staged before it.
	if (!m_Configured)
	{
		return;
	}

	Configure();
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
		// **Zero by zero until a person takes hold of an edge, and it is the truthful answer rather than a
		// placeholder.** The protocol reads it as *pick your own size*, which is exactly what a compositor
		// placing windows at their natural size has to say (141), and a number invented here would be gyro
		// doing layout — the thing decision 51 keeps out of the compositor. A resize is the one thing that
		// makes gyro have an opinion, and it is not layout: the number comes from where a person's hand is
		// (166). The size and the states are both the toplevel's own, so it is asked rather than told.
		//
		// **The room, though, is the world's**, so it is refreshed here where the store is reachable and
		// immediately before it is sent. This is the call that matters for a window opening: the first
		// configure goes out before anything is mapped, so `SyncWindows` — which walks mapped windows —
		// has never seen this one, and a toolkit sizing its first frame would be working from the
		// `wl_output` arithmetic this event exists to correct.
		static_cast<void>(RefreshRoom());

		m_Toplevel->Configure();
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

	return surface.Current().Extent();
}

void ClientXdgSurface::OnSurfaceCommitted(ClientSurface& surface)
{
	if (m_GeometryStaged)
	{
		m_Geometry = m_PendingGeometry;
		m_GeometryStaged = false;
	}

	// Beside the geometry because it is the same kind of fact and the same double buffering: what the
	// client says its window can be, landing with the commit that stated it.
	if (m_Toplevel != nullptr)
	{
		m_Toplevel->ApplyBounds();

		// And the material, for the same reason one step further: a shell that changes what its launcher
		// is made of and redraws it in one commit must not be seen with one of the two applied (187).
		m_Toplevel->ApplyMaterial();
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
		// **Only where there is a window to take away.** A commit carrying no buffer is two different
		// things, and the difference is whether one is on screen: a client putting its window away, and a
		// client that has not put one up yet. The second is ordinary — a toolkit commits again between
		// the configure and its first frame to land a geometry, an opaque region, a buffer scale — and
		// reading it as an unmap throws away a negotiation the client is in the middle of, so the
		// acknowledgement it is about to send comes back to a compositor that has forgotten sending the
		// configure. That is a window that never appears and a client killed for a protocol error it did
		// not commit.
		if (!IsMapped())
		{
			return;
		}

		// Attaching nothing unmaps the window, and the next buffer maps it again — which the protocol
		// says restarts the whole configure sequence, so the acknowledgement goes with it.
		Unmap();

		m_Configured = false;
		m_Acked = false;
		m_AckedSerial = 0;

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

	// The surface's own quad, which is what the client said its surface is: a viewport destination
	// where there is one, and the buffer divided by the declared scale where there is not. Not the
	// natural size — those differ by exactly the shadow margin a toolkit draws outside its window.
	const Size<SurfaceSpace, float> extent = state.Extent();

	// The texels behind that quad. Stated rather than left empty so that `Frame/Projection.h` can count
	// them: a window whose buffer scale matches the panel it is on is texel for texel and can go on a
	// plane, and a node that says nothing here can never be told apart from one being stretched.
	const Rect<BufferSpace> source = state.Texels();

	const bool mapping = m_Window.IsNull();

	// **Where the window hangs, which is the whole of the difference between the two roles.** A
	// toplevel goes under the session floor and is placed by the Floorplanner (141); a popup goes under
	// the window it is anchored on and is placed by the arithmetic the client described. So a popup
	// moves with its parent for free, draws over it because the sibling list is the z order (55), and is
	// hit-tested consistently with what is drawn, because neither the frame walk nor `Scene/Hit.h` clips
	// a child to its parent.
	wl_client* const client = Object().WireClient();

	// **A third answer to the same question, and it is the whole of what being chrome does to the
	// scene** (187): a surface the shell declared goes on its session's chrome root rather than on its
	// floor. Both are roots of the session and the chrome one is the later, so decision 55 draws it in
	// front of every window — and a click raising a window (162) reorders the floor's chain, which the
	// launcher is not in.
	const bool chrome = m_Toplevel != nullptr && m_Toplevel->IsChrome();

	EntityId container = chrome ? m_Context->Chrome(client) : m_Context->Floor(client);

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
			// **Chrome takes the keyboard and is stepped over by the walk**, which is the split
			// `Scene/Focus.h`'s `FocusKind` exists for: a launcher a person cannot type into is not one,
			// and an `Alt+Tab` that landed on the shell's own panel would be the switcher listing itself.
			// **And the session it is offered for is the client's own**, which is what keeps a window
			// reachable exactly where it is presented (`Scene/Focus.h`): the keyboard leaves with the
			// screen when an output is handed to somebody else, rather than staying on a window that is
			// no longer on any of them.
			scene->Focus().Offer(*window, chrome ? FocusKind::Chrome : FocusKind::Window, m_Context->Session(client));

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

		// **On the window container rather than on the pixels, and inside the client's own commit.** The
		// material is drawn behind what the client painted, so it belongs to the node whose quad is the
		// window (111) — the image child is the pixels themselves and a material behind those would be a
		// blur the size of the buffer rather than of the window. Written whether or not it changed, which
		// costs a byte store and removes the state that would otherwise have to remember whether it did.
		if (chrome)
		{
			static_cast<void>(commit.Dress(m_Window, m_Toplevel->Dress()));
		}
	}

	if (mapping && m_Popup == nullptr)
	{
		// **Chrome is placed exactly as a window is, and that is a gap rather than a decision** (187). The
		// Floorplanner centres it on the output holding the pointer, which is what a launcher wants and
		// is not what a panel wants — there is no way for a shell to say *along the top edge* and no way
		// to reserve the space a maximised window must not cover. Docs/Open.md carries it.
		//
		// **A second transaction, because this one is gyro's.** The placement is not something the
		// client asked for — decision 141 has the Floorplanner author it, stamped with the arrival of
		// the window because nothing routed it and there is no earlier moment an entrance could point
		// at. Commits do not nest, so the client's closes above before this opens.
		SceneCommit placement{ *scene, CommitAuthor::Compositor, scene->Now() };

		PlaceOnFloor(placement, *scene, m_Context->Session(client), m_Window, natural);
	}
	else if (m_Popup == nullptr && m_Context->Drag().IsResizing() && m_Context->Drag().Window() == m_Window)
	{
		// **Decision 166's other half: the client produced a size and gyro decides where that rectangle
		// is pinned.** A person pulling the left edge is holding the right one still, so the origin comes
		// from the size that actually arrived rather than from the size that was asked for — a client is
		// free to round or to clamp, and a window positioned from the request would jitter its own fixed
		// edge by the difference on every frame of the drag.
		const Vector3<double> anchored = m_Context->Drag().Anchored(natural);
		const Entity* const window = scene->Find(m_Window);

		// Written only where it moved, for the popup's reason below: pulling the right or the bottom edge
		// holds the origin still, and a retarget per commit on a channel whose target never changed is
		// the cost Docs/Architecture.md#doing-nothing-must-cost-nothing exists to keep out.
		if (window != nullptr && window->Translation.Model() != anchored)
		{
			SceneCommit placement{ *scene, CommitAuthor::Compositor, scene->Now() };

			static_cast<void>(placement.Move(m_Window, anchored, Immediate()));
		}
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

	// The subsurfaces go first, while the tree is still readable: each of them has a binding of its own
	// to take out, and the retire below takes the whole subtree at once — after which a child could not
	// tell an id that has been freed from one it never had.
	if (m_Surface != nullptr)
	{
		m_Surface->UnmapChildren();
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
	m_AckedSerial = 0;
}

void ClientXdgSurface::Report(std::string_view request, std::uint32_t serial, GestureRefusal why)
{
	if (why == GestureRefusal::None)
	{
		m_ReportedGesture = serial;

		spdlog::info("{} (serial {}): {}", request, serial, Describe(why));

		return;
	}

	// A client that retries the same press says it once. See the member for why the serial is the right
	// thing to compare: a retry quotes the press it already quoted, and anything else is a new gesture.
	if (m_ReportedGesture == serial)
	{
		return;
	}

	m_ReportedGesture = serial;

	spdlog::warn("{} (serial {}): {}", request, serial, Describe(why));
}

void ClientXdgSurface::BeginMove(Wayland::Server::WlSeat seat, std::uint32_t serial)
{
	SceneStore* const scene = m_Context->Store();
	SeatGlobal* const from = SeatOf(seat);

	// Unmapped, outside a dispatch, or an id that is not a seat. A client is entitled to ask before its
	// window exists — a toolkit that hands a press to its own titlebar before the first frame is
	// ordinary — and there is nothing in the world to move.
	if (scene == nullptr || m_Window.IsNull())
	{
		Report("xdg_toplevel.move", serial, GestureRefusal::Unmapped);

		return;
	}

	if (from == nullptr)
	{
		Report("xdg_toplevel.move", serial, GestureRefusal::NoSeat);

		return;
	}

	Report("xdg_toplevel.move", serial, from->BeginMove(*scene, m_Window, serial));
}

void ClientXdgSurface::BeginResize(Wayland::Server::WlSeat seat, std::uint32_t serial, ResizeEdges edges)
{
	// The edges are in the line because they are the half a person cannot infer: a corner that does
	// nothing and a corner the toolkit asked for as an *edge* look identical from the outside.
	const std::string request = std::format("xdg_toplevel.resize {}", Names(edges));

	SceneStore* const scene = m_Context->Store();
	SeatGlobal* const from = SeatOf(seat);

	if (scene == nullptr || m_Window.IsNull())
	{
		Report(request, serial, GestureRefusal::Unmapped);

		return;
	}

	if (from == nullptr)
	{
		Report(request, serial, GestureRefusal::NoSeat);

		return;
	}

	Report(request, serial, from->BeginResize(*scene, m_Window, serial, edges));
}

bool ClientXdgSurface::SetActivated(bool activated) noexcept
{
	return m_Toplevel != nullptr && m_Toplevel->SetActivated(activated);
}

bool ClientXdgSurface::SetResizing(bool resizing) noexcept
{
	return m_Toplevel != nullptr && m_Toplevel->SetResizing(resizing);
}

PixelOffset<SurfaceSpace> ClientXdgSurface::PendingShift(const PopupPlacement& rules) const
{
	// **Both or neither.** The protocol pairs them — the size is what the window will be and the serial
	// is which configure that is an answer to — and a size with no serial names no moment, so there is
	// nothing to say how far ahead of the world it is.
	if (!rules.ParentExtent || !rules.ParentConfigure)
	{
		return {};
	}

	// **A configure still in flight: sent, and not yet answered.** Not merely one gyro once sent — a
	// positioner naming a configure the client has already acknowledged is describing a window that has
	// since arrived, and treating it as the future would walk a menu away from its own window for as
	// long as a person kept dragging. Mutter keeps a list of unacknowledged configurations and adjusts
	// nothing when the serial is not in it (`meta_wayland_xdg_positioner_to_placement`, trimmed at every
	// ack in `meta-window-wayland.c`); serials only go up, so the same set is two integers here.
	//
	// **A range rather than a membership test, and the difference is stated because it is real**: a
	// serial is the display's rather than this object's, so a number in the gap that belonged to some
	// other event is accepted. What that costs is honouring a size the client itself stated while
	// quoting a serial it made up, which the protocol already calls undefined — and the arithmetic below
	// retires itself the moment the client draws that size, so a wrong answer here cannot persist.
	if (!m_Configured || *rules.ParentConfigure > m_Serial || *rules.ParentConfigure <= m_AckedSerial)
	{
		return {};
	}

	const WindowDrag& drag = m_Context->Drag();
	const SceneStore* const scene = m_Context->Store();

	// **Nothing else in gyro gives a window a future**, so a window that is not under the hand is a
	// window whose size will be whatever it already is. A shell that moved or resized windows of its own
	// accord would be the second answer here, and there is none (51).
	if (scene == nullptr || m_Window.IsNull() || !drag.IsResizing() || drag.Window() != m_Window)
	{
		return {};
	}

	const Entity* const entity = scene->Find(m_Window);

	if (entity == nullptr)
	{
		return {};
	}

	const Size<SurfaceSpace, float> future{ static_cast<float>(rules.ParentExtent->Width),
		                                    static_cast<float>(rules.ParentExtent->Height) };

	// From the size the window is to the size the client says it is about to be. Measured between two
	// sizes rather than against the window's own translation, so that the answer is the same whether or
	// not the client has committed since the gesture began.
	const Offset<GlobalSpace> shift = drag.Between(entity->Extent, future);

	// Global to the parent's own space is a translation while a window hangs on a floor, which is
	// [Drag.h](Drag.h)'s assumption and breaks where that one does.
	return { static_cast<std::int32_t>(std::lround(shift.X)), static_cast<std::int32_t>(std::lround(shift.Y)) };
}

void ClientXdgSurface::SyncPopups()
{
	// Over a copy for the same reason `SyncWindows` walks one: a configure is a wire write, and a client
	// whose connection has already failed is torn down inside libwayland, which destroys its popups and
	// so edits this list underneath the walk.
	const std::vector<ClientXdgPopup*> children{ m_Children.begin(), m_Children.end() };

	for (ClientXdgPopup* const child : children)
	{
		ClientXdgSurface* const surface = child->Surface();

		if (surface == nullptr)
		{
			continue;
		}

		// **Dismissed is inert and unmapped has nothing to resolve against**, which is the same pair of
		// guards every other verb on a popup carries: a menu the compositor has taken away must not be put
		// back on screen by a walk, and one that has never been committed has no window to be moved.
		if (child->Rules().Reactive && !child->IsDismissed() && surface->IsMapped())
		{
			const PixelRect<SurfaceSpace> before = child->Placement();

			child->Resolve();

			// **Configured only where it moved.** A menu that is still in the right place is a client that
			// should not be woken, and a configure per dispatch iteration would have every open menu redraw
			// at input rate for as long as it is open.
			if (child->Placement() != before)
			{
				surface->Configure();
			}
		}

		// A submenu hangs off the menu rather than off the window, so the chain is a tree and this is the
		// step down it. After the parent's own resolve, because a child is positioned in the parent's space.
		surface->SyncPopups();
	}
}

bool ClientXdgSurface::RefreshRoom()
{
	const SceneStore* const scene = m_Context->Store();

	if (m_Toplevel == nullptr || scene == nullptr)
	{
		// **The last room stands rather than being cleared**, which is the difference between *outside a
		// dispatch* and *on no screen*: the store being unreachable says nothing about where the window
		// is, and zeroing on it would tell a client its bounds are unknown every time gyro asked from the
		// wrong place.
		return false;
	}

	const SceneOutput* const output = OutputFor(*scene, m_Context->Session(Object().WireClient()), m_Window);

	if (output == nullptr)
	{
		return m_Toplevel->SetRoom({});
	}

	// **The output's logical extent taken as a surface size, which is exact for as long as a window
	// hangs on a floor** — a floor is a top-level container at the origin with the identity transform
	// ([Floor.h](Floor.h)), so the two spaces differ by a translation and a size survives it unchanged.
	// It is the same assumption [Drag.h](Drag.h) resizes under, and it breaks in the same place: the day
	// a window sits inside a container that is scaled, this is the output's rectangle pulled through
	// that container rather than read off.
	//
	// **Truncated rather than rounded, because this is a ceiling.** Rounding up recommends a window
	// half a pixel wider than the screen it is on, which is the one direction an upper bound must never
	// go.
	return m_Toplevel->SetRoom(
		{ static_cast<std::int32_t>(output->Bounds.Extent.Width),
	      static_cast<std::int32_t>(output->Bounds.Extent.Height) }
	);
}

bool ClientXdgSurface::SetSize(PixelSize<SurfaceSpace> size) noexcept
{
	return m_Toplevel != nullptr && m_Toplevel->SetSize(size);
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

void SyncWindows(HostContext& context, const SceneStore& scene, EntityId focused, EntityId leaving)
{
	const WindowDrag& drag = context.Drag();

	// Iterated over a copy for the destructor's reason: a configure is a wire write, and a client whose
	// connection has already failed is torn down inside libwayland — which runs handlers that unmap
	// windows, and so edits the registry underneath this walk.
	const std::vector<ClientXdgSurface*> windows{ context.Windows().begin(), context.Windows().end() };

	for (ClientXdgSurface* const window : windows)
	{
		// **Or on the window whose session is leaving the screen**, which is the `activated` half of what
		// `Scene/Focus.h`'s `Leaving` is for: a titlebar going grey in the middle of a lock animation
		// announces the switch before the switch is visible, on the one screen a person is watching.
		// The keyboard has already gone — this is what the window is still being *told*.
		bool owed = window->SetActivated(
			FocusRestsOn(scene, window->Window(), focused) || FocusRestsOn(scene, window->Window(), leaving)
		);

		const bool resizing = drag.IsResizing() && drag.Window() == window->Window();

		owed = window->SetResizing(resizing) || owed;

		// **Beside the other two because it is the same kind of comparison**: a fact about the world held
		// against what this client has been told, with a configure owed where they disagree. What moves it
		// is a person dragging a window onto another monitor, and on a machine with one panel it changes
		// exactly once — at the first configure, which happens before this walk can see the window at all.
		owed = window->RefreshRoom() || owed;

		// **The menus, which are not in this registry and hang off the windows that are.** Outside the
		// `owed` fold on purpose: a popup is configured through its own `xdg_surface` and carries a
		// position rather than a size and a state, so the comparison that decides whether one is owed is
		// its placement rather than this window's.
		window->SyncPopups();

		if (resizing)
		{
			// Rounded rather than truncated: the wire carries whole logical pixels and the pointer is
			// subpixel (52), so truncating would make a window one pixel smaller than the hand asked for
			// on average and never one larger.
			const Size<SurfaceSpace, float> wanted = drag.Wanted();

			owed = window->SetSize(
					   { static_cast<std::int32_t>(std::lround(wanted.Width)),
			             static_cast<std::int32_t>(std::lround(wanted.Height)) }
				   ) ||
			       owed;
		}

		if (owed)
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
