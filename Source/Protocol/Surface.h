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
#include "Protocol/Sync.h"
#include "Wayland/Server/Wayland.h"
#include "Wayland/Server/Weak.h"

class ClientPresentationFeedback;
class ClientBuffer;
class ClientSyncSurface;

// One frame reaching the glass, as much of it as a client is owed.
//
// **It is the panel's account rather than gyro's**, which is what separates it from the instant a
// frame callback carries. `wl_surface.frame` answers *draw again* and needs only a stamp; a
// `wp_presentation_feedback` is a client asking how good that stamp is — whether the flip was
// synchronized, whether the time came from display hardware, which panel it happened on and how long
// until the next one — and every field here exists because the protocol has a place for it and nothing
// else in this module can obtain it. [Presentation.h](Presentation.h) carries the rest.
struct SurfacePresentation
{
	// When the frame reached the glass, in the one timebase. The same instant a frame callback is
	// stamped from, so the two events a commit produces never disagree about the frame they describe.
	Instant At{};

	// How long after `At` the next refresh is expected. The output's observed period where the backend
	// measured one and the mode's nominal period otherwise, because the protocol's own answer for *no
	// useful prediction* is zero and a mode gyro programmed is a better prediction than none.
	Duration Refresh{};

	// The panel's own retrace counter for this flip, or zero on an output that has no such thing. Never
	// gyro's published sequence, which counts something else entirely.
	std::uint64_t Vblank = 0;

	// The output the flip happened on, as a resource in *this surface's client's* id space — invalid
	// where that client never bound the global, which the protocol handles by simply not sending
	// `sync_output`.
	Wayland::Server::WlOutput Output{};

	bool Vsync = false;
	bool HardwareClock = false;
	bool ZeroCopy = false;
};

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

// `wp_viewport`'s crop and scale.
//
// **It is state on the surface rather than on the viewport object, which is the protocol's own
// placement and not a convenience.** The viewport is a handle onto one part of a `wl_surface`'s
// double buffering — its two requests stage exactly as `set_buffer_scale` stages, land at the same
// commit, and are removed by the object being destroyed — so putting the fields anywhere else would
// be a second buffering to keep in step with this one.
//
// Both halves are independent and either may be set alone, which is what the two `optional`s carry.
struct SurfaceViewport
{
	// What part of the buffer the surface shows, in **surface-local coordinates**: the protocol's own
	// ordering is transform, then scale, then crop, so the rectangle a client states is in the
	// coordinates the surface would have had if it had never set a viewport at all. Nothing is the
	// whole buffer.
	std::optional<Rect<SurfaceSpace>> Source;

	// What the surface's size becomes, whatever the buffer under it is — the half Firefox and every
	// fractionally scaled toolkit actually use, and the reason a window is not twice the size it
	// should be. Nothing leaves the size derived from the buffer and the scale.
	//
	// Integer, because the protocol says so: a destination that is not a whole number of surface-local
	// pixels is refused at the request rather than rounded here.
	std::optional<PixelSize<SurfaceSpace>> Destination;
};

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

	// The crop and scale a `wp_viewport` staged into this state, or two absences for the surfaces that
	// have never had one — which is most of them, and costs two words rather than an indirection.
	SurfaceViewport Viewport;

	// **How big the surface is, which is the one question three call sites were each answering.** A
	// destination overrides everything; a source with no destination crops without scaling, so the
	// size is the source's; and with neither it is the buffer divided by the scale the client declared.
	//
	// The division is real rather than integer. A buffer that is not a whole multiple of its own scale
	// is a client that has already gone wrong, and truncating the size while still sampling every texel
	// is a window stretched by a fraction of a pixel — which is invisible in a screenshot and visible
	// as one soft row along an edge.
	[[nodiscard]] Size<SurfaceSpace, float> Extent() const noexcept;

	// The texels those pixels come from, in the buffer's own space: the viewport's source rectangle
	// carried back through the buffer scale, or the whole buffer where there is none.
	//
	// **Stated rather than left empty even when it is the whole buffer**, which is decision 152's
	// partition asking to be able to tell a window drawn texel for texel from one being resampled: a
	// node that says nothing here can never be told apart from one that is being stretched, and the
	// first is what goes on a plane.
	[[nodiscard]] Rect<BufferSpace> Texels() const noexcept;
};

