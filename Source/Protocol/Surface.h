#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "Geometry/Space.h"
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
// **Nothing here reaches the scene yet, and the file is honest about which half is which.** A surface
// with no buffer is not shown by Wayland's own definition, and the buffer path — `wl_shm`, the
// adoption into the texture space, the entity parented into the floor — is the step after this one.
// What is complete is the state machine: the fields, the staging, the protocol errors a client can
// provoke, and the atomicity. What is absent is a consumer, so `Apply` moves pending to current and
// stops there.
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
	SurfaceRegion Opaque;

	// Where the surface accepts pointer and touch input, or nothing for the default, which is
	// **infinite** — the whole surface and, per the protocol, beyond it. The `optional` is carrying
	// that distinction and not an unset flag: an input region a client explicitly emptied is a window
	// that deliberately lets clicks through, and it must not read the same as one that never set one.
	std::optional<SurfaceRegion> Input;

	// What changed, in each of the two spaces the client may name it in.
	std::vector<PixelRect<SurfaceSpace>> SurfaceDamage;
	std::vector<PixelRect<BufferSpace>> BufferDamage;
};

class ClientSurface final : public Wayland::Server::WlSurfaceHandler
{
public:
	ClientSurface() = default;

	~ClientSurface() override;

	// What the last `commit` made true. The only state anything outside this object may read.
	[[nodiscard]] const SurfaceState& Current() const noexcept { return m_Current; }

	// How many frame callbacks the last commit made due.
	//
	// **They are held rather than answered, and that is this step's one visible gap.** A callback is
	// answered when the surface's content is about to be shown, which is a fact only the return leg
	// knows — `Scene/Return.h` drains what reached the glass — and there is nothing on the far end of
	// it for a surface with no buffer. Until the buffer path lands, a client that asks for one waits;
	// with no way to map a window, there is nothing for it to be waiting on. The count is what a test
	// can assert on in the meantime, and the list is private because the only legitimate thing to do
	// with one is send it.
	[[nodiscard]] std::size_t DueCallbackCount() const noexcept { return m_DueCallbacks.size(); }

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
	[[nodiscard]] static SurfaceRegion ShapeOf(Wayland::Server::WlRegion region);

	// True once the client has been ended for overrunning `MaxDamageRects`.
	bool m_Overrun = false;

	SurfaceState m_Pending;
	SurfaceState m_Current;

	// Callbacks the client has asked for since the last commit, and the ones a commit has made due.
	// The handler rather than the resource, because the handler is what has a lifetime — the resource
	// is a pointer to libwayland's object and reaching it is `Object()`.
	std::vector<FrameCallback*> m_PendingCallbacks;
	std::vector<FrameCallback*> m_DueCallbacks;
};
