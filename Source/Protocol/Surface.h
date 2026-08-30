#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "Core/Texture.h"
#include "Core/Time.h"
#include "Geometry/Space.h"
#include "Protocol/Context.h"
#include "Protocol/Region.h"
#include "Wayland/Server/Wayland.h"

// A `wl_surface`: the thing a client draws into, and the double buffering the protocol wraps around
// it.
//
// **Every request below stages, and `commit` is the only thing that changes what the world sees.**
// That is Wayland's own rule and it is also the one gyro most needs to hold, because
// `Scene/Commit.h`'s scope is opened by the wire request that closes it — a request that wrote
// straight through would be a retarget with no transaction around it, and two of them inside one
// frame would be the case decision 89 exists to get right, resolved wrong. So the pending state here
// is not an optimisation or a convenience for the client; it is what makes a surface's whole change
// arrive as one commit with one origin.
//
// **A commit turns the attached buffer into a texture id, and hands the buffer straight back.** gyro
// copies the pixels rather than sampling the client's memory for as long as it draws them, so the
// moment `Apply` returns there is nothing left to wait for — `wl_buffer.release` goes out in the same
// step, and a toolkit that keeps one buffer never stalls behind a compositor that has not finished
// with it. Every commit that attaches produces a *new* id and retires the old one, which is
// [Seam/Importer.h](../Seam/Importer.h)'s own rule: the previous frame's pixels stay drawable while
// the next frame's arrive, because the frame thread may still be recording from a snapshot that names
// them.
//
// **What is still absent is the node, and the reason is the role rather than the shell.** A surface
// with content has an id and an extent and nothing showing it, because a `wl_surface` on its own is
// not a window — `xdg_toplevel` is what says it is one, and only then is the entity parented into the
// session's floor and shown when something *places* it. The absence of a shell is not what stops that:
// the rule that gyro owns every container a window can occupy and places one itself when no shell is
// there to (141) has a Floorplanner centre it on the pointer's output the moment it arrives. There is
// simply no role, no floor and no planner yet. What is complete is the state machine, the pixels, and
// the texture space's side of the handoff.
//
// **Damage is accumulated in both spaces and reconciled later, rather than converted on arrival.**
// `wl_surface.damage` is surface-local and `damage_buffer` is in buffer coordinates, and the adapter
// between them is the buffer's scale, transform and — once there is a viewport — its source
// rectangle, none of which are settled until the commit that carries them. Converting at the request
// would apply the *previous* commit's adapter to the current commit's damage, which is a smear of
// stale pixels down one edge of a window being resized, exactly at the moment a person is watching
// that edge.

// SPEC: how many damage rectangles one commit may stage, per space. Decision 27's rule again: a
// client past it is not served a larger allocation, it is told `no_memory`. Real toolkits send one to
// a few dozen; a text editor repainting glyph runs is the busiest and is nowhere near this.
//
// **Dropping the surplus would be safe here and is still not what happens**, which is the difference
// from `MaxRegionRects` worth naming: damage is a hint, and losing one rectangle costs a stale patch
// on screen rather than a click in the wrong window. But a client generating four thousand damage
// rectangles in one frame is broken in a way that will not fix itself, and a compositor that silently
// paints most of what it was asked to paint is a bug report nobody can reproduce.
inline constexpr std::uint32_t MaxDamageRects = 4096;

// What one commit carries. Staged by the requests, adopted whole by `Apply`.
//
// Aggregate rather than a class with setters, because the requests are its only writer and they are
// all in one file — the invariants live in the handler that fills it, and a setter per field would be
// the same code with a second name for every field.
struct SurfaceState
{
	// Where the buffer sits relative to the surface's own origin. `wl_surface.attach`'s `x` and `y`
	// through version 4, and `wl_surface.offset` from version 5 onward — the same field either way,
	// which is what the protocol's own deprecation says.
	PixelOffset<SurfaceSpace> Offset{};

	// The client's declared scale for the buffer's contents. Never zero or negative: a client sending
	// one is ended with `invalid_scale` before this is written.
	std::int32_t BufferScale = 1;

