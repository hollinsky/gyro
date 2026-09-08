#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Animation/Author/Catalog.h"
#include "Core/Handle.h"
#include "Core/Session.h"
#include "Geometry/NodeTransform.h"
#include "Geometry/Space.h"
#include "Protocol/Shell.h"
#include "Wayland/Server/GyroSceneV1.h"
#include "Wayland/Server/Wayland.h"

class ClientContainer;
class ClientXdgSurface;
class HostContext;
class HostOutputs;
class SceneCommit;
class SceneManager;
struct SceneOutput;

// `gyro_scene_v1`: where the windows are, said by the shell.
//
// **This is the seam decision 51 has always described and nothing implemented.** A shell could be
// summoned (186) and could draw something that is not a window (187), and could do nothing at all
// with the windows themselves — `ext_foreign_toplevel_list_v1` enumerates them and
// [Foreign.h](Foreign.h) says in as many words that acting on one "needs the seam that does not exist
// yet". This is it: a container, a window put into one, and a name for the kind of change that is.
//
// **What a shell says is a destination and a name; what gyro says is how the world gets there.** No
// request here carries a duration, a curve, a damping ratio or a frame, and decision 51's falsifiable
// test is what that is for — *if a shell can produce motion that does not match the catalog, the line
// is in the wrong place.* Two desktops built on gyro then look nothing alike and feel like the same
// machine, because everything on both accelerates, overshoots and settles identically. A shell able
// to write its own springs would be a shell whose overview animates a little differently from the
// launcher beside it, and nobody could say why the machine felt cheap.
//
// **There is no way to say *no transition*, and that is decision 198's whole argument.** Decision 112
// left exactly this half open — a transition meaning *none* has to exist for client commits whatever
// is decided for shells, and whether a shell may name it freely "decides whether the catalog is
// enforceable". It may not: a shell that could set a position with no motion sixty times a second
// would be hand-animating the desktop with a nicer spelling. What that would have cost — a shell with
// no way to state a coordinate that does not animate — is paid instead at `get_container`, where the
// position is applied at birth because a container being created has nowhere to have come from.
//
// **A window is named by its `ext_foreign_toplevel_handle_v1` rather than by a handle of this
// protocol's own.** That object is already `System` tier, already filtered to the asking client's own
// session, and already excludes the shell's own chrome (187) — which is exactly the set a shell may
// move. Two names for one window would be two ways for a shell to hold a stale one, and the protocol
// that enumerates is the one that already promises never to reuse a name.
//
// **A container is gyro's and this object is a reference to it**, which is decision 141 held
// structurally: a shell that crashes destroys every object it held and not one window moves, and the
// shell that comes back asks for the same names and is handed the same containers with the same
// windows still in them. What a person gets is that a crash they did not cause does not cost them
// their arrangement.
//
// **What is not here is a gesture**, and it is absent rather than deferred out of tidiness.
// [Scene/Entity.h](../Scene/Entity.h) carries four sprung channels and no driven one, so there is
// nothing dispatch-side for a progress parameter to be authored into;
// [Input/Devices.cpp](../Input/Devices.cpp) drops libinput's swipe and pinch events for want of a
// recognizer; and Docs/Open.md says the gesture vocabulary "wants a screen rather than an argument".
// A request specified against all three absences would be a request with nothing behind it. Adding
// one is a version bump.
//
// **What is not here either is a reference node**, decision 95's fourth kind — the one an overview
// needs to show a live window in two places at once. It wants the backward-index rule and the hiding
// of the originals stated on the wire, which is its own design problem rather than a request this one
// forgot.
inline constexpr std::uint32_t SceneVersion = 1;

// The longest container name gyro will hold. A name is a shell's own word for a workspace, and the
// cap is here because the string is stored for the life of the session rather than for the life of
// the request that carried it.
inline constexpr std::size_t MaximumContainerName = 256;

// The wire state set as gyro's record of it. Total, and bits this compositor does not know are
// dropped rather than refused — a capability or a state is a shell describing itself, so a newer
// shell saying more than this compositor can carry should lose the surplus rather than the session.
[[nodiscard]] WindowStates StatesOf(Wayland::Server::GyroSceneV1State states) noexcept;

// The same for the capability set.
[[nodiscard]] WindowCapabilities CapabilitiesOf(Wayland::Server::GyroSceneV1Capability capabilities) noexcept;

