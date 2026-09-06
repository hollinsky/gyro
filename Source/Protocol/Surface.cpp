#include "Protocol/Surface.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "Protocol/Buffer.h"
#include "Protocol/ExplicitSync.h"
#include "Protocol/Presentation.h"
#include "Protocol/Region.h"
#include "Protocol/Subcompositor.h"
#include "Protocol/Viewporter.h"

namespace
{
// Tell every feedback in a list that what it was waiting on will never be seen, and free it.
//
// **Emptied first and sent out of a local**, which is `ClientSurface::Present`'s arrangement and is
// there for the same hazard: `Destroy` runs the feedback's `OnGone`, which calls `ForgetFeedback` back
// into the surface and erases from the list being walked.
void Discard(std::vector<ClientPresentationFeedback*>& held) noexcept
{
	std::vector<ClientPresentationFeedback*> going;

	going.swap(held);

	for (ClientPresentationFeedback* const feedback : going)
	{
		// The event and then the destruction, in that order and both from here — `discarded` releases the
		// object on the client's side, and the resource is still the server's to free.
		feedback->Object().Discarded();
		feedback->Object().Destroy();
	}
}

// A damage rectangle as the wire spells one. Negative extents are clamped away for Region.h's reason:
// nothing forbids a client sending one, and an inverted interval would make every containment test
// downstream answer backwards.
template<typename S>
[[nodiscard]] PixelRect<S> DamageRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) noexcept
{
	return { { x, y }, { std::max(width, 0), std::max(height, 0) } };
}

// Whether the wire handed over a value the enumeration actually has. libwayland validates an
// argument's *type* and not its range, so an out-of-range transform arrives as an enumerator that
// does not exist — and a switch over it later would fall through to whatever the compiler chose.
[[nodiscard]] bool IsKnown(Wayland::Server::WlOutputTransform transform) noexcept
{
	switch (transform)
	{
		case Wayland::Server::WlOutputTransform::Normal:
		case Wayland::Server::WlOutputTransform::_90:
		case Wayland::Server::WlOutputTransform::_180:
		case Wayland::Server::WlOutputTransform::_270:
		case Wayland::Server::WlOutputTransform::Flipped:
		case Wayland::Server::WlOutputTransform::Flipped90:
		case Wayland::Server::WlOutputTransform::Flipped180:
		case Wayland::Server::WlOutputTransform::Flipped270:
			return true;
	}

	return false;
}

// One `wl_fixed` unit, which is the tolerance the source rectangle is bounded against.
//
// **A rounded comparison rather than an exact one, because only one side of it came off the wire.** A
// client states the source in 256ths and gyro compares it against the buffer divided by the buffer
// scale, and where that scale does not divide the buffer the second number is not representable in
// the units the first one was written in. An exact `>` there refuses a client that asked for exactly
// the whole of its own buffer, which is the commonest source rectangle there is.
constexpr float FixedUnit = 1.0F / 256.0F;
} // namespace

Size<SurfaceSpace, float> SurfaceState::Extent() const noexcept
{
	if (Viewport.Destination.has_value())
	{
		return { static_cast<float>(Viewport.Destination->Width), static_cast<float>(Viewport.Destination->Height) };
	}

	if (Viewport.Source.has_value())
	{
		return Viewport.Source->Extent;
	}

	const auto scale = static_cast<float>(BufferScale > 0 ? BufferScale : 1);

	return { static_cast<float>(ContentSize.Width) / scale, static_cast<float>(ContentSize.Height) / scale };
}

Rect<BufferSpace> SurfaceState::Texels() const noexcept
{
	if (!Viewport.Source.has_value())
	{
		return { {}, { static_cast<float>(ContentSize.Width), static_cast<float>(ContentSize.Height) } };
	}

	// Surface-local back into the buffer's own space, which is the buffer scale and nothing else — the
	// transform belongs in the same conversion and is not applied anywhere in this compositor yet, so
	// putting half of it here would be a rotated video cropped along the wrong axis.
	const auto scale = static_cast<float>(BufferScale > 0 ? BufferScale : 1);
	const Rect<SurfaceSpace>& source = *Viewport.Source;

	return { { source.Origin.X * scale, source.Origin.Y * scale },
		     { source.Extent.Width * scale, source.Extent.Height * scale } };
}

bool ClientSurface::AdoptRole(SurfaceRole& role) noexcept
{
	if (m_Role != nullptr)
	{
		return false;
	}

	m_Role = &role;

	return true;
}

void ClientSurface::ForgetRole(const SurfaceRole& role) noexcept
{
	if (m_Role == &role)
	{
		m_Role = nullptr;
	}
}

bool ClientSurface::AdoptViewport(ClientViewport& viewport) noexcept
{
	if (m_Viewport != nullptr)
	{
		return false;
	}

	m_Viewport = &viewport;

	return true;
}