	// How the buffer's contents are oriented relative to the surface.
	Wayland::Server::WlOutputTransform BufferTransform = Wayland::Server::WlOutputTransform::Normal;

	// Where the client promises to be fully opaque. Empty by default, which is the protocol's own
	// default and means *assume nothing* rather than *assume transparent*.
	SurfaceShape Opaque;

	// Where the surface accepts pointer and touch input, or nothing for the default, which is
	// **infinite** — the whole surface and, per the protocol, beyond it. The `optional` is carrying
	// that distinction and not an unset flag: an input region a client explicitly emptied is a window
	// that deliberately lets clicks through, and it must not read the same as one that never set one.
	std::optional<SurfaceShape> Input;

	// What changed, in each of the two spaces the client may name it in.
	std::vector<PixelRect<SurfaceSpace>> SurfaceDamage;
	std::vector<PixelRect<BufferSpace>> BufferDamage;

	// The pixels the last commit adopted, and the extent they were adopted at. Null until a buffer has
	// been attached and committed, which is Wayland's own definition of a surface that is not shown —
	// and null again the moment a client attaches nothing, which is how a window takes itself off the
	// screen without destroying anything.
	TextureId Content{};
	PixelSize<BufferSpace> ContentSize{};
};

class ClientSurface;
class ClientSubsurface;

// What a `wl_surface` becomes when something gives it one.
//
// **A surface with no role is not shown, which is Wayland's own rule and the reason this is where the
// scene is written from rather than in the surface.** A `wl_surface` is a rectangle of pixels with no
// opinion about whether it is a window, a cursor, a subsurface or a drag icon, and what it means to
// commit one is entirely the role's — an `xdg_toplevel` maps a window and a cursor surface moves a
// pointer. So the surface owns the pixels and hands the fact of the commit to whatever claimed it.
//
// **One role at a time and never a second**, which the protocol states for every role object there is
// and which `AdoptRole` enforces in one place rather than in each of them.
class SurfaceRole
{
public:
	SurfaceRole() = default;

	virtual ~SurfaceRole() = default;

	SurfaceRole(const SurfaceRole&) = delete;
	SurfaceRole& operator=(const SurfaceRole&) = delete;
	SurfaceRole(SurfaceRole&&) = delete;
	SurfaceRole& operator=(SurfaceRole&&) = delete;

	// The surface's pending state has just become current. Everything the role reads — the content, the
	// extent, the scale — is `Current()` by the time this runs.
	virtual void OnSurfaceCommitted(ClientSurface& surface) = 0;

	// The `wl_surface` is going away underneath the role, which the protocol calls a client error and
	// still requires the compositor to survive. The role has whatever it authored to take down.
	virtual void OnSurfaceGone() = 0;

	// Whether a commit on this surface is *cached* rather than applied, which is `wl_subsurface`'s
	// synchronized mode and the only role that has one.
	//
	// **It is a question for the role because only the role knows the answer**: a subsurface is
	// synchronized if it says so or if anything above it does, and the surface itself has no parent to
	// ask. Every other role answers false, which is the state a toplevel and a popup are permanently in.
	[[nodiscard]] virtual bool IsSynchronized() const noexcept { return false; }

	// The container this surface's subsurfaces are ordered inside, and the node its own pixels are.
	//
	// **The pair is decision 111's toplevel stated as two questions**: a surface in the world is a
	// container holding its below-subsurfaces, its own image, and its above-subsurfaces, so a child
	// parents into the first and stacks relative to the second. `wl_subsurface.place_below` naming the
	// parent surface itself as a legal reference is the whole reason the second one has to be
	// addressable at all.
	//
	// Both are null while the role has nothing in the world, which is a surface nobody can hang a
	// subsurface off yet rather than an error — a child of an unmapped window waits for it, exactly as
	// a popup waits for the window it is anchored on.
	[[nodiscard]] virtual EntityId RoleContainer() const noexcept { return {}; }

	[[nodiscard]] virtual EntityId RoleContent() const noexcept { return {}; }

