#include "Protocol/Surface.h"

#include <algorithm>
#include <vector>

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

ClientSurface::~ClientSurface()
{
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

void ClientSurface::Forget(const FrameCallback& callback) noexcept
{
	const auto matches = [&callback](const FrameCallback* held) noexcept { return held == &callback; };

	std::erase_if(m_PendingCallbacks, matches);
	std::erase_if(m_DueCallbacks, matches);
}

SurfaceRegion ClientSurface::ShapeOf(Wayland::Server::WlRegion region)
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
	// **The buffer is deliberately not held yet.** `wl_shm` is the next step and there is nothing to
	// adopt one into, so a surface still has no content and Wayland's own rule — a surface with no
	// buffer is not shown — is the honest state rather than a stub. What is honoured here is the
	// offset, because it is surface state and not buffer state: the protocol folded `attach`'s `x` and
	// `y` into `wl_surface.offset` at version 5 precisely because they were never about the buffer.
	(void)buffer;

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
	m_Pending.Opaque = region.IsValid() ? ShapeOf(region) : SurfaceRegion{};
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

void ClientSurface::Apply()
{
	// The callbacks the client asked for since the last commit are the ones this commit owes. Appended
	// rather than replacing, because a commit whose callbacks were never sent still owes them — which
	// is every commit until the return leg is wired up, and after that is a surface committing twice
	// inside one frame.
	m_DueCallbacks.insert(m_DueCallbacks.end(), m_PendingCallbacks.begin(), m_PendingCallbacks.end());
	m_PendingCallbacks.clear();

	m_Current = m_Pending;

	// Damage is consumed by the commit and everything else is sticky, which is the protocol's own
	// asymmetry. The assignment above is a copy rather than a move for the same reason: pending has to
	// remain exactly what it was minus the damage, and a state object that is sometimes moved out of
	// is one whose sticky fields are sticky only until somebody reorders this function.
	m_Pending.SurfaceDamage.clear();
	m_Pending.BufferDamage.clear();
}
