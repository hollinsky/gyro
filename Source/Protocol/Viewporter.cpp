#include "Protocol/Viewporter.h"

#include <optional>

#include <wayland-server-core.h>

#include "Geometry/Space.h"

namespace
{
// The four arguments of `set_source` all at -1, which is the protocol's spelling of *unset* and the
// one combination of negatives that is not an error.
[[nodiscard]] bool IsUnset(wl_fixed_t x, wl_fixed_t y, wl_fixed_t width, wl_fixed_t height) noexcept
{
	const wl_fixed_t minusOne = wl_fixed_from_int(-1);

	return x == minusOne && y == minusOne && width == minusOne && height == minusOne;
}
} // namespace

ClientViewport::ClientViewport(HostContext& context, ClientSurface* surface) noexcept
	: m_Context{ &context }, m_Surface{ surface }
{}

ClientViewport::~ClientViewport()
{
	if (m_Surface != nullptr)
	{
		m_Surface->ForgetViewport(*this);
		m_Surface = nullptr;
	}
}

bool ClientViewport::HasSurface() const
{
	if (m_Surface != nullptr)
	{
		return true;
	}

	Object().PostError(
		Wayland::Server::WpViewportError::NoSurface,
		"a wp_viewport request after the wl_surface under it was destroyed"
	);

	return false;
}

void ClientViewport::OnSetSource(wl_fixed_t x, wl_fixed_t y, wl_fixed_t width, wl_fixed_t height)
{
	if (!HasSurface())
	{
		return;
	}

	if (IsUnset(x, y, width, height))
	{
		m_Surface->StageViewportSource(std::nullopt);

		return;
	}

	// **Refused at the request rather than at the commit, unlike the other two errors**, and the line
	// between them is whether the answer depends on the buffer. A negative origin or an empty extent is
	// wrong on its own terms whatever is attached; a rectangle reaching past the buffer is not, because
	// the buffer it will be measured against may not have arrived yet.
	if (x < 0 || y < 0 || width <= 0 || height <= 0)
	{
		Object().PostError(
			Wayland::Server::WpViewportError::BadValue,
			"wp_viewport.set_source with a negative origin or an empty extent"
		);

		return;
	}

	const Rect<SurfaceSpace> source{ { static_cast<float>(wl_fixed_to_double(x)),
		                               static_cast<float>(wl_fixed_to_double(y)) },
		                             { static_cast<float>(wl_fixed_to_double(width)),
		                               static_cast<float>(wl_fixed_to_double(height)) } };

	m_Surface->StageViewportSource(source);
}

void ClientViewport::OnSetDestination(std::int32_t width, std::int32_t height)
{
	if (!HasSurface())
	{
		return;
	}

	if (width == -1 && height == -1)
	{
		m_Surface->StageViewportDestination(std::nullopt);

		return;
	}

	if (width <= 0 || height <= 0)
	{
		Object().PostError(
			Wayland::Server::WpViewportError::BadValue,
			"wp_viewport.set_destination with a size that is not positive"
		);

		return;
	}

	m_Surface->StageViewportDestination(PixelSize<SurfaceSpace>{ width, height });
}

Wayland::Server::WpViewportHandler* ClientViewporter::OnGetViewport(Wayland::Server::WlSurface surface)
{
	ClientSurface* const onto = ClientSurface::Of(surface);

	// An inert object rather than a null, for `Protocol/Subcompositor.cpp`'s reason: the client is
	// holding an id and libwayland needs something behind it, and the connection is already on its way
	// out. There is no `bad_surface` here — `wp_viewporter` declares one error and it is not this — so
	// the client is ended through the object it named the argument on.
	if (onto == nullptr)
	{
		Object().PostNoMemory();

		return new ClientViewport{ *m_Context, nullptr };
	}

	auto* const viewport = new ClientViewport{ *m_Context, onto };

	// **Claimed last, exactly as a role is**, and a surface that already has a viewport keeps it: the
	// object the client just asked for stays inert rather than becoming a second writer of one
	// surface's crop and scale.
	if (!onto->AdoptViewport(*viewport))
	{
		Object().PostError(
			Wayland::Server::WpViewporterError::ViewportExists,
			"wp_viewporter.get_viewport on a surface that already has one"
		);

		viewport->ForgetSurface();
	}

	return viewport;
}

Wayland::Server::WpViewporterHandler* ViewporterGlobal::OnBind(wl_client& client, std::uint32_t version)
{
	(void)client;
	(void)version;

	return new ClientViewporter{ *m_Context };
}