class ClientSurface;
class ClientSubsurface;
class ClientViewport;

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
	//
	// **The two events it answers are the same fact told to different depths**, and both are answered
	// here so that neither can be sent for a frame the other was not: a client that received a
	// `presented` and no callback would draw once and stop.
	void Present(const SurfacePresentation& shown) noexcept;

	// How many `wp_presentation_feedback` objects are waiting on a frame reaching the glass. Beside
	// `DueCallbackCount` and for its reason — the only legitimate thing to do with one is answer it.
	[[nodiscard]] std::size_t DueFeedbackCount() const noexcept { return m_DueFeedback.size(); }

	// Stage a feedback against the next commit. `wp_presentation.feedback` is asked before the content
	// update it is about, exactly as `wl_surface.frame` is.
	void AdoptFeedback(ClientPresentationFeedback& feedback);

	// Drop a feedback from whichever list holds it, without sending it anything. Called by the feedback
	// itself as it goes away, which is the client having disconnected or having destroyed the surface
	// under it — in both cases there is nobody left to tell.
	void ForgetFeedback(const ClientPresentationFeedback& feedback) noexcept;

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

	// Claim this surface's crop and scale. False where a `wp_viewport` already has it, which is
	// `wp_viewporter`'s `viewport_exists` and is raised by the caller for the role's reason.
	//
	// **Separate from the role and never in competition with it.** A viewport is not what a surface
	// *is* — a window with a viewport on it is still a window — so a surface carries at most one of
	// each and the two questions are asked independently.
	[[nodiscard]] bool AdoptViewport(ClientViewport& viewport) noexcept;

	// The viewport is going away, which the protocol says removes the crop and scale from the surface
	// at its next commit. So the staged state goes with it rather than at the destroy, which is the
	// same double buffering the two requests had.
	void ForgetViewport(const ClientViewport& viewport) noexcept;

	// The two halves of the crop and scale, staged into this surface's pending state.
	//
	// **Verbs rather than a mutable reference into `m_Pending`**, because the pending state's writers
	// are otherwise all in this file and a handle handed out to another one is how that stops being
	// checkable. What a viewport may write is these two fields and nothing else, and that is said here
	// rather than trusted there.
	void StageViewportSource(std::optional<Rect<SurfaceSpace>> source) noexcept { m_Pending.Viewport.Source = source; }

	void StageViewportDestination(std::optional<PixelSize<SurfaceSpace>> destination) noexcept
	{
		m_Pending.Viewport.Destination = destination;
	}

	// Claim this surface's explicit synchronization. False where a `wp_linux_drm_syncobj_surface_v1`
	// already has it, which is that protocol's `surface_exists` and is raised by the caller for the
	// role's reason.
	//
	// **A third claim beside the role and the viewport, and independent of both**, because it is not
	// what a surface *is* either: a window, a subsurface and a cursor can each be explicitly
	// synchronized, and each may have exactly one of these.
	[[nodiscard]] bool AdoptSync(ClientSyncSurface& sync) noexcept;

	// The synchronization object is going away. The staged points go with it, which is what the protocol
	// says a destroy may do to points set since the last commit; a commit already held keeps its wait,
	// because the object holds that and takes it down itself.
	void ForgetSync(const ClientSyncSurface& sync) noexcept;

	// The acquire point this surface's commit was held on has signalled. Called from the event loop
	// source the synchronization object armed, which runs inside `Server::Poll` and therefore inside an
	// `Advance` — so the store and the texture space are on the stack exactly as they are for a request.
	void OnAcquireSignalled();

	// Whether a commit is waiting on a client's acquire point. A test's way of asserting that a late
	// client was held rather than that nothing was drawn.
	[[nodiscard]] bool IsHeldForAcquire() const noexcept { return m_HeldForAcquire; }

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

	// The trace row this surface's client owns, per [Trace.h](Trace.h). Asked per record rather than
	// held, because the answer is the connection's and a surface must not be the thing that keeps a row
	// number alive after the client behind it has gone.
	[[nodiscard]] std::uint16_t TraceRow() const noexcept;

	// Answer everything the last commit made due with a `presented`. `Present`'s other half, split out
	// because the two events have different units and different rules about being superseded, and one
	// function carrying both would be two unrelated derivations under one name.
	void PresentFeedback(const SurfacePresentation& shown) noexcept;

	// Discard what this commit superseded and make what the client staged due. Called from both commit
	// paths, which is what keeps a synchronized subsurface's cached update landing at the parent's
	// commit rather than at its own.
	void StageFeedback();

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

	// Whether this commit has to wait, and the arming of that wait.
	//
	// **The check is a query and never a wait**, which is the whole of decision 174: a client's fence
	// must not reach gyro's queue or a plane, so what happens here is a `DRM_IOCTL_SYNCOBJ_QUERY` and,
	// where it says *not yet*, an eventfd the kernel will increment. False for a point that has already
	// signalled, for a surface with no synchronization object, and for a wait that could not be armed —
	// the last of which publishes rather than holding the window forever, because a kernel that refused
	// the ioctl is gyro's problem and a frozen window would be the client's punishment for it.
	[[nodiscard]] bool HoldForAcquire();

	// The state the client stated becomes the state the world has. The tail of `Apply`, split out
	// because it is also what an acquire point signalling resumes — and the split is where the whole of
	// this protocol lands: everything above it is bookkeeping a commit does immediately, and everything
	// below it is what a person can see.
	void Publish();

	// Pending becomes current, and pending's per-commit accumulations reset.
	//
	// **Damage resets and the rest does not**, which is the asymmetry the protocol actually specifies
	// and the one a hand-written commit gets wrong: scale, transform, offset and the two regions are
	// *sticky* — a client that sets a scale once and commits ten times has that scale on all ten — but
	// damage describes one frame and a commit consumes it. Carrying damage forward would repaint a
	// region that has been correct for nine frames, every frame, forever.
	void Apply();

	// **The two errors `wp_viewport` defers to the commit**, asked of the state about to become current
	// and answered on the viewport object, because they are its errors and not the surface's.
	//
	// They are here rather than at the request for the reason the protocol gives: both are questions
	// about the *buffer*, and a client is free to state a source rectangle before it attaches the
	// buffer that rectangle fits in. Asking at `set_source` would refuse a legal ordering.
	//
	// False having ended the client. A viewport already destroyed leaves nothing to check and nothing
	// to report it on, which is also a state whose crop and scale is on its way out.
	[[nodiscard]] bool CheckViewport(const SurfaceState& state) const noexcept;

	// The shape a `wl_region` resource currently describes, or an empty shape for a null resource. A
	// copy, per Region.h: the client may destroy the region the instant this returns and usually does.
	[[nodiscard]] static SurfaceShape ShapeOf(Wayland::Server::WlRegion region);

	// Turn the staged buffer into pending content, retire what it replaces, and release it back to the
	// client. Called by `Apply` and only when an attach is actually pending, because a commit that did
	// not attach keeps the content it had.
	void TakeContent(ITextures& textures);

	// Offer this commit's buffer to Scene/Capture.h's sink, where the chord has armed one. Called from
	// `TakeContent` while the attach is still alive and the damage has not been cleared.
	// `content` is the id this commit adopted, which is what a descriptor offers in place of rows.
	void Capture(ClientBuffer& buffer, TextureId content);

	// Hand a buffer back and forget it was staged. Safe on an invalid resource, which is what a detach
	// stages.
	void ReleaseStaged() noexcept;

	// The world this surface's requests act on, for the duration of the `Advance` they arrive in. Never
	// null; what is null outside a dispatch is what it points at.
	HostContext* m_Context = nullptr;

	// The `wp_viewport` on this surface, or null for a surface that has never had one. Not owned, and
	// it lets go through `ForgetViewport` exactly as the role does.
	ClientViewport* m_Viewport = nullptr;

	// Whatever gave this surface a meaning, or null while it has none. Not owned: a role object is a
	// protocol object of its own with its own lifetime, and it lets go through `ForgetRole`.
	SurfaceRole* m_Role = nullptr;

	// The `wp_linux_drm_syncobj_surface_v1` on this surface, or null for every surface that has never
	// had one — which is most of them. Not owned, and it lets go through `ForgetSync`.
	ClientSyncSurface* m_Sync = nullptr;

	// The two points the commit being applied named, moved out of the synchronization object by
	// `TakeCommit` and consumed by `TakeContent` and `HoldForAcquire` respectively.
	//
	// **They are on the surface rather than on the object because they belong to the state**, exactly
	// as the crop and scale a `wp_viewport` stages do: the object is a handle that may be destroyed the
	// instant after a commit, and what has to survive that is the commit.
	SyncTimelinePoint m_CommitAcquire;
	SyncTimelinePoint m_CommitRelease;

	// True while a commit is waiting on `m_CommitAcquire`. The state stays in `m_Pending` or `m_Cached`
	// meanwhile and the world goes on showing the last frame this client finished.
	bool m_HeldForAcquire = false;

	// True once the client has been ended for overrunning `MaxDamageRects`.
	bool m_Overrun = false;

	// Which outputs this surface has been sent an `enter` for and not a `leave`. Zero is a surface
	// nobody has told anything, which is where every surface starts and where an unmapped one returns.
	std::uint32_t m_Entered = 0;

	// The buffer `wl_surface.attach` staged, if it staged one. **The `optional` is the attach and the
	// resource inside it is the buffer**, which is not the same question: attaching nothing is a client
	// taking its window off the screen and has to be told apart from a commit that did not attach at
	// all, which keeps whatever was there.
	//
	// **Weak, because this is the one resource on this surface that outlives the request that named
	// it.** An attach stages a buffer and the commit that consumes it may be several requests later,
	// and a client is entitled to destroy the buffer in between — at which point libwayland frees the
	// resource and hands the block back for whatever that client makes next. A raw resource here was a
	// `wl_buffer.release` sent to a `wl_region`.
	std::optional<Wayland::Server::Weak<Wayland::Server::WlBuffer>> m_Attached;

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

	// The same two lists for `wp_presentation_feedback`, and they are separate lists rather than a
	// second field on the callbacks because the two protocols disagree about what a second commit
	// means.
	//
	// **A callback that was superseded is still owed and a feedback that was superseded is
	// discarded.** `wl_surface.frame` answers *you may draw again*, which stays true however many
	// commits a client made inside one refresh — so those merge, and `Apply` appends. A
	// `wp_presentation_feedback` is about one specific content update, and a client that committed over
	// it before the panel scanned is one whose pixels were never seen; telling it they were would put a
	// timestamp on a frame that does not exist, which is the one lie this protocol is asked not to
	// tell. So `Apply` discards what is due before staging what is pending.
	std::vector<ClientPresentationFeedback*> m_PendingFeedback;
	std::vector<ClientPresentationFeedback*> m_DueFeedback;
};