// The part of one output ordinary windows belong inside: the screen, less whatever chrome has
// reserved along its edges.
//
// **The fold is empty today and this is the seam it will land on.** Nothing can reserve anything yet
// — `gyro_chrome_v1` says a surface is chrome and cannot say *along the top edge*, let alone how much
// room to keep for it (187, and Docs/Open.md carries both halves) — so this is the output's own
// rectangle, and every caller is already asking the right question. What it buys before the keepout
// exists is that the three places that need the answer ask it once: a window is told how much room it
// has at `configure_bounds`, a menu is kept inside it, and a shell is told it at `work_area`. Those
// disagreeing is what an application opening underneath a panel actually is.
//
// **Not the whole output for a fullscreen window**, which deliberately ignores this and covers the
// panel — that is what the state means, and it is why the caller decides rather than this.
[[nodiscard]] Rect<GlobalSpace> WorkArea(const SceneOutput& output) noexcept;

// The client that has claimed placement for this session, or null where nobody has — which is a
// session with no shell, a shell that has not claimed, and the gap while one restarts.
//
// **A scan over the bound scene objects rather than a table**, at the length of the shells on the
// machine, which is one per logged-in session at most. It runs when a client asks for a window state
// and once per window per iteration for the capability comparison, not per frame.
[[nodiscard]] SceneManager* PlacerFor(const HostContext& context, SessionId session) noexcept;

// What this window's session has been told it may offer, which is what its shell declared and is
// empty where no shell holds placement.
//
// **Empty is the answer that matters and it is reached by a shell going away**, not only by one that
// never spoke: a session whose shell has crashed has nothing that answers a maximise, so the windows
// are told so and their controls go, rather than staying on screen and doing nothing until it comes
// back.
[[nodiscard]] WindowCapabilities CapabilitiesFor(const HostContext& context, const ClientXdgSurface& window);

// Offer a client's window-state request to its session's shell, answering whether anybody took it.
//
// False where there is no shell, where no client holds placement, or where the holder has not bound
// `ext_foreign_toplevel_list_v1` and so has no name for this window — the last of which is a real
// case rather than a defensive one, since the two protocols are bound separately and a client is
// entitled to take one without the other. The caller answers the request itself where this is false,
// which is what keeps a window from waiting forever on a shell that was never going to hear it.
[[nodiscard]] bool ForwardWindowRequest(
	HostContext& context,
	const ClientXdgSurface& window,
	Wayland::Server::GyroSceneV1Action action,
	Wayland::Server::WlOutput preferred
);

// Tell every bound shell what part of each screen its windows belong in, sending nothing where
// nothing has moved. Called once per `Advance`, beside the foreign list's own walk and for the same
// reason: the commit that moved an output and the step that notices are the same wakeup of the same
// thread, so there is nothing to observe and no signal to keep in step.
void SyncWorkAreas(const HostContext& context, const HostOutputs& outputs);

// The wire transition as the catalog's, or nothing where the client named one gyro does not know.
//
// **A `std::optional` rather than a fall back to `None`**, and here that is stronger than the same
// choice in [Chrome.h](Chrome.h): `Transition::None` is the one entry a shell may not name at all, so
// falling back to it would hand a shell built against a newer protocol precisely the un-animated
// write this design exists to withhold. The protocol makes it an error and this returns nothing.
[[nodiscard]] constexpr std::optional<Transition>
TransitionOf(Wayland::Server::GyroSceneV1Transition transition) noexcept
{
	switch (transition)
	{
		case Wayland::Server::GyroSceneV1Transition::WindowOpen:
			return Transition::WindowOpen;
		case Wayland::Server::GyroSceneV1Transition::WindowClose:
			return Transition::WindowClose;
		case Wayland::Server::GyroSceneV1Transition::MenuAppear:
			return Transition::MenuAppear;
		case Wayland::Server::GyroSceneV1Transition::MenuDismiss:
			return Transition::MenuDismiss;
		case Wayland::Server::GyroSceneV1Transition::WorkspaceSwitch:
			return Transition::WorkspaceSwitch;
		case Wayland::Server::GyroSceneV1Transition::FocusChange:
			return Transition::FocusChange;
		case Wayland::Server::GyroSceneV1Transition::MatchedMove:
			return Transition::MatchedMove;
		case Wayland::Server::GyroSceneV1Transition::BackgroundChange:
			return Transition::BackgroundChange;
	}

	return std::nullopt;
}

