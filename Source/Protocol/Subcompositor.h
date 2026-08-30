#pragma once

#include <cstdint>

#include "Core/Handle.h"
#include "Geometry/Space.h"
#include "Protocol/Context.h"
#include "Protocol/Surface.h"
#include "Wayland/Server/Wayland.h"

// `wl_subcompositor`: the one place in the core protocol where a client says how its own parts are
// stacked.
//
// **A subsurface is a surface that belongs to another surface**, and what a toolkit uses it for is
// the thing it cannot draw itself: a video frame it wants scanned out untouched, a decoration drawn
// around a canvas it does not own, a menu that has to be one buffer rather than a composite. gyro
// gets a window it can put on a plane out of the first of those and nothing at all out of the last.
//
// **Advertised at version 1, and there is no other.** `wl_subcompositor` has never been revised.
//
// **The shape in the world is decision 111's toplevel, one level down and unchanged.** A surface that
// is in the world is a container holding its below-subsurfaces, its own image, and its
// above-subsurfaces; a subsurface is exactly the same pair — a container of its own with an image
// under it — parented into its parent's container beside its parent's image. That is not a
// convenience: `wl_subsurface.place_below` names the *parent surface itself* as a legal reference, so
// a child may sit under its parent's pixels, and the only encoding of that in a preorder run whose
// sibling order is the z order (55) is a container the parent's own image is one entry of.
//
// **A container even where nothing is nested under it**, which is the shape a leaf subsurface would
// not need. It costs one node per subsurface, and what it buys is that the recursion has one case: a
// subsurface of a subsurface is ordered against its own parent's pixels by the same code that ordered
// its parent against the window's, and there is no moment where a leaf grows a container underneath a
// client that is already stacking against it.
//
// **What gyro does not do is snap the position, and decision 68 is the reason it will.** That entry
// settles that a subsurface lands on the device grid the client rasterized against, because the
// half-pixel disagreement on a fractional output is already inside the client before gyro is
// involved. The mechanism it names is decision 54's snap *at settle*, which nothing applies to a
// window yet — and `Node::Snap` is inherited by a subtree and rounds the composed position, so
// setting it on a child whose parent is at a fractional offset would round the child absolutely and
// pull it off the parent it is glued to. Half of decision 68 is worse than none of it, so the
// position is written plainly and the snap arrives with the one the window gets.
inline constexpr std::uint32_t SubcompositorVersion = 1;

// One `wl_surface` hung off another.
//
// **Position and z order are state on the *parent*, and that is the protocol's rule rather than an
// implementation choice.** A toolkit moving a video and the controls over it states three changes and
// commits the window, and what it is buying is that the three arrive in one frame — so `set_position`
// and the two `place_*` requests stage here and land when the parent commits, exactly as
// `wl_surface`'s own requests stage and land when it does.
//
// **Synchronized is the default and is the mode that does the work.** A synchronized subsurface's
// `wl_surface.commit` does not change what is on screen: it caches, and the parent's commit is what
// applies the whole tree at once. Desynchronized is the escape hatch for the case the batching is
// wrong for — a video decoder that wants its own cadence and does not want to wait for a toolkit's
// next repaint — and it is per subsurface with the parent's mode overriding it, which is what makes a
// desynchronized child of a synchronized parent still wait.
class ClientSubsurface final : public Wayland::Server::WlSubsurfaceHandler, public SurfaceRole
{
public:
	// Either may be null, and both are only where the client named something that was not a
	// `wl_surface` at all. The object still has to exist — the client is holding an id and libwayland
	// needs something behind it — and every request on it then does nothing, which costs nothing
	// because the connection has already been ended by the caller. `Protocol/Shell.cpp` refuses the
	// same case the same way.
	ClientSubsurface(HostContext& context, ClientSurface* surface, ClientSurface* parent) noexcept;

	~ClientSubsurface() override;

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	// Staged, per the class comment: the parent's commit is what moves it.
	void OnSetPosition(std::int32_t x, std::int32_t y) override { m_PendingPosition = { x, y }; }

	void OnPlaceAbove(Wayland::Server::WlSurface sibling) override;

	void OnPlaceBelow(Wayland::Server::WlSurface sibling) override;

	void OnSetSync() override { m_Synchronized = true; }

	void OnSetDesync() override;

	// `SurfaceRole`.
	void OnSurfaceCommitted(ClientSurface& surface) override;