void ClientSurface::ForgetViewport(const ClientViewport& viewport) noexcept
{
	if (m_Viewport != &viewport)
	{
		return;
	}

	m_Viewport = nullptr;

	// **The crop and scale goes with the object, and it goes into the pending state rather than the
	// current one.** The protocol says destroying a `wp_viewport` removes the state from the surface
	// and that the change lands on the next commit — so a client that destroys its viewport and never
	// commits again keeps the size it last showed, which is the difference between a window returning
	// to its buffer size when it is asked to and doing it under a person mid-frame.
	m_Pending.Viewport = {};
}

bool ClientSurface::CheckViewport(const SurfaceState& state) const noexcept
{
	if (m_Viewport == nullptr || !state.Viewport.Source.has_value())
	{
		return true;
	}

	const Rect<SurfaceSpace>& source = *state.Viewport.Source;

	// **A source with no destination crops without scaling, so the surface's size *is* the source's** —
	// and a size the protocol requires to be whole cannot come out of a rectangle stated in 256ths.
	if (!state.Viewport.Destination.has_value())
	{
		const float width = source.Extent.Width;
		const float height = source.Extent.Height;

		if (width != std::floor(width) || height != std::floor(height))
		{
			m_Viewport->Object().PostError(
				Wayland::Server::WpViewportError::BadSize,
				"wp_viewport.set_source with a fractional size and no destination to scale it to"
			);

			return false;
		}
	}

	// A surface with no content has no buffer to be outside of, which the protocol states explicitly:
	// a client is free to describe the rectangle before it attaches the buffer that holds it.
	if (state.Content.IsNull())
	{
		return true;
	}

	const auto scale = static_cast<float>(state.BufferScale > 0 ? state.BufferScale : 1);
	const float width = static_cast<float>(state.ContentSize.Width) / scale;
	const float height = static_cast<float>(state.ContentSize.Height) / scale;

	if (source.Right() > width + FixedUnit || source.Bottom() > height + FixedUnit)
	{
		m_Viewport->Object().PostError(
			Wayland::Server::WpViewportError::OutOfBuffer,
			"wp_viewport.set_source with a rectangle reaching past the buffer it was applied to"
		);

		return false;
	}

	return true;
}

ClientSurface::~ClientSurface()
{
	// The role is told first, while the surface is still readable: it has a window in the scene to take
	// down, and a client destroying a `wl_surface` before its `xdg_surface` is a protocol error the
	// compositor still has to survive without leaving a node nothing can ever author again.
	if (SurfaceRole* const role = m_Role; role != nullptr)
	{
		m_Role = nullptr;

		role->OnSurfaceGone();
	}

	// **And the viewport, which is an object the client still holds after this.** Every request on a
	// `wp_viewport` whose surface has gone owes `no_surface` rather than reaching through a pointer
	// that is about to be freed — a client destroying a `wl_surface` and then its viewport, in that
	// order, is legal and is what a toolkit tearing a window down actually does.
	if (ClientViewport* const viewport = m_Viewport; viewport != nullptr)
	{
		m_Viewport = nullptr;

		viewport->ForgetSurface();
	}

	// **And the synchronization object, for the viewport's reason exactly.** A client destroying a
	// `wl_surface` and then its `wp_linux_drm_syncobj_surface_v1` is legal, and every request on the
	// second after the first owes `no_surface` rather than reaching through a freed pointer. Whatever
	// wait it had armed goes down with it, and any release point already handed to a buffer is the
	// buffer's now and is still owed.
	if (ClientSyncSurface* const sync = m_Sync; sync != nullptr)
	{
		m_Sync = nullptr;

		sync->ForgetSurface();
	}

	// The client is gone and its window with it, so the pixels stop being drawn. Retiring is the id
	// giving up its name, not the memory going away — the registry holds both until the frame thread's
	// watermark says no published snapshot can still be recording from it.
	//
	// Outside a dispatch there is nothing to retire into: the display is destroyed with the host, and
	// the texture space is the composition root's and on its way out too.
	if (ITextures* const textures = m_Context->Textures(); textures != nullptr)
	{
		textures->Retire(m_Current.Content);

		// **And the id a cache is holding, which is a second live name rather than the same one.** A
		// synchronized subsurface that committed and was never applied has adopted pixels the world has
		// not seen; nobody else will ever give that name up.
		if (m_Pending.Content != m_Current.Content)
		{
			textures->Retire(m_Pending.Content);
		}
	}

	ReleaseStaged();

	// The children are told before anything else goes, while the tree is still readable: each of them
	// has a subtree in the world hanging off this surface's own, and a `wl_subsurface` outliving the
	// `wl_surface` it was parented onto is a client error the compositor still has to survive.
	const SurfaceStack applied = m_Stack;
	const SurfaceStack stated = m_PendingStack;

	m_Stack = {};
	m_PendingStack = {};

	for (const SurfaceStack* const run : { &applied, &stated })
	{
		for (ClientSubsurface* const child : run->Below)
		{
			child->ForgetParent();
		}

		for (ClientSubsurface* const child : run->Above)
		{
			child->ForgetParent();
		}
	}

	// Destroying a callback runs its `OnGone`, which calls `Forget` back into this object and erases
	// from the list being walked. So the lists are emptied first and the resources destroyed out of
	// local copies — `Forget` then finds nothing and does nothing, which is what it is written to do.
	std::vector<FrameCallback*> pending;
	std::vector<FrameCallback*> due;

	pending.swap(m_PendingCallbacks);
	due.swap(m_DueCallbacks);

	for (FrameCallback* const callback : pending)
	{
		callback->Object().Destroy();
	}

	for (FrameCallback* const callback : due)
	{
		callback->Object().Destroy();
	}

	// **Both feedback lists are discarded rather than dropped**, and that is the difference between the
	// two protocols showing up at teardown: a frame callback that will never fire is simply destroyed,
	// because a client whose surface is gone is not waiting to draw into it. A
	// `wp_presentation_feedback` has an event for exactly this — the content update whose surface was
	// destroyed was never displayed — and sending it costs nothing where the client has already gone.
	Discard(m_PendingFeedback);
	Discard(m_DueFeedback);
}