	// The surface this one hangs off, which only a subsurface has. It is what
	// `wl_subcompositor.get_subsurface` walks to refuse a parent that is the surface itself or
	// something under it — a cycle in the scene tree, which is a preorder walk that never terminates on
	// the frame thread.
	[[nodiscard]] virtual ClientSurface* RoleParent() const noexcept { return nullptr; }
};

class ClientSurface final : public Wayland::Server::WlSurfaceHandler
{
public:
	explicit ClientSurface(HostContext& context) noexcept : m_Context{ &context } {}

	~ClientSurface() override;

	// What the last `commit` made true. The only state anything outside this object may read.
	[[nodiscard]] const SurfaceState& Current() const noexcept { return m_Current; }

	// How many frame callbacks are waiting on a frame reaching the glass.
	//
	// **They are answered by `Present` below and by nothing else**, because when a client may draw again
	// is a fact only the return leg holds: `Scene/Return.h` drains what reached the glass and says which
	// entity's pixels were in it (115). A count rather than the list, because the only legitimate thing
	// to do with one of these is send it.
	[[nodiscard]] std::size_t DueCallbackCount() const noexcept { return m_DueCallbacks.size(); }

	// The content this surface committed has been shown, at `at`. Answer everything the last commit made
	// due.
	//
	// **The timestamp is the instant the frame reached the glass**, in milliseconds of the compositor's
	// own clock — which is what `wl_callback.done` carries for a frame callback and what a toolkit
	// differences to work out how long a frame took. Decision 57's conversion at the edge: the domain is
	// an `Instant` everywhere inside gyro and becomes a truncated millisecond count exactly here, where
	// the protocol demands one.
	//
	// **It is safe to call on a surface with nothing due**, which is the ordinary case: a window is
	// presented on every frame the panel scans and asks to be told about the ones it drew for.
	void Present(Instant at) noexcept;

	// The outputs this surface has been told it is on, as `Scene/Reach.h`'s mask over the world's output
	// set. Held here rather than on the role because `wl_surface.enter` is the surface's event, and read
	// back by the comparison in [Output.h](Output.h) that decides what to send.
	//
	// **It is what a client has been told rather than what is true**, which is the same distinction the
	// seat draws about focus: a window on an output whose global that client never bound is on it in the
	// world and not in the conversation, and the difference is what makes the entry arrive when the bind
	// does.
	[[nodiscard]] std::uint32_t Entered() const noexcept { return m_Entered; }

	void SetEntered(std::uint32_t outputs) noexcept { m_Entered = outputs; }

	// The implementation behind an id a client named, or null where the id was not a `wl_surface` gyro
	// made. `ClientBuffer::Of`'s argument exactly: `Implementation` checks the interface and the
	// dispatch table before it touches any user data, and gyro creates exactly one kind of thing
	// against `wl_surface`'s table.
	[[nodiscard]] static ClientSurface* Of(Wayland::Server::WlSurface surface) noexcept
	{
		return static_cast<ClientSurface*>(surface.Implementation());
	}

	// The subsurfaces hanging off this surface, in the two runs `wl_subsurface.place_below` divides
	// them into.
	//
	// **Two lists rather than one with a marker in it**, because the thing being ordered against is the
	// parent's own pixels and it is not a subsurface: `place_below` names the parent surface itself as
	// a legal reference, so what a client states is *before mine* or *after mine* and the two runs say
	// exactly that. Published, the pair becomes one sibling chain — below, the parent's image, above —
	// which is decision 111's toplevel and is what `Restack` writes.
	//
	// Borrowed, and each subsurface takes itself out as it goes.
	struct SurfaceStack
	{
		std::vector<ClientSubsurface*> Below;
		std::vector<ClientSubsurface*> Above;
	};

	// What the client has stated and what the world has, which are the same double buffering every
	// other request on this object gets: **z order and position are state on the *parent*, applied by
	// the parent's commit**, which is the protocol's own rule and the reason a toolkit can move three
	// subsurfaces and have them arrive as one change.
	[[nodiscard]] const SurfaceStack& Stack() const noexcept { return m_Stack; }

	[[nodiscard]] SurfaceStack& Pending() noexcept { return m_PendingStack; }

