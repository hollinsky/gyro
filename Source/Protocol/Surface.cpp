#include "Protocol/Surface.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "Protocol/Buffer.h"
#include "Protocol/Region.h"

namespace
{
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
} // namespace

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

	// The client is gone and its window with it, so the pixels stop being drawn. Retiring is the id
	// giving up its name, not the memory going away — the registry holds both until the frame thread's
	// watermark says no published snapshot can still be recording from it.
	//
	// Outside a dispatch there is nothing to retire into: the display is destroyed with the host, and
	// the texture space is the composition root's and on its way out too.
	if (ITextures* const textures = m_Context->Textures(); textures != nullptr)
	{
		textures->Retire(m_Current.Content);
	}

	ReleaseStaged();

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
}

void ClientSurface::Present(Instant at) noexcept
{
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
		static_cast<std::uint32_t>(static_cast<std::uint64_t>(Monotonic::ToNanoseconds(at) / 1'000'000));

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

	m_Attached = buffer;

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
	Apply();
}

void ClientSurface::ReleaseStaged() noexcept
{
	if (m_Attached.has_value() && m_Attached->IsValid())
	{
		m_Attached->Release();
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
	ClientBuffer* const buffer = m_Attached->IsValid() ? ClientBuffer::Of(*m_Attached) : nullptr;

	if (buffer != nullptr)
	{
		if (const Result<TextureId> adopted = buffer->Adopt(textures); adopted)
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

	// Retired after the new id exists rather than before, so a device that fails the import leaves the
	// surface holding nothing rather than holding a name it has already given up.
	textures.Retire(replaced);

	// **A copied buffer goes back now and a borrowed one does not.** `wl_shm` pixels are gyro's the
	// moment `Adopt` returns, so the client may draw the next frame into the same memory immediately —
	// which is the whole reason for copying rather than sampling in place. Descriptors are borrowed
	// instead, and a client told it may reuse one would be drawing into the buffer a panel is scanning
	// out; that buffer answers its own release when the watermark says nobody is reading it.
	//
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

void ClientSurface::Apply()
{
	// The callbacks the client asked for since the last commit are the ones this commit owes. Appended
	// rather than replacing, because a commit whose callbacks were never sent still owes them — which
	// is every commit until the return leg is wired up, and after that is a surface committing twice
	// inside one frame.
	m_DueCallbacks.insert(m_DueCallbacks.end(), m_PendingCallbacks.begin(), m_PendingCallbacks.end());
	m_PendingCallbacks.clear();

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

	m_Current = m_Pending;

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
}