void ClientSurface::Present(const SurfacePresentation& shown) noexcept
{
	PresentFeedback(shown);

	if (m_DueCallbacks.empty())
	{
		return;
	}

	// Milliseconds, truncated, and wrapping at thirty-two bits because that is the width the protocol
	// gives it. A toolkit differences two of these, and a difference across the wrap is correct in
	// unsigned arithmetic — which is why it is a truncation rather than a clamp.
	//
	// **`Monotonic::ToNanoseconds` is sanctioned here for the reason it names**: the count is handed
	// straight to something outside the process that takes one, which is a Wayland client rather than
	// io_uring or a trace file, but is the same statement. Nothing here does arithmetic in the domain —
	// decision 57 keeps that inside `Core/Time.h` — it converts once, at the edge, exactly where the
	// protocol demands a unit gyro does not otherwise use.
	const auto milliseconds =
		static_cast<std::uint32_t>(static_cast<std::uint64_t>(Monotonic::ToNanoseconds(shown.At) / 1'000'000));

	// Emptied first and sent out of a local, for the destructor's reason exactly: `Destroy` runs the
	// callback's `OnGone`, which calls `Forget` back into this object and erases from the list being
	// walked. `Forget` then finds nothing, which is what it is written to do.
	std::vector<FrameCallback*> due;

	due.swap(m_DueCallbacks);

	for (FrameCallback* const callback : due)
	{
		// The event and then the destruction, in that order and both from here. `wl_callback.done` is a
		// destructor request on the client's side — it releases the object on receiving this — but the
		// resource is the server's to free, and one left behind would be an object id nothing ever reuses
		// for as long as the client lives.
		callback->Object().Done(milliseconds);
		callback->Object().Destroy();
	}
}