// One `gyro_container_v1`: a reference to a container gyro holds, and the double buffer for what the
// shell has said about it since the last commit.
//
// **Destroying this object leaves the container standing, which is the opposite of what destroying an
// object usually means and is the point of the name.** A shell exits or crashes and every object it
// held is destroyed; if that took the containers with it, a crash would dissolve every workspace in
// the session and pile forty windows into one heap. `remove` is the deliberate act, and it is a
// different request for that reason.
class ClientContainer final : public Wayland::Server::GyroContainerV1Handler
{
public:
	ClientContainer(SceneManager* manager, EntityId container, Vector3<double> at, bool visible) noexcept
		: m_Manager{ manager }, m_Container{ container }, m_State{ at }, m_Shown{ visible }
	{}

	void OnGone() override;

	// **The `state` event goes here rather than from the factory**, because the resource does not exist
	// until the request that created it has returned: an event sent from `get_container` would be one
	// sent on an id nothing is behind. The generated bindings carry `OnBound` for exactly the
	// interfaces that owe something before their client has said anything, and this is one — a
	// restarted shell has to be told what survived before it can draw a workspace strip.
	void OnBound() override;

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	void OnRemove() override;

	void OnSetPosition(wl_fixed_t x, wl_fixed_t y, wl_fixed_t z) override;

	void OnSetVisible(std::uint32_t visible) override;

	// The container in the store, or null once this object has been made inert by `remove` or by the
	// session going away underneath it.
	[[nodiscard]] EntityId Container() const noexcept { return m_Container; }

	// Whether `remove` has been asked for and not yet applied. Read by the manager at commit, which is
	// where the removal actually happens — a container emptied and taken away is one arrangement, and
	// splitting it across two frames would show the windows land on the floor before the workspace
	// went.
	[[nodiscard]] bool IsRemoving() const noexcept { return m_Removing; }

	// Apply whatever is staged into an open commit, and forget it. Everything here is a write the
	// commit's transition governs, except visibility, which lands whatever the transition says because
	// there is nothing between shown and hidden to draw.
	void Apply(SceneCommit& commit);

	// `remove` has been applied, so this object is inert: it exists only to answer the destroy the
	// client still owes.
	void Removed() noexcept
	{
		m_Container = {};
		m_Removing = false;
	}

	// The manager went first, which is a client destroying its `gyro_scene_v1` while still holding
	// containers. Legal, and it leaves the containers themselves untouched — see the class comment.
	void Forget() noexcept { m_Manager = nullptr; }

private:
	SceneManager* m_Manager = nullptr;

	EntityId m_Container{};

	// Staged, per the protocol's double buffering. Empty means the shell said nothing about it in this
	// batch, which is different from having said the value it already has: a container nobody moved
	// must not have its spring retargeted at the commit's origin, or every workspace in the session
	// would restart its motion every time any one of them moved.
	std::optional<Vector3<double>> m_Position;
	std::optional<bool> m_Visible;

	// What the container actually is, read at creation and sent once. Held rather than re-read at
	// `OnBound` because the store is reachable only inside a dispatch and the answer cannot change
	// between the two: nothing runs in between.
	Vector3<double> m_State{};
	bool m_Shown = true;

	bool m_Removing = false;
};

// One client's `gyro_scene_v1`: the placement claim, the containers it made, and the window
// placements staged since its last commit.
class SceneManager final : public Wayland::Server::GyroSceneV1Handler
{
public:
	explicit SceneManager(HostContext& context) noexcept : m_Context{ &context } {}

	void OnGone() override;

	// **The work areas go out here rather than being left to the walk**, which is the reason
	// `ForeignToplevelList` runs its own sync from the same place: a client is entitled to bind, round
	// trip and read the answer, and a round trip's reply is already queued by libwayland while this bind
	// is being dispatched. A first announcement deferred to the end of the iteration arrives *after* the
	// reply that was supposed to follow it, so a shell written exactly as the protocol describes would
	// come up not knowing where any of its screens are.
	void OnBound() override;

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	void OnClaimPlacement() override;

	void OnSetCapabilities(Wayland::Server::GyroSceneV1Capability capabilities) override;

	void OnSetWindowSize(
		Wayland::Server::ExtForeignToplevelHandleV1 toplevel,
		std::int32_t width,
		std::int32_t height
	) override;

	void OnSetWindowStates(
		Wayland::Server::ExtForeignToplevelHandleV1 toplevel,
		Wayland::Server::GyroSceneV1State states
	) override;