	// A new subsurface, which the protocol puts topmost. Into the pending run, so it appears with the
	// parent commit that maps it rather than the moment the object was made.
	void AddChild(ClientSubsurface& child);

	// Out of every run that names it, current and pending both. A subsurface being destroyed, and a
	// client that destroyed the `wl_surface` under one.
	void RemoveChild(const ClientSubsurface& child) noexcept;

	// Whether a commit on this surface caches rather than applies, which is the role's answer and is
	// asked here because the surface is what a commit arrives on.
	[[nodiscard]] bool IsSynchronized() const noexcept { return m_Role != nullptr && m_Role->IsSynchronized(); }

	// Where a subsurface of this surface hangs, and the node it stacks against. The role's answers,
	// reached through the surface because a child holds its parent as a surface rather than as a role —
	// a `wl_subsurface` names a `wl_surface` and never what claimed it.
	[[nodiscard]] EntityId Container() const noexcept
	{
		return m_Role != nullptr ? m_Role->RoleContainer() : EntityId{};
	}

	[[nodiscard]] EntityId ContentNode() const noexcept
	{
		return m_Role != nullptr ? m_Role->RoleContent() : EntityId{};
	}

	// The surface this one is a subsurface of, or null for every surface that is not one.
	[[nodiscard]] ClientSurface* ParentSurface() const noexcept
	{
		return m_Role != nullptr ? m_Role->RoleParent() : nullptr;
	}

	// The state a synchronized commit cached becomes current, and this surface's own children commit
	// behind it. Called by the parent's commit and by nothing else.
	//
	// **The role is deliberately not told from here.** The party applying a cache is the parent, and it
	// is the parent that acts on the whole of the change at once — the position, the stacking and the
	// content are one arrangement, and a role told halfway through would map a subsurface where it used
	// to be.
	void ApplyCached();

	// Everything under this surface goes off screen, because this surface has. A window unmapping, a
	// role let go, a client destroying a surface with children still on it.
	void UnmapChildren() noexcept;

	// Claim this surface. False where something already has it, which is every role object's own
	// `role` error and is raised by the caller because only it knows which one to name.
	[[nodiscard]] bool AdoptRole(SurfaceRole& role) noexcept;

	// Give the surface back, from a role object that is being destroyed. Ignores a role that is not the
	// one holding it, so a role tearing down after the surface already went finds nothing to undo.
	void ForgetRole(const SurfaceRole& role) noexcept;

	[[nodiscard]] bool HasRole() const noexcept { return m_Role != nullptr; }

	void OnGone() override;

	// The resource is already destroyed when this runs, and `OnGone` follows immediately.
	void OnDestroy() override {}

	void OnAttach(Wayland::Server::WlBuffer buffer, std::int32_t x, std::int32_t y) override;