void ClientSurface::PresentFeedback(const SurfacePresentation& shown) noexcept
{
	if (m_DueFeedback.empty())
	{
		return;
	}

	// **Seconds and nanoseconds rather than the callback's truncated milliseconds**, which is the whole
	// reason a client asks for one of these: the frame callback's unit cannot express a refresh
	// boundary, and a media player matching audio to a 144 Hz panel is measuring at seven milliseconds a
	// frame. Decision 57's conversion at the edge, at the resolution the protocol offers.
	const std::int64_t nanoseconds = Monotonic::ToNanoseconds(shown.At);
	const std::uint64_t seconds = static_cast<std::uint64_t>(nanoseconds) / 1'000'000'000U;
	const auto remainder = static_cast<std::uint32_t>(static_cast<std::uint64_t>(nanoseconds) % 1'000'000'000U);

	// The refresh is nanoseconds until the next one, and zero is the protocol's own *no useful
	// prediction*. A negative period is not expressible and would be a backend reporting nonsense, so it
	// lands as the same zero rather than as an enormous unsigned number.
	const auto refresh = static_cast<std::uint32_t>(
		shown.Refresh > Duration::zero() ? static_cast<std::uint64_t>(shown.Refresh.count()) : 0U
	);

	auto flags = static_cast<std::uint32_t>(0);

	if (shown.Vsync)
	{
		flags |= static_cast<std::uint32_t>(Wayland::Server::WpPresentationFeedbackKind::Vsync);
	}

	if (shown.HardwareClock)
	{
		flags |= static_cast<std::uint32_t>(Wayland::Server::WpPresentationFeedbackKind::HwClock);
	}

	if (shown.ZeroCopy)
	{
		flags |= static_cast<std::uint32_t>(Wayland::Server::WpPresentationFeedbackKind::ZeroCopy);
	}

	// **`hw_completion` is deliberately not among them.** It says the display hardware signalled the
	// start of the presentation as opposed to a timer having guessed, and nothing gyro reads answers
	// that question separately from `hw_clock` — `Seam/PresentationInfo.h` has three flags because those
	// are the three a backend can honestly fill in. Claiming a fourth from the strength of a third is
	// the fabrication that whole type exists to prevent.

	// Emptied first and sent out of a local, for `Present`'s reason exactly.
	std::vector<ClientPresentationFeedback*> due;

	due.swap(m_DueFeedback);

	for (ClientPresentationFeedback* const feedback : due)
	{
		// **`sync_output` before `presented`, and only where the client bound that output**, which the
		// protocol states both halves of: the event names a resource rather than an output, so a client
		// that never bound the global gyro would have named simply does not hear which panel it was.
		if (shown.Output.IsValid())
		{
			feedback->Object().SyncOutput(shown.Output);
		}

		feedback->Object().Presented(
			static_cast<std::uint32_t>(seconds >> 32U),
			static_cast<std::uint32_t>(seconds & 0xffffffffU),
			remainder,
			refresh,
			static_cast<std::uint32_t>(shown.Vblank >> 32U),
			static_cast<std::uint32_t>(shown.Vblank & 0xffffffffU),
			static_cast<Wayland::Server::WpPresentationFeedbackKind>(flags)
		);

		feedback->Object().Destroy();
	}
}

void ClientSurface::StageFeedback()
{
	// **What was already due is discarded, and this is the line the two protocols part on.** The
	// callbacks above merged, because *you may draw again* survives being asked twice. A feedback is
	// about one content update, and this commit is the update that superseded it — so the pixels it was
	// waiting on are ones nobody will ever see, and the protocol has an event that says exactly that.
	// Answering it later with this frame's timestamp would be a client measuring the latency of a frame
	// it never drew.
	Discard(m_DueFeedback);

	m_DueFeedback.swap(m_PendingFeedback);
}

void ClientSurface::AdoptFeedback(ClientPresentationFeedback& feedback)
{
	m_PendingFeedback.push_back(&feedback);
}

void ClientSurface::ForgetFeedback(const ClientPresentationFeedback& feedback) noexcept
{
	const auto matches = [&feedback](const ClientPresentationFeedback* held) noexcept { return held == &feedback; };

	std::erase_if(m_PendingFeedback, matches);
	std::erase_if(m_DueFeedback, matches);
}

void ClientSurface::Forget(const FrameCallback& callback) noexcept
{
	const auto matches = [&callback](const FrameCallback* held) noexcept { return held == &callback; };

	std::erase_if(m_PendingCallbacks, matches);
	std::erase_if(m_DueCallbacks, matches);
}

SurfaceShape ClientSurface::ShapeOf(Wayland::Server::WlRegion region)
{
	// A null region is the protocol's own way of saying *unset*, and both callers below give that its
	// own meaning before asking. What reaches here is a resource the client named, which may be one of
	// somebody else's objects entirely — `Implementation` is what refuses that rather than reading it,
	// checking the interface and the dispatch table before it touches any user data.
	//
	// The downcast is static because that check has already made it sound: the only party that creates
	// a `wl_region` against gyro's table is `Compositor.cpp`, and it creates a `ClientRegion` every
	// time. There is deliberately no second implementation of this interface for it to be wrong about.
	const Wayland::Server::WlRegionHandler* const handler = region.Implementation();

	if (handler == nullptr)
	{
		return {};
	}

	return static_cast<const ClientRegion*>(handler)->Shape();
}

void ClientSurface::OnGone()
{
	delete this;
}

void ClientSurface::OnAttach(Wayland::Server::WlBuffer buffer, std::int32_t x, std::int32_t y)
{
	// A second attach before a commit supersedes the first, and the buffer nobody ever read goes
	// straight back — the client is free to reuse it, and holding it would be a toolkit waiting on a
	// release for pixels gyro never looked at.
	ReleaseStaged();

	m_Attached.emplace(buffer);

	// The offset is surface state rather than buffer state, which is why it survives the attach being
	// superseded: the protocol folded `attach`'s `x` and `y` into `wl_surface.offset` at version 5
	// precisely because they were never about the buffer.
	if (Object().Version() >= 5)
	{
		if (x != 0 || y != 0)
		{
			Object().PostError(
				Wayland::Server::WlSurfaceError::InvalidOffset,
				"wl_surface.attach with a non-zero offset at version 5 or above"
			);
		}

		return;
	}

	m_Pending.Offset = { x, y };
}

void ClientSurface::OnDamage(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
	if (m_Overrun)
	{
		return;
	}

	if (m_Pending.SurfaceDamage.size() >= MaxDamageRects)
	{
		m_Overrun = true;
		Object().PostNoMemory();

		return;
	}

	m_Pending.SurfaceDamage.push_back(DamageRect<SurfaceSpace>(x, y, width, height));
}

void ClientSurface::OnDamageBuffer(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
	if (m_Overrun)
	{
		return;
	}

	if (m_Pending.BufferDamage.size() >= MaxDamageRects)
	{
		m_Overrun = true;
		Object().PostNoMemory();

		return;
	}

	m_Pending.BufferDamage.push_back(DamageRect<BufferSpace>(x, y, width, height));
}

Wayland::Server::WlCallbackHandler* ClientSurface::OnFrame()
{
	// Staged rather than answered: the callback becomes due at the commit that follows it, which is
	// what makes a client asking twice inside one commit get two callbacks at one instant rather than
	// one now and one later.
	auto* const callback = new FrameCallback{ *this };

	m_PendingCallbacks.push_back(callback);

	return callback;
}

void ClientSurface::OnSetOpaqueRegion(Wayland::Server::WlRegion region)
{
	// A null region is the empty one, which is the protocol's default and means the client promises
	// nothing about its opacity.
	m_Pending.Opaque = region.IsValid() ? ShapeOf(region) : SurfaceShape{};
}

void ClientSurface::OnSetInputRegion(Wayland::Server::WlRegion region)
{
	// A null region is *infinite* here rather than empty, which is the one place the two requests
	// differ and the place a compositor that shared their code gets wrong. `nullopt` is that value.
	if (!region.IsValid())
	{
		m_Pending.Input.reset();

		return;
	}

	m_Pending.Input = ShapeOf(region);
}

void ClientSurface::OnSetBufferTransform(Wayland::Server::WlOutputTransform transform)
{
	if (!IsKnown(transform))
	{
		Object().PostError(
			Wayland::Server::WlSurfaceError::InvalidTransform,
			"wl_surface.set_buffer_transform with a transform this protocol does not define"
		);

		return;
	}

	m_Pending.BufferTransform = transform;
}

void ClientSurface::OnSetBufferScale(std::int32_t scale)
{
	if (scale <= 0)
	{
		Object().PostError(
			Wayland::Server::WlSurfaceError::InvalidScale,
			"wl_surface.set_buffer_scale with a scale that is not positive"
		);

		return;
	}

	m_Pending.BufferScale = scale;
}

void ClientSurface::OnOffset(std::int32_t x, std::int32_t y)
{
	m_Pending.Offset = { x, y };
}

Wayland::Server::WlCallbackHandler* ClientSurface::OnGetRelease()
{
	// Unreachable at the version `wl_compositor` is advertised at, and the refusal is what keeps it
	// that way rather than a comment saying so. Reaching this means gyro advertised a contract it does
	// not implement, which is gyro's mistake and not the client's — so it is recorded as a fault, and
	// the null ends only the one client that asked, because there is no honest answer to give it.
	Wayland::Server::RecordFault("wl_surface.get_release reached a surface that does not implement it");

	return nullptr;
}

void ClientSurface::OnCommit()
{
	// **Explicit synchronization's four commit-time errors, asked before either path runs.** They are
	// questions about the pair the client staged rather than about the world, so they are answered where
	// the pair lives — and answered first, because a client that got them wrong is ended rather than
	// cached. The two points come across into this surface's own state in the same step, since the
	// object holding them may be destroyed the instant after this returns.
	if (m_Sync != nullptr && !m_Sync->TakeCommit(m_Attached, m_CommitAcquire, m_CommitRelease))
	{
		return;
	}

	// **A synchronized subsurface's commit changes nothing a person can see, and that is the whole of
	// the mode.** A toolkit moving a video and the controls over it commits each of them and then the
	// window, and what it is buying is that the three arrive together — a compositor that applied each
	// one as it landed would show the controls a frame away from the video they belong to, on every
	// frame of a drag.
	if (IsSynchronized())
	{
		Cache();

		return;
	}

	Apply();
}

void ClientSurface::AddChild(ClientSubsurface& child)
{
	// Topmost, which is where the protocol puts a new subsurface: above every sibling and above the
	// parent's own pixels.
	m_PendingStack.Above.push_back(&child);
}

void ClientSurface::RemoveChild(const ClientSubsurface& child) noexcept
{
	const auto matches = [&child](const ClientSubsurface* held) noexcept { return held == &child; };

	std::erase_if(m_Stack.Below, matches);
	std::erase_if(m_Stack.Above, matches);
	std::erase_if(m_PendingStack.Below, matches);
	std::erase_if(m_PendingStack.Above, matches);
}

void ClientSurface::UnmapChildren() noexcept
{
	// Out of a copy, because a child taking itself off screen may end up destroying a `wl_buffer` or a
	// resource whose teardown edits these runs. The same rule the callback lists are walked under.
	const SurfaceStack stack = m_Stack;

	for (ClientSubsurface* const child : stack.Below)
	{
		child->Unmap();
	}

	for (ClientSubsurface* const child : stack.Above)
	{
		child->Unmap();
	}
}

void ClientSurface::ApplyCached()
{
	if (!m_HasCached)
	{
		// **Nothing cached is nothing to cascade into.** A surface that has not committed since its own
		// parent last did has stated no arrangement, and its children's caches are waiting on *its*
		// commit rather than on this one — which is what makes the mode compose down a tree at all.
		return;
	}

	// **The cache waits on the same point a direct commit does, and the surface rather than the parent
	// is what waits.** A synchronized subsurface's state is applied by whoever commits above it, so the
	// exact reading of the protocol would hold the *parent's* whole commit until every child's acquire
	// point had signalled — which is the atomicity the mode exists for. gyro holds only the child, and
	// what that costs is a late subsurface landing a beat after the parent arrangement it belongs to.
	// It is a real cost and it is stated in Open.md rather than hidden: the alternative is one late
	// client freezing every surface in a tree, which is the failure this whole design refuses one level
	// up.
	if (HoldForAcquire())
	{
		return;
	}

	if (!CheckViewport(m_Cached))
	{
		return;
	}

	m_HeldForAcquire = false;
	m_CommitAcquire = {};

	const TextureId shown = m_Current.Content;

	m_Current = std::move(m_Cached);
	m_Cached = {};
	m_HasCached = false;

	if (shown != m_Current.Content)
	{
		if (ITextures* const textures = m_Context->Textures(); textures != nullptr)
		{
			textures->Retire(shown);
		}
	}

	// The callbacks stayed pending while the state was cached, which is what makes a client that asked
	// to be paced and was never applied go on waiting rather than being told about a frame its pixels
	// were not in. So did the feedbacks, and they land the same way — the content update this parent is
	// applying is the one the child described, however long ago it described it.
	m_DueCallbacks.insert(m_DueCallbacks.end(), m_PendingCallbacks.begin(), m_PendingCallbacks.end());
	m_PendingCallbacks.clear();

	StageFeedback();

	CommitChildren();
}

void ClientSurface::Cache()
{
	if (m_Attached.has_value())
	{
		if (ITextures* const textures = m_Context->Textures(); textures != nullptr)
		{
			TakeContent(*textures);
		}
		else
		{
			ReleaseStaged();
		}
	}

	// Damage survives the replacement and nothing else does. The older rectangles go first, which is
	// the order they were drawn in.
	std::vector<PixelRect<SurfaceSpace>> surface;
	std::vector<PixelRect<BufferSpace>> buffer;

	if (m_HasCached)
	{
		surface = std::move(m_Cached.SurfaceDamage);
		buffer = std::move(m_Cached.BufferDamage);
	}

	m_Cached = m_Pending;
	m_HasCached = true;

	surface.insert(surface.end(), m_Cached.SurfaceDamage.begin(), m_Cached.SurfaceDamage.end());
	buffer.insert(buffer.end(), m_Cached.BufferDamage.begin(), m_Cached.BufferDamage.end());

	m_Cached.SurfaceDamage = std::move(surface);
	m_Cached.BufferDamage = std::move(buffer);

	m_Pending.SurfaceDamage.clear();
	m_Pending.BufferDamage.clear();
}

void ClientSurface::CommitChildren()
{
	if (m_Stack.Below.empty() && m_Stack.Above.empty() && m_PendingStack.Below.empty() && m_PendingStack.Above.empty())
	{
		return;
	}

	m_Stack = m_PendingStack;

	// Out of a copy for `UnmapChildren`'s reason: a child mapping can end its own client, which unwinds
	// through every object that client holds and edits these runs underneath the walk.
	const SurfaceStack stack = m_Stack;

	for (ClientSubsurface* const child : stack.Below)
	{
		child->ParentCommitted();
	}

	for (ClientSubsurface* const child : stack.Above)
	{
		child->ParentCommitted();
	}

	Restack();
}

void ClientSurface::Restack()
{
	SceneStore* const scene = m_Context->Store();
	const EntityId container = Container();
	const EntityId content = ContentNode();

	if (scene == nullptr || container.IsNull() || content.IsNull())
	{
		return;
	}

	// Each node is placed after the last one that took its place, so the chain comes out in exactly the
	// order it is walked here — and a child with no node yet, which is one whose client has not drawn
	// into it, is skipped rather than reserving a gap.
	EntityId after{};

	for (const ClientSubsurface* const child : m_Stack.Below)
	{
		if (const EntityId node = child->Node(); !node.IsNull() && scene->Order(node, after))
		{
			after = node;
		}
	}

	if (scene->Order(content, after))
	{
		after = content;
	}

	for (const ClientSubsurface* const child : m_Stack.Above)
	{
		if (const EntityId node = child->Node(); !node.IsNull() && scene->Order(node, after))
		{
			after = node;
		}
	}
}

void ClientSurface::ReleaseStaged() noexcept
{
	if (m_Attached.has_value() && m_Attached->IsValid())
	{
		m_Attached->Get().Release();
	}

	m_Attached.reset();
}

void ClientSurface::TakeContent(ITextures& textures)
{
	const TextureId replaced = m_Pending.Content;

	m_Pending.Content = {};
	m_Pending.ContentSize = {};

	// A client that attached nothing is taking its window off the screen. Everything else about the
	// surface survives, which is what makes the next attach put it straight back.
	ClientBuffer* const buffer = m_Attached->IsValid() ? ClientBuffer::Of(m_Attached->Get()) : nullptr;

	if (buffer != nullptr)
	{
		// **The release point goes in with the adoption**, which is what pairs it with the one id it is
		// about: a buffer committed twice before the first frame left the screen has two ids and two
		// release points against it, and the buffer is what keeps them apart.
		if (const Result<TextureId> adopted = buffer->Adopt(textures, std::move(m_CommitRelease)); adopted)
		{
			m_Pending.Content = *adopted;
			m_Pending.ContentSize = buffer->Extent();
		}
		else
		{
			// gyro could not take the pixels — the texture space is full, or a renderer refused them.
			// The client is told so rather than left with a window that never appears, because a
			// compositor that quietly draws nothing is a bug report nobody can reproduce.
			Object().PostNoMemory();
		}
	}

	// **The capture is taken here and not one line later**, because `ReleaseStaged` below hands a
	// `wl_shm` buffer straight back to the client — the pixels are still the ones this commit was made
	// of only while the attach is alive. `m_Pending.BufferDamage` is live for the same reason: `Apply`
	// clears it after this returns, so this is the last point at which what the client *said* it
	// changed still exists to be written down beside what it actually handed over.
	if (buffer != nullptr)
	{
		Capture(*buffer, m_Pending.Content);
	}

	// Retired after the new id exists rather than before, so a device that fails the import leaves the
	// surface holding nothing rather than holding a name it has already given up.
	//
	// **Except the one the world is still drawing**, which is what a cache makes possible: a
	// synchronized subsurface adopts a frame the parent has not applied, so the id under it is still on
	// screen and giving up its name here would be a window sampling memory the registry has reclaimed.
	// `ApplyCached` retires it at the instant the scene stops naming it.
	if (replaced != m_Current.Content)
	{
		textures.Retire(replaced);
	}

	// **A copied buffer goes back now and a borrowed one does not.** `wl_shm` pixels are gyro's the
	// moment `Adopt` returns, so the client may draw the next frame into the same memory immediately —
	// which is the whole reason for copying rather than sampling in place. Descriptors are borrowed
	// instead, and a client told it may reuse one would be drawing into the buffer a panel is scanning
	// out; that buffer answers its own release when the watermark says nobody is reading it.
	//
	// Consumed whether or not there was a buffer to hand it to, because it described *this* commit and
	// a point left staged would be signalled for the next one's buffer.
	m_CommitRelease = {};

	// The staged attach is dropped either way, because it has been consumed whichever it was.
	if (buffer == nullptr || buffer->ReleasesImmediately())
	{
		ReleaseStaged();
	}
	else
	{
		m_Attached.reset();
	}
}

void ClientSurface::Capture(ClientBuffer& buffer, TextureId content)
{
	ISurfaceCapture* const sink = m_Context->Capture();

	// **Asked before the rows are fetched**, which is the reason Scene/Capture.h has two verbs rather
	// than one: a pool gyro could not map is read with `pread` into a scratch buffer, and doing that on
	// every commit for a key nobody pressed would be a copy of every window on the dispatch thread.
	if (sink == nullptr || !sink->Wanted())
	{
		return;
	}

	const std::span<const std::byte> pixels = buffer.MappedRows();

	// **Empty rows are a descriptor and are offered anyway**, which is the whole of what dmabuf capture
	// changes on this side. The frame thread reads those pixels off the device later, and what it has
	// no other way to learn is on this stack right now: the extent the client declared, what its top
	// byte means, and above all the damage `Apply` is about to drop on the floor. So the offer goes out
	// with the id in place of the rows.
	//
	// What is refused instead is a commit that adopted nothing — a texture space that was full, or a
	// renderer that would not take the layout. There is no image behind that id and the client has
	// already been told so with `PostNoMemory`.
	if (pixels.empty() && content.IsNull())
	{
		return;
	}

	// The wire id rather than a name of gyro's own, so a capture reads beside a `WAYLAND_DEBUG` log
	// with no table in between — and the pid beside it, because a wire id is per connection and two
	// clients own the same low numbers. Scene/Capture.h carries what that cost before it was sent.
	const std::uint32_t id = ::wl_resource_get_id(Object().WireResource());

	pid_t pid = 0;
	uid_t uid = 0;
	gid_t gid = 0;

	::wl_client_get_credentials(Object().WireClient(), &pid, &uid, &gid);

	sink->Offer(
		SurfaceCapture{ .Surface = id,
	                    .Client = static_cast<std::uint32_t>(pid),
	                    .Size = buffer.Extent(),
	                    .Stride = buffer.MappedStride(),
	                    .Alpha = buffer.MappedAlpha(),
	                    .Pixels = pixels,
	                    .Texture = content,
	                    .Damage = m_Pending.BufferDamage }
	);
}

bool ClientSurface::AdoptSync(ClientSyncSurface& sync) noexcept
{
	if (m_Sync != nullptr)
	{
		return false;
	}

	m_Sync = &sync;

	return true;
}

void ClientSurface::ForgetSync(const ClientSyncSurface& sync) noexcept
{
	if (m_Sync == &sync)
	{
		m_Sync = nullptr;
	}
}

bool ClientSurface::HoldForAcquire()
{
	// **An unset point is not a client that is ready, it is a commit with nothing to synchronize** —
	// every surface without one of these objects, and every commit that did not attach.
	if (!m_CommitAcquire.IsSet())
	{
		return false;
	}

	// The query, which is the only question asked of the client's counter on this path. A point that has
	// already signalled is the ordinary case for a client that finished its frame before it committed,
	// and it costs one ioctl and no wait at all.
	if (m_CommitAcquire.HasSignalled())
	{
		m_CommitAcquire = {};

		return false;
	}

	if (m_Sync == nullptr || !m_Sync->Watch(m_CommitAcquire))
	{
		// Nothing could be armed, so waiting would be waiting forever. Publishing samples a buffer the
		// client has not finished, which is a tear; a window that never updates again is worse, and the
		// cause is gyro's kernel rather than the client. The warning is at the arming site, where the
		// errno is.
		m_CommitAcquire = {};

		return false;
	}

	m_HeldForAcquire = true;

	return true;
}

void ClientSurface::OnAcquireSignalled()
{
	m_HeldForAcquire = false;
	m_CommitAcquire = {};

	// **Which half resumes is which half held.** A synchronized subsurface's state is in the cache and a
	// direct commit's is in pending, and the two are exclusive: a surface is synchronized or it is not,
	// for as long as its role says so.
	if (m_HasCached)
	{
		ApplyCached();

		return;
	}

	Publish();
}

void ClientSurface::Apply()
{
	// The callbacks the client asked for since the last commit are the ones this commit owes. Appended
	// rather than replacing, because a commit whose callbacks were never sent still owes them — which
	// is every commit until the return leg is wired up, and after that is a surface committing twice
	// inside one frame.
	m_DueCallbacks.insert(m_DueCallbacks.end(), m_PendingCallbacks.begin(), m_PendingCallbacks.end());
	m_PendingCallbacks.clear();

	StageFeedback();

	// **A commit that did not attach keeps the content it had**, which is the protocol's rule and the
	// reason this is gated on the attach rather than on the buffer: a client committing a new input
	// region and nothing else must not lose its window.
	if (m_Attached.has_value())
	{
		if (ITextures* const textures = m_Context->Textures(); textures != nullptr)
		{
			TakeContent(*textures);
		}
		else
		{
			ReleaseStaged();
		}
	}

	// **Held here and nowhere earlier**, which is what makes explicit synchronization cost a late
	// client a frame and cost gyro nothing. Everything above has already happened — the buffer is
	// imported, the callbacks are owed, the release point is registered against the id — and what waits
	// is only the moment the world starts naming it. The client's fence never reaches gyro's queue or a
	// plane, so a client that is late holds up its own window and no other. See decision 174.
	if (HoldForAcquire())
	{
		return;
	}

	Publish();
}

void ClientSurface::Publish()
{
	m_HeldForAcquire = false;
	m_CommitAcquire = {};

	// **After the attach and before the adoption**, which is the one point the buffer this state will
	// be shown with is known: `out_of_buffer` is a question about the buffer that arrived in this very
	// commit, and asking before `TakeContent` would measure the source against the previous frame's.
	if (!CheckViewport(m_Pending))
	{
		return;
	}

	const TextureId shown = m_Current.Content;

	m_Current = m_Pending;

	// The id the world has just stopped naming, for `TakeContent`'s reason: it declines to retire one
	// that is still on screen, and this is where it stops being.
	if (shown != m_Current.Content)
	{
		if (ITextures* const textures = m_Context->Textures(); textures != nullptr)
		{
			textures->Retire(shown);
		}
	}

	// Damage is consumed by the commit and everything else is sticky, which is the protocol's own
	// asymmetry. The assignment above is a copy rather than a move for the same reason: pending has to
	// remain exactly what it was minus the damage, and a state object that is sometimes moved out of
	// is one whose sticky fields are sticky only until somebody reorders this function.
	m_Pending.SurfaceDamage.clear();
	m_Pending.BufferDamage.clear();

	// **Last, so the role reads a surface that has finished committing.** Everything above is what the
	// commit made true; the role turns that into a window, and it must not see a half-applied state —
	// which is the same atomicity the double buffering exists for, one level up.
	if (m_Role != nullptr)
	{
		m_Role->OnSurfaceCommitted(*this);
	}

	// **And last of all the children, because a subsurface needs the surface it hangs off to be in the
	// world before it can be.** The role above is what puts it there.
	CommitChildren();
}