	Wayland::Server::GyroContainerV1Handler*
	OnGetContainer(std::string_view name, wl_fixed_t x, wl_fixed_t y, wl_fixed_t z) override;

	void OnPlaceWindow(
		Wayland::Server::ExtForeignToplevelHandleV1 toplevel,
		Wayland::Server::GyroContainerV1 container,
		wl_fixed_t x,
		wl_fixed_t y,
		wl_fixed_t z
	) override;

	void OnCommit(
		Wayland::Server::GyroSceneV1Transition transition,
		std::uint32_t seconds_hi,
		std::uint32_t seconds_lo,
		std::uint32_t nanoseconds
	) override;

	// A container let go of its resource. Borrowed pointers throughout, the way
	// [Foreign.h](Foreign.h)'s list holds its handles: each deregisters itself as it dies, and this
	// tells each of them to forget it if it goes first.
	void Remove(ClientContainer& container) noexcept;

	// Whether this object holds its session's placement, which is what makes it the one client a window
	// state request is offered to.
	[[nodiscard]] bool IsPlacing() const noexcept { return m_Placing; }

	// What this shell declared it will answer for. Empty until it says, which is what a client's
	// titlebar is told and is why an unspoken shell produces no window controls rather than dead ones.
	[[nodiscard]] const WindowCapabilities& Capabilities() const noexcept { return m_Capabilities; }

	// The work area of every output, sent where it has moved since this client was last told and where
	// the client has a `wl_output` to name. An output it has not bound stays owed rather than being
	// dropped, which is `SyncOutputEntry`'s rule for an `enter` with nothing to name: a shell that
	// walks the registry late hears about every screen when it gets there.
	void SendWorkAreas(const HostOutputs& outputs);

private:
	// One window the shell has said where to put, since the last commit.
	struct Placement
	{
		EntityId Window{};

		// Null for the session's floor, which is where a shell with no containers at all puts
		// everything.
		EntityId Container{};

		Vector3<double> At{};
	};

	// One window whose size or state the shell has stated, since the last commit. Staged separately
	// from `Placement` above because a shell may state any of the three without the others — a window
	// maximising states all three, a tiling shell moving one across a gap states two, and an overview
	// states only where things are.
	struct Arrangement
	{
		EntityId Window{};

		// Nothing where this batch said nothing about it, which is different from having said the value
		// it already has: applying an unstated size would hand a window back its natural size every time
		// the shell moved it.
		std::optional<PixelSize<SurfaceSpace>> Size;
		std::optional<WindowStates> States;
	};

	// The work area one output was last reported as having, so a comparison rather than a signal
	// decides whether an event is owed — the shape every other per-iteration walk in this module has.
	struct Reported
	{
		OutputId Output{};
		Rect<GlobalSpace> Area{};
	};

	// The arrangement staged for a window, created where this batch has not touched it yet.
	[[nodiscard]] Arrangement& Staged(EntityId window);

	HostContext* m_Context = nullptr;

	// The containers minted on this object. Borrowed, and each removes itself as it dies.
	std::vector<ClientContainer*> m_Containers;

	// Staged sizes and states, in the order the shell stated them, for `m_Placements`' reason.
	std::vector<Arrangement> m_Arrangements;

	// What this shell says it answers for, applied immediately rather than at commit: it is a standing
	// statement about the shell rather than a change to anything a person is watching.
	WindowCapabilities m_Capabilities{};

	// What this client has been told about each screen. Cleared of nothing when an output goes — the
	// entry is dropped by the next walk, which is over the outputs that exist.
	std::vector<Reported> m_WorkAreas;

	// Staged placements, in the order the shell stated them. A vector rather than a set keyed on the
	// window because the last word wins and stating one twice in a batch is a shell's own business —
	// applying both in order is what makes that true without a search per request.
	std::vector<Placement> m_Placements;

	// Whether this object holds its session's placement, so that giving it back on the way out does
	// not have to ask the floors who the holder was.
	bool m_Placing = false;
};

// The global. One per compositor, System tier, so an application is never offered it — a client able
// to move other people's windows is a client able to hide the one somebody is typing into, and to put
// the window they are looking for on a workspace they cannot reach.
class SceneGlobal final : public Wayland::Server::GyroSceneV1Binding
{
public:
	explicit SceneGlobal(HostContext& context) noexcept : m_Context{ &context } {}

	Wayland::Server::GyroSceneV1Handler* OnBind(wl_client& client, std::uint32_t version) override;

private:
	HostContext* m_Context = nullptr;
};