	void OnSurfaceGone() override;

	[[nodiscard]] bool IsSynchronized() const noexcept override;

	[[nodiscard]] EntityId RoleContainer() const noexcept override { return m_Node; }

	[[nodiscard]] EntityId RoleContent() const noexcept override { return m_Content; }

	[[nodiscard]] ClientSurface* RoleParent() const noexcept override { return m_Parent; }

	// The subsurface's own container, or null while it is not on screen. What `ClientSurface::Restack`
	// orders and what a test asks instead of counting nodes.
	[[nodiscard]] EntityId Node() const noexcept { return m_Node; }

	// The `wl_surface` under this role, or null once the client has destroyed it.
	[[nodiscard]] ClientSurface* Content() const noexcept { return m_Surface; }

	// The parent committed: the staged position lands, this surface's cache becomes current and
	// cascades into its own children, and the result is put in the world.
	void ParentCommitted();

	// Off screen, and everything under it with it. The parent unmapping, the parent's surface going
	// away, and this object being destroyed.
	void Unmap() noexcept;

	// The parent `wl_surface` has gone, which the protocol makes a client error and still leaves this
	// object alive holding an id the client has not destroyed.
	void ForgetParent() noexcept;

private:
	// Map, update or unmap, from whatever the surface currently holds. Idempotent, because two parties
	// reach it — a desynchronized commit through the role, and the parent's commit through
	// `ParentCommitted` — and neither can know whether the other has already run this iteration.
	void Sync();

	// Where the subsurface's container sits inside the parent's, which is the parent's *own image*'s
	// position plus the offset the client stated.
	//
	// **Read out of the world rather than recomputed**, because the parent's image is where the parent
	// surface's origin actually is — for an `xdg_surface` that is its declared window geometry pushed
	// back, and for another subsurface it is zero. Asking the node keeps the child glued to the pixels
	// it was drawn against whatever put them there, instead of to a second derivation of the same fact
	// that drifts the first time one of them changes.
	[[nodiscard]] Vector3<double> Origin(const SceneStore& scene) const noexcept;

	// The subsurface the client named, among this one's siblings, or null where it named something that
	// is not one. The parent surface itself is *not* one of these and is checked for separately, because
	// it is a legal reference and is not a subsurface.
	[[nodiscard]] ClientSubsurface* SiblingFor(Wayland::Server::WlSurface reference) const noexcept;

	// Put this subsurface back into the parent's staged runs, `offset` entries from `reference` — which
	// is the run and the position `place_above` and `place_below` both resolve to.
	void Restage(ClientSubsurface* reference, bool above);

	HostContext* m_Context = nullptr;

	// The surface this role was taken on, and the one it hangs from. Neither owned; both null out as
	// their objects go.
	ClientSurface* m_Surface = nullptr;
	ClientSurface* m_Parent = nullptr;

	// The client's own mode, which is not the effective one — see `IsSynchronized`. True is the
	// protocol's default and the one a toolkit almost always leaves alone.
	bool m_Synchronized = true;

	// Where the client says this surface sits in its parent's coordinates, staged and current.
	PixelOffset<SurfaceSpace> m_PendingPosition{};
	PixelOffset<SurfaceSpace> m_Position{};

	// The subsurface's own two nodes, per decision 111. Null while it is not on screen.
	EntityId m_Node{};
	EntityId m_Content{};
};

// One client's `wl_subcompositor`. No per-client state, exactly as `wl_compositor` has none; a handler
// per bind because the bindings pair a handler with one resource.
class ClientSubcompositor final : public Wayland::Server::WlSubcompositorHandler
{
public:
	explicit ClientSubcompositor(HostContext& context) noexcept : m_Context{ &context } {}

	void OnGone() override { delete this; }

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	Wayland::Server::WlSubsurfaceHandler*
	OnGetSubsurface(Wayland::Server::WlSurface surface, Wayland::Server::WlSurface parent) override;

private:
	HostContext* m_Context = nullptr;
};

// The global itself, owned by whoever advertises it and outliving every client that binds it.
class SubcompositorGlobal final : public Wayland::Server::WlSubcompositorBinding
{
public:
	explicit SubcompositorGlobal(HostContext& context) noexcept : m_Context{ &context } {}

	Wayland::Server::WlSubcompositorHandler* OnBind(wl_client& client, std::uint32_t version) override;

private:
	HostContext* m_Context = nullptr;
};