	void OnDamage(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override;

	Wayland::Server::WlCallbackHandler* OnFrame() override;

	void OnSetOpaqueRegion(Wayland::Server::WlRegion region) override;

	void OnSetInputRegion(Wayland::Server::WlRegion region) override;

	void OnCommit() override;

	void OnSetBufferTransform(Wayland::Server::WlOutputTransform transform) override;

	void OnSetBufferScale(std::int32_t scale) override;

	void OnDamageBuffer(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override;

	void OnOffset(std::int32_t x, std::int32_t y) override;

	Wayland::Server::WlCallbackHandler* OnGetRelease() override;

private:
	// One `wl_callback` the client is waiting on.
	//
	// **It carries a pointer back to the surface, and that is what keeps the surface's two lists from
	// dangling.** A `wl_callback` has no destroy request — the server destroys it when it fires — but
	// a client that disconnects takes every one of its objects with it, and the surface would be left
	// holding freed resources it intends to send events to. So the callback removes itself on the way
	// out, and the surface's lists hold only live objects by construction rather than by a validity
	// check nobody would remember to write.
	class FrameCallback final : public Wayland::Server::WlCallbackHandler
	{
	public:
		explicit FrameCallback(ClientSurface& surface) noexcept : m_Surface{ &surface } {}

		void OnGone() override
		{
			m_Surface->Forget(*this);

			delete this;
		}

	private:
		ClientSurface* m_Surface = nullptr;
	};

	// Drop a callback from whichever list holds it. Called by the callback itself as it goes away.
	void Forget(const FrameCallback& callback) noexcept;

	// Pending becomes *cached* instead of current, which is a synchronized subsurface's commit: the
	// client has stated an arrangement and the parent decides when the world sees it.
	//
	// **Damage accumulates and everything else replaces**, which is the same asymmetry `Apply` has one
	// level down: two cached commits describe one arrangement and two sets of changed pixels, and a
	// cache that dropped the first commit's damage would leave a patch of a video frame nobody
	// repainted.
	void Cache();

	// The subsurface stack the client stated becomes the one the world has, every child under it
	// commits, and the sibling chain is rewritten to match. The tail of both `Apply` and `ApplyCached`,
	// because a synchronized surface's cache landing *is* its commit.
	void CommitChildren();

	// The published order: below, this surface's own image, above. One relink per child through
	// `SceneStore::Order`, and nothing at all where there are no children — which is every window on an
	// ordinary machine, so the walk is not paid for by the case that does not have one.
	void Restack();

	// Pending becomes current, and pending's per-commit accumulations reset.
	//
	// **Damage resets and the rest does not**, which is the asymmetry the protocol actually specifies
	// and the one a hand-written commit gets wrong: scale, transform, offset and the two regions are
	// *sticky* — a client that sets a scale once and commits ten times has that scale on all ten — but
	// damage describes one frame and a commit consumes it. Carrying damage forward would repaint a
	// region that has been correct for nine frames, every frame, forever.
	void Apply();

	// The shape a `wl_region` resource currently describes, or an empty shape for a null resource. A
	// copy, per Region.h: the client may destroy the region the instant this returns and usually does.
	[[nodiscard]] static SurfaceShape ShapeOf(Wayland::Server::WlRegion region);

	// Turn the staged buffer into pending content, retire what it replaces, and release it back to the
	// client. Called by `Apply` and only when an attach is actually pending, because a commit that did
	// not attach keeps the content it had.
	void TakeContent(ITextures& textures);

	// Hand a buffer back and forget it was staged. Safe on an invalid resource, which is what a detach
	// stages.
	void ReleaseStaged() noexcept;

	// The world this surface's requests act on, for the duration of the `Advance` they arrive in. Never
	// null; what is null outside a dispatch is what it points at.
	HostContext* m_Context = nullptr;

	// Whatever gave this surface a meaning, or null while it has none. Not owned: a role object is a
	// protocol object of its own with its own lifetime, and it lets go through `ForgetRole`.
	SurfaceRole* m_Role = nullptr;

	// True once the client has been ended for overrunning `MaxDamageRects`.
	bool m_Overrun = false;

	// Which outputs this surface has been sent an `enter` for and not a `leave`. Zero is a surface
	// nobody has told anything, which is where every surface starts and where an unmapped one returns.
	std::uint32_t m_Entered = 0;

	// The buffer `wl_surface.attach` staged, if it staged one. **The `optional` is the attach and the
	// resource inside it is the buffer**, which is not the same question: attaching nothing is a client
	// taking its window off the screen and has to be told apart from a commit that did not attach at
	// all, which keeps whatever was there.
	std::optional<Wayland::Server::WlBuffer> m_Attached;

	SurfaceState m_Pending;
	SurfaceState m_Current;

	// What a synchronized commit staged and the parent has not yet applied. **`m_HasCached` rather than
	// an `optional`, because the cache is the size of a whole surface state and a surface that has ever
	// been synchronized would otherwise pay an allocation every time it goes empty and fills again.**
	SurfaceState m_Cached;
	bool m_HasCached = false;

	// The subsurfaces, stated and applied.
	SurfaceStack m_Stack;
	SurfaceStack m_PendingStack;

	// Callbacks the client has asked for since the last commit, and the ones a commit has made due.
	// The handler rather than the resource, because the handler is what has a lifetime — the resource
	// is a pointer to libwayland's object and reaching it is `Object()`.
	std::vector<FrameCallback*> m_PendingCallbacks;
	std::vector<FrameCallback*> m_DueCallbacks;
};
