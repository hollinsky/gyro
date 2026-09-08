#include "Protocol/Fractional.h"

#include <wayland-server-core.h>

#include <algorithm>
#include <cstddef>

#include "Protocol/Surface.h"
#include "Scene/Store.h"

Scale PreferredScale(OutputReach reach, std::span<const SceneOutput> outputs) noexcept
{
	if (outputs.empty())
	{
		return Scale{};
	}

	Scale preferred{};
	bool found = false;

	for (std::size_t index = 0; index < outputs.size() && index < MaxReachableOutputs; ++index)
	{
		// Every output where the surface is on none of them — see the header for why the empty reach
		// folds this way rather than to identity.
		if (reach != 0 && (reach & (OutputReach{ 1 } << index)) == 0)
		{
			continue;
		}

		preferred = found ? std::max(preferred, outputs[index].Density) : outputs[index].Density;
		found = true;
	}

	return preferred;
}

ClientFractionalScale::ClientFractionalScale(HostContext& context, ClientSurface* surface) noexcept
	: m_Context{ &context }, m_Surface{ surface }
{}

ClientFractionalScale::~ClientFractionalScale()
{
	if (m_Surface != nullptr)
	{
		m_Surface->ForgetFractionalScale(*this);
		m_Surface = nullptr;
	}
}

void ClientFractionalScale::Send(Scale scale)
{
	if (m_Sent.has_value() && *m_Sent == scale)
	{
		return;
	}

	m_Sent = scale;

	Object().PreferredScale(static_cast<std::uint32_t>(scale.Numerator()));
}

void ClientFractionalScale::OnBound()
{
	// **The first event goes out here rather than waiting for the surface to be on a screen**, and it
	// carries the densest output because this surface is on none yet — see `PreferredScale` for the
	// trade. `SyncOutputEntry` runs later in the same `Advance` and corrects it before the client is
	// flushed, so a window opening on the densest panel is told once and a window opening elsewhere is
	// told twice, before it has drawn either time.
	//
	// **`OnBound` rather than the request handler that made this object**, which is the whole of the
	// fix and is not a matter of taste: the binding layer creates the `wl_resource` *after*
	// `OnGetFractionalScale` returns, so an event sent from in there is written to an object that does
	// not exist yet and goes nowhere. On its own that would only lose the early send — what made it
	// silent and total is `Send`'s dedupe, which recorded the scale as sent and then suppressed every
	// later, real one from `SyncEntry`. The protocol delivered nothing at all to a surface sitting on
	// the densest output, which on a one-monitor machine is every surface there is.
	if (const SceneStore* const store = m_Context->Store(); store != nullptr)
	{
		Send(PreferredScale(0, store->Outputs()));
	}
}

Wayland::Server::WpFractionalScaleV1Handler*
ClientFractionalScaleManager::OnGetFractionalScale(Wayland::Server::WlSurface surface)
{
	ClientSurface* const onto = ClientSurface::Of(surface);

	// An inert object rather than a null, for `Protocol/Viewporter.cpp`'s reason: the client is holding
	// an id and libwayland needs something behind it, and the connection is already on its way out.
	// This protocol declares one error and it is not this, so the client is ended through the object it
	// named the argument on.
	if (onto == nullptr)
	{
		Object().PostNoMemory();

		return new ClientFractionalScale{ *m_Context, nullptr };
	}

	auto* const fractional = new ClientFractionalScale{ *m_Context, onto };

	// **Claimed last, exactly as a viewport is**, and a surface that already has one keeps it: the
	// object the client just asked for stays inert rather than becoming a second thing told to speak
	// for one surface's scale.
	if (!onto->AdoptFractionalScale(*fractional))
	{
		Object().PostError(
			Wayland::Server::WpFractionalScaleManagerV1Error::FractionalScaleExists,
			"wp_fractional_scale_manager_v1.get_fractional_scale on a surface that already has one"
		);

		fractional->ForgetSurface();

		return fractional;
	}

	// The first event goes out from `OnBound` rather than from here, because here is too early for it
	// to go anywhere at all — see `ClientFractionalScale::OnBound`.
	return fractional;
}

Wayland::Server::WpFractionalScaleManagerV1Handler*
FractionalScaleGlobal::OnBind(wl_client& client, std::uint32_t version)
{
	(void)client;
	(void)version;

	return new ClientFractionalScaleManager{ *m_Context };
}
